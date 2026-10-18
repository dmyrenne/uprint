/*
 * Status-LEDs: ein Task liest etwa zehnmal pro Sekunde den Zustand von Drucker und WLAN, wählt den Zustand mit
 * der höchsten Priorität und zeichnet dessen Effekt mit ~30 Bildern pro Sekunde. Den Strip legt nur dieser Task
 * an und ändert ihn, Web-Handler setzen nur Anforderungen (unter s_lock).
 *
 * Effekte und Zeiten bildet tools/mock_server.py (virtueller Strip) nach.
 */
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"

#include "printer.h"
#include "settings.h"
#include "status_led.h"
#include "wifi.h"

static const char *TAG = "status_led";

#define NVS_NAMESPACE   "leds"
#define NVS_KEY         "config"
#define CONFIG_VERSION  1
#define MAX_LEDS        100
#define FRAME_MS        33
#define POLL_FRAMES     3       // Zustand nur jedes dritte Bild abfragen
#define FIND_MS         10000
#define PREVIEW_MS      5000
#define PREVIEW_VALUE   0.6f
#define ID_SHOW_MS      2000
#define BOOT_MS         1500
#define BREATHE_MS      3000
#define BLINK_MS        500     // 2 Hz
#define CHASE_MS        1200
#define BAR_EDGE_MS     2000
#define BAR_PULSE_MS    2500
#define GAMMA           2.2f
#define IDLE_MA         1       // Ruhestrom je LED (Schätzung wie in WLED)

typedef struct {
    float r, g, b;
} px_t;

// geschützt durch s_lock
static SemaphoreHandle_t s_lock;
static led_config_t s_cfg;
static bool s_reapply = true;
static int64_t s_find_until;
static int64_t s_preview_until;
static led_state_t s_preview_state;
static led_look_t s_preview_look;
static int s_ota = -1;

// Nur im LED-Task
static led_strip_handle_t s_strip;
static int s_count;
static bool s_white;
static float s_brightness;
static int s_max_ma;          // 0 = keine Begrenzung
static int s_led_ma;          // Strom einer LED bei vollem Weiß
static volatile int s_current_ma;   // geschätzter Strom des letzten Bildes
static int64_t s_boot_until;
static px_t s_px[MAX_LEDS];

