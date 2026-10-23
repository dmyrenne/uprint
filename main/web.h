#pragma once

#include "esp_err.h"

/*
 * HTTP-Server auf Port 80:
 *   GET    /                        Weboberfläche
 *   GET    /api/status              Druckerstatus (JSON), mit finished/duration für den letzten fertigen Druck
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
 *   GET    /api/auth                {"password": gesetzt?, "session": angemeldet?, "setup": nach einem fragen?}
 *   POST   /api/auth                password=<…> anmelden, setzt das Sitzungs-Cookie
 *   DELETE /api/auth                abmelden (Cookie löschen)
 *   POST   /api/auth/password       password=<neu>&current=<bisher>&ap=1 festlegen bzw. ändern, meldet alle ab;
 *                                   remove=1&current=<bisher> entfernt es
 *   POST   /api/auth/skip           "Ohne Passwort fortfahren": nicht mehr nach einem Passwort fragen
 *   GET    /api/log                 Log als Text (diag.h): ?since=<pos> neue Zeilen, sonst die letzten ?tail=<bytes>;
 *                                   X-Log-Next = Position für die nächste Abfrage, X-Log-Serial = Mitschnitt 0–2
 *   DELETE /api/log                 Log leeren
 *   POST   /api/log/serial          ?level=0|1|2 Mitschnitt der Druckerkommunikation (diag_serial_t)
 *   GET    /api/diag                Diagnose zum Herunterladen (Textdatei mit Zustand, Einstellungen und Log)
 *
 * Ist ein Passwort gesetzt (auth.h), brauchen alle Aufrufe außer der Oberfläche, /api/auth, GET /api/status,
 * GET /api/files und GET /api/download ein gültiges Sitzungs-Cookie, sonst 401.
 */
esp_err_t web_start(void);
