#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "http_util.h"
#include "printer.h"
#include "settings.h"
#include "slicer.h"
#include "storage.h"

static const char *TAG = "slicer";

// Die Slicer prüfen, ob "text" mit "OctoPrint" (bzw. "PrusaLink") beginnt
#define OCTOPRINT_VERSION "1.10.0"
#define FILES_PREFIX      "/api/v1/files"

// ---------- Hilfsfunktionen ----------

static bool authorized(httpd_req_t *req)
{
    settings_t cfg;
    char buf[96];
    settings_get(&cfg);
    if (httpd_req_get_hdr_value_str(req, "X-Api-Key", buf, sizeof(buf)) == ESP_OK) {
        return strcmp(buf, cfg.api_key) == 0;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) == ESP_OK && strncmp(buf, "Bearer ", 7) == 0) {
        return strcmp(buf + 7, cfg.api_key) == 0;
    }
    char query[CONFIG_HTTPD_MAX_URI_LEN];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "apikey", buf, sizeof(buf)) == ESP_OK) {
        return strcmp(buf, cfg.api_key) == 0;
    }
    return false;
}

static esp_err_t deny(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Anfrage ohne gültigen API-Schlüssel: %s", req->uri);
    return http_send_error(req, "403 Forbidden", "Ungültiger API-Schlüssel (µprint → Einstellungen → Slicer)");
}

// "local" bzw. leer: SD-Karte, falls vorhanden, sonst interner Speicher
static bool resolve_storage(const char *id, storage_vol_t *vol)
{
    if (id[0] == '\0' || strcmp(id, "local") == 0) {
        if (storage_ready(STORAGE_SD)) {
            *vol = STORAGE_SD;
            return true;
        }
        if (storage_ready(STORAGE_FLASH)) {
            *vol = STORAGE_FLASH;
            return true;
        }
        return false;
    }
    return storage_from_id(id, vol) && storage_ready(*vol);
}

static bool is_binary_gcode(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".bgcode") == 0;
}

static const char *check_name(const char *name)
{
    if (is_binary_gcode(name)) {
        return "Binärer G-Code (.bgcode) wird nicht unterstützt. In PrusaSlicer unter Druckereinstellungen → "
               "Allgemein → Firmware \"Unterstützt binären G-Code\" ausschalten.";
    }
    if (!storage_name_valid(name) || !http_is_gcode(name)) {
        return "Ungültiger Dateiname (erlaubt: .gcode, .gco, .g)";
    }
    return NULL;
}