static int s_id_hue;   // Farbton der Standardkennung, aus der MAC-Adresse

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static const char *const STATE_NAMES[LED_STATE_COUNT] = {
    "find", "ota", "error", "heating", "printing", "paused", "finished", "ap", "connecting", "no_device", "ready", "id",
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Farbton 0–359 bei voller Sättigung und Helligkeit
static void hue_rgb(int hue, uint8_t out[3])
{
    hue %= 360;
    int x = (hue % 60) * 255 / 60;
    uint8_t up = (uint8_t)x, down = (uint8_t)(255 - x);
    const uint8_t table[6][3] = {
        {255, up, 0}, {down, 255, 0}, {0, 255, up}, {0, down, 255}, {up, 0, 255}, {255, 0, down},
    };
    memcpy(out, table[hue / 60], 3);
}

void status_led_default_config(led_config_t *out)
{
    // Effekt, Farbe, Helligkeit je Zustand (README, Abschnitt Status-LEDs)
    static const led_look_t DEFAULT_LOOKS[LED_STATE_COUNT] = {
        [LED_STATE_FIND]       = {LED_FX_BLINK,     255, 255, 255, 50},
        [LED_STATE_OTA]        = {LED_FX_BAR,       140,   0, 255, 50},
        [LED_STATE_ERROR]      = {LED_FX_BREATHE,   255,   0,   0, 50},
        [LED_STATE_HEATING]    = {LED_FX_BAR,       255,  60,   0, 50},
        [LED_STATE_PRINTING]   = {LED_FX_BAR,         0,  80, 255, 50},
        [LED_STATE_PAUSED]     = {LED_FX_BAR_PULSE, 255, 150,   0, 50},
        [LED_STATE_FINISHED]   = {LED_FX_SOLID,       0, 255,  40, 50},
        [LED_STATE_AP]         = {LED_FX_BREATHE,   140,   0, 255, 30},
        [LED_STATE_CONNECTING] = {LED_FX_CHASE,       0, 200, 255, 50},
        [LED_STATE_NO_DEVICE]  = {LED_FX_FIRST,     255, 150,   0, 10},
        [LED_STATE_READY]      = {LED_FX_FIRST,       0, 255,  40, 10},
        [LED_STATE_ID]         = {LED_FX_SOLID,       0,   0,   0, 50},
    };
    memset(out, 0, sizeof(*out));
    memcpy(out->look, DEFAULT_LOOKS, sizeof(DEFAULT_LOOKS));
    // Kennung: Farbe aus der MAC-Adresse, die Zusatzfarben um 120° und 240° versetzt
    uint8_t rgb[3];
    hue_rgb(s_id_hue, rgb);
    led_look_t *id = &out->look[LED_STATE_ID];
    id->r = rgb[0];
    id->g = rgb[1];
    id->b = rgb[2];
    hue_rgb(s_id_hue + 120, out->seg[0]);
    hue_rgb(s_id_hue + 240, out->seg[1]);
}

static bool has_value(led_state_t state)
{
    return state == LED_STATE_OTA || state == LED_STATE_HEATING || state == LED_STATE_PRINTING ||
           state == LED_STATE_PAUSED;
}

bool status_led_effect_allowed(led_state_t state, led_effect_t effect)
{
    if (state >= LED_STATE_COUNT || effect >= LED_FX_COUNT) {
        return false;
    }
    if (effect == LED_FX_BAR || effect == LED_FX_BAR_PULSE) {
        return has_value(state);
    }
    if (effect == LED_FX_SEGMENTS) {
        return state == LED_STATE_ID;
    }
    return true;
}

const char *status_led_state_name(led_state_t state)
{
    return state < LED_STATE_COUNT ? STATE_NAMES[state] : "";
}

static const char *config_error(const led_config_t *c)
{
    for (int i = 0; i < LED_STATE_COUNT; i++) {
        if (!status_led_effect_allowed(i, c->look[i].effect)) {
            return "Effekt ist für diesen Zustand nicht möglich";
        }
        if (c->look[i].bright > 100) {
            return "Helligkeit: 0–100 %";
        }
    }
    if (c->id_interval != 0 && c->id_interval != 10 && c->id_interval != 30 && c->id_interval != 60) {
        return "Abstand der Kennung: aus, 10, 30 oder 60 s";
    }
    return NULL;
}

static void load_config(void)
{
    status_led_default_config(&s_cfg);
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t blob[1 + sizeof(led_config_t)];
    size_t len = sizeof(blob);
    if (nvs_get_blob(nvs, NVS_KEY, blob, &len) == ESP_OK && len == sizeof(blob) && blob[0] == CONFIG_VERSION) {
        led_config_t c;
        memcpy(&c, blob + 1, sizeof(c));
        if (!config_error(&c)) {
            s_cfg = c;
        } else {
            ESP_LOGW(TAG, "Gespeicherte Darstellung ungültig, nehme die Standardwerte");
        }
    }
    nvs_close(nvs);
}

static esp_err_t store_config(const led_config_t *c)
{
    uint8_t blob[1 + sizeof(led_config_t)];
    blob[0] = CONFIG_VERSION;
    memcpy(blob + 1, c, sizeof(*c));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY, blob, sizeof(blob));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return err;
}

// ---------- Strip ----------

static void strip_close(void)
{
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
}

