#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Alle Einstellungen von uprint; sie liegen im NVS und werden über die Weboberfläche geändert.
typedef enum {
    DEVICE_PRINTER = 0,   // 3D-Drucker mit Marlin/Prusa-Firmware
    DEVICE_AXIDRAW = 1,   // AxiDraw/NextDraw (EiBotBoard)
} device_type_t;

typedef enum {
    LED_TYPE_WS2812 = 0,
    LED_TYPE_SK6812 = 1,
    LED_TYPE_WS2811 = 2,
} led_type_t;

typedef enum {
    LED_ORDER_RGB, LED_ORDER_RBG, LED_ORDER_GRB, LED_ORDER_GBR, LED_ORDER_BRG, LED_ORDER_BGR,
} led_order_t;

typedef struct {
    // Anzeigename des Geräts, leer = "µprint" (sofort wirksam); der Hostname wird daraus gebildet
    char device_name[33];
    // Netzwerk: Hostname sofort (mDNS, DHCP; der Access Point heißt beim nächsten Öffnen so),
    // Passwort des Access Points nach Neustart
    char hostname[33];
    char ap_password[64];
    // Angeschlossenes Gerät, device_type_t (sofort wirksam, verbindet neu)
    int device_type;
    // Drucker (wirksam ab der nächsten USB-Verbindung)
    int baud;
    // Pausieren / Abbrechen, in mm (sofort wirksam)
    int pause_lift;
    int cancel_lift;
    int park_x;
    int park_y;
    // Plotter (AxiDraw), wirksam ab dem nächsten Plot
    int plot_draw;        // mm/s, Höchstgeschwindigkeit mit Stift unten
    int plot_travel;      // mm/s, mit Stift oben
    int plot_accel;       // mm/s²
    int pen_servo;        // plot_servo_t: 0 Standard (AxiDraw), 1 bürstenlos (NextDraw, Upgrade-Kit)
    int pen_up;           // Servoposition 0–100 %
    int pen_down;
    int pen_delay;        // ms Wartezeit nach jedem Heben/Senken
    // Zeichenfläche des Plotters in mm (sofort wirksam): geht an µplot und begrenzt die Warnung bei zu großen Dateien
    int plot_w;
    int plot_h;
    // microSD über SPI (wirksam nach Neustart); ohne sd_enable werden die Pins weder geprüft noch belegt
    int sd_enable;
    int sd_mosi;
    int sd_miso;
    int sd_sclk;
    int sd_cs;
    // Log-Ausgabe über UART0 (XIAO: D6 TX, D7 RX; sonst GPIO 43/44), wirksam nach Neustart. Aus: beide Pins frei
    // für SD und LEDs, das Log gibt es dann nur in der Weboberfläche (diag.h)
    int console;
    // Status-LEDs (adressierbar, z. B. SK6812), sofort wirksam
    int led_pin;          // -1 = aus
    int led_count;        // 0 = aus
    int led_brightness;   // Gesamthelligkeit in %, zusätzlich zur Helligkeit je Zustand (status_led.c)
    int led_type;         // led_type_t
    int led_order;        // led_order_t: Reihenfolge der Farbkanäle
    int led_white;        // 1 = RGBW (Weißkanal nach den Farben)
    int led_max_ma;       // Strombegrenzung für den ganzen Strip in mA, 0 = keine (wie in WLED)
    int led_ma;           // Strom einer LED bei vollem Weiß in mA, für die Schätzung
    // Schlüssel für Slicer (PrusaLink/OctoPrint-API), wird beim ersten Start erzeugt
    char api_key[33];
    // Adresse von µplot (SVG → G-Code), leer = keins; der Browser ruft es direkt auf, nicht µprint (sofort wirksam)
    char uplot_url[96];
} settings_t;

// Alle ganzzahligen Felder; der Name ist zugleich NVS-Schlüssel und Feldname in der Web-API
typedef struct {
    const char *key;
    size_t offset;
} settings_int_field_t;

extern const settings_int_field_t SETTINGS_INT_FIELDS[];
extern const size_t SETTINGS_INT_FIELD_COUNT;
#define SETTINGS_INT(s, f) (*(int *)((char *)(s) + (f)->offset))

void settings_init(void);
void settings_get(settings_t *out);

// Prüft und speichert. Bei ungültigen Werten ESP_ERR_INVALID_ARG und eine Meldung in *error.
esp_err_t settings_update(const settings_t *in, const char **error);

// true, wenn gespeicherte Werte erst nach einem Neustart gelten
bool settings_reboot_required(void);

// Erzeugt einen neuen API-Schlüssel und speichert ihn
esp_err_t settings_new_api_key(void);

// Hostname ab Werk, z. B. "uprint-3a5c" (aus der MAC-Adresse)
const char *settings_default_hostname(void);

// Hostname aus dem Gerätenamen: klein, Umlaute umschrieben, alles andere als Bindestrich, höchstens 32 Zeichen,
// z. B. "MK3S Werkstatt" -> "mk3s-werkstatt"; ohne verwertbare Zeichen der Hostname ab Werk.
// hostFromName() in index.html macht dasselbe für die Vorschau.
void settings_hostname_from_name(const char *name, char *out, size_t len);

// Löscht alle gespeicherten Einstellungen (wirksam nach dem Neustart); all = false behält API-Schlüssel und
// Access-Point-Passwort
esp_err_t settings_reset(bool all);

#ifdef __cplusplus
}
#endif
