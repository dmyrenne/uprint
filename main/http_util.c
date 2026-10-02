#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_log.h"

#include "http_util.h"
#include "printer.h"

static const char *TAG = "http";

#define RECV_MAX_RETRIES 10
#define UPLOAD_TMP_NAME  ".upload.tmp"

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void http_url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && hex_value(s[1]) >= 0 && hex_value(s[2]) >= 0) {
            *out++ = (char)(hex_value(s[1]) << 4 | hex_value(s[2]));
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

void http_json_escape(char *out, size_t len, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < len; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += snprintf(out + o, len - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
}

esp_err_t http_send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char esc[256];
    char body[300];
    http_json_escape(esc, sizeof(esc), msg);
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

esp_err_t http_send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

int http_recv(httpd_req_t *req, char *buf, size_t len)
{
    for (int retries = 0;; retries++) {
        int r = httpd_req_recv(req, buf, len);
        if (r != HTTPD_SOCK_ERR_TIMEOUT || retries >= RECV_MAX_RETRIES) {
            return r;
        }
    }
}

bool http_is_gcode(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".gcode") == 0 || strcasecmp(dot, ".gco") == 0 || strcasecmp(dot, ".g") == 0);
}

const char *upload_begin(upload_t *u, storage_vol_t vol, const char *name, size_t size, const char **status)
{
    uint64_t total, avail;
    memset(u, 0, sizeof(*u));
    *status = "400 Bad Request";
    if (!storage_name_valid(name) || !http_is_gcode(name)) {
        return "Ungültiger Dateiname (erlaubt: .gcode, .gco, .g)";
    }
    if (storage_usage(vol, &total, &avail) != ESP_OK) {
        *status = "503 Service Unavailable";
        return "Speicher nicht verfügbar";
    }
    // Die Datei wird erst temporär geschrieben, der volle Platz muss also frei sein
    if (size > avail) {
        *status = "507 Insufficient Storage";
        return "Nicht genug freier Speicher";
    }
    if (printer_is_using(vol, name)) {
        *status = "409 Conflict";
        return "Datei wird gerade gedruckt";
    }
    if (!storage_acquire(vol)) {
        *status = "503 Service Unavailable";
        return "Speicher nicht verfügbar";
    }
    u->held = true;
    u->vol = vol;
    strlcpy(u->name, name, sizeof(u->name));
    storage_path(u->tmp, sizeof(u->tmp), vol, UPLOAD_TMP_NAME);
    storage_path(u->path, sizeof(u->path), vol, name);
    u->f = fopen(u->tmp, "wb");
    if (!u->f) {
        upload_abort(u);
        *status = "500 Internal Server Error";
        return "Datei kann nicht angelegt werden";
    }
    return NULL;
}

bool upload_write(upload_t *u, const void *data, size_t len)
{
    if (!u->f || fwrite(data, 1, len, u->f) != len) {
        return false;
    }
    u->written += len;
    return true;
}

const char *upload_finish(upload_t *u)
{
    const char *failure = NULL;
    if (fclose(u->f) != 0) {
        failure = "Schreibfehler";
    }
    u->f = NULL;
    if (!failure && printer_is_using(u->vol, u->name)) {
        failure = "Datei wird gerade gedruckt";
    }
    if (!failure) {
        unlink(u->path);
        if (rename(u->tmp, u->path) != 0) {
            failure = "Umbenennen fehlgeschlagen";
        }
    }
    if (failure) {
        unlink(u->tmp);
        upload_abort(u);
        ESP_LOGW(TAG, "Upload %s: %s", u->name, failure);
        return failure;
    }
    storage_release(u->vol);
    u->held = false;
    storage_changed();
    ESP_LOGI(TAG, "Hochgeladen: %s (%s, %u Bytes)", u->name, storage_label(u->vol), (unsigned)u->written);
    return NULL;
}

void upload_abort(upload_t *u)
{
    if (u->f) {
        fclose(u->f);
        u->f = NULL;
        unlink(u->tmp);
    }
    if (u->held) {
        storage_release(u->vol);
        u->held = false;
    }
}