static void strip_open(void)
{
    strip_close();
    settings_t cfg;
    settings_get(&cfg);
    s_count = cfg.led_count;
    s_white = cfg.led_white == 1;
    s_brightness = cfg.led_brightness / 100.0f;
    s_max_ma = cfg.led_max_ma;
    s_led_ma = cfg.led_ma;
    s_current_ma = 0;
    if (cfg.led_pin < 0 || cfg.led_count <= 0) {
        return;
    }
    // Position von R, G, B im Datenstrom je Reihenfolge, W immer dahinter
    static const uint8_t ORDER[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {2, 0, 1}, {1, 2, 0}, {2, 1, 0}};
    static const led_model_t MODELS[] = {LED_MODEL_WS2812, LED_MODEL_SK6812, LED_MODEL_WS2811};
    const uint8_t *pos = ORDER[cfg.led_order];
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = cfg.led_pin,
        .max_leds = cfg.led_count,
        .led_model = MODELS[cfg.led_type],
        .color_component_format = {.format = {.r_pos = pos[0], .g_pos = pos[1], .b_pos = pos[2], .w_pos = 3,
                                              .bytes_per_color = 1, .num_components = s_white ? 4 : 3}},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED-Strip an GPIO %d nicht verfügbar: %s", cfg.led_pin, esp_err_to_name(err));
        s_strip = NULL;
        return;
    }
    ESP_LOGI(TAG, "%d LEDs an GPIO %d", cfg.led_count, cfg.led_pin);
    s_boot_until = now_ms() + BOOT_MS;
}

// Geschätzter Strom in mA: jeder Kanal bei 255 zieht ein Drittel des Stroms bei vollem Weiß (wie in WLED)
static float estimate_ma(float level_sum)
{
    return s_count * IDLE_MA + level_sum / 255.0f * s_led_ma / 3.0f;
}

static void strip_show(void)
{
    // Summe aller Kanäle nach Gesamthelligkeit; der Weißkanal zieht bei RGBW etwa so viel wie ein Farbkanal
    float sum = 0.0f;
    for (int i = 0; i < s_count; i++) {
        float r = fminf(255.0f, s_px[i].r * s_brightness);
        float g = fminf(255.0f, s_px[i].g * s_brightness);
        float b = fminf(255.0f, s_px[i].b * s_brightness);
        sum += s_white ? r + g + b - 2.0f * fminf(r, fminf(g, b)) : r + g + b;
    }
    // Strombegrenzung: alle LEDs gleichmäßig dunkler, bis die Schätzung unter der Grenze liegt
    float scale = s_brightness;
    float ma = estimate_ma(sum);
    if (s_max_ma > 0 && ma > s_max_ma && sum > 0.0f) {
        float budget = fmaxf(0.0f, s_max_ma - s_count * IDLE_MA);
        float k = budget / (ma - s_count * IDLE_MA);
        scale *= k;
        ma = estimate_ma(sum * k);
    }
    s_current_ma = (int)lroundf(ma);

    for (int i = 0; i < s_count; i++) {
        uint32_t r = (uint32_t)lroundf(fminf(255.0f, s_px[i].r * scale));
        uint32_t g = (uint32_t)lroundf(fminf(255.0f, s_px[i].g * scale));
        uint32_t b = (uint32_t)lroundf(fminf(255.0f, s_px[i].b * scale));
        if (s_white) {
            // Gemeinsamen Weißanteil auf den Weißkanal legen
            uint32_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);
            led_strip_set_pixel_rgbw(s_strip, i, r - w, g - w, b - w, w);
        } else {
            led_strip_set_pixel(s_strip, i, r, g, b);
        }
    }
    led_strip_refresh(s_strip);
}

// ---------- Effekte ----------

// 0 … 1 … 0 über eine Periode
static float wave(int64_t t, int period)
{
    return 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)(t % period) / (float)period);
}

static float gamma_level(float x)
{
    return powf(fmaxf(0.0f, fminf(1.0f, x)), GAMMA);
}

static void fill(px_t c, float level)
{
    for (int i = 0; i < s_count; i++) {
        s_px[i] = (px_t){c.r * level, c.g * level, c.b * level};
    }
}

