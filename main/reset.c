#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "printer.h"
#include "reset.h"
#include "settings.h"
#include "status_led.h"
#include "wifi.h"

static const char *TAG = "reset";

#define BUTTON_GPIO      0
#define BUTTON_HOLD_MS   10000
#define BUTTON_WARN_MS   3000     // ab hier blinken die LEDs als Warnung ("Gerät finden")
#define RESTART_DELAY_US (800 * 1000)

static void restart_cb(void *arg)
{
    esp_restart();
}

esp_err_t reset_device(bool all)
{
    if (printer_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = settings_reset(all);
    status_led_reset_config();
    if (all && err == ESP_OK) {
        err = wifi_erase_stored();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zurücksetzen fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "%s zurückgesetzt, starte neu", all ? "Alles" : "Einstellungen");
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = {.callback = restart_cb, .name = "reset"};
        esp_timer_create(&args, &timer);
    }
    esp_timer_start_once(timer, RESTART_DELAY_US);
    return ESP_OK;
}

static void button_task(void *arg)
{
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(BUTTON_GPIO) != 0 || printer_busy()) {
            held_ms = 0;
            continue;
        }
        held_ms += 100;
        if (held_ms == BUTTON_WARN_MS) {
            ESP_LOGW(TAG, "BOOT-Taste gedrückt, nach %d s wird alles zurückgesetzt", BUTTON_HOLD_MS / 1000);
            status_led_find();
        }
        if (held_ms >= BUTTON_HOLD_MS) {
            reset_device(true);
            vTaskDelete(NULL);
        }
    }
}

void reset_button_start(void)
{
    settings_t cfg;
    settings_get(&cfg);
    bool used = cfg.led_pin == BUTTON_GPIO ||
                (cfg.sd_enable && (cfg.sd_mosi == BUTTON_GPIO || cfg.sd_miso == BUTTON_GPIO ||
                                   cfg.sd_sclk == BUTTON_GPIO || cfg.sd_cs == BUTTON_GPIO));
    if (used) {
        ESP_LOGI(TAG, "GPIO %d ist belegt, Zurücksetzen über die BOOT-Taste ist aus", BUTTON_GPIO);
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(button_task, "reset_btn", 2560, NULL, 1, NULL);
}
