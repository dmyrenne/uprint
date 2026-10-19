#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "auth.h"
#include "axidraw.h"
#include "captive.h"
#include "http_util.h"
#include "printer.h"
#include "reset.h"
#include "slicer.h"
#include "settings.h"
#include "variant.h"
#include "status_led.h"
#include "storage.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "web";


#define EMBED(sym) \
    extern const char sym##_start[] asm("_binary_" #sym "_start"); \
    extern const char sym##_end[] asm("_binary_" #sym "_end");
EMBED(index_html)
EMBED(i18n_js)
EMBED(space_grotesk_woff2)
EMBED(jetbrains_mono_woff2)

typedef struct {
    const char *start;
    const char *end;
    const char *type;
    const char *cache;
} asset_t;

static const asset_t ASSET_INDEX = {index_html_start, index_html_end, "text/html; charset=utf-8", "no-cache"};
static const asset_t ASSET_I18N = {i18n_js_start, i18n_js_end, "text/javascript; charset=utf-8", "no-cache"};
static const asset_t ASSET_FONT_SANS = {space_grotesk_woff2_start, space_grotesk_woff2_end, "font/woff2", "max-age=31536000"};
static const asset_t ASSET_FONT_MONO = {jetbrains_mono_woff2_start, jetbrains_mono_woff2_end, "font/woff2", "max-age=31536000"};

// Liest ?storage=<id>&name=<datei>; false bei fehlenden/ungültigen Werten
static bool get_file(httpd_req_t *req, storage_vol_t *vol, char *name, size_t len)
{
    char query[CONFIG_HTTPD_MAX_URI_LEN];
    char id[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "storage", id, sizeof(id)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, len) != ESP_OK) {
        return false;
    }
    http_url_decode(name);
    return storage_from_id(id, vol) && storage_name_valid(name);
}

static esp_err_t asset_get(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Cache-Control", a->cache);
    return httpd_resp_send(req, a->start, a->end - a->start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    static char file[ESCAPED_NAME_MAX];
    static char msg[sizeof(((printer_status_t *)0)->message) * 6];
    static char body[ESCAPED_NAME_MAX + sizeof(msg) + 320];
    printer_status_t st;
    settings_t cfg;
    led_config_t leds;

    printer_get_status(&st);
    settings_get(&cfg);
    status_led_get_config(&leds);
    // Kennungsfarbe für den Punkt neben dem Gerätenamen, leer ohne Status-LEDs
    char led_id[8] = "";
    if (cfg.led_pin >= 0 && cfg.led_count > 0) {
        const uint8_t *id = leds.id_pattern[0];
        snprintf(led_id, sizeof(led_id), "#%02x%02x%02x", id[0], id[1], id[2]);
    }
    http_json_escape(file, sizeof(file), st.file);
    http_json_escape(msg, sizeof(msg), st.message);
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"file\":\"%s\",\"size\":%" PRIu32 ",\"pos\":%" PRIu32
             ",\"elapsed\":%" PRIu32 ",\"hotend\":%.1f,\"hotend_target\":%.1f"
             ",\"bed\":%.1f,\"bed_target\":%.1f,\"message\":\"%s\",\"storage\":\"%s\""
             ",\"finished\":%s,\"duration\":%" PRIu32 ",\"files_rev\":%" PRIu32 ",\"led_id\":\"%s\",\"led_ma\":%d,\"unresponsive\":%s,\"progress\":%.1f,\"remaining\":%" PRId32,
             printer_state_name(st.state), file, st.file_size, st.file_pos, st.elapsed_s,
             st.hotend, st.hotend_target, st.bed, st.bed_target, msg, storage_id(st.vol),
             st.finished ? "true" : "false", st.duration_s, storage_revision(), led_id, status_led_current_ma(),
             st.unresponsive ? "true" : "false", st.progress, st.remaining_s);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, body);

    // Meldung und Rückfrage des Druckers (#33), als eigene Stücke, damit der Puffer oben klein bleibt
    static char esc[sizeof(st.prompt_text) * 6];
    http_json_escape(esc, sizeof(esc), st.notice);
    snprintf(body, sizeof(body), ",\"notice\":\"%s\",\"prompt\":", esc);
    httpd_resp_sendstr_chunk(req, body);
    if (!st.prompt) {
        httpd_resp_sendstr_chunk(req, "null}");
        return httpd_resp_sendstr_chunk(req, NULL);
    }
    http_json_escape(esc, sizeof(esc), st.prompt_text);
    snprintf(body, sizeof(body), "{\"text\":\"%s\",\"choices\":[", esc);
    httpd_resp_sendstr_chunk(req, body);
    for (int i = 0; i < st.prompt_choices; i++) {
        http_json_escape(esc, sizeof(esc), st.prompt_choice[i]);
        snprintf(body, sizeof(body), "%s\"%s\"", i ? "," : "", esc);
        httpd_resp_sendstr_chunk(req, body);
    }
    httpd_resp_sendstr_chunk(req, "]}}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t files_get(httpd_req_t *req)
{
    static char path[STORAGE_PATH_MAX + 256];
    static char esc[ESCAPED_NAME_MAX];
    static char item[ESCAPED_NAME_MAX + 96];
    bool first = true;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"volumes\":[");
    bool first_vol = true;
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (!storage_present(v)) {
            continue;
        }
        uint64_t total, avail;
        bool ready = storage_usage(v, &total, &avail) == ESP_OK;
        snprintf(item, sizeof(item),
                 "%s{\"id\":\"%s\",\"label\":\"%s\",\"ready\":%s,\"total\":%llu,\"free\":%llu}",
                 first_vol ? "" : ",", storage_id(v), storage_label(v), ready ? "true" : "false", total, avail);
        httpd_resp_sendstr_chunk(req, item);
        first_vol = false;
    }
    httpd_resp_sendstr_chunk(req, "],\"files\":[");

    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (!storage_acquire(v)) {
            continue;
        }
        DIR *dir = opendir(storage_mount(v));
        if (!dir) {
            storage_release(v);
            continue;
        }
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            if (e->d_type == DT_DIR || !storage_name_valid(e->d_name)) {
                continue;
            }
            struct stat st;
            storage_path(path, sizeof(path), v, e->d_name);
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            http_json_escape(esc, sizeof(esc), e->d_name);
            snprintf(item, sizeof(item), "%s{\"storage\":\"%s\",\"name\":\"%s\",\"size\":%ld}",
                     first ? "" : ",", storage_id(v), esc, (long)st.st_size);
            httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
        closedir(dir);
        storage_release(v);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t files_delete(httpd_req_t *req)
{
    storage_vol_t vol;
    char name[STORAGE_NAME_MAX];
    char path[STORAGE_PATH_MAX];
    if (!get_file(req, &vol, name, sizeof(name))) {
        return http_send_error(req, "400 Bad Request", "Ungültiger Speicher oder Dateiname");
    }
    if (printer_is_using(vol, name)) {
        return http_send_error(req, "409 Conflict", "Datei wird gerade gedruckt");
    }
    if (!storage_acquire(vol)) {
        return http_send_error(req, "503 Service Unavailable", "Speicher nicht verfügbar");
    }
    storage_path(path, sizeof(path), vol, name);
    int removed = unlink(path);
    storage_release(vol);
    if (removed != 0) {
        return http_send_error(req, "404 Not Found", "Datei nicht gefunden");
    }
    storage_changed();
    ESP_LOGI(TAG, "Gelöscht: %s (%s)", name, storage_label(vol));
    return http_send_ok(req);
}

