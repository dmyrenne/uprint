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
#include "captive.h"
#include "http_util.h"
#include "printer.h"
#include "slicer.h"
#include "settings.h"
#include "variant.h"
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
    static char body[ESCAPED_NAME_MAX + sizeof(msg) + 256];
    printer_status_t st;

    printer_get_status(&st);
    http_json_escape(file, sizeof(file), st.file);
    http_json_escape(msg, sizeof(msg), st.message);
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"file\":\"%s\",\"size\":%" PRIu32 ",\"pos\":%" PRIu32
             ",\"elapsed\":%" PRIu32 ",\"hotend\":%.1f,\"hotend_target\":%.1f"
             ",\"bed\":%.1f,\"bed_target\":%.1f,\"message\":\"%s\",\"storage\":\"%s\""
             ",\"finished\":%s,\"duration\":%" PRIu32 ",\"files_rev\":%" PRIu32 "}",
             printer_state_name(st.state), file, st.file_size, st.file_pos, st.elapsed_s,
             st.hotend, st.hotend_target, st.bed, st.bed_target, msg, storage_id(st.vol),
             st.finished ? "true" : "false", st.duration_s, storage_revision());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
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
    if (req->content_len == 0) {
        return http_send_error(req, "400 Bad Request", "Leere Datei");
    }
    upload_t up;
    const char *status;
    const char *failure = upload_begin(&up, vol, name, req->content_len, &status);
    if (failure) {
        return http_send_error(req, status, failure);
    }
    char *buf = malloc(HTTP_CHUNK);
    if (!buf) {
        upload_abort(&up);
        return http_send_error(req, "500 Internal Server Error", "Kein Speicher");
    }
    size_t remaining = req->content_len;
    while (remaining > 0 && !failure) {
        int r = http_recv(req, buf, remaining < HTTP_CHUNK ? remaining : HTTP_CHUNK);
        if (r <= 0) {
            failure = "Upload abgebrochen";
        } else if (!upload_write(&up, buf, r)) {
            failure = "Schreibfehler (Speicher voll?)";
        } else {
            remaining -= r;
        }
    }
    free(buf);
    if (failure) {
        upload_abort(&up);
        return http_send_error(req, "500 Internal Server Error", failure);
    }
    failure = upload_finish(&up);
    if (failure) {
        return http_send_error(req, "500 Internal Server Error", failure);
    }
    return http_send_ok(req);
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
    char body[512];
    char ssid[33 * 3 + 1];
    char pass[64 * 3 + 1];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
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
    esp_err_t err = wifi_set_ap_only();
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", "Konnte nicht gespeichert werden");
    }
    return http_send_ok(req);
}

static esp_err_t wifi_delete(httpd_req_t *req)
{
    return send_result(req, wifi_forget(), "");
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    settings_t c;
    char host[33 * 6];
    char pass[64 * 6];
    char body[1536];
    char version[32 * 6];
    char name[33 * 6];
    settings_get(&c);
    http_json_escape(name, sizeof(name), c.device_name);
    http_json_escape(version, sizeof(version), esp_app_get_description()->version);
    http_json_escape(host, sizeof(host), c.hostname);
    http_json_escape(pass, sizeof(pass), c.ap_password);
    snprintf(body, sizeof(body),
             "{\"device_name\":\"%s\",\"hostname\":\"%s\",\"ap_password\":\"%s\",\"baud\":%d,\"pause_lift\":%d,\"cancel_lift\":%d"
             ",\"park_x\":%d,\"park_y\":%d,\"sd_mosi\":%d,\"sd_miso\":%d,\"sd_sclk\":%d,\"sd_cs\":%d"
             ",\"reboot_required\":%s,\"version\":\"%s\",\"variant\":\"%s\",\"api_key\":\"%s\"}",
             name, host, pass, c.baud, c.pause_lift, c.cancel_lift, c.park_x, c.park_y,
             c.sd_mosi, c.sd_miso, c.sd_sclk, c.sd_cs, settings_reboot_required() ? "true" : "false", version, uprint_variant(), c.api_key);
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
    char body[1024];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

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
    form_int(body, "baud", &c.baud, &bad);
    form_int(body, "pause_lift", &c.pause_lift, &bad);
    form_int(body, "cancel_lift", &c.cancel_lift, &bad);
    form_int(body, "park_x", &c.park_x, &bad);
    form_int(body, "park_y", &c.park_y, &bad);
    form_int(body, "sd_mosi", &c.sd_mosi, &bad);
    form_int(body, "sd_miso", &c.sd_miso, &bad);
    form_int(body, "sd_sclk", &c.sd_sclk, &bad);
    form_int(body, "sd_cs", &c.sd_cs, &bad);
    if (bad) {
        return http_send_error(req, "400 Bad Request", "Bitte nur ganze Zahlen eingeben");
    }

    const char *error = NULL;
    esp_err_t err = settings_update(&c, &error);
    if (err != ESP_OK) {
        return http_send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", error);
    }
    wifi_apply_device_name();
    return settings_get_handler(req);
}

