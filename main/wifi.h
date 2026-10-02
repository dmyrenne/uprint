#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * WLAN mit Zugangsdaten aus dem NVS. Ohne gespeicherte Daten, oder wenn die Verbindung
 * länger als 30 s fehlt, öffnet uprint zusätzlich einen eigenen Access Point
 * (SSID = Hostname, http://192.168.4.1), über den sich das WLAN einrichten lässt.
 */
esp_err_t wifi_init(void);

// Gerätename aus den Einstellungen im mDNS übernehmen (sichtbar z. B. bei der Druckersuche im Slicer)
void wifi_apply_device_name(void);

typedef struct {
    bool configured;     // Zugangsdaten gespeichert
    bool connected;      // mit dem WLAN verbunden und IP erhalten
    bool ap_active;      // eigener Access Point offen
    bool ap_only;        // ohne WLAN fortgefahren: Access Point bleibt an, kein Captive Portal
    int ap_off_in;       // Sekunden, bis sich der Access Point schließt, -1 = nicht geplant
    char ssid[33];
    char hostname[33];   // aktiver Hostname (= SSID des Access Points)
    char ip[16];
    int rssi;            // dBm, 0 wenn nicht verbunden
    int last_reason;     // letzter Trennungsgrund (wifi_err_reason_t), 0 = keiner
} wifi_status_t;

void wifi_get_status(wifi_status_t *out);

// Speichert die Zugangsdaten und verbindet neu (kurz verzögert, damit die HTTP-Antwort noch rausgeht).
esp_err_t wifi_set_credentials(const char *ssid, const char *password);

// Löscht die Zugangsdaten und öffnet den Access Point.
esp_err_t wifi_forget(void);

// "Ohne WLAN fortfahren": Access Point bleibt dauerhaft an (auch nach einem Neustart), das Captive Portal
// ist aus. Neue Zugangsdaten oder wifi_forget() heben das wieder auf.
esp_err_t wifi_set_ap_only(void);

// Access Point offen und nicht im Modus "nur Access Point": Geräte sollen auf die Einrichtung geleitet werden
bool wifi_portal_active(void);

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} wifi_network_t;

// Blockiert einige Sekunden. Rückgabe: Anzahl gefundener Netze (ohne Duplikate) oder -1.
int wifi_scan(wifi_network_t *out, int max);
