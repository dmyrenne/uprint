#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"

#include "variant.h"

// UPRINT_VARIANT setzt CMakeLists.txt je nach Build-Variante
const __attribute__((section(".rodata_custom_desc"))) uprint_desc_t uprint_desc = {
    .magic = UPRINT_DESC_MAGIC,
    .variant = UPRINT_VARIANT,
};

const char *uprint_variant(void)
{
    return uprint_desc.variant;
}

bool uprint_read_desc(const esp_partition_t *part, uprint_desc_t *out)
{
    const size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (esp_partition_read(part, offset, out, sizeof(*out)) != ESP_OK) {
        return false;
    }
    out->variant[sizeof(out->variant) - 1] = '\0';
    return memcmp(out->magic, UPRINT_DESC_MAGIC, sizeof(UPRINT_DESC_MAGIC)) == 0;
}