static esp_err_t api_key_post(httpd_req_t *req)
{
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

static bool printer_busy(void)
{
    printer_status_t st;
    printer_get_status(&st);
    return st.state == PRINTER_PRINTING || st.state == PRINTER_PAUSED;
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
    }
    free(buf);
    if (failure) {
        esp_ota_abort(ota);
        return http_send_error(req, "400 Bad Request", failure);
    }
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        return http_send_error(req, "400 Bad Request",
                          err == ESP_ERR_OTA_VALIDATE_FAILED ? "Firmware-Datei ist beschädigt oder für einen anderen Chip" : esp_err_to_name(err));
    }

    // Nur Firmware von uprint annehmen
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK || strcmp(desc.project_name, "uprint") != 0) {
        return http_send_error(req, "400 Bad Request", "Das ist keine uprint-Firmware");
    }
    // Eine Firmware für ein anderes Board würde nicht starten (Flash-Größe, PSRAM, Partitionen)
    uprint_desc_t id;
    if (!uprint_read_desc(part, &id) || strcmp(id.variant, uprint_variant()) != 0) {
        static char msg[128];
        snprintf(msg, sizeof(msg), "Falsche Variante: dieses Gerät braucht die Firmware \"%s\"", uprint_variant());
        return http_send_error(req, "400 Bad Request", msg);
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return http_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
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

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (printer_busy()) {
        return http_send_error(req, "409 Conflict", "Während eines Drucks nicht möglich");
    }
    schedule_restart();
    ESP_LOGI(TAG, "Neustart über die Weboberfläche");
    return http_send_ok(req);
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
    cfg.max_uri_handlers = 40;
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
        {.uri = "/api/wifi",      .method = HTTP_GET,    .handler = wifi_get},
        {.uri = "/api/wifi",      .method = HTTP_POST,   .handler = wifi_post},
        {.uri = "/api/wifi",      .method = HTTP_DELETE, .handler = wifi_delete},
        {.uri = "/api/wifi/scan", .method = HTTP_GET,    .handler = wifi_scan_get},
        {.uri = "/api/wifi/ap-only", .method = HTTP_POST, .handler = wifi_ap_only_post},
        {.uri = "/api/settings",  .method = HTTP_GET,    .handler = settings_get_handler},
        {.uri = "/api/settings",  .method = HTTP_POST,   .handler = settings_post_handler},
        {.uri = "/api/reboot",    .method = HTTP_POST,   .handler = reboot_post},
        {.uri = "/api/settings/apikey", .method = HTTP_POST, .handler = api_key_post},
        {.uri = "/api/ota",       .method = HTTP_POST,   .handler = ota_post},
        {.uri = "/api/download",  .method = HTTP_GET,    .handler = download_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route %s", routes[i].uri);
    }
    ESP_RETURN_ON_ERROR(slicer_api_register(server), TAG, "Slicer-API");
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    // Am Access Point fragen Handys laufend Prüfadressen ab, die sollen das Log nicht fluten
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Webserver läuft auf Port 80");
    return ESP_OK;
}
