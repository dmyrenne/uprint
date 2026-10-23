#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "esp_cache.h"
#endif

#include "diag.h"
#include "variant.h"

static const char *TAG = "diag";

#define NVS_NAMESPACE "diag"
#define RING_MAGIC    0x55444731u   // "UDG1"
#define FMT_MAX      320           // längere Ausgaben werden gekürzt

#if CONFIG_SPIRAM
#define DIAG_LOG_SIZE (1024 * 1024)
#else
#define DIAG_LOG_SIZE (16 * 1024)
#endif

typedef struct {
#if CONFIG_SPIRAM
    // Beim Start schreibt das Timing-Tuning des Octal-PSRAM ein Testmuster an den Anfang des PSRAM
    // (MSPI_TIMING_PSRAM_TEST_DATA_ADDR 0, 64 Bytes), und dort liegt .ext_ram_noinit
    char tuning[4096];
#endif
    uint32_t magic;
    uint32_t size;
    uint32_t boots;
    uint32_t reserved;
    uint64_t total;   // seit dem Einschalten geschriebene Bytes
    uint64_t start;   // Position beim letzten Leeren
    char buf[DIAG_LOG_SIZE];
} ring_t;

// Mit PSRAM landet das in .ext_ram_noinit (CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY), sonst in .noinit
static EXT_RAM_NOINIT_ATTR ring_t s_ring;

static SemaphoreHandle_t s_lock;
static vprintf_like_t s_uart = vprintf;
static char s_fmt[FMT_MAX];
static bool s_kept;
static esp_reset_reason_t s_reason;
static volatile diag_serial_t s_serial;
static esp_err_t s_sync_err = ESP_OK;   // erster Fehler beim Zurückschreiben, für die Diagnose

// Der Datencache des ESP32-S3 schreibt verzögert ins PSRAM; ein Absturz oder Watchdog würde den Rest verlieren
static void sync(const void *addr, size_t len)
{
#if CONFIG_SPIRAM
    if (len > 0) {
        esp_err_t err = esp_cache_msync((void *)addr, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        if (err != ESP_OK && s_sync_err == ESP_OK) {
            s_sync_err = err;
        }
    }
#endif
}

// Aufrufer hält s_lock
static void append(const char *data, size_t len)
{
    if (len > DIAG_LOG_SIZE) {
        data += len - DIAG_LOG_SIZE;
        len = DIAG_LOG_SIZE;
    }
    size_t at = s_ring.total % DIAG_LOG_SIZE;
    size_t first = len < DIAG_LOG_SIZE - at ? len : DIAG_LOG_SIZE - at;
    memcpy(s_ring.buf + at, data, first);
    memcpy(s_ring.buf, data + first, len - first);
    s_ring.total += len;
    sync(s_ring.buf + at, first);
    sync(s_ring.buf, len - first);
    sync(&s_ring.magic, offsetof(ring_t, buf) - offsetof(ring_t, magic));
}

static bool can_lock(void)
{
    return s_lock && !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_uart(fmt, ap);
    // Nicht blockieren: Ist der Puffer gerade belegt, fehlt die Zeile eben im Log
    if (can_lock() && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = vsnprintf(s_fmt, sizeof(s_fmt), fmt, copy);
        if (n > 0) {
            if ((size_t)n >= sizeof(s_fmt)) {
                n = sizeof(s_fmt) - 1;
                s_fmt[n - 1] = '\n';
            }
            append(s_fmt, n);
        }
        xSemaphoreGive(s_lock);
    }
    va_end(copy);
    return ret;
}

void diag_line(const char *tag, const char *fmt, ...)
{
    if (!can_lock() || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    const int cap = sizeof(s_fmt) - 1;   // Platz für den Zeilenumbruch
    int n = snprintf(s_fmt, cap, "S (%" PRIu32 ") %s: ", esp_log_timestamp(), tag);
    n = n < 0 ? 0 : n >= cap ? cap - 1 : n;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(s_fmt + n, cap - n, fmt, ap);
    va_end(ap);
    if (m > 0) {
        n += m < cap - n ? m : cap - n - 1;
    }
    s_fmt[n++] = '\n';
    append(s_fmt, n);
    xSemaphoreGive(s_lock);
}

static const char *reason_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "Einschalten";
    case ESP_RST_EXT:        return "Reset-Pin";
    case ESP_RST_SW:         return "Neustart durch die Firmware";
    case ESP_RST_PANIC:      return "Absturz";
    case ESP_RST_INT_WDT:    return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT:   return "Task-Watchdog";
    case ESP_RST_WDT:        return "Watchdog";
    case ESP_RST_DEEPSLEEP:  return "Aufwachen aus dem Tiefschlaf";
    case ESP_RST_BROWNOUT:   return "Unterspannung (Brownout)";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "eFuse-Fehler";
    case ESP_RST_PWR_GLITCH: return "Spannungseinbruch";
    case ESP_RST_CPU_LOCKUP: return "CPU blockiert";
    default:                 return "unbekannt";
    }
}

