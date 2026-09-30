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

// Nach Änderung des Gerätetyps: neu verbinden. ESP_ERR_INVALID_STATE während eines Drucks.
esp_err_t printer_device_changed(void);

// true, wenn die Datei gerade gedruckt wird (nicht löschen/überschreiben).
bool printer_is_using(storage_vol_t vol, const char *name);
