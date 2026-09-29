#include "esp_log.h"
#include "nvs_flash.h"

#include "printer.h"
#include "settings.h"
#include "storage.h"
#include "usb_serial.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "uprint";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_init();

    // Ohne Speicher läuft die Weboberfläche weiter und zeigt den Fehler an
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Kein Dateispeicher verfügbar");
    }

    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(usb_serial_init());
    ESP_ERROR_CHECK(printer_init());
    ESP_ERROR_CHECK(web_start());

    ESP_LOGI(TAG, "uprint gestartet");
}
