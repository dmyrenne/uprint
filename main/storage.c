#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

#include "settings.h"
#include "storage.h"

static const char *TAG = "storage";

typedef struct {
    const char *id;
    const char *label;
    const char *mount;
    bool ready;
} volume_t;

static volume_t s_vol[STORAGE_COUNT] = {
    [STORAGE_SD]    = {"sd",    "SD-Karte",          "/sdcard", false},
    [STORAGE_FLASH] = {"flash", "Interner Speicher", "/flash",  false},
};

static esp_err_t mount_sd(void)
{
    settings_t cfg;
    settings_get(&cfg);
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus = {
        .mosi_io_num = cfg.sd_mosi,
        .miso_io_num = cfg.sd_miso,
        .sclk_io_num = cfg.sd_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA), TAG, "SPI-Bus");

    // SD im SPI-Modus braucht Pull-ups auf MISO/MOSI/CS; interne sind nur eine Notlösung
    gpio_pullup_en(cfg.sd_miso);
    gpio_pullup_en(cfg.sd_mosi);
    gpio_pullup_en(cfg.sd_cs);

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = cfg.sd_cs;
    slot.host_id = host.slot;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdspi_mount(s_vol[STORAGE_SD].mount, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD-Karte nicht verfügbar: %s (MOSI=%d MISO=%d SCLK=%d CS=%d)", esp_err_to_name(err),
                 cfg.sd_mosi, cfg.sd_miso, cfg.sd_sclk, cfg.sd_cs);
        spi_bus_free(host.slot);
        return err;
    }
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

static esp_err_t mount_flash(void)
{
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = true,   // beim ersten Start leer formatieren
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    wl_handle_t wl;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(s_vol[STORAGE_FLASH].mount, "storage", &mount, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Interner Speicher nicht verfügbar: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_init(void)
{
    s_vol[STORAGE_FLASH].ready = mount_flash() == ESP_OK;
    s_vol[STORAGE_SD].ready = mount_sd() == ESP_OK;

    bool any = false;
    for (int v = 0; v < STORAGE_COUNT; v++) {
        uint64_t total, free;
        if (storage_usage(v, &total, &free) == ESP_OK) {
            ESP_LOGI(TAG, "%s (%s): %llu KB frei von %llu KB", s_vol[v].label, s_vol[v].mount,
                     free / 1024, total / 1024);
            any = true;
        }
    }
    return any ? ESP_OK : ESP_FAIL;
}

bool storage_ready(storage_vol_t vol)
{
    return vol < STORAGE_COUNT && s_vol[vol].ready;
}

const char *storage_mount(storage_vol_t vol)
{
    return s_vol[vol].mount;
}

const char *storage_id(storage_vol_t vol)
{
    return s_vol[vol].id;
}

const char *storage_label(storage_vol_t vol)
{
    return s_vol[vol].label;
}

bool storage_from_id(const char *id, storage_vol_t *out)
{
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (strcmp(id, s_vol[v].id) == 0) {
            *out = v;
            return true;
        }
    }
    return false;
}

esp_err_t storage_usage(storage_vol_t vol, uint64_t *total, uint64_t *free)
{
    *total = 0;
    *free = 0;
    if (!storage_ready(vol)) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(s_vol[vol].mount, total, free);
}

bool storage_name_valid(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= STORAGE_NAME_MAX || name[0] == '.') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || strchr("/\\:*?\"<>|", *p)) {
            return false;
        }
    }
    return true;
}

void storage_path(char *out, size_t len, storage_vol_t vol, const char *name)
{
    snprintf(out, len, "%s/%s", s_vol[vol].mount, name);
}
