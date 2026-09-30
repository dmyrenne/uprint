#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Testmodus für AxiDraw/NextDraw (EiBotBoard). Der Ablauf läuft im Drucker-Task,
// solange als Gerätetyp AxiDraw eingestellt ist.

esp_err_t axidraw_init(void);

// Aus dem Drucker-Task nach jeder (Neu-)Verbindung: fragt die Firmware ab und
// schreibt eine kurze Statusmeldung nach msg.
void axidraw_link(char *msg, size_t len);

// Aus dem Drucker-Task, regelmäßig: führt einen angeforderten Test aus (blockiert bis zum Ende)
// und verwirft sonst eintreffende Zeilen.
void axidraw_poll(void);

// ESP_ERR_INVALID_STATE, wenn schon ein Test läuft oder kein AxiDraw verbunden ist.
esp_err_t axidraw_test_start(bool motion);
void axidraw_test_abort(void);

typedef struct {
    bool running;
    bool done;        // mindestens ein Test ist gelaufen, das Log ist gefüllt
    char prompt[160]; // was der Nutzer gerade tun oder beobachten soll
    size_t log_len;
} axidraw_test_status_t;

void axidraw_test_status(axidraw_test_status_t *out);

// Kopiert einen Ausschnitt des Logs ab offset; Rückgabe: Anzahl Bytes (0 = Ende)
size_t axidraw_test_log(char *out, size_t len, size_t offset);