static esp_err_t upload_post(httpd_req_t *req)
{
    storage_vol_t vol;
    char name[STORAGE_NAME_MAX];
    if (!get_file(req, &vol, name, sizeof(name))) {
        return http_send_error(req, "400 Bad Request", "Ungültiger Speicher oder Dateiname");
    }
    const char *status;
    const char *failure = http_upload_body(req, vol, name, &status);
    return failure ? http_send_error(req, status, failure) : http_send_ok(req);
}

// Dateiname für Content-Disposition (RFC 5987): alles außer A–Z, a–z, 0–9 und -._ prozentkodiert
static void percent_encode(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < len; p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_') {
            out[o++] = (char)*p;
        } else {
            o += snprintf(out + o, len - o, "%%%02X", *p);
        }
    }
    out[o] = '\0';
}

static esp_err_t download_get(httpd_req_t *req)
{
    storage_vol_t vol;
    char name[STORAGE_NAME_MAX];
    char path[STORAGE_PATH_MAX];
    if (!get_file(req, &vol, name, sizeof(name))) {
        return http_send_error(req, "400 Bad Request", "Ungültiger Speicher oder Dateiname");
    }
    if (!storage_acquire(vol)) {
        return http_send_error(req, "503 Service Unavailable", "Speicher nicht verfügbar");
    }
    storage_path(path, sizeof(path), vol, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        storage_release(vol);
        return http_send_error(req, "404 Not Found", "Datei nicht gefunden");
    }
    char *buf = malloc(HTTP_CHUNK);
    if (!buf) {
        fclose(f);
        storage_release(vol);
        return http_send_error(req, "500 Internal Server Error", "Kein Speicher");
    }

    char encoded[STORAGE_NAME_MAX * 3];
    char disposition[STORAGE_NAME_MAX * 3 + 64];
    percent_encode(encoded, sizeof(encoded), name);
    snprintf(disposition, sizeof(disposition), "attachment; filename*=UTF-8''%s", encoded);
    httpd_resp_set_type(req, "text/x-gcode");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    esp_err_t err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, HTTP_CHUNK, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
        if (err != ESP_OK) {
            break;   // Client hat abgebrochen
        }
    }
    free(buf);
    fclose(f);
    storage_release(vol);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

// Während eines Drucks sind Einstellungen und WLAN gesperrt (#16); true = Antwort schon gesendet
static bool locked_while_printing(httpd_req_t *req)
{
    if (!printer_busy()) {
        return false;
    }
    http_send_error(req, "409 Conflict", "Während eines Drucks gesperrt");
    return true;
}

static esp_err_t send_result(httpd_req_t *req, esp_err_t err, const char *conflict_msg)
{
    if (err == ESP_ERR_INVALID_STATE) {
        return http_send_error(req, "409 Conflict", conflict_msg);
    }
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return http_send_ok(req);
}

static esp_err_t print_post(httpd_req_t *req)
{
    storage_vol_t vol;
    char name[STORAGE_NAME_MAX];
    char path[STORAGE_PATH_MAX];
    struct stat st;
    if (!get_file(req, &vol, name, sizeof(name))) {
        return http_send_error(req, "400 Bad Request", "Ungültiger Speicher oder Dateiname");
    }
    if (!storage_acquire(vol)) {
        return http_send_error(req, "503 Service Unavailable", "Speicher nicht verfügbar");
    }
    storage_path(path, sizeof(path), vol, name);
    int found = stat(path, &st);
    storage_release(vol);
    if (found != 0) {
        return http_send_error(req, "404 Not Found", "Datei nicht gefunden");
    }
    return send_result(req, printer_start(vol, name), "Drucker ist nicht bereit");
}

static esp_err_t pause_post(httpd_req_t *req)
{
    return send_result(req, printer_pause(), "Es läuft kein Druck");
}

static esp_err_t resume_post(httpd_req_t *req)
{
    return send_result(req, printer_resume(), "Druck ist nicht pausiert");
}

// ?choice=<n>: Antwort auf eine Rückfrage des Druckers (#33)
static esp_err_t prompt_post(httpd_req_t *req)
{
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "choice", val, sizeof(val)) != ESP_OK) {
        return http_send_error(req, "400 Bad Request", "choice fehlt");
    }
    return send_result(req, printer_prompt_answer(atoi(val)), "Keine offene Rückfrage des Druckers");
}