static px_t look_color(const uint8_t rgb[3], uint8_t bright)
{
    float k = bright / 100.0f;
    return (px_t){rgb[0] * k, rgb[1] * k, rgb[2] * k};
}

static void render(const led_config_t *cfg, led_state_t state, float value, int64_t t)
{
    const led_look_t *lk = &cfg->look[state];
    const uint8_t rgb[3] = {lk->r, lk->g, lk->b};
    px_t c = look_color(rgb, lk->bright);
    int n = s_count;
    memset(s_px, 0, sizeof(s_px[0]) * n);

    switch (lk->effect) {
    case LED_FX_SOLID:
        fill(c, 1.0f);
        break;
    case LED_FX_BREATHE:
        fill(c, gamma_level(0.1f + 0.9f * wave(t, BREATHE_MS)));
        break;
    case LED_FX_BLINK:
        fill(c, t % BLINK_MS < BLINK_MS / 2 ? 1.0f : 0.0f);
        break;
    case LED_FX_CHASE: {
        // Kopf mit kurzem Schweif
        float head = (float)(t % CHASE_MS) / CHASE_MS * n;
        for (int i = 0; i < n; i++) {
            float d = fmodf(head - i + n, (float)n);
            float level = d < 1.0f ? 1.0f : d < 2.0f ? 0.35f : d < 3.0f ? 0.1f : 0.0f;
            s_px[i] = (px_t){c.r * level, c.g * level, c.b * level};
        }
        break;
    }
    case LED_FX_FIRST:
        if (n > 0) {
            s_px[0] = c;
        }
        break;
    case LED_FX_BAR:
    case LED_FX_BAR_PULSE: {
        float f = fmaxf(0.0f, fminf(1.0f, value)) * n;
        int full = (int)f;
        float pulse = lk->effect == LED_FX_BAR_PULSE ? gamma_level(0.3f + 0.7f * wave(t, BAR_PULSE_MS)) : 1.0f;
        for (int i = 0; i < n; i++) {
            float level = i < full ? 1.0f : 0.0f;
            if (i == full) {
                // Vorderkante: anteilig hell, beim einfachen Balken zusätzlich leicht atmend
                level = gamma_level(f - full);
                if (lk->effect == LED_FX_BAR) {
                    level = fmaxf(level, 0.3f * gamma_level(wave(t, BAR_EDGE_MS)));
                }
            }
            level *= pulse;
            s_px[i] = (px_t){c.r * level, c.g * level, c.b * level};
        }
        break;
    }
    case LED_FX_SEGMENTS: {
        const uint8_t *colors[3] = {rgb, cfg->seg[0], cfg->seg[1]};
        for (int i = 0; i < n; i++) {
            s_px[i] = look_color(colors[i * 3 / n], lk->bright);
        }
        break;
    }
    default:
        break;
    }
}

static void render_boot(int64_t t)
{
    for (int i = 0; i < s_count; i++) {
        uint8_t rgb[3];
        hue_rgb((int)(t * 360 / BOOT_MS) + i * 360 / s_count, rgb);
        s_px[i] = look_color(rgb, 50);
    }
}

// ---------- Zustand ----------

