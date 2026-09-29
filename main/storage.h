#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STORAGE_NAME_MAX 128   // inkl. Nullterminator
#define STORAGE_PATH_MAX (8 + STORAGE_NAME_MAX)

typedef enum {
    STORAGE_SD,
    STORAGE_FLASH,
    STORAGE_COUNT,
} storage_vol_t;

// Bindet alle verfügbaren Speicher ein; ESP_OK, wenn mindestens einer bereit ist.
esp_err_t storage_init(void);

bool storage_ready(storage_vol_t vol);
const char *storage_mount(storage_vol_t vol);
const char *storage_id(storage_vol_t vol);      // "sd", "flash" (für die API)
const char *storage_label(storage_vol_t vol);   // für die Anzeige
bool storage_from_id(const char *id, storage_vol_t *out);
esp_err_t storage_usage(storage_vol_t vol, uint64_t *total, uint64_t *free);

// Nur einfache Dateinamen im Wurzelverzeichnis, keine Pfade, keine versteckten Dateien.
bool storage_name_valid(const char *name);

void storage_path(char *out, size_t len, storage_vol_t vol, const char *name);