static esp_err_t retry_post(httpd_req_t *req)
{
    return send_result(req, printer_retry(), "Drucker wartet gerade nicht auf eine Verbindung");
}

static esp_err_t cancel_post(httpd_req_t *req)
{
    return send_result(req, printer_cancel(), "Es läuft kein Druck");
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    wifi_status_t st;
    char ssid[33 * 6];
    char body[640];
    wifi_get_status(&st);
    http_json_escape(ssid, sizeof(ssid), st.ssid);
    bool via_ap = captive_via_ap(httpd_req_to_sockfd(req));
    // portal: Seite wurde über den Access Point zur Einrichtung geöffnet, die UI zeigt dann nur das WLAN
    snprintf(body, sizeof(body),
             "{\"configured\":%s,\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d"
             ",\"reason\":%d,\"ap\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"192.168.4.1\",\"hostname\":\"%s\""
             ",\"ap_only\":%s,\"ap_off_in\":%d,\"via_ap\":%s,\"portal\":%s}",
             st.configured ? "true" : "false", st.connected ? "true" : "false", ssid, st.ip, st.rssi,
             st.last_reason, st.ap_active ? "true" : "false", st.hostname, st.hostname,
             st.ap_only ? "true" : "false", st.ap_off_in, via_ap ? "true" : "false",
             via_ap && st.ap_active && !st.ap_only ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    enum { MAX_NETWORKS = 20 };
    wifi_network_t *nets = calloc(MAX_NETWORKS, sizeof(*nets));
    if (!nets) {
        return http_send_error(req, "500 Internal Server Error", "Kein Speicher");
    }
    int n = wifi_scan(nets, MAX_NETWORKS);
    if (n < 0) {
        free(nets);
        return http_send_error(req, "503 Service Unavailable", "Suche gerade nicht möglich, bitte erneut versuchen");
    }
    char esc[33 * 6];
    char item[33 * 6 + 64];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < n; i++) {
        http_json_escape(esc, sizeof(esc), nets[i].ssid);
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                 i ? "," : "", esc, nets[i].rssi, nets[i].secure ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
    }
    free(nets);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Body: ssid=<…>&password=<…> (application/x-www-form-urlencoded)
static esp_err_t wifi_post(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    char body[512];
    char ssid[33 * 3 + 1];
    char pass[64 * 3 + 1];
    if (!http_recv_body(req, body, sizeof(body))) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return http_send_error(req, "400 Bad Request", "SSID fehlt");
    }
    if (httpd_query_key_value(body, "password", pass, sizeof(pass)) != ESP_OK) {
        pass[0] = '\0';
    }
    http_url_decode(ssid);
    http_url_decode(pass);
    esp_err_t err = wifi_set_credentials(ssid, pass);
    if (err == ESP_ERR_INVALID_ARG) {
        return http_send_error(req, "400 Bad Request", "SSID 1–32 Zeichen, Passwort leer oder 8–63 Zeichen");
    }
    return send_result(req, err, "");
}

static esp_err_t wifi_ap_only_post(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    esp_err_t err = wifi_set_ap_only();
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", "Konnte nicht gespeichert werden");
    }
    return http_send_ok(req);
}

static esp_err_t wifi_delete(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    return send_result(req, wifi_forget(), "");
}

// Version eines Updates, das beim Start fehlgeschlagen ist (der Bootloader ist zurückgekehrt), sonst ""
static void failed_update(char *out, size_t len)
{
    out[0] = '\0';
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (bad && esp_ota_get_partition_description(bad, &desc) == ESP_OK) {
        http_json_escape(out, len, desc.version);
    }
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    settings_t c;
    char host[33 * 6];
    char pass[64 * 6];
    char body[1792];
    char version[32 * 6];
    char name[33 * 6];
    settings_get(&c);
    http_json_escape(name, sizeof(name), c.device_name);
    http_json_escape(version, sizeof(version), esp_app_get_description()->version);
    http_json_escape(host, sizeof(host), c.hostname);
    http_json_escape(pass, sizeof(pass), c.ap_password);
    char failed[32 * 6];
    failed_update(failed, sizeof(failed));
    size_t n = snprintf(body, sizeof(body),
                        "{\"device_name\":\"%s\",\"hostname\":\"%s\",\"ap_password\":\"%s\",\"api_key\":\"%s\""
                        ",\"reboot_required\":%s,\"version\":\"%s\",\"variant\":\"%s\",\"failed_update\":\"%s\""
                        ",\"default_hostname\":\"%s\"",
                        name, host, pass, c.api_key, settings_reboot_required() ? "true" : "false", version,
                        uprint_variant(), failed, settings_default_hostname());
    for (size_t i = 0; i < SETTINGS_INT_FIELD_COUNT && n < sizeof(body); i++) {
        const settings_int_field_t *f = &SETTINGS_INT_FIELDS[i];
        n += snprintf(body + n, sizeof(body) - n, ",\"%s\":%d", f->key, SETTINGS_INT(&c, f));
    }
    if (n < sizeof(body) - 1) {
        body[n++] = '}';
        body[n] = '\0';
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// Liest einen Wert aus einem urlencodierten Body; false, wenn der Schlüssel fehlt
static bool form_value(const char *body, const char *key, char *out, size_t len)
{
    if (httpd_query_key_value(body, key, out, len) != ESP_OK) {
        return false;
    }
    http_url_decode(out);
    return true;
}

static bool form_int(const char *body, const char *key, int *out, bool *bad)
{
    char buf[16];
    if (!form_value(body, key, buf, sizeof(buf))) {
        return false;
    }
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0') {
        *bad = true;
        return false;
    }
    *out = (int)v;
    return true;
}

// Body: urlencodierte Felder wie bei GET; fehlende Felder bleiben unverändert
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    char body[1280];
    if (!http_recv_body(req, body, sizeof(body))) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }

    settings_t c;
    settings_get(&c);
    char buf[64 * 3 + 1];
    if (form_value(body, "device_name", buf, sizeof(buf))) {
        if (strlen(buf) >= sizeof(c.device_name)) {
            return http_send_error(req, "400 Bad Request", "Gerätename: höchstens 32 Bytes");
        }
        strlcpy(c.device_name, buf, sizeof(c.device_name));
    }
    if (form_value(body, "hostname", buf, sizeof(buf))) {
        strlcpy(c.hostname, buf, sizeof(c.hostname));
    }
    if (form_value(body, "ap_password", buf, sizeof(buf))) {
        strlcpy(c.ap_password, buf, sizeof(c.ap_password));
    }
    bool bad = false;
    int old_device = c.device_type;
    for (size_t i = 0; i < SETTINGS_INT_FIELD_COUNT; i++) {
        form_int(body, SETTINGS_INT_FIELDS[i].key, &SETTINGS_INT(&c, &SETTINGS_INT_FIELDS[i]), &bad);
    }
    if (bad) {
        return http_send_error(req, "400 Bad Request", "Bitte nur ganze Zahlen eingeben");
    }

    bool device_changed = c.device_type != old_device;
    const char *error = NULL;
    esp_err_t err = settings_update(&c, &error);
    if (err != ESP_OK) {
        return http_send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", error);
    }
    if (device_changed) {
        printer_device_changed();
    }
    wifi_apply_device_name();
    status_led_apply();
    return settings_get_handler(req);
}

