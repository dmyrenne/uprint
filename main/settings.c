#include <ctype.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "bootloader_random.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "settings.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "settings"

static const settings_t DEFAULTS = {
    .hostname = "uprint",
    .ap_password = "",   // offen bis zur Ersteinrichtung, die ein Passwort festlegt (#31)
    .device_type = DEVICE_PRINTER,
    .baud = 115200,
    .pause_lift = 5,
    .cancel_lift = 10,
    .park_x = 0,
    .park_y = 200,       // MK3S: Bett nach vorn
    .plot_draw = 50,
    .plot_travel = 150,
    .plot_accel = 1000,
    .pen_servo = 0,
    .pen_up = 60,        // Vorgaben der AxiDraw-Software
    .pen_down = 30,
    .pen_delay = 200,
    .sd_enable = 1,
#ifdef UPRINT_BOARD_XIAO
    // Steckplatz auf dem Kamerabrett des XIAO ESP32S3 Sense (GPIO 10–18 belegt dort die Kamera)
    .sd_mosi = 9,   // D10
    .sd_miso = 8,   // D9
    .sd_sclk = 7,   // D8
    .sd_cs = 21,    // nur über den Steckplatz erreichbar
#else
    .sd_mosi = 11,
    .sd_miso = 13,
    .sd_sclk = 12,
    .sd_cs = 10,
#endif
#ifdef UPRINT_BOARD_XIAO
    .led_pin = 1,          // D0
#else
    .led_pin = -1,
#endif
    .led_count = 0,
    .led_brightness = 20,
    .led_max_ma = 500,
    .led_ma = 55,          // WS2812/SK6812 bei vollem Weiß
    .led_type = LED_TYPE_SK6812,
    .led_order = LED_ORDER_GRB,
    .led_white = 0,
};

static SemaphoreHandle_t s_lock;
static settings_t s_cur;
static settings_t s_boot;   // Stand beim Start, für "Neustart nötig"
static char s_default_host[33];   // "uprint-3a5c": eindeutig je Gerät, damit sich mehrere nicht im Netz in die Quere kommen

#define FIELD(name) {#name, offsetof(settings_t, name)}
const settings_int_field_t SETTINGS_INT_FIELDS[] = {
    FIELD(device_type), FIELD(baud), FIELD(pause_lift), FIELD(cancel_lift), FIELD(park_x), FIELD(park_y),
    FIELD(sd_enable), FIELD(sd_mosi), FIELD(sd_miso), FIELD(sd_sclk), FIELD(sd_cs),
    FIELD(plot_draw), FIELD(plot_travel), FIELD(plot_accel), FIELD(pen_servo), FIELD(pen_up), FIELD(pen_down), FIELD(pen_delay),
    FIELD(led_pin), FIELD(led_count), FIELD(led_brightness), FIELD(led_type), FIELD(led_order), FIELD(led_white),
    FIELD(led_max_ma), FIELD(led_ma),
};
const size_t SETTINGS_INT_FIELD_COUNT = sizeof(SETTINGS_INT_FIELDS) / sizeof(SETTINGS_INT_FIELDS[0]);

// Zeichenketten im NVS; fehlt ein Eintrag, gilt der Wert aus DEFAULTS
typedef struct {
    const char *key;
    size_t offset;
    size_t size;
} str_field_t;

#define STR_FIELD(name) {#name, offsetof(settings_t, name), sizeof(((settings_t *)0)->name)}
static const str_field_t STR_FIELDS[] = {
    STR_FIELD(hostname), STR_FIELD(device_name), STR_FIELD(api_key), STR_FIELD(ap_password),
};

#define STR_AT(s, f) ((char *)(s) + (f)->offset)

