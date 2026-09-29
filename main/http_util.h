#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "storage.h"

#define HTTP_CHUNK       (32 * 1024)
#define ESCAPED_NAME_MAX (STORAGE_NAME_MAX * 6)

void http_url_decode(char *s);
void http_json_escape(char *out, size_t len, const char *in);
esp_err_t http_send_error(httpd_req_t *req, const char *status, const char *msg);
esp_err_t http_send_ok(httpd_req_t *req);

// httpd_req_recv mit Wiederholung bei Zeitüberschreitung; <= 0 bei Abbruch
int http_recv(httpd_req_t *req, char *buf, size_t len);

bool http_is_gcode(const char *name);

// Datei-Upload in einen Speicher: erst in eine temporäre Datei, am Ende umbenennen.
// So bleibt eine vorhandene Datei gleichen Namens bis zum erfolgreichen Abschluss erhalten.
typedef struct {
    storage_vol_t vol;
    char name[STORAGE_NAME_MAX];
    char path[STORAGE_PATH_MAX];
    char tmp[STORAGE_PATH_MAX];
    FILE *f;
    size_t written;
} upload_t;

// Prüft Speicher, Platz (size ist eine Obergrenze) und laufenden Druck.
// Bei Fehler: Meldung zurück, *status enthält den HTTP-Status.
const char *upload_begin(upload_t *u, storage_vol_t vol, const char *name, size_t size, const char **status);
bool upload_write(upload_t *u, const void *data, size_t len);
// NULL bei Erfolg, sonst Fehlermeldung (die temporäre Datei ist dann gelöscht)
const char *upload_finish(upload_t *u);
void upload_abort(upload_t *u);