// Wert eines strukturierten Header-Felds wie "?1" (RFC 8941) oder "true"
static bool header_true(httpd_req_t *req, const char *name)
{
    char buf[16];
    if (httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    return strcmp(buf, "?1") == 0 || strcasecmp(buf, "true") == 0 || strcmp(buf, "1") == 0;
}

static const char *start_after_upload(storage_vol_t vol, const char *name)
{
    esp_err_t err = printer_start(vol, name);
    if (err == ESP_ERR_INVALID_STATE) {
        return "Datei hochgeladen, aber der Drucker ist nicht bereit";
    }
    if (err != ESP_OK) {
        return esp_err_to_name(err);
    }
    ESP_LOGI(TAG, "Druck von %s nach Upload gestartet", name);
    return NULL;
}

static esp_err_t send_created(httpd_req_t *req, storage_vol_t vol, const char *name)
{
    char esc[ESCAPED_NAME_MAX];
    char body[2 * ESCAPED_NAME_MAX + 128];
    http_json_escape(esc, sizeof(esc), name);
    snprintf(body, sizeof(body),
             "{\"done\":true,\"files\":{\"local\":{\"name\":\"%s\",\"path\":\"%s\",\"origin\":\"local\",\"storage\":\"%s\"}}}",
             esc, esc, storage_id(vol));
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// ---------- Version und Speicher ----------

static esp_err_t version_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    settings_t cfg;
    char host[33 * 6];
    char version[32 * 6];
    char body[512];
    settings_get(&cfg);
    http_json_escape(host, sizeof(host), cfg.hostname);
    http_json_escape(version, sizeof(version), esp_app_get_description()->version);
    snprintf(body, sizeof(body),
             "{\"api\":\"0.1\",\"server\":\"" OCTOPRINT_VERSION "\",\"text\":\"OctoPrint " OCTOPRINT_VERSION
             " (µprint %s)\",\"hostname\":\"%s\",\"capabilities\":{\"upload-by-put\":true}}",
             version, host);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t storage_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char item[160];
    bool first = true;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"storage_list\":[");
    for (int v = 0; v < STORAGE_COUNT; v++) {
        uint64_t total, avail;
        if (!storage_present(v) || storage_usage(v, &total, &avail) != ESP_OK) {
            continue;
        }
        snprintf(item, sizeof(item),
                 "%s{\"name\":\"%s\",\"path\":\"/%s\",\"type\":\"LOCAL\",\"read_only\":false,\"available\":true,\"free_space\":%llu}",
                 first ? "" : ",", storage_label(v), storage_id(v), avail);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// ---------- PrusaLink: PUT /api/v1/files/<speicher>/<pfad> ----------

static esp_err_t file_put(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    // Pfad ohne Query: /api/v1/files/sd/Unterordner/Datei.gcode
    char path[CONFIG_HTTPD_MAX_URI_LEN];
    strlcpy(path, req->uri + strlen(FILES_PREFIX), sizeof(path));
    char *q = strchr(path, '?');
    if (q) {
        *q = '\0';
    }
    char *id = path[0] == '/' ? path + 1 : path;
    char *rest = strchr(id, '/');
    if (!rest || rest[1] == '\0') {
        return http_send_error(req, "400 Bad Request", "Dateiname fehlt");
    }
    *rest++ = '\0';
    http_url_decode(rest);
    // µprint speichert flach, Unterordner aus dem Slicer werden ignoriert
    const char *slash = strrchr(rest, '/');
    const char *name = slash ? slash + 1 : rest;

    storage_vol_t vol;
    if (!resolve_storage(id, &vol)) {
        return http_send_error(req, "404 Not Found", "Speicher nicht vorhanden");
    }
    const char *failure = check_name(name);
    if (failure) {
        return http_send_error(req, "415 Unsupported Media Type", failure);
    }
    if (req->content_len == 0) {
        return http_send_error(req, "400 Bad Request", "Leere Datei");
    }

    upload_t up;
    const char *status;
    failure = upload_begin(&up, vol, name, req->content_len, &status);
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
    if (header_true(req, "Print-After-Upload")) {
        failure = start_after_upload(vol, name);
        if (failure) {
            return http_send_error(req, "409 Conflict", failure);
        }
    }
    return send_created(req, vol, name);
}

// ---------- OctoPrint: POST /api/files/local (multipart/form-data) ----------

#define MP_DELIM_MAX 80
#define MP_BUF       (HTTP_CHUNK + MP_DELIM_MAX)

typedef enum { MP_PREAMBLE, MP_AFTER_DELIM, MP_HEADERS, MP_DATA, MP_DONE } mp_state_t;

typedef struct {
    char delim[MP_DELIM_MAX];   // "\r\n--<boundary>"
    size_t dlen;
    mp_state_t state;
    bool is_file;
    char field[24];
    char value[64];
    size_t vlen;
    // Ergebnis
    size_t size_hint;           // Obergrenze für die Dateigröße (Content-Length)
    bool print;
    bool have_file;
    storage_vol_t vol;
    upload_t up;
    const char *error;
    const char *error_status;
} multipart_t;

// Liest ein Attribut wie name="…" aus einer Content-Disposition-Zeile
static bool disposition_param(const char *headers, const char *key, char *out, size_t len)
{
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "%s=\"", key);
    const char *p = headers;
    while ((p = strcasestr(p, pattern)) != NULL) {
        // "filename=" soll nicht als "name=" gelten
        if (p == headers || p[-1] == ' ' || p[-1] == ';') {
            p += strlen(pattern);
            const char *end = strchr(p, '"');
            if (!end) {
                return false;
            }
            size_t n = (size_t)(end - p) < len - 1 ? (size_t)(end - p) : len - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

static void mp_begin_part(multipart_t *mp, char *headers)
{
    char filename[STORAGE_NAME_MAX];
    mp->field[0] = '\0';
    mp->vlen = 0;
    mp->is_file = false;
    disposition_param(headers, "name", mp->field, sizeof(mp->field));
    if (strcmp(mp->field, "file") != 0 || !disposition_param(headers, "filename", filename, sizeof(filename))) {
        return;
    }
    // Manche Clients schicken einen Pfad mit
    const char *name = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    const char *failure = check_name(name);
    if (failure) {
        mp->error = failure;
        mp->error_status = "415 Unsupported Media Type";
        return;
    }
    failure = upload_begin(&mp->up, mp->vol, name, mp->size_hint, &mp->error_status);
    if (failure) {
        mp->error = failure;
        return;
    }
    mp->is_file = true;
}

static void mp_data(multipart_t *mp, const char *data, size_t len)
{
    if (mp->is_file) {
        if (!upload_write(&mp->up, data, len)) {
            mp->error = "Schreibfehler (Speicher voll?)";
            mp->error_status = "500 Internal Server Error";
        }
        return;
    }
    size_t n = len < sizeof(mp->value) - 1 - mp->vlen ? len : sizeof(mp->value) - 1 - mp->vlen;
    memcpy(mp->value + mp->vlen, data, n);
    mp->vlen += n;
    mp->value[mp->vlen] = '\0';
}

static void mp_end_part(multipart_t *mp)
{
    if (mp->is_file) {
        const char *failure = upload_finish(&mp->up);
        if (failure) {
            mp->error = failure;
            mp->error_status = "500 Internal Server Error";
        } else {
            mp->have_file = true;
        }
        mp->is_file = false;
    } else if (strcmp(mp->field, "print") == 0) {
        mp->print = strcasecmp(mp->value, "true") == 0 || strcmp(mp->value, "1") == 0;
    }
}

// Verarbeitet den Puffer so weit wie möglich; liefert die Anzahl verbrauchter Bytes
static size_t mp_process(multipart_t *mp, char *buf, size_t have, bool final)
{
    size_t used = 0;
    while (!mp->error && mp->state != MP_DONE) {
        char *p = buf + used;
        size_t n = have - used;
        if (mp->state == MP_PREAMBLE || mp->state == MP_DATA) {
            char *hit = memmem(p, n, mp->delim, mp->dlen);
            if (hit) {
                if (mp->state == MP_DATA) {
                    mp_data(mp, p, hit - p);
                    mp_end_part(mp);
                }
                used += (hit - p) + mp->dlen;
                mp->state = MP_AFTER_DELIM;
                continue;
            }
            // Kein Trenner: alles bis auf ein mögliches angeschnittenes Trenner-Ende weitergeben
            size_t safe = n > mp->dlen ? n - mp->dlen : 0;
            if (mp->state == MP_DATA && safe > 0) {
                mp_data(mp, p, safe);
            }
            used += safe;
            break;
        }
        if (mp->state == MP_AFTER_DELIM) {
            if (n < 2) {
                break;
            }
            if (p[0] == '-' && p[1] == '-') {
                mp->state = MP_DONE;
                used = have;
                break;
            }
            used += (p[0] == '\r' && p[1] == '\n') ? 2 : 0;
            mp->state = MP_HEADERS;
            continue;
        }
        if (mp->state == MP_HEADERS) {
            char *end = memmem(p, n, "\r\n\r\n", 4);
            if (!end) {
                if (n > 2048) {
                    mp->error = "Ungültige Anfrage (Kopfzeilen zu lang)";
                    mp->error_status = "400 Bad Request";
                }
                break;
            }
            *end = '\0';
            mp_begin_part(mp, p);
            used += (end - p) + 4;
            mp->state = MP_DATA;
        }
    }
    if (final && mp->state != MP_DONE && !mp->error) {
        mp->error = "Upload unvollständig";
        mp->error_status = "400 Bad Request";
    }
    return used;
}

static esp_err_t octoprint_upload(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char ctype[160];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK ||
        strncasecmp(ctype, "multipart/form-data", 19) != 0) {
        return http_send_error(req, "400 Bad Request", "multipart/form-data erwartet");
    }
    const char *b = strcasestr(ctype, "boundary=");
    if (!b) {
        return http_send_error(req, "400 Bad Request", "Boundary fehlt");
    }
    b += 9;
    char boundary[72];
    size_t bl = 0;
    if (*b == '"') {
        b++;
    }
    while (b[bl] && b[bl] != '"' && b[bl] != ';' && bl < sizeof(boundary) - 1) {
        boundary[bl] = b[bl];
        bl++;
    }
    boundary[bl] = '\0';

    multipart_t *mp = calloc(1, sizeof(*mp));
    char *buf = malloc(MP_BUF);
    if (!mp || !buf) {
        free(mp);
        free(buf);
        return http_send_error(req, "500 Internal Server Error", "Kein Speicher");
    }
    mp->dlen = snprintf(mp->delim, sizeof(mp->delim), "\r\n--%s", boundary);
    mp->state = MP_PREAMBLE;
    mp->size_hint = req->content_len;
    if (!resolve_storage("local", &mp->vol)) {
        free(mp);
        free(buf);
        return http_send_error(req, "503 Service Unavailable", "Kein Speicher verfügbar");
    }

    // Vorangestelltes CRLF, damit auch der erste Trenner die Form "\r\n--<boundary>" hat
    size_t have = 2;
    memcpy(buf, "\r\n", 2);
    size_t remaining = req->content_len;
    while (!mp->error && mp->state != MP_DONE) {
        if (remaining > 0 && have < MP_BUF) {
            size_t want = MP_BUF - have < remaining ? MP_BUF - have : remaining;
            int r = http_recv(req, buf + have, want);
            if (r <= 0) {
                mp->error = "Upload abgebrochen";
                mp->error_status = "400 Bad Request";
                break;
            }
            have += r;
            remaining -= r;
        }
        size_t used = mp_process(mp, buf, have, remaining == 0);
        memmove(buf, buf + used, have - used);
        have -= used;
        if (remaining == 0 && used == 0) {
            break;
        }
    }
    free(buf);

    esp_err_t ret;
    if (mp->error || !mp->have_file) {
        upload_abort(&mp->up);
        ret = http_send_error(req, mp->error_status ? mp->error_status : "400 Bad Request",
                              mp->error ? mp->error : "Keine Datei im Upload");
    } else if (mp->print && (mp->error = start_after_upload(mp->vol, mp->up.name)) != NULL) {
        ret = http_send_error(req, "409 Conflict", mp->error);
    } else {
        ret = send_created(req, mp->vol, mp->up.name);
    }
    free(mp);
    return ret;
}

// ---------- OctoPrint: Status ----------

static const char *octoprint_state(printer_state_t state)
{
    switch (state) {
    case PRINTER_PRINTING:   return "Printing";
    case PRINTER_PAUSED:     return "Paused";
    case PRINTER_IDLE:       return "Operational";
    case PRINTER_ERROR:      return "Error";
    case PRINTER_CONNECTING: return "Connecting";
    default:                 return "Offline";
    }
}

static esp_err_t job_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    printer_status_t st;
    char esc[ESCAPED_NAME_MAX];
    char body[ESCAPED_NAME_MAX + 512];
    printer_get_status(&st);
    bool active = st.state == PRINTER_PRINTING || st.state == PRINTER_PAUSED;
    float completion = active && st.file_size ? 100.0f * st.file_pos / st.file_size : 0;
    long left = active && completion > 2 ? (long)(st.elapsed_s * (100 - completion) / completion) : -1;
    http_json_escape(esc, sizeof(esc), active ? st.file : "");
    char left_str[16] = "null";
    if (left >= 0) {
        snprintf(left_str, sizeof(left_str), "%ld", left);
    }
    if (active) {
        snprintf(body, sizeof(body),
                 "{\"job\":{\"file\":{\"name\":\"%s\",\"origin\":\"local\",\"size\":%" PRIu32 "}},"
                 "\"progress\":{\"completion\":%.1f,\"filepos\":%" PRIu32 ",\"printTime\":%" PRIu32 ",\"printTimeLeft\":%s},"
                 "\"state\":\"%s\"}",
                 esc, st.file_size, completion, st.file_pos, st.elapsed_s, left_str, octoprint_state(st.state));
    } else {
        snprintf(body, sizeof(body),
                 "{\"job\":{\"file\":{\"name\":null}},\"progress\":{\"completion\":null,\"filepos\":null,"
                 "\"printTime\":null,\"printTimeLeft\":null},\"state\":\"%s\"}",
                 octoprint_state(st.state));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t printer_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    printer_status_t st;
    char body[640];
    printer_get_status(&st);
    if (st.state == PRINTER_DISCONNECTED || st.state == PRINTER_CONNECTING) {
        return http_send_error(req, "409 Conflict", "Printer is not operational");
    }
    bool printing = st.state == PRINTER_PRINTING, paused = st.state == PRINTER_PAUSED;
    bool error = st.state == PRINTER_ERROR, ready = st.state == PRINTER_IDLE;
    snprintf(body, sizeof(body),
             "{\"temperature\":{\"tool0\":{\"actual\":%.1f,\"target\":%.1f,\"offset\":0},"
             "\"bed\":{\"actual\":%.1f,\"target\":%.1f,\"offset\":0}},"
             "\"state\":{\"text\":\"%s\",\"flags\":{\"operational\":%s,\"printing\":%s,\"paused\":%s,\"pausing\":false,"
             "\"cancelling\":false,\"ready\":%s,\"error\":%s,\"closedOrError\":%s,\"sdReady\":false}}}",
             st.hotend, st.hotend_target, st.bed, st.bed_target, octoprint_state(st.state),
             error ? "false" : "true", printing ? "true" : "false", paused ? "true" : "false",
             ready ? "true" : "false", error ? "true" : "false", error ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// {"command": "pause", "action": "pause" | "resume" | "toggle"} oder {"command": "cancel"}
static esp_err_t job_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return deny(req);
    }
    char body[256];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return http_send_error(req, "400 Bad Request", "Ungültige Anfrage");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = http_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    esp_err_t err;
    if (strstr(body, "\"cancel\"")) {
        err = printer_cancel();
    } else if (strstr(body, "\"pause\"")) {
        printer_status_t st;
        printer_get_status(&st);
        bool resume = strstr(body, "\"resume\"") || (strstr(body, "\"toggle\"") && st.state == PRINTER_PAUSED);
        err = resume ? printer_resume() : printer_pause();
    } else {
        return http_send_error(req, "400 Bad Request", "Unbekannter Befehl (möglich: pause, cancel)");
    }
    if (err != ESP_OK) {
        return http_send_error(req, "409 Conflict", "Im aktuellen Zustand nicht möglich");
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t slicer_api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/version",        .method = HTTP_GET,  .handler = version_get},
        {.uri = "/api/v1/storage",     .method = HTTP_GET,  .handler = storage_get},
        {.uri = FILES_PREFIX "/*",     .method = HTTP_PUT,  .handler = file_put},
        {.uri = "/api/files/local",    .method = HTTP_POST, .handler = octoprint_upload},
        {.uri = "/api/job",            .method = HTTP_GET,  .handler = job_get},
        {.uri = "/api/job",            .method = HTTP_POST, .handler = job_post},
        {.uri = "/api/printer",        .method = HTTP_GET,  .handler = printer_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route %s", routes[i].uri);
    }
    return ESP_OK;
}
