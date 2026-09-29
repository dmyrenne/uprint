#pragma once

#include "esp_err.h"

/*
 * HTTP-Server auf Port 80:
 *   GET    /                        Weboberfläche
 *   GET    /api/status              Druckerstatus (JSON)
 *   GET    /api/files               Speicher und Dateien (JSON)
 *   POST   /api/upload?storage=<sd|flash>&name=<datei>  Rohdaten im Body
 *   DELETE /api/files?storage=<sd|flash>&name=<datei>
 *   GET    /api/download?storage=<sd|flash>&name=<datei>
 *   POST   /api/print?storage=<sd|flash>&name=<datei>
 *   POST   /api/pause | /api/resume | /api/cancel
 *   GET    /api/wifi                WLAN-Status
 *   GET    /api/wifi/scan           Netze in Reichweite
 *   POST   /api/wifi                ssid=<…>&password=<…> speichern und verbinden
 *   DELETE /api/wifi                Zugangsdaten löschen, Access Point öffnen
 *   GET    /api/settings            Einstellungen (JSON)
 *   POST   /api/settings            geänderte Felder urlencodiert, Antwort wie GET
 *   POST   /api/reboot              Neustart (nicht während eines Drucks)
 *   POST   /api/ota                 Firmware-Image im Body, danach Neustart
 */
esp_err_t web_start(void);