void diag_init(void)
{
    s_reason = esp_reset_reason();
    // Nach dem Einschalten steht Zufall im RAM; sonst gilt der Puffer, wenn der Kopf stimmt
    s_kept = s_reason != ESP_RST_POWERON && s_reason != ESP_RST_UNKNOWN && s_ring.magic == RING_MAGIC &&
             s_ring.size == DIAG_LOG_SIZE;
    if (!s_kept) {
        s_ring.magic = RING_MAGIC;
        s_ring.size = DIAG_LOG_SIZE;
        s_ring.boots = 0;
        s_ring.total = 0;
        s_ring.start = 0;
    }
    s_ring.boots++;
    sync(&s_ring.magic, offsetof(ring_t, buf) - offsetof(ring_t, magic));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "serial", &v) == ESP_OK && v <= DIAG_SERIAL_ALL) {
            s_serial = v;
        }
        nvs_close(nvs);
    }

    s_lock = xSemaphoreCreateMutex();
    char line[160];
    int n = snprintf(line, sizeof(line), "\n===== Start %" PRIu32 ": %s, Firmware %s (%s) =====\n", s_ring.boots,
                     reason_text(s_reason), esp_app_get_description()->version, uprint_variant());
    append(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    s_uart = esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "Log-Puffer %u KB, %s", DIAG_LOG_SIZE / 1024,
             s_kept ? "Inhalt vom vorigen Lauf übernommen" : "neu begonnen");
    if (s_sync_err != ESP_OK) {
        ESP_LOGW(TAG, "Zurückschreiben ins PSRAM fehlgeschlagen: %s", esp_err_to_name(s_sync_err));
    }
}

diag_serial_t diag_serial(void)
{
    return s_serial;
}

void diag_set_serial(diag_serial_t level)
{
    if (level > DIAG_SERIAL_ALL) {
        return;
    }
    s_serial = level;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "serial", level);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Mitschnitt der Druckerkommunikation: %s",
             level == DIAG_SERIAL_ALL ? "alles" : level == DIAG_SERIAL_RX ? "Meldungen des Druckers" : "aus");
}

// Aufrufer hält s_lock
static uint64_t oldest(void)
{
    uint64_t kept = s_ring.total > DIAG_LOG_SIZE ? s_ring.total - DIAG_LOG_SIZE : 0;
    return kept > s_ring.start ? kept : s_ring.start;
}

size_t diag_log_read(uint64_t *pos, char *out, size_t len)
{
    if (!s_lock || len == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t start = oldest();
    bool skip = false;   // im überschriebenen Bereich: die angeschnittene erste Zeile weglassen
    if (*pos < start || *pos > s_ring.total) {
        *pos = start;
        skip = start > s_ring.start;
    }
    uint64_t avail = s_ring.total - *pos;
    size_t n = avail < len ? (size_t)avail : len;
    size_t at = *pos % DIAG_LOG_SIZE;
    size_t first = n < DIAG_LOG_SIZE - at ? n : DIAG_LOG_SIZE - at;
    memcpy(out, s_ring.buf + at, first);
    memcpy(out + first, s_ring.buf, n - first);
    xSemaphoreGive(s_lock);

    *pos += n;
    if (skip) {
        char *nl = memchr(out, '\n', n);
        size_t cut = nl ? (size_t)(nl - out) + 1 : n;
        memmove(out, out + cut, n - cut);
        n -= cut;
    }
    return n;
}

uint64_t diag_log_end(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t end = s_ring.total;
    xSemaphoreGive(s_lock);
    return end;
}

uint64_t diag_log_start(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t start = oldest();
    xSemaphoreGive(s_lock);
    return start;
}

void diag_log_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Positionen laufen weiter, damit offene Ansichten nur die neuen Zeilen holen
    s_ring.start = s_ring.total;
    sync(&s_ring.magic, offsetof(ring_t, buf) - offsetof(ring_t, magic));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Log geleert");
}

size_t diag_log_size(void)
{
    return DIAG_LOG_SIZE;
}

uint32_t diag_boot_count(void)
{
    return s_ring.boots;
}

bool diag_log_kept(void)
{
    return s_kept;
}

const char *diag_reset_reason(void)
{
    return reason_text(s_reason);
}
