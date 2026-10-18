#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Adressierbare Status-LEDs (SK6812, WS2812, WS2811; RGB oder RGBW). Pin, Anzahl, Typ und Farbreihenfolge
 * kommen aus den Einstellungen, die Darstellung je Zustand (Effekt, Farbe, Helligkeit) liegt im eigenen
 * NVS-Namespace "leds". Ein eigener Task zeichnet; Web-Handler setzen nur Anforderungen.
 *
 * Die Effekte bildet tools/mock_server.py für den virtuellen Strip nach, Änderungen dort mitziehen.
 */

// Zustände in absteigender Priorität; LED_STATE_ID ist die Kennung, die im Wechsel mit dem Zustand erscheint
typedef enum {
    LED_STATE_FIND,         // "Gerät finden"
    LED_STATE_OTA,          // Firmware-Update, Wert = Fortschritt
    LED_STATE_ERROR,
    LED_STATE_HEATING,      // Wert = aktuelle / Zieltemperatur
    LED_STATE_PRINTING,     // Wert = Fortschritt in der Datei
    LED_STATE_PAUSED,
    LED_STATE_FINISHED,
    LED_STATE_AP,           // Access Point zur Einrichtung offen
    LED_STATE_CONNECTING,   // WLAN oder Drucker verbindet
    LED_STATE_NO_DEVICE,    // kein Drucker/AxiDraw
    LED_STATE_READY,
    LED_STATE_ID,
    LED_STATE_COUNT,
} led_state_t;

typedef enum {
    LED_FX_OFF,
    LED_FX_SOLID,
    LED_FX_BREATHE,
    LED_FX_BLINK,
    LED_FX_CHASE,
    LED_FX_FIRST,       // nur die erste LED
    LED_FX_BAR,         // Fortschrittsbalken, Vorderkante atmet
    LED_FX_BAR_PULSE,   // Fortschrittsbalken, ganzer Balken pulsiert
    LED_FX_COUNT,
} led_effect_t;

typedef struct {
    uint8_t effect;   // led_effect_t
    uint8_t r, g, b;
    uint8_t bright;   // %
} led_look_t;

#define STATUS_LED_MAX 100

typedef struct {
    led_look_t look[LED_STATE_COUNT];   // bei LED_STATE_ID gilt die Farbe aus id_pattern, nicht r/g/b
    uint8_t id_pattern[STATUS_LED_MAX][3];   // Kennung: Farbe je LED
    uint8_t id_interval;    // s zwischen zwei Kennungen, 0 = aus
} led_config_t;

void status_led_init(void);

// Einstellungen (Pin, Anzahl, Typ …) geändert: Strip neu anlegen
void status_led_apply(void);

void status_led_get_config(led_config_t *out);
void status_led_default_config(led_config_t *out);
// Prüft und speichert; bei ungültigen Werten ESP_ERR_INVALID_ARG und eine Meldung in *error
esp_err_t status_led_set_config(const led_config_t *in, const char **error);
esp_err_t status_led_reset_config(void);

// Name für die API ("find", "ota", …) und ob ein Effekt für den Zustand erlaubt ist
const char *status_led_state_name(led_state_t state);
bool status_led_effect_allowed(led_state_t state, led_effect_t effect);

void status_led_find(void);                    // 10 s
// 5 s, Balken bei 60 %; look bzw. pattern != NULL zeigen eine noch nicht gespeicherte Darstellung
// (pattern nur für die Kennung, STATUS_LED_MAX Farben)
void status_led_preview(led_state_t state, const led_look_t *look, const uint8_t (*pattern)[3]);
void status_led_ota(int percent);              // -1 = Update beendet oder abgebrochen

// Geschätzter Strom des Strips in mA (nach der Strombegrenzung), 0 ohne Strip
int status_led_current_ma(void);