static const char *sd_pins_error(const settings_t *s);
static const char *led_error(const settings_t *s);

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cur = DEFAULTS;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_default_host, sizeof(s_default_host), "uprint-%02x%02x", mac[4], mac[5]);
    } else {
        strlcpy(s_default_host, DEFAULTS.hostname, sizeof(s_default_host));
    }
    strlcpy(s_cur.hostname, s_default_host, sizeof(s_cur.hostname));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        for (size_t i = 0; i < sizeof(STR_FIELDS) / sizeof(STR_FIELDS[0]); i++) {
            const str_field_t *f = &STR_FIELDS[i];
            size_t len = f->size;
            if (nvs_get_str(nvs, f->key, STR_AT(&s_cur, f), &len) != ESP_OK) {
                // Ohne gespeicherten Hostnamen gilt der eindeutige Standard (#35), bestehende Geräte behalten ihren
                const char *def = f->offset == offsetof(settings_t, hostname) ? s_default_host : STR_AT(&DEFAULTS, f);
                strlcpy(STR_AT(&s_cur, f), def, f->size);
            }
        }
        for (size_t i = 0; i < SETTINGS_INT_FIELD_COUNT; i++) {
            int32_t v;
            if (nvs_get_i32(nvs, SETTINGS_INT_FIELDS[i].key, &v) == ESP_OK) {
                SETTINGS_INT(&s_cur, &SETTINGS_INT_FIELDS[i]) = v;
            }
        }
        nvs_close(nvs);
    }
    // Z. B. nach dem Wechsel von basic auf xiao: Dort belegt die Kamera die alten Standard-Pins
    if (s_cur.sd_enable && sd_pins_error(&s_cur)) {
        ESP_LOGW(TAG, "Gespeicherte SD-Pins passen nicht zu dieser Variante, nehme die Standardwerte");
        s_cur.sd_mosi = DEFAULTS.sd_mosi;
        s_cur.sd_miso = DEFAULTS.sd_miso;
        s_cur.sd_sclk = DEFAULTS.sd_sclk;
        s_cur.sd_cs = DEFAULTS.sd_cs;
    }
    if (led_error(&s_cur)) {
        ESP_LOGW(TAG, "Gespeicherte LED-Einstellungen ungültig, nehme die Standardwerte");
        s_cur.led_pin = DEFAULTS.led_pin;
        s_cur.led_count = DEFAULTS.led_count;
        s_cur.led_brightness = DEFAULTS.led_brightness;
        s_cur.led_type = DEFAULTS.led_type;
        s_cur.led_order = DEFAULTS.led_order;
        s_cur.led_white = DEFAULTS.led_white;
        s_cur.led_max_ma = DEFAULTS.led_max_ma;
        s_cur.led_ma = DEFAULTS.led_ma;
    }
    s_boot = s_cur;
    if (strlen(s_cur.api_key) != 32) {
        // Erster Start: WLAN läuft noch nicht, daher Entropiequelle des Bootloaders zuschalten
        bootloader_random_enable();
        settings_new_api_key();
        bootloader_random_disable();
    }
    ESP_LOGI(TAG, "Hostname %s, %d Baud, SD MOSI=%d MISO=%d SCLK=%d CS=%d", s_cur.hostname, s_cur.baud,
             s_cur.sd_mosi, s_cur.sd_miso, s_cur.sd_sclk, s_cur.sd_cs);
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

