#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Alle Einstellungen von uprint; sie liegen im NVS und werden über die Weboberfläche geändert.
typedef enum {
    DEVICE_PRINTER = 0,   // 3D-Drucker mit Marlin/Prusa-Firmware
    DEVICE_AXIDRAW = 1,   // AxiDraw/NextDraw (EiBotBoard)
} device_type_t;

typedef struct {
    // Anzeigename des Geräts, leer = "µprint" (sofort wirksam)
    char device_name[33];
    // Netzwerk (wirksam nach Neustart)
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
    // microSD über SPI (wirksam nach Neustart)
    int sd_mosi;
    int sd_miso;
    int sd_sclk;
    int sd_cs;
    // Schlüssel für Slicer (PrusaLink/OctoPrint-API), wird beim ersten Start erzeugt
    char api_key[33];
} settings_t;

void settings_init(void);
void settings_get(settings_t *out);

// Prüft und speichert. Bei ungültigen Werten ESP_ERR_INVALID_ARG und eine Meldung in *error.
esp_err_t settings_update(const settings_t *in, const char **error);

// true, wenn gespeicherte Werte erst nach einem Neustart gelten
bool settings_reboot_required(void);

// Erzeugt einen neuen API-Schlüssel und speichert ihn
esp_err_t settings_new_api_key(void);

#ifdef __cplusplus
}
#endif
