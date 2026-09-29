#include <cstring>

#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"

#include "settings.h"
#include "usb_serial.h"

using namespace esp_usb;

static const char *TAG = "usb_serial";

#define RX_BUFFER_SIZE   4096
#define TX_BUFFER_SIZE   256
#define OPEN_TIMEOUT_MS  1000
#define TX_TIMEOUT_MS    1000

static StreamBufferHandle_t s_rx;
static SemaphoreHandle_t s_dev_lock;
static SemaphoreHandle_t s_disconnected;
static CdcAcmDevice *s_dev;
static volatile bool s_connected;
static volatile uint32_t s_generation;

// Nur vom lesenden Task benutzt
static char s_line[256];
static size_t s_line_len;
static uint8_t s_chunk[64];
static size_t s_chunk_len;
static size_t s_chunk_pos;

static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    if (xStreamBufferSend(s_rx, data, len, 0) < len) {
        ESP_LOGW(TAG, "RX-Puffer voll, Daten verworfen");
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *arg)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-Fehler %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        // Gerät darf nicht im Callback geschlossen werden
        xSemaphoreGive(s_disconnected);
        break;
    default:
        break;
    }
}

// Meldet jedes Gerät, das am Host-Port enumeriert wird, auch wenn es kein serielles ist
static void on_new_device(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) == ESP_OK) {
        ESP_LOGI(TAG, "USB-Gerät erkannt: VID 0x%04x, PID 0x%04x, Klasse 0x%02x",
                 desc->idVendor, desc->idProduct, desc->bDeviceClass);
    }
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static CdcAcmDevice *open_device(const cdc_acm_host_device_config_t *cfg)
{
    // USB-Seriell-Wandler anhand VID/PID
    CdcAcmDevice *dev = VCP::open(cfg);
    if (dev) {
        return dev;
    }
    // Sonst generisches CDC-ACM (MK3S mit ATmega32U2, Boards mit nativem USB)
    dev = new CdcAcmDevice();
    if (dev->open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, cfg) == ESP_OK) {
        return dev;
    }
    delete dev;
    return nullptr;
}

static void connection_task(void *arg)
{
    cdc_acm_host_device_config_t cfg = {};
    cfg.connection_timeout_ms = OPEN_TIMEOUT_MS;
    cfg.out_buffer_size = TX_BUFFER_SIZE;
    cfg.event_cb = on_event;
    cfg.data_cb = on_rx;
    cfg.user_arg = nullptr;

    for (;;) {
        xSemaphoreTake(s_disconnected, 0);

        CdcAcmDevice *dev = open_device(&cfg);
        if (!dev) {
            continue;
        }

        settings_t settings;
        settings_get(&settings);
        cdc_acm_line_coding_t coding = {};
        coding.dwDTERate = settings.baud;
        coding.bCharFormat = 0;   // 1 Stoppbit
        coding.bParityType = 0;   // keine Parität
        coding.bDataBits = 8;

        // DTR setzen: Die meisten Boards (auch der MK3S) starten dabei neu
        if (dev->line_coding_set(&coding) != ESP_OK || dev->set_control_line_state(true, true) != ESP_OK) {
            // z. B. CH34x, das kurz vor dem VCP-Treiber als generisches CDC geöffnet wurde
            ESP_LOGW(TAG, "Gerät akzeptiert Leitungsparameter nicht, neuer Versuch");
            delete dev;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        xSemaphoreTake(s_dev_lock, portMAX_DELAY);
        s_dev = dev;
        s_generation = s_generation + 1;
        s_connected = true;
        xSemaphoreGive(s_dev_lock);
        ESP_LOGI(TAG, "Drucker verbunden (%d Baud)", settings.baud);

        xSemaphoreTake(s_disconnected, portMAX_DELAY);

        xSemaphoreTake(s_dev_lock, portMAX_DELAY);
        s_connected = false;
        delete s_dev;
        s_dev = nullptr;
        xSemaphoreGive(s_dev_lock);
        ESP_LOGW(TAG, "Drucker getrennt");
    }
}

extern "C" esp_err_t usb_serial_init(void)
{
    s_rx = xStreamBufferCreate(RX_BUFFER_SIZE, 1);
    s_dev_lock = xSemaphoreCreateMutex();
    s_disconnected = xSemaphoreCreateBinary();
    if (!s_rx || !s_dev_lock || !s_disconnected) {
        return ESP_ERR_NO_MEM;
    }

    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
    xTaskCreate(usb_lib_task, "usb_lib", 4096, nullptr, 10, nullptr);

    cdc_acm_host_driver_config_t drv_cfg = {};
    drv_cfg.driver_task_stack_size = 4096;
    drv_cfg.driver_task_priority = 10;
    drv_cfg.xCoreID = 0;
    drv_cfg.new_dev_cb = on_new_device;
    ESP_RETURN_ON_ERROR(cdc_acm_host_install(&drv_cfg), TAG, "cdc_acm_host_install");
    VCP::register_driver<FT23x>();
    VCP::register_driver<CP210x>();
    VCP::register_driver<CH34x>();

    xTaskCreate(connection_task, "usb_conn", 4096, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "USB-Host bereit, warte auf Drucker am nativen USB-Port");
    return ESP_OK;
}

extern "C" bool usb_serial_connected(void)
{
    return s_connected;
}

extern "C" uint32_t usb_serial_generation(void)
{
    return s_generation;
}

extern "C" esp_err_t usb_serial_write(const char *data, size_t len)
{
    if (len > TX_BUFFER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_dev_lock, portMAX_DELAY);
    if (s_dev && s_connected) {
        err = s_dev->tx_blocking((uint8_t *)data, len, TX_TIMEOUT_MS);
    }
    xSemaphoreGive(s_dev_lock);
    return err;
}

extern "C" int usb_serial_readline(char *out, size_t out_len, TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        while (s_chunk_pos < s_chunk_len) {
            char c = (char)s_chunk[s_chunk_pos++];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                size_t n = s_line_len < out_len - 1 ? s_line_len : out_len - 1;
                memcpy(out, s_line, n);
                out[n] = '\0';
                s_line_len = 0;
                return (int)n;
            }
            if (s_line_len < sizeof(s_line)) {
                s_line[s_line_len++] = c;
            }
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return -1;
        }
        s_chunk_pos = 0;
        s_chunk_len = xStreamBufferReceive(s_rx, s_chunk, sizeof(s_chunk), timeout - elapsed);
        if (s_chunk_len == 0) {
            return -1;
        }
    }
}

extern "C" void usb_serial_flush_rx(void)
{
    xStreamBufferReset(s_rx);
    s_line_len = 0;
    s_chunk_len = 0;
    s_chunk_pos = 0;
}