static esp_err_t api_key_post(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    esp_err_t err = settings_new_api_key();
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    return settings_get_handler(req);
}

static void restart_cb(void *arg)
{
    esp_restart();
}

// Kurz verzögert neu starten, damit die HTTP-Antwort noch ankommt
static void schedule_restart(void)
{
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = {.callback = restart_cb, .name = "restart"};
        esp_timer_create(&args, &timer);
    }
    esp_timer_start_once(timer, 800 * 1000);
}

// Fehler nach Beginn der Übertragung: Fortschrittsanzeige der Status-LEDs beenden
static esp_err_t ota_fail(httpd_req_t *req, const char *status, const char *msg)
{
    status_led_ota(-1);
    return http_send_error(req, status, msg);
}

// Body: Firmware-Image (build/uprint.bin) als Rohdaten
static esp_err_t ota_post(httpd_req_t *req)
{
    if (printer_busy()) {
        return http_send_error(req, "409 Conflict", "Während eines Drucks nicht möglich");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return http_send_error(req, "500 Internal Server Error", "Keine OTA-Partition vorhanden");
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        return http_send_error(req, "400 Bad Request", "Datei leer oder größer als die App-Partition");
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    char *buf = malloc(HTTP_CHUNK);
    if (!buf) {
        esp_ota_abort(ota);
        return http_send_error(req, "500 Internal Server Error", "Kein Speicher");
    }

    ESP_LOGI(TAG, "Firmware-Update: %u Bytes nach %s", (unsigned)req->content_len, part->label);
    const char *failure = NULL;
    size_t remaining = req->content_len;
    status_led_ota(0);
    while (remaining > 0) {
        int r = http_recv(req, buf, remaining < HTTP_CHUNK ? remaining : HTTP_CHUNK);
        if (r <= 0) {
            failure = "Upload abgebrochen";
            break;
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            failure = err == ESP_ERR_OTA_VALIDATE_FAILED ? "Keine gültige Firmware-Datei" : "Schreiben fehlgeschlagen";
            break;
        }
        remaining -= r;
        status_led_ota((int)((req->content_len - remaining) * 100 / req->content_len));
    }
    free(buf);
    if (failure) {
        esp_ota_abort(ota);
        return ota_fail(req, "400 Bad Request", failure);
    }
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        return ota_fail(req, "400 Bad Request",
                        err == ESP_ERR_OTA_VALIDATE_FAILED ? "Firmware-Datei ist beschädigt oder für einen anderen Chip" : esp_err_to_name(err));
    }

    // Nur Firmware von uprint annehmen
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK || strcmp(desc.project_name, "uprint") != 0) {
        return ota_fail(req, "400 Bad Request", "Das ist keine uprint-Firmware");
    }
    // Eine Firmware für ein anderes Board würde nicht starten (Flash-Größe, PSRAM, Partitionen)
    uprint_desc_t id;
    if (!uprint_read_desc(part, &id) || strcmp(id.variant, uprint_variant()) != 0) {
        static char msg[128];
        snprintf(msg, sizeof(msg), "Falsche Variante: dieses Gerät braucht die Firmware \"%s\"", uprint_variant());
        return ota_fail(req, "400 Bad Request", msg);
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return ota_fail(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    char version[sizeof(desc.version) * 6];
    char body[sizeof(version) + 32];
    http_json_escape(version, sizeof(version), desc.version);
    snprintf(body, sizeof(body), "{\"ok\":true,\"version\":\"%s\"}", version);
    ESP_LOGI(TAG, "Firmware %s installiert, starte neu", desc.version);
    schedule_restart();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// ---------- Status-LEDs ----------

static void color_hex(char out[8], const uint8_t rgb[3])
{
    snprintf(out, 8, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
}

// "#rrggbb" oder "rrggbb"
static bool parse_color(const char *s, uint8_t rgb[3])
{
    if (*s == '#') {
        s++;
    }
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        char part[3] = {s[2 * i], s[2 * i + 1], '\0'};
        rgb[i] = (uint8_t)strtol(part, NULL, 16);
    }
    return true;
}

// "rrggbb,rrggbb,…" ab LED 0 (zerlegt s); nicht genannte LEDs bleiben
static bool parse_pattern(char *s, uint8_t (*out)[3])
{
    char *save = NULL;
    int i = 0;
    for (char *tok = strtok_r(s, ",", &save); tok; tok = strtok_r(NULL, ",", &save), i++) {
        if (i >= STATUS_LED_MAX || !parse_color(tok, out[i])) {
            return false;
        }
    }
    return true;
}

static void look_json(char *out, size_t len, const led_look_t *lk)
{
    char color[8];
    const uint8_t rgb[3] = {lk->r, lk->g, lk->b};
    color_hex(color, rgb);
    snprintf(out, len, "\"effect\":%d,\"color\":\"%s\",\"bright\":%d", lk->effect, color, lk->bright);
}

// Alle Zustände mit erlaubten Effekten und Standardwerten (für die Unterseite)
static esp_err_t leds_get(httpd_req_t *req)
{
    led_config_t cur, def;
    char item[320], look[64], look_def[64], effects[40];
    status_led_get_config(&cur);
    status_led_default_config(&def);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"states\":[");
    for (int i = 0; i < LED_STATE_COUNT; i++) {
        size_t n = 0;
        for (int fx = 0; fx < LED_FX_COUNT; fx++) {
            if (status_led_effect_allowed(i, fx)) {
                n += snprintf(effects + n, sizeof(effects) - n, "%s%d", n ? "," : "", fx);
            }
        }
        look_json(look, sizeof(look), &cur.look[i]);
        look_json(look_def, sizeof(look_def), &def.look[i]);
        snprintf(item, sizeof(item), "%s{\"id\":\"%s\",%s,\"effects\":[%s],\"default\":{%s}}", i ? "," : "",
                 status_led_state_name(i), look, effects, look_def);
        httpd_resp_sendstr_chunk(req, item);
    }
    // Muster der Kennung, immer für die größtmögliche Anzahl LEDs
    const led_config_t *src[2] = {&cur, &def};
    const char *open[2] = {"],\"pattern\":[", "],\"pattern_default\":["};
    for (int k = 0; k < 2; k++) {
        httpd_resp_sendstr_chunk(req, open[k]);
        for (int i = 0; i < STATUS_LED_MAX; i += 10) {
            size_t n = 0;
            for (int j = i; j < i + 10 && j < STATUS_LED_MAX; j++) {
                char color[8];
                color_hex(color, src[k]->id_pattern[j]);
                n += snprintf(item + n, sizeof(item) - n, "%s\"%s\"", j ? "," : "", color);
            }
            httpd_resp_sendstr_chunk(req, item);
        }
    }
    snprintf(item, sizeof(item), "],\"id_interval\":%d,\"id_interval_default\":%d}", cur.id_interval, def.id_interval);
    httpd_resp_sendstr_chunk(req, item);
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Body: <zustand>_effect, <zustand>_color, <zustand>_bright, pattern (rrggbb,rrggbb,… ab LED 0), id_interval;
// fehlende Felder bleiben. Statisch, weil der Webserver Anfragen nacheinander abarbeitet.
static esp_err_t leds_post(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    static char body[3072];
    static char pattern[STATUS_LED_MAX * 12 + 1];   // URL-kodiert: "%23rrggbb%2C" je LED
    if (!http_recv_body(req, body, sizeof(body))) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }
    led_config_t c;
    status_led_get_config(&c);
    bool bad = false;
    char key[24], val[16];
    for (int i = 0; i < LED_STATE_COUNT && !bad; i++) {
        led_look_t *lk = &c.look[i];
        int v;
        snprintf(key, sizeof(key), "%s_effect", status_led_state_name(i));
        if (form_int(body, key, &v, &bad)) {
            bad |= v < 0 || v >= LED_FX_COUNT;
            lk->effect = (uint8_t)v;
        }
        snprintf(key, sizeof(key), "%s_bright", status_led_state_name(i));
        if (form_int(body, key, &v, &bad)) {
            bad |= v < 0 || v > 100;
            lk->bright = (uint8_t)v;
        }
        snprintf(key, sizeof(key), "%s_color", status_led_state_name(i));
        uint8_t rgb[3];
        if (form_value(body, key, val, sizeof(val))) {
            if (parse_color(val, rgb)) {
                lk->r = rgb[0];
                lk->g = rgb[1];
                lk->b = rgb[2];
            } else {
                bad = true;
            }
        }
    }
    if (!bad && form_value(body, "pattern", pattern, sizeof(pattern))) {
        bad = !parse_pattern(pattern, c.id_pattern);
    }
    int interval;
    if (!bad && form_int(body, "id_interval", &interval, &bad)) {
        bad |= interval < 0 || interval > 255;
        c.id_interval = (uint8_t)interval;
    }
    if (bad) {
        return http_send_error(req, "400 Bad Request", "Ungültiger Wert (Farbe als #rrggbb, Zahlen 0–100)");
    }
    const char *error = NULL;
    esp_err_t err = status_led_set_config(&c, &error);
    if (err != ESP_OK) {
        return http_send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", error);
    }
    return leds_get(req);
}

static esp_err_t leds_reset_post(httpd_req_t *req)
{
    if (locked_while_printing(req)) {
        return ESP_OK;
    }
    status_led_reset_config();
    return leds_get(req);
}

// ?state=<zustand>, optional effect, color (rrggbb), bright: noch nicht gespeicherte Darstellung zeigen.
// Bei der Kennung optional pattern=rrggbb,… im Body (passt nicht in die URL).
static esp_err_t leds_preview_post(httpd_req_t *req)
{
    static char body[STATUS_LED_MAX * 12 + 16];
    static char pattern[STATUS_LED_MAX * 12 + 1];
    static uint8_t colors[STATUS_LED_MAX][3];
    char query[128], name[16], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "state", name, sizeof(name)) != ESP_OK) {
        return http_send_error(req, "400 Bad Request", "Unbekannter Zustand");
    }
    for (int i = 0; i < LED_STATE_COUNT; i++) {
        if (strcmp(name, status_led_state_name(i)) != 0) {
            continue;
        }
        led_config_t c;
        status_led_get_config(&c);
        led_look_t look = c.look[i];
        uint8_t rgb[3];
        if (httpd_query_key_value(query, "effect", val, sizeof(val)) == ESP_OK) {
            int fx = atoi(val);
            if (!status_led_effect_allowed(i, fx)) {
                return http_send_error(req, "400 Bad Request", "Effekt ist für diesen Zustand nicht möglich");
            }
            look.effect = (uint8_t)fx;
        }
        if (httpd_query_key_value(query, "color", val, sizeof(val)) == ESP_OK && parse_color(val, rgb)) {
            look.r = rgb[0];
            look.g = rgb[1];
            look.b = rgb[2];
        }
        if (httpd_query_key_value(query, "bright", val, sizeof(val)) == ESP_OK) {
            int b = atoi(val);
            look.bright = (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b);
        }
        bool with_pattern = false;
        if (req->content_len > 0) {
            memcpy(colors, c.id_pattern, sizeof(colors));
            if (!http_recv_body(req, body, sizeof(body)) ||
                (form_value(body, "pattern", pattern, sizeof(pattern)) && !parse_pattern(pattern, colors))) {
                return http_send_error(req, "400 Bad Request", "Ungültiges Muster");
            }
            with_pattern = true;
        }
        status_led_preview(i, &look, with_pattern ? (const uint8_t (*)[3])colors : NULL);
        return http_send_ok(req);
    }
    return http_send_error(req, "400 Bad Request", "Unbekannter Zustand");
}

