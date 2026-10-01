#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

#include "settings.h"
#include "storage.h"

static const char *TAG = "storage";

// So oft wird geprüft, ob die SD-Karte noch steckt bzw. eine eingesteckt wurde
#define SD_POLL_MS 2000

typedef struct {
    const char *id;
    const char *label;
    const char *mount;
    bool present;
    volatile bool ready;   // eingebunden und erreichbar
    bool mounted;          // nur SD: im VFS eingebunden (kann nach dem Entfernen noch kurz gelten)
    int users;             // offene Zugriffe über storage_acquire()
} volume_t;

static volume_t s_vol[STORAGE_COUNT] = {
    [STORAGE_SD]    = {"sd",    "SD-Karte",          "/sdcard", true,  false},
    [STORAGE_FLASH] = {"flash", "Interner Speicher", "/flash",  false, false},
};

static SemaphoreHandle_t s_lock;
static volatile uint32_t s_revision;
static sdmmc_host_t s_sd_host = SDSPI_HOST_DEFAULT();
static sdspi_device_config_t s_sd_slot = SDSPI_DEVICE_CONFIG_DEFAULT();
static sdmmc_card_t *s_sd_card;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// Beim wiederholten Einbinden ohne Karte würden die SD-Treiber jedes Mal Fehler loggen
static const char *const SD_LOG_TAGS[] = {"sdmmc_common", "sdmmc_init", "sdmmc_sd", "sdmmc_cmd",
                                          "sdspi_host", "vfs_fat_sdmmc", "diskio_sdmmc"};

static void sd_logs_quiet(bool quiet)
{
    static esp_log_level_t saved[sizeof(SD_LOG_TAGS) / sizeof(SD_LOG_TAGS[0])];
    for (size_t i = 0; i < sizeof(SD_LOG_TAGS) / sizeof(SD_LOG_TAGS[0]); i++) {
        if (quiet) {
            saved[i] = esp_log_level_get(SD_LOG_TAGS[i]);
            esp_log_level_set(SD_LOG_TAGS[i], ESP_LOG_NONE);
        } else {
            esp_log_level_set(SD_LOG_TAGS[i], saved[i]);
        }
    }
}

static esp_err_t sd_bus_init(void)
{
    settings_t cfg;
    settings_get(&cfg);

    spi_bus_config_t bus = {
        .mosi_io_num = cfg.sd_mosi,
        .miso_io_num = cfg.sd_miso,
        .sclk_io_num = cfg.sd_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(s_sd_host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI-Bus für die SD-Karte: %s (MOSI=%d MISO=%d SCLK=%d)", esp_err_to_name(err),
                 cfg.sd_mosi, cfg.sd_miso, cfg.sd_sclk);
        return err;
    }

    // SD im SPI-Modus braucht Pull-ups auf MISO/MOSI/CS; interne sind nur eine Notlösung
    gpio_pullup_en(cfg.sd_miso);
    gpio_pullup_en(cfg.sd_mosi);
    gpio_pullup_en(cfg.sd_cs);

    s_sd_slot.gpio_cs = cfg.sd_cs;
    s_sd_slot.host_id = s_sd_host.slot;
    return ESP_OK;
}

static esp_err_t sd_mount(bool quiet)
{
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        // Jeder Zugriff prüft, ob die Karte noch antwortet; so fällt das Entfernen sofort auf
        .disk_status_check_enable = true,
    };
    if (quiet) {
        sd_logs_quiet(true);
    }
    esp_err_t err = esp_vfs_fat_sdspi_mount(s_vol[STORAGE_SD].mount, &s_sd_host, &s_sd_slot, &mount, &s_sd_card);
    if (quiet) {
        sd_logs_quiet(false);
    }
    if (err != ESP_OK) {
        if (!quiet) {
            ESP_LOGW(TAG, "SD-Karte nicht verfügbar: %s (CS=%d), wird alle %d s erneut gesucht", esp_err_to_name(err),
                     s_sd_slot.gpio_cs, SD_POLL_MS / 1000);
        }
        s_sd_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_sd_card);
    return ESP_OK;
}

void storage_changed(void)
{
    s_revision = s_revision + 1;
}

// Erkennt das Entfernen und Einstecken der SD-Karte (Module ohne Card-Detect-Pin)
static void sd_poll_task(void *arg)
{
    volume_t *v = &s_vol[STORAGE_SD];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SD_POLL_MS));

        if (v->mounted && v->ready) {
            uint64_t total, avail;
            // Läuft über FatFs (mit dessen Sperre) und fragt dabei den Kartenstatus ab
            sd_logs_quiet(true);
            esp_err_t err = esp_vfs_fat_info(v->mount, &total, &avail);
            sd_logs_quiet(false);
            if (err != ESP_OK) {
                v->ready = false;
                ESP_LOGW(TAG, "SD-Karte entfernt");
                storage_changed();
            }
        }
        if (v->mounted && !v->ready) {
            // Erst aushängen, wenn keine Datei mehr offen ist (Druck, Upload, Download)
            LOCK();
            if (v->users == 0) {
                sd_logs_quiet(true);
                esp_vfs_fat_sdcard_unmount(v->mount, s_sd_card);
                sd_logs_quiet(false);
                s_sd_card = NULL;
                v->mounted = false;
            }
            UNLOCK();
        }
        if (!v->mounted && sd_mount(true) == ESP_OK) {
            LOCK();
            v->mounted = true;
            v->ready = true;
            UNLOCK();
            ESP_LOGI(TAG, "SD-Karte eingesteckt");
            storage_changed();
        }
    }
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
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    // Den internen Speicher gibt es nur, wenn die Partitionstabelle eine Partition "storage" hat
    s_vol[STORAGE_FLASH].present =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage") != NULL;
    s_vol[STORAGE_FLASH].ready = s_vol[STORAGE_FLASH].present && mount_flash() == ESP_OK;
    s_vol[STORAGE_FLASH].mounted = s_vol[STORAGE_FLASH].ready;

    // Ohne gültigen SPI-Bus (z. B. falsche Pins) gibt es auch später keine SD-Karte
    if (sd_bus_init() == ESP_OK) {
        s_vol[STORAGE_SD].mounted = s_vol[STORAGE_SD].ready = sd_mount(false) == ESP_OK;
        xTaskCreate(sd_poll_task, "sd_poll", 4096, NULL, 2, NULL);
    }

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

bool storage_present(storage_vol_t vol)
{
    return vol < STORAGE_COUNT && s_vol[vol].present;
}

bool storage_ready(storage_vol_t vol)
{
    return vol < STORAGE_COUNT && s_vol[vol].ready;
}

bool storage_acquire(storage_vol_t vol)
{
    if (vol >= STORAGE_COUNT) {
        return false;
    }
    LOCK();
    bool ok = s_vol[vol].ready;
    if (ok) {
        s_vol[vol].users++;
    }
    UNLOCK();
    return ok;
}

void storage_release(storage_vol_t vol)
{
    LOCK();
    if (s_vol[vol].users > 0) {
        s_vol[vol].users--;
    }
    UNLOCK();
}

uint32_t storage_revision(void)
{
    return s_revision;
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
    if (!storage_acquire(vol)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_vfs_fat_info(s_vol[vol].mount, total, free);
    storage_release(vol);
    return err;
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
