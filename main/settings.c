#include <ctype.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
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
    .ap_password = "uprint123",
    .baud = 115200,
    .pause_lift = 5,
    .cancel_lift = 10,
    .park_x = 0,
    .park_y = 200,       // MK3S: Bett nach vorn
    .sd_mosi = 11,
    .sd_miso = 13,
    .sd_sclk = 12,
    .sd_cs = 10,
};

static SemaphoreHandle_t s_lock;
static settings_t s_cur;
static settings_t s_boot;   // Stand beim Start, für "Neustart nötig"

typedef struct {
    const char *key;
    size_t offset;
} int_field_t;

#define FIELD(name) {#name, offsetof(settings_t, name)}
static const int_field_t INT_FIELDS[] = {
    FIELD(baud), FIELD(pause_lift), FIELD(cancel_lift), FIELD(park_x), FIELD(park_y),
    FIELD(sd_mosi), FIELD(sd_miso), FIELD(sd_sclk), FIELD(sd_cs),
};

#define INT_AT(s, f) (*(int *)((char *)(s) + (f)->offset))

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cur = DEFAULTS;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_cur.hostname);
        if (nvs_get_str(nvs, "hostname", s_cur.hostname, &len) != ESP_OK) {
            strlcpy(s_cur.hostname, DEFAULTS.hostname, sizeof(s_cur.hostname));
        }
        len = sizeof(s_cur.device_name);
        if (nvs_get_str(nvs, "device_name", s_cur.device_name, &len) != ESP_OK) {
            s_cur.device_name[0] = '\0';
        }
        len = sizeof(s_cur.api_key);
        if (nvs_get_str(nvs, "api_key", s_cur.api_key, &len) != ESP_OK) {
            s_cur.api_key[0] = '\0';
        }
        len = sizeof(s_cur.ap_password);
        if (nvs_get_str(nvs, "ap_password", s_cur.ap_password, &len) != ESP_OK) {
            strlcpy(s_cur.ap_password, DEFAULTS.ap_password, sizeof(s_cur.ap_password));
        }
        for (size_t i = 0; i < sizeof(INT_FIELDS) / sizeof(INT_FIELDS[0]); i++) {
            int32_t v;
            if (nvs_get_i32(nvs, INT_FIELDS[i].key, &v) == ESP_OK) {
                INT_AT(&s_cur, &INT_FIELDS[i]) = v;
            }
        }
        nvs_close(nvs);
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

// ESP32-S3 mit Octal-PSRAM (N16R8): 19/20 USB, 22–25 gibt es nicht, 26–37 Flash/PSRAM, 43/44 Konsole
static bool pin_usable(int pin)
{
    return pin >= 0 && pin <= 48 && pin != 19 && pin != 20 && !(pin >= 22 && pin <= 37) && pin != 43 && pin != 44;
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
    const int pins[] = {s->sd_mosi, s->sd_miso, s->sd_sclk, s->sd_cs};
    for (int i = 0; i < 4; i++) {
        if (!pin_usable(pins[i])) {
            return "SD-Pins: GPIO 0–18, 21 oder 38–48 (außer 43/44)";
        }
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                return "SD-Pins müssen verschieden sein";
            }
        }
    }
    return NULL;
}

esp_err_t settings_update(const settings_t *in, const char **error)
{
    *error = validate(in);
    if (*error) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "hostname", in->hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "ap_password", in->ap_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "api_key", in->api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "device_name", in->device_name);
    }
    for (size_t i = 0; err == ESP_OK && i < sizeof(INT_FIELDS) / sizeof(INT_FIELDS[0]); i++) {
        err = nvs_set_i32(nvs, INT_FIELDS[i].key, INT_AT(in, &INT_FIELDS[i]));
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
           cur.sd_mosi != s_boot.sd_mosi || cur.sd_miso != s_boot.sd_miso ||
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
