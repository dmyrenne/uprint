#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE   "wifi"
#define RECONNECT_US    (5 * 1000000LL)
#define FALLBACK_US     (30 * 1000000LL)    // so lange ohne Verbindung, dann Access Point öffnen
#define AP_LINGER_US    (120 * 1000000LL)   // Access Point nach erfolgreicher Verbindung noch offen lassen
#define APPLY_DELAY_US  (500 * 1000LL)

// Alle Änderungen am WLAN laufen über den Event-Loop und damit nacheinander
ESP_EVENT_DEFINE_BASE(UPRINT_WIFI_EVENT);
enum { EV_RECONNECT, EV_FALLBACK, EV_AP_OFF, EV_APPLY, EV_FORGET };

static SemaphoreHandle_t s_lock;
static esp_netif_t *s_sta;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_fallback_timer;
static esp_timer_handle_t s_ap_off_timer;
static esp_timer_handle_t s_apply_timer;

// geschützt durch s_lock
static char s_ssid[33];
static char s_pass[65];
static bool s_configured;
static bool s_connected;
static bool s_ap_active;
static esp_ip4_addr_t s_ip;
static int s_last_reason;

static volatile bool s_scanning;

// beim Start aus den Einstellungen übernommen, Änderungen gelten nach Neustart
static char s_hostname[33];
static char s_ap_password[64];

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void post_timer_event(void *arg)
{
    esp_event_post(UPRINT_WIFI_EVENT, (int32_t)(intptr_t)arg, NULL, 0, 0);
}

static esp_timer_handle_t make_timer(int event, const char *name)
{
    esp_timer_create_args_t args = {
        .callback = post_timer_event,
        .arg = (void *)(intptr_t)event,
        .name = name,
    };
    esp_timer_handle_t timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    return timer;
}

static void restart_timer(esp_timer_handle_t timer, int64_t us)
{
    esp_timer_stop(timer);
    esp_timer_start_once(timer, us);
}

static void load_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t ssid_len = sizeof(s_ssid);
    size_t pass_len = sizeof(s_pass);
    if (nvs_get_str(nvs, "ssid", s_ssid, &ssid_len) == ESP_OK && s_ssid[0]) {
        if (nvs_get_str(nvs, "pass", s_pass, &pass_len) != ESP_OK) {
            s_pass[0] = '\0';
        }
        s_configured = true;
    }
    nvs_close(nvs);
}

static esp_err_t store_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void set_ap(bool on)
{
    LOCK();
    bool active = s_ap_active;
    s_ap_active = on;
    UNLOCK();
    if (on == active) {
        return;
    }

    if (on) {
        wifi_config_t cfg = {0};
        strlcpy((char *)cfg.ap.ssid, s_hostname, sizeof(cfg.ap.ssid));
        cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
        strlcpy((char *)cfg.ap.password, s_ap_password, sizeof(cfg.ap.password));
        cfg.ap.authmode = strlen(s_ap_password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        cfg.ap.channel = 1;
        cfg.ap.max_connection = 4;
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &cfg);
        ESP_LOGI(TAG, "Access Point \"%s\" offen: http://192.168.4.1", s_hostname);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "Access Point geschlossen");
    }
}