#ifdef UPRINT_BOARD_XIAO
// XIAO ESP32S3: herausgeführt sind D0–D5 (GPIO 1–6), D6/D7 (GPIO 43/44, Konsole) und D8–D10 (GPIO 7–9), GPIO 21
// ist CS des SD-Steckplatzes auf dem Kamerabrett. Die übrigen GPIOs belegen Kamera, Mikrofon, PSRAM und USB.
// D7 ist der Empfangspin der Konsole und darf belegt werden, die Log-Ausgabe auf D6 bleibt frei. Die Web-UI zeigt
// die Pins mit ihrer Beschriftung auf dem Board, gespeichert wird die GPIO-Nummer.
#define PIN_HINT "SD-Pins: D0–D5, D7–D10 oder der Steckplatz des Sense (CS)"
static bool pin_usable(int pin)
{
    return (pin >= 1 && pin <= 9) || pin == 21 || pin == 44;
}
#else
// ESP32-S3 mit Octal-PSRAM (N16R8): 19/20 USB, 22–25 gibt es nicht, 26–37 Flash/PSRAM, 43/44 Konsole
#define PIN_HINT "SD-Pins: GPIO 0–18, 21 oder 38–48 (außer 43/44)"
static bool pin_usable(int pin)
{
    return pin >= 0 && pin <= 48 && pin != 19 && pin != 20 && !(pin >= 22 && pin <= 37) && pin != 43 && pin != 44;
}
#endif

static const char *sd_pins_error(const settings_t *s)
{
    const int pins[] = {s->sd_mosi, s->sd_miso, s->sd_sclk, s->sd_cs};
    for (int i = 0; i < 4; i++) {
        if (!pin_usable(pins[i])) {
            return PIN_HINT;
        }
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                return "SD-Pins müssen verschieden sein";
            }
        }
    }
    return NULL;
}

static const char *led_error(const settings_t *s)
{
    if (s->led_count < 0 || s->led_count > 100) {
        return "Anzahl LEDs: 0–100";
    }
    if (s->led_brightness < 1 || s->led_brightness > 100) {
        return "LED-Helligkeit: 1–100 %";
    }
    if (s->led_type < LED_TYPE_WS2812 || s->led_type > LED_TYPE_WS2811 || s->led_order < LED_ORDER_RGB ||
        s->led_order > LED_ORDER_BGR || (s->led_white != 0 && s->led_white != 1)) {
        return "Unbekannter LED-Typ oder unbekannte Farbreihenfolge";
    }
    if (s->led_max_ma != 0 && (s->led_max_ma < 100 || s->led_max_ma > 10000)) {
        return "LED-Stromgrenze: 0 (keine) oder 100–10000 mA";
    }
    if (s->led_ma < 5 || s->led_ma > 100) {
        return "Strom je LED: 5–100 mA";
    }
    bool on_sd = s->sd_enable && (s->led_pin == s->sd_mosi || s->led_pin == s->sd_miso || s->led_pin == s->sd_sclk ||
                                  s->led_pin == s->sd_cs);
    if (s->led_pin != -1 && (!pin_usable(s->led_pin) || on_sd)) {
        return "LED-Pin: -1 (aus) oder ein freier Pin, der nicht von der SD-Karte belegt ist";
    }
    return NULL;
}

