#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "auth.h"
#include "printer.h"
#include "reset.h"
#include "settings.h"
#include "status_led.h"
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
    auth_init();

    // Ohne Speicher läuft die Weboberfläche weiter und zeigt den Fehler an
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Kein Dateispeicher verfügbar");
    }

    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(usb_serial_init());
    ESP_ERROR_CHECK(printer_init());
    // Nach Drucker und WLAN, deren Zustand die LEDs anzeigen
    status_led_init();
    reset_button_start();
    ESP_ERROR_CHECK(web_start());

    // Nach einem OTA-Update: bis hierher gekommen, also gilt die neue Firmware als gut (sonst Rollback, #32)
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Neue Firmware läuft, als gültig markiert");
    }

    ESP_LOGI(TAG, "uprint gestartet");
}