static esp_err_t led_find_post(httpd_req_t *req)
{
    status_led_find();
    return http_send_ok(req);
}

// ---------- AxiDraw-Testmodus ----------

static esp_err_t axidraw_test_get(httpd_req_t *req)
{
    axidraw_test_status_t st;
    char prompt[sizeof(st.prompt) * 6];
    char body[sizeof(prompt) + 96];
    axidraw_test_status(&st);
    http_json_escape(prompt, sizeof(prompt), st.prompt);
    snprintf(body, sizeof(body), "{\"running\":%s,\"done\":%s,\"prompt\":\"%s\",\"log_len\":%u}",
             st.running ? "true" : "false", st.done ? "true" : "false", prompt, (unsigned)st.log_len);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// ?motion=1 fährt zusätzlich ein kleines Quadrat
static esp_err_t axidraw_test_post(httpd_req_t *req)
{
    char query[32], val[4];
    bool motion = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                  httpd_query_key_value(query, "motion", val, sizeof(val)) == ESP_OK && strcmp(val, "1") == 0;
    if (printer_busy()) {
        return http_send_error(req, "409 Conflict", "Während eines Plots ist kein Test möglich");
    }
    return send_result(req, axidraw_test_start(motion), "Kein AxiDraw verbunden oder Test läuft bereits");
}

static esp_err_t axidraw_abort_post(httpd_req_t *req)
{
    axidraw_test_abort();
    return http_send_ok(req);
}

static esp_err_t axidraw_log_get(httpd_req_t *req)
{
    char buf[1024];
    size_t off = 0, n;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    while ((n = axidraw_test_log(buf, sizeof(buf), off)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            return ESP_FAIL;
        }
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ?scope=settings|all (#13): Einstellungen bzw. alles inklusive WLAN zurücksetzen, danach Neustart
static esp_err_t reset_post(httpd_req_t *req)
{
    char query[32], scope[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "scope", scope, sizeof(scope)) != ESP_OK ||
        (strcmp(scope, "settings") != 0 && strcmp(scope, "all") != 0)) {
        return http_send_error(req, "400 Bad Request", "scope=settings oder scope=all");
    }
    return send_result(req, reset_device(strcmp(scope, "all") == 0), "Während eines Drucks nicht möglich");
}

// ?storage=<id>&format=0|1 (#14): alle G-Code-Dateien löschen bzw. den Speicher formatieren
static esp_err_t wipe_post(httpd_req_t *req)
{
    char query[64], id[16], format[4] = "0";
    storage_vol_t vol;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "storage", id, sizeof(id)) != ESP_OK || !storage_from_id(id, &vol) ||
        !storage_present(vol)) {
        return http_send_error(req, "400 Bad Request", "Ungültiger Speicher");
    }
    httpd_query_key_value(query, "format", format, sizeof(format));
    int deleted = 0;
    esp_err_t err = storage_wipe(vol, strcmp(format, "1") == 0, http_is_wipeable, &deleted);
    if (err == ESP_ERR_INVALID_STATE) {
        return http_send_error(req, "409 Conflict", "Speicher wird gerade benutzt (Druck, Upload oder Download) oder ist nicht bereit");
    }
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", "Nicht alle Dateien ließen sich löschen");
    }
    char body[48];
    snprintf(body, sizeof(body), "{\"ok\":true,\"deleted\":%d}", deleted);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (printer_busy()) {
        return http_send_error(req, "409 Conflict", "Während eines Drucks nicht möglich");
    }
    schedule_restart();
    ESP_LOGI(TAG, "Neustart über die Weboberfläche");
    return http_send_ok(req);
}

