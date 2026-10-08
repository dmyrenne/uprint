#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// AxiDraw/NextDraw (EiBotBoard). Alles läuft im Drucker-Task, solange als Gerätetyp AxiDraw
// eingestellt ist: Verbindung, Plotten (G-Code → EBB, siehe plot.h) und Testmodus.

esp_err_t axidraw_init(void);

// Aus dem Drucker-Task nach jeder (Neu-)Verbindung: fragt die Firmware ab und
// schreibt eine kurze Statusmeldung nach msg.
void axidraw_link(char *msg, size_t len);

// Aus dem Drucker-Task, regelmäßig: führt einen angeforderten Test aus (blockiert bis zum Ende)
// und verwirft sonst eintreffende Zeilen.
void axidraw_poll(void);

// Plotten, nur aus dem Drucker-Task. false bei einem Fehler, Text über axidraw_plot_error().
// Vertrag für die Datei (erste Zeile, Z 0/1): docs/axidraw.md
#define AXIDRAW_FILE_TAG "; uplot-axidraw 1"
bool axidraw_plot_begin(void);
bool axidraw_plot_line(const char *line);
bool axidraw_plot_end(void);
// Pause: gepufferte Strecken ausfahren, Stift hoch, Motoren bleiben an. Fortsetzen: Stift wieder runter.
void axidraw_plot_pause(void);
void axidraw_plot_resume(void);
// Abbrechen: Puffer verwerfen, Stift hoch, zurück auf den Nullpunkt, Motoren aus
void axidraw_plot_cancel(void);
const char *axidraw_plot_error(void);
// PRG-Taste (fragt höchstens alle 100–200 ms ab): 1 = neu gedrückt. Eine gehaltene Taste zählt erst
// wieder, nachdem sie losgelassen war.
int axidraw_plot_button(bool paused);

bool axidraw_test_running(void);

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