static void apply_sta_config(void)
{
    wifi_config_t cfg = {0};
    LOCK();
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    bool configured = s_configured;
    bool ap_active = s_ap_active;
    s_connected = false;
    s_last_reason = 0;
    UNLOCK();

    esp_timer_stop(s_reconnect_timer);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (!configured) {
        return;
    }
    ESP_LOGI(TAG, "Verbinde mit \"%s\"", (char *)cfg.sta.ssid);
    esp_wifi_connect();
    if (!ap_active) {
        restart_timer(s_fallback_timer, FALLBACK_US);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        LOCK();
        bool configured = s_configured;
        UNLOCK();
        if (configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        LOCK();
        bool was_connected = s_connected;
        bool configured = s_configured;
        bool ap_active = s_ap_active;
        s_connected = false;
        if (ev->reason != WIFI_REASON_ASSOC_LEAVE) {
            s_last_reason = ev->reason;
        }
        UNLOCK();
        // ASSOC_LEAVE = selbst getrennt (neue Zugangsdaten), kein erneuter Versuch
        if (!configured || ev->reason == WIFI_REASON_ASSOC_LEAVE) {
            return;
        }
        ESP_LOGW(TAG, "%s (Grund %d), neuer Versuch in 5 s",
                 was_connected ? "Verbindung verloren" : "Verbindung fehlgeschlagen", ev->reason);
        restart_timer(s_reconnect_timer, RECONNECT_US);
        if (!ap_active && !esp_timer_is_active(s_fallback_timer)) {
            esp_timer_start_once(s_fallback_timer, FALLBACK_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        LOCK();
        s_ip = ev->ip_info.ip;
        s_connected = true;
        s_last_reason = 0;
        bool ap_active = s_ap_active;
        UNLOCK();
        esp_timer_stop(s_fallback_timer);
        ESP_LOGI(TAG, "Verbunden: http://" IPSTR "  (http://%s.local)", IP2STR(&ev->ip_info.ip), s_hostname);
        if (ap_active) {
            restart_timer(s_ap_off_timer, AP_LINGER_US);
        }
    } else if (base == UPRINT_WIFI_EVENT) {
        LOCK();
        bool configured = s_configured;
        bool connected = s_connected;
        UNLOCK();
        switch (id) {
        case EV_RECONNECT:
            if (s_scanning) {
                restart_timer(s_reconnect_timer, 2 * 1000000LL);
            } else if (configured && !connected) {
                esp_wifi_connect();
            }
            break;
        case EV_FALLBACK:
            if (!connected) {
                ESP_LOGW(TAG, "Keine WLAN-Verbindung, öffne Access Point zum Einrichten");
                set_ap(true);
            }
            break;
        case EV_AP_OFF:
            if (configured && connected) {
                set_ap(false);
            }
            break;
        case EV_APPLY:
            apply_sta_config();
            break;
        case EV_FORGET:
            esp_timer_stop(s_reconnect_timer);
            esp_timer_stop(s_fallback_timer);
            esp_timer_stop(s_ap_off_timer);
            esp_wifi_disconnect();
            set_ap(true);
            break;
        }
    }
}

esp_err_t wifi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    settings_t settings;
    settings_get(&settings);
    strlcpy(s_hostname, settings.hostname, sizeof(s_hostname));
    strlcpy(s_ap_password, settings.ap_password, sizeof(s_ap_password));
    load_credentials();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, s_hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    // Zugangsdaten verwaltet uprint selbst im NVS
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    s_reconnect_timer = make_timer(EV_RECONNECT, "wifi_reconnect");
    s_fallback_timer = make_timer(EV_FALLBACK, "wifi_fallback");
    s_ap_off_timer = make_timer(EV_AP_OFF, "wifi_ap_off");
    s_apply_timer = make_timer(EV_APPLY, "wifi_apply");

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(UPRINT_WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    if (s_configured) {
        wifi_config_t cfg = {0};
        strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
        ESP_LOGI(TAG, "Verbinde mit \"%s\"", s_ssid);
    } else {
        ESP_LOGI(TAG, "Kein WLAN eingerichtet");
        set_ap(true);
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    // Energiesparen bremst Uploads spürbar
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (s_configured) {
        esp_timer_start_once(s_fallback_timer, FALLBACK_US);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_hostname);
        mdns_instance_name_set("uprint");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS nicht verfügbar");
    }
    return ESP_OK;
}

void wifi_get_status(wifi_status_t *out)
{
    LOCK();
    out->configured = s_configured;
    out->connected = s_connected;
    out->ap_active = s_ap_active;
    out->last_reason = s_last_reason;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    strlcpy(out->hostname, s_hostname, sizeof(out->hostname));
    esp_ip4_addr_t ip = s_ip;
    UNLOCK();

    out->ip[0] = '\0';
    out->rssi = 0;
    if (out->connected) {
        esp_ip4addr_ntoa(&ip, out->ip, sizeof(out->ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

esp_err_t wifi_set_credentials(const char *ssid, const char *password)
{
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || (pass_len > 0 && pass_len < 8) || pass_len > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(store_credentials(ssid, password), TAG, "NVS");
    LOCK();
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password, sizeof(s_pass));
    s_configured = true;
    UNLOCK();
    restart_timer(s_apply_timer, APPLY_DELAY_US);
    return ESP_OK;
}

esp_err_t wifi_forget(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    LOCK();
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_configured = false;
    s_connected = false;
    s_last_reason = 0;
    UNLOCK();
    ESP_LOGI(TAG, "WLAN-Zugangsdaten gelöscht");
    return esp_event_post(UPRINT_WIFI_EVENT, EV_FORGET, NULL, 0, pdMS_TO_TICKS(100));
}

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_network_t *)b)->rssi - ((const wifi_network_t *)a)->rssi;
}

int wifi_scan(wifi_network_t *out, int max)
{
    s_scanning = true;
    esp_err_t err = ESP_FAIL;
    // Während eines Verbindungsversuchs lehnt der Treiber einen Scan ab
    for (int attempt = 0; attempt < 5; attempt++) {
        err = esp_wifi_scan_start(NULL, true);
        if (err != ESP_ERR_WIFI_STATE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "Scan fehlgeschlagen: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        s_scanning = false;
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    s_scanning = false;

    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) {
            continue;   // verstecktes Netz
        }
        int j = 0;
        while (j < count && strcmp(out[j].ssid, ssid) != 0) {
            j++;
        }
        if (j < count) {
            if (recs[i].rssi > out[j].rssi) {
                out[j].rssi = recs[i].rssi;
            }
            continue;
        }
        if (count == max) {
            continue;
        }
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(recs);
    qsort(out, count, sizeof(*out), by_rssi);
    return count;
}
