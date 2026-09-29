#pragma once

#include <stdbool.h>

#include "esp_partition.h"

// Steht an fester Stelle im App-Image (direkt nach esp_app_desc_t), damit ein OTA-Update
// prüfen kann, ob das neue Image zur Board-Variante passt.
#define UPRINT_DESC_MAGIC "UPRINT1"

typedef struct {
    char magic[8];
    char variant[24];   // "n16r8", "basic"
} uprint_desc_t;

// Variante der laufenden Firmware
const char *uprint_variant(void);

// Liest die Kennung aus einem App-Image in einer Partition; false, wenn keine vorhanden ist
bool uprint_read_desc(const esp_partition_t *part, uprint_desc_t *out);