// ---------- Anmeldung (#31) ----------

#define SESSION_COOKIE "uprint_session"

// Ohne gesetztes Passwort ist alles offen, sonst braucht es ein gültiges Sitzungs-Cookie
static bool session_ok(httpd_req_t *req)
{
    if (!auth_password_set()) {
        return true;
    }
    char token[AUTH_TOKEN_LEN + 1];
    size_t len = sizeof(token);
    return httpd_req_get_cookie_val(req, SESSION_COOKIE, token, &len) == ESP_OK && auth_token_valid(token);
}

// Vor jeden geschützten Handler gesetzt; user_ctx ist der eigentliche Handler
static esp_err_t guarded(httpd_req_t *req)
{
    if (!session_ok(req)) {
        return http_send_error(req, "401 Unauthorized", "Bitte anmelden");
    }
    esp_err_t (*handler)(httpd_req_t *) = req->user_ctx;
    return handler(req);
}

// Ohne Anmeldung erreichbar: die Oberfläche selbst, Status und Dateien lesen und die Anmeldung
static bool route_open(const httpd_uri_t *r)
{
    if (r->handler == asset_get || strcmp(r->uri, "/api/auth") == 0) {
        return true;
    }
    return r->method == HTTP_GET && (strcmp(r->uri, "/api/status") == 0 || strcmp(r->uri, "/api/files") == 0 ||
                                     strcmp(r->uri, "/api/download") == 0);
}

