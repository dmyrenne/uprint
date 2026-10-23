#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage.h"

typedef enum {
    PRINTER_DISCONNECTED,
    PRINTER_CONNECTING,
    PRINTER_IDLE,
    PRINTER_PRINTING,
    PRINTER_PAUSED,
    PRINTER_ERROR,
} printer_state_t;

#define PRINTER_PROMPT_CHOICES 4

typedef struct {
    printer_state_t state;
    storage_vol_t vol;
    char file[STORAGE_NAME_MAX];
    uint32_t file_size;
    uint32_t file_pos;
    uint32_t elapsed_s;
    int64_t started_us;
    float hotend;
    float hotend_target;
    float bed;
    float bed_target;
    char message[160];
    // Letzter Druck lief bis zum Dateiende (file, vol, file_size bleiben bis zum nächsten Start erhalten)
    bool finished;
    uint32_t duration_s;
    // Aus M73 in der Datei (Slicer), -1 = keine Angabe; sonst schätzt die Oberfläche nach Bytes (#34)
    float progress;       // %
    int32_t remaining_s;
    // Meldung des Druckers über "// action:notification …", leer = keine
    char notice[64];
    // Drucker wartet während des Drucks auf eine Eingabe an seinem Display ("busy: paused for user", z. B. M600)
    bool user_wait;
    // Erste Bewegung mit Extrusion gesendet (nach Aufheizen, Referenzieren und Vermessen beginnt der Druck)
    bool extruding;
    // Rückfrage des Druckers ("// action:prompt_…"), Antwort über printer_prompt_answer()
    bool prompt;
    char prompt_text[64];
    char prompt_choice[PRINTER_PROMPT_CHOICES][24];
    int prompt_choices;
    // USB-Gerät da, antwortet aber seit CONNECT_TIMEOUT nicht (z. B. Drucker aus); im Hintergrund wird weiter versucht
    bool unresponsive;
} printer_status_t;

esp_err_t printer_init(void);

void printer_get_status(printer_status_t *out);
const char *printer_state_name(printer_state_t state);

// Alle liefern ESP_ERR_INVALID_STATE, wenn die Aktion im aktuellen Zustand nicht geht.
esp_err_t printer_start(storage_vol_t vol, const char *name);
esp_err_t printer_pause(void);
esp_err_t printer_resume(void);
// Bricht einen Druck ab bzw. quittiert einen Fehler.
esp_err_t printer_cancel(void);

// Antwort auf eine Rückfrage des Druckers (Index der Auswahl); ESP_ERR_INVALID_STATE ohne offene Rückfrage
esp_err_t printer_prompt_answer(int choice);

// Antwortet der Drucker nicht (unresponsive): sofort neu versuchen, die Wartezeit beginnt von vorn.
// ESP_ERR_INVALID_STATE in jedem anderen Zustand.
esp_err_t printer_retry(void);

// Nach Änderung des Gerätetyps: neu verbinden. ESP_ERR_INVALID_STATE während eines Drucks.
esp_err_t printer_device_changed(void);

// true, solange ein Druck läuft oder pausiert ist
bool printer_busy(void);

// true, wenn die Datei gerade gedruckt wird (nicht löschen/überschreiben).
bool printer_is_using(storage_vol_t vol, const char *name);