static led_state_t pick_state(float *value)
{
    printer_status_t ps;
    wifi_status_t ws;
    printer_get_status(&ps);
    wifi_get_status(&ws);
    *value = ps.file_size ? (float)ps.file_pos / ps.file_size : 0.0f;

    if (ps.state == PRINTER_ERROR) {
        return LED_STATE_ERROR;
    }
    if (ps.state == PRINTER_PRINTING) {
        bool hot = ps.hotend_target > 0 && ps.hotend < ps.hotend_target - 3;
        bool bed = ps.bed_target > 0 && ps.bed < ps.bed_target - 2;
        if (hot || bed) {
            float h = hot ? ps.hotend / ps.hotend_target : 1.0f;
            float b = bed ? ps.bed / ps.bed_target : 1.0f;
            *value = fminf(h, b);
            return LED_STATE_HEATING;
        }
        return LED_STATE_PRINTING;
    }
    if (ps.state == PRINTER_PAUSED) {
        return LED_STATE_PAUSED;
    }
    if (ps.state == PRINTER_IDLE && ps.finished) {
        return LED_STATE_FINISHED;
    }
    if (ws.ap_active && !ws.ap_only) {
        return LED_STATE_AP;
    }
    if ((ws.configured && !ws.connected) || ps.state == PRINTER_CONNECTING) {
        return LED_STATE_CONNECTING;
    }
    if (ps.state == PRINTER_DISCONNECTED) {
        return LED_STATE_NO_DEVICE;
    }
    return LED_STATE_READY;
}

static void led_task(void *arg)
{
    led_state_t state = LED_STATE_READY;
    float value = 0.0f;
    for (int frame = 0;; frame++) {
        LOCK();
        bool reapply = s_reapply;
        s_reapply = false;
        led_config_t cfg = s_cfg;
        int64_t find_until = s_find_until, preview_until = s_preview_until;
        led_state_t preview = s_preview_state;
        if (now_ms() < s_preview_until) {
            cfg.look[preview] = s_preview_look;
        }
        int ota = s_ota;
        UNLOCK();

        if (reapply) {
            strip_open();
        }
        if (!s_strip) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (frame % POLL_FRAMES == 0) {
            state = pick_state(&value);
        }

        int64_t t = now_ms();
        led_state_t show = state;
        float show_value = value;
        if (t < preview_until) {
            show = preview;
            show_value = PREVIEW_VALUE;
        } else if (t < find_until) {
            show = LED_STATE_FIND;
        } else if (ota >= 0) {
            show = LED_STATE_OTA;
            show_value = ota / 100.0f;
        } else if (cfg.id_interval > 0 && t % (cfg.id_interval * 1000) < ID_SHOW_MS) {
            show = LED_STATE_ID;
        }

        if (t < s_boot_until) {
            render_boot(t);
        } else {
            render(&cfg, show, show_value, t);
        }
        strip_show();
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

// ---------- Schnittstelle ----------

void status_led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    uint8_t mac[6];
    uint32_t hash = 2166136261u;   // FNV-1a
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        for (int i = 0; i < 6; i++) {
            hash = (hash ^ mac[i]) * 16777619u;
        }
    }
    s_id_hue = (int)(hash % 360);
    load_config();
    xTaskCreate(led_task, "status_led", 3072, NULL, 2, NULL);
}

void status_led_apply(void)
{
    LOCK();
    s_reapply = true;
    UNLOCK();
}

void status_led_get_config(led_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

esp_err_t status_led_set_config(const led_config_t *in, const char **error)
{
    *error = config_error(in);
    if (*error) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = store_config(in);
    if (err != ESP_OK) {
        *error = "Speichern fehlgeschlagen";
        return err;
    }
    LOCK();
    s_cfg = *in;
    UNLOCK();
    return ESP_OK;
}

esp_err_t status_led_reset_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    led_config_t c;
    status_led_default_config(&c);
    LOCK();
    s_cfg = c;
    UNLOCK();
    return ESP_OK;
}

void status_led_find(void)
{
    LOCK();
    s_find_until = now_ms() + FIND_MS;
    UNLOCK();
}

void status_led_preview(led_state_t state, const led_look_t *look)
{
    if (state >= LED_STATE_COUNT) {
        return;
    }
    LOCK();
    s_preview_state = state;
    s_preview_look = look ? *look : s_cfg.look[state];
    s_preview_until = now_ms() + PREVIEW_MS;
    UNLOCK();
}

int status_led_current_ma(void)
{
    return s_strip ? s_current_ma : 0;
}

void status_led_ota(int percent)
{
    LOCK();
    s_ota = percent;
    UNLOCK();
}