// Neues Sitzungs-Cookie setzen und {"ok":true} senden
static esp_err_t send_session(httpd_req_t *req)
{
    char token[AUTH_TOKEN_LEN + 1];
    char cookie[AUTH_TOKEN_LEN + 96];
    auth_new_token(token);
    snprintf(cookie, sizeof(cookie), SESSION_COOKIE "=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return http_send_ok(req);
}

static esp_err_t auth_get(httpd_req_t *req)
{
    char body[80];
    snprintf(body, sizeof(body), "{\"password\":%s,\"session\":%s,\"setup\":%s}", auth_password_set() ? "true" : "false",
             auth_password_set() && session_ok(req) ? "true" : "false", auth_setup_pending() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// Prüft das Passwort; bei Fehler ist die Antwort schon gesendet
static bool password_ok(httpd_req_t *req, const char *password, esp_err_t *ret)
{
    int wait_s;
    switch (auth_check_password(password, &wait_s)) {
    case AUTH_OK:
        return true;
    case AUTH_LOCKED: {
        char msg[96];
        snprintf(msg, sizeof(msg), "Zu viele Fehlversuche, bitte in %d s erneut versuchen", wait_s);
        *ret = http_send_error(req, "429 Too Many Requests", msg);
        return false;
    }
    default:
        *ret = http_send_error(req, "403 Forbidden", "Falsches Passwort");
        return false;
    }
}

// Anmelden, Body: password=<…>
static esp_err_t auth_post(httpd_req_t *req)
{
    char body[256];
    char pass[AUTH_PASSWORD_MAX * 3 + 1];
    if (!http_recv_body(req, body, sizeof(body)) || httpd_query_key_value(body, "password", pass, sizeof(pass)) != ESP_OK) {
        return http_send_error(req, "400 Bad Request", "Passwort fehlt");
    }
    http_url_decode(pass);
    if (!auth_password_set()) {
        return http_send_ok(req);
    }
    esp_err_t ret;
    if (!password_ok(req, pass, &ret)) {
        return ret;
    }
    ESP_LOGI(TAG, "Angemeldet");
    return send_session(req);
}

// Abmelden: nur dieses Gerät, das Cookie wird gelöscht
static esp_err_t auth_delete(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Set-Cookie", SESSION_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return http_send_ok(req);
}

// "Ohne Passwort fortfahren": nicht mehr nach einem Passwort fragen
static esp_err_t auth_skip_post(httpd_req_t *req)
{
    if (auth_password_set()) {
        return http_send_error(req, "409 Conflict", "Es ist schon ein Passwort gesetzt");
    }
    return send_result(req, auth_skip(), "");
}

// Passwort festlegen oder ändern, Body: password=<neu>&current=<bisher>&ap=1 (auch für den Access Point).
// Meldet alle anderen Sitzungen ab, diese bekommt ein neues Cookie. remove=1&current=<bisher> entfernt es.
static esp_err_t password_post(httpd_req_t *req)
{
    char body[640];
    char pass[AUTH_PASSWORD_MAX * 3 + 1] = "";
    char current[AUTH_PASSWORD_MAX * 3 + 1] = "";
    char ap[4] = "0", remove[4] = "0";
    if (!http_recv_body(req, body, sizeof(body))) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }
    httpd_query_key_value(body, "remove", remove, sizeof(remove));
    bool removing = strcmp(remove, "1") == 0;
    if (!removing && httpd_query_key_value(body, "password", pass, sizeof(pass)) != ESP_OK) {
        return http_send_error(req, "400 Bad Request", "Passwort fehlt");
    }
    http_url_decode(pass);
    httpd_query_key_value(body, "ap", ap, sizeof(ap));
    size_t len = strlen(pass);
    if (!removing && (len < AUTH_PASSWORD_MIN || len > AUTH_PASSWORD_MAX)) {
        return http_send_error(req, "400 Bad Request", "Passwort: 8–63 Zeichen");
    }
    if (auth_password_set()) {
        httpd_query_key_value(body, "current", current, sizeof(current));
        http_url_decode(current);
        esp_err_t ret;
        if (!password_ok(req, current, &ret)) {
            return ret;
        }
    }
    if (removing) {
        // Das Passwort des Access Points bleibt, es lässt sich unter "Netzwerk" ändern
        if (auth_remove() != ESP_OK) {
            return http_send_error(req, "500 Internal Server Error", "Passwort konnte nicht entfernt werden");
        }
        return auth_delete(req);
    }
    if (strcmp(ap, "1") == 0) {
        // Gleiches Passwort für den Access Point, der dann nicht mehr offen ist (wirksam nach Neustart)
        settings_t c;
        settings_get(&c);
        strlcpy(c.ap_password, pass, sizeof(c.ap_password));
        const char *error;
        if (settings_update(&c, &error) != ESP_OK) {
            return http_send_error(req, "500 Internal Server Error", error);
        }
    }
    if (auth_set_password(pass) != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", "Passwort konnte nicht gespeichert werden");
    }
    return send_session(req);
}

// Prüfadressen der Betriebssysteme und die Antwort, die "Internet erreichbar" bedeutet
typedef struct {
    const char *path;
    const char *status;
    const char *body;
} probe_t;

static const probe_t PROBES[] = {
    {"/generate_204", "204 No Content", ""},                         // Android, Chrome OS
    {"/gen_204", "204 No Content", ""},
    {"/hotspot-detect.html", "200 OK", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},   // Apple
    {"/library/test/success.html", "200 OK", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
    {"/connecttest.txt", "200 OK", "Microsoft Connect Test"},        // Windows
    {"/ncsi.txt", "200 OK", "Microsoft NCSI"},
    {"/success.txt", "200 OK", "success\n"},                        // Firefox
    {"/canonical.html", "200 OK", "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>"},
};

// Am Access Point führt jeder unbekannte Pfad zur Startseite. Darüber erkennen Handys und Laptops
// das Captive Portal und öffnen die Seite von selbst. Nach "ohne WLAN fortfahren" bekommen die
// Prüfadressen stattdessen die erwartete Antwort, damit das Portal schließt und das Gerät verbunden
// bleibt. Im WLAN bleibt es beim normalen 404.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (!captive_via_ap(httpd_req_to_sockfd(req))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }
    if (!wifi_portal_active()) {
        size_t len = strcspn(req->uri, "?");
        for (size_t i = 0; i < sizeof(PROBES) / sizeof(PROBES[0]); i++) {
            if (strlen(PROBES[i].path) == len && strncmp(req->uri, PROBES[i].path, len) == 0) {
                httpd_resp_set_status(req, PROBES[i].status);
                httpd_resp_set_type(req, "text/html");
                httpd_resp_set_hdr(req, "Cache-Control", "no-store");
                return httpd_resp_sendstr(req, PROBES[i].body);
            }
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", CAPTIVE_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // iOS erkennt das Portal nur, wenn die Antwort auch einen Inhalt hat
    return httpd_resp_sendstr(req, "uprint: " CAPTIVE_URL);
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 56;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // für PUT /api/v1/files/*
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;

    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start");

    const httpd_uri_t routes[] = {
        {.uri = "/",            .method = HTTP_GET,    .handler = asset_get, .user_ctx = (void *)&ASSET_INDEX},
        {.uri = "/i18n.js",     .method = HTTP_GET,    .handler = asset_get, .user_ctx = (void *)&ASSET_I18N},
        {.uri = "/fonts/space-grotesk.woff2",  .method = HTTP_GET, .handler = asset_get, .user_ctx = (void *)&ASSET_FONT_SANS},
        {.uri = "/fonts/jetbrains-mono.woff2", .method = HTTP_GET, .handler = asset_get, .user_ctx = (void *)&ASSET_FONT_MONO},
        {.uri = "/api/status",  .method = HTTP_GET,    .handler = status_get},
        {.uri = "/api/files",   .method = HTTP_GET,    .handler = files_get},
        {.uri = "/api/files",   .method = HTTP_DELETE, .handler = files_delete},
        {.uri = "/api/upload",  .method = HTTP_POST,   .handler = upload_post},
        {.uri = "/api/print",   .method = HTTP_POST,   .handler = print_post},
        {.uri = "/api/pause",   .method = HTTP_POST,   .handler = pause_post},
        {.uri = "/api/resume",  .method = HTTP_POST,   .handler = resume_post},
        {.uri = "/api/cancel",  .method = HTTP_POST,   .handler = cancel_post},
        {.uri = "/api/printer/retry", .method = HTTP_POST, .handler = retry_post},
        {.uri = "/api/printer/prompt", .method = HTTP_POST, .handler = prompt_post},
        {.uri = "/api/wifi",      .method = HTTP_GET,    .handler = wifi_get},
        {.uri = "/api/wifi",      .method = HTTP_POST,   .handler = wifi_post},
        {.uri = "/api/wifi",      .method = HTTP_DELETE, .handler = wifi_delete},
        {.uri = "/api/wifi/scan", .method = HTTP_GET,    .handler = wifi_scan_get},
        {.uri = "/api/wifi/ap-only", .method = HTTP_POST, .handler = wifi_ap_only_post},
        {.uri = "/api/settings",  .method = HTTP_GET,    .handler = settings_get_handler},
        {.uri = "/api/settings",  .method = HTTP_POST,   .handler = settings_post_handler},
        {.uri = "/api/reboot",    .method = HTTP_POST,   .handler = reboot_post},
        {.uri = "/api/reset",     .method = HTTP_POST,   .handler = reset_post},
        {.uri = "/api/storage/wipe", .method = HTTP_POST, .handler = wipe_post},
        {.uri = "/api/settings/apikey", .method = HTTP_POST, .handler = api_key_post},
        {.uri = "/api/ota",       .method = HTTP_POST,   .handler = ota_post},
        {.uri = "/api/download",  .method = HTTP_GET,    .handler = download_get},
        {.uri = "/api/axidraw/test",  .method = HTTP_GET,  .handler = axidraw_test_get},
        {.uri = "/api/axidraw/test",  .method = HTTP_POST, .handler = axidraw_test_post},
        {.uri = "/api/axidraw/abort", .method = HTTP_POST, .handler = axidraw_abort_post},
        {.uri = "/api/axidraw/log",   .method = HTTP_GET,  .handler = axidraw_log_get},
        {.uri = "/api/leds",          .method = HTTP_GET,  .handler = leds_get},
        {.uri = "/api/leds",          .method = HTTP_POST, .handler = leds_post},
        {.uri = "/api/leds/reset",    .method = HTTP_POST, .handler = leds_reset_post},
        {.uri = "/api/leds/preview",  .method = HTTP_POST, .handler = leds_preview_post},
        {.uri = "/api/led/find",      .method = HTTP_POST, .handler = led_find_post},
        {.uri = "/api/auth",          .method = HTTP_GET,    .handler = auth_get},
        {.uri = "/api/auth",          .method = HTTP_POST,   .handler = auth_post},
        {.uri = "/api/auth",          .method = HTTP_DELETE, .handler = auth_delete},
        {.uri = "/api/auth/password", .method = HTTP_POST,   .handler = password_post},
        {.uri = "/api/auth/skip",     .method = HTTP_POST,   .handler = auth_skip_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t r = routes[i];
        if (!route_open(&r)) {
            r.user_ctx = (void *)r.handler;
            r.handler = guarded;
        }
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &r), TAG, "route %s", r.uri);
    }
    ESP_RETURN_ON_ERROR(slicer_api_register(server), TAG, "Slicer-API");
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    // Am Access Point fragen Handys laufend Prüfadressen ab, die sollen das Log nicht fluten
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Webserver läuft auf Port 80");
    return ESP_OK;
}
