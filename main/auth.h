#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Passwort für die Weboberfläche (#31). Gespeichert wird nur ein Hash (PBKDF2-HMAC-SHA256 mit Salt) im
 * NVS-Namensraum "auth". Eine Anmeldung ergibt ein Sitzungs-Token für ein Cookie: Zufallswert plus HMAC mit
 * einem Geheimnis des Geräts. Es übersteht Neustarts und wird ungültig, sobald das Passwort neu gesetzt wird.
 * Das Passwort ist freiwillig: Ohne ist alles offen. Die Weboberfläche fragt nach der WLAN-Einrichtung einmal
 * danach; "Ohne Passwort fortfahren" merkt sich das Gerät (auth_skip), bis alles zurückgesetzt wird.
 */

#define AUTH_PASSWORD_MIN 8
#define AUTH_PASSWORD_MAX 63
#define AUTH_TOKEN_LEN    64   // Hex-Zeichen ohne Nullbyte

void auth_init(void);

bool auth_password_set(void);

// Noch kein Passwort und die Frage danach nicht übersprungen: die Weboberfläche soll fragen
bool auth_setup_pending(void);

// "Ohne Passwort fortfahren": nicht mehr nach einem Passwort fragen
esp_err_t auth_skip(void);

// Entfernt das Passwort (danach alles offen, ohne erneute Frage)
esp_err_t auth_remove(void);

// Setzt ein neues Passwort und meldet damit alle Sitzungen ab. ESP_ERR_INVALID_ARG bei falscher Länge.
esp_err_t auth_set_password(const char *password);

typedef enum {
    AUTH_OK,
    AUTH_WRONG,
    AUTH_LOCKED,   // zu viele Fehlversuche, *wait_s nennt die Sekunden bis zum nächsten Versuch
} auth_result_t;

// Prüft das Passwort, mit Sperre nach mehreren Fehlversuchen (für alle Clients gemeinsam)
auth_result_t auth_check_password(const char *password, int *wait_s);

// Neues Sitzungs-Token, out mindestens AUTH_TOKEN_LEN + 1 Bytes
void auth_new_token(char *out);

bool auth_token_valid(const char *token);

// Löscht Passwort, Geheimnis und das Überspringen (Werksreset "alles", wirksam sofort)
esp_err_t auth_erase(void);