static const char *validate(const settings_t *s)
{
    for (const unsigned char *p = (const unsigned char *)s->device_name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return "Gerätename: keine Steuerzeichen";
        }
    }
    size_t len = strlen(s->hostname);
    if (len == 0 || len > 32) {
        return "Hostname: 1–32 Zeichen";
    }
    for (const char *p = s->hostname; *p; p++) {
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-')) {
            return "Hostname: nur a–z, 0–9 und Bindestrich";
        }
    }
    if (s->hostname[0] == '-' || s->hostname[len - 1] == '-') {
        return "Hostname darf nicht mit einem Bindestrich beginnen oder enden";
    }
    len = strlen(s->ap_password);
    if (len != 0 && (len < 8 || len > 63)) {
        return "Access-Point-Passwort: leer (offen) oder 8–63 Zeichen";
    }
    if (s->device_type != DEVICE_PRINTER && s->device_type != DEVICE_AXIDRAW) {
        return "Unbekannter Gerätetyp";
    }
    if (s->baud < 1200 || s->baud > 2000000) {
        return "Baudrate: 1200–2000000";
    }
    if (s->pause_lift < 0 || s->pause_lift > 50) {
        return "Anheben beim Pausieren: 0–50 mm";
    }
    if (s->cancel_lift < 0 || s->cancel_lift > 100) {
        return "Anheben beim Abbrechen: 0–100 mm";
    }
    if (s->park_x < -50 || s->park_x > 1000 || s->park_y < -50 || s->park_y > 1000) {
        return "Parkposition: -50–1000 mm";
    }
    if (s->plot_draw < 1 || s->plot_draw > 250 || s->plot_travel < 1 || s->plot_travel > 250) {
        return "Plotter-Geschwindigkeit: 1–250 mm/s";
    }
    if (s->plot_accel < 100 || s->plot_accel > 10000) {
        return "Plotter-Beschleunigung: 100–10000 mm/s²";
    }
    if (s->pen_servo != 0 && s->pen_servo != 1) {
        return "Unbekannter Stift-Servo";
    }
    if (s->pen_up < 0 || s->pen_up > 100 || s->pen_down < 0 || s->pen_down > 100) {
        return "Stifthöhen: 0–100 %";
    }
    if (s->pen_up <= s->pen_down) {
        return "Stift oben muss höher sein als Stift unten";
    }
    if (s->pen_delay < 0 || s->pen_delay > 1000) {
        return "Wartezeit des Stifts: 0–1000 ms";
    }
    if (s->sd_enable != 0 && s->sd_enable != 1) {
        return "microSD: 0 (aus) oder 1 (an)";
    }
    const char *err = s->sd_enable ? sd_pins_error(s) : NULL;
    return err ? err : led_error(s);
}

esp_err_t settings_update(const settings_t *in, const char **error)
{
    *error = validate(in);
    if (*error) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    for (size_t i = 0; err == ESP_OK && i < sizeof(STR_FIELDS) / sizeof(STR_FIELDS[0]); i++) {
        err = nvs_set_str(nvs, STR_FIELDS[i].key, STR_AT(in, &STR_FIELDS[i]));
    }
    for (size_t i = 0; err == ESP_OK && i < SETTINGS_INT_FIELD_COUNT; i++) {
        err = nvs_set_i32(nvs, SETTINGS_INT_FIELDS[i].key, SETTINGS_INT(in, &SETTINGS_INT_FIELDS[i]));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        *error = "Speichern fehlgeschlagen";
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur = *in;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Einstellungen gespeichert");
    return ESP_OK;
}

bool settings_reboot_required(void)
{
    settings_t cur;
    settings_get(&cur);
    return strcmp(cur.hostname, s_boot.hostname) != 0 || strcmp(cur.ap_password, s_boot.ap_password) != 0 ||
           cur.sd_enable != s_boot.sd_enable || cur.sd_mosi != s_boot.sd_mosi || cur.sd_miso != s_boot.sd_miso ||
           cur.sd_sclk != s_boot.sd_sclk || cur.sd_cs != s_boot.sd_cs;
}

esp_err_t settings_new_api_key(void)
{
    uint8_t raw[16];
    char key[33];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++) {
        snprintf(key + 2 * i, 3, "%02x", raw[i]);
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "api_key", key);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "API-Schlüssel nicht gespeichert: %s", esp_err_to_name(err));
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cur.api_key, key, sizeof(s_cur.api_key));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Neuer API-Schlüssel erzeugt");
    return ESP_OK;
}

esp_err_t settings_reset(bool all)
{
    settings_t cur;
    settings_get(&cur);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(nvs);
    // Ohne gespeicherten Schlüssel erzeugt settings_init() beim nächsten Start einen neuen. Das Passwort des
    // Access Points bleibt wie das der Weboberfläche, sonst wäre er nach dem Neustart offen.
    if (err == ESP_OK && !all) {
        err = nvs_set_str(nvs, "api_key", cur.api_key);
    }
    if (err == ESP_OK && !all) {
        err = nvs_set_str(nvs, "ap_password", cur.ap_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

const char *settings_default_hostname(void)
{
    return s_default_host;
}
