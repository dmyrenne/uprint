#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Zurücksetzen auf den Werkszustand (#13). Dateien auf SD-Karte und im internen Speicher bleiben erhalten.
 *   settings: Einstellungen und LED-Darstellung; WLAN, API-Schlüssel und die Passwörter von Weboberfläche und
 *             Access Point bleiben, das Gerät bleibt erreichbar
 *   all:      zusätzlich WLAN (auch "nur Access Point"), das Passwort der Weboberfläche und ein neuer
 *             API-Schlüssel; danach offener Access Point mit Captive Portal zur Ersteinrichtung
 * Danach startet µprint kurz verzögert neu (die HTTP-Antwort soll noch ankommen).
 * ESP_ERR_INVALID_STATE während eines Drucks.
 */
esp_err_t reset_device(bool all);

// BOOT-Taste (GPIO 0) im laufenden Betrieb 10 s gedrückt halten = alles zurücksetzen. Nur, wenn GPIO 0 nicht als
// SD- oder LED-Pin eingestellt ist; beim Einschalten gedrückt startet der ESP32-S3 stattdessen den Bootloader.
void reset_button_start(void);
