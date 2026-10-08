#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// Startet den USB-Host und verbindet automatisch mit dem ersten seriellen Gerät
// (CDC-ACM, CH34x, CP210x, FTDI).
esp_err_t usb_serial_init(void);

bool usb_serial_connected(void);

// Wird bei jeder neuen Verbindung erhöht, damit Nutzer einen Reconnect erkennen.
uint32_t usb_serial_generation(void);

// Beschreibung des zuletzt verbundenen Geräts (VID/PID/Klasse, Treiber), für Diagnose
void usb_serial_info(char *out, size_t len);

esp_err_t usb_serial_write(const char *data, size_t len);

// Liest eine Zeile ohne Zeilenende. Rückgabe: Länge, oder -1 bei Timeout.
// Nur aus einem einzigen Task aufrufen.
int usb_serial_readline(char *out, size_t out_len, TickType_t timeout);

void usb_serial_flush_rx(void);

#ifdef __cplusplus
}
#endif
