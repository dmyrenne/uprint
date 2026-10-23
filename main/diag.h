#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Diagnose (#36): Ringpuffer mit allem, was über ESP_LOG ausgegeben wird (die Ausgabe auf UART bleibt), dazu
 * auf Wunsch die Kommunikation mit dem Drucker. Der Puffer liegt in einem .noinit-Bereich (mit PSRAM dort, 1 MB,
 * sonst 16 KB im internen RAM) und übersteht Neustarts durch Firmware, Absturz und Watchdog; nur beim
 * Einschalten beginnt er leer. Jeder Start schreibt eine Trennzeile mit dem Grund des Neustarts.
 *
 * Positionen zählen die seit dem Einschalten geschriebenen Bytes und laufen über Neustarts und Leeren weiter;
 * im Puffer stehen die letzten diag_log_size() davon.
 */

// Mitschnitt der Druckerkommunikation (NVS, sofort wirksam)
typedef enum {
    DIAG_SERIAL_OFF = 0,
    DIAG_SERIAL_RX = 1,    // Zeilen vom Drucker außer "ok" und den regelmäßigen Temperaturmeldungen
    DIAG_SERIAL_ALL = 2,   // alles in beide Richtungen
} diag_serial_t;

// So früh wie möglich in app_main, nach nvs_flash_init
void diag_init(void);

// Zeile nur in den Puffer schreiben (nicht auf UART), Format wie ESP_LOG mit Kennbuchstabe S:
// "S (<ms>) <tag>: <text>"
void diag_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Nach settings_init: trennt den Empfang der Konsole (UART0 RX) ab; ohne Konsole geht das Log auch nicht mehr
// auf UART0 TX, beide Pins sind dann frei (settings_t.console). Der Bootloader schreibt beim Start trotzdem kurz
// auf TX.
void diag_console(bool on);

diag_serial_t diag_serial(void);
void diag_set_serial(diag_serial_t level);

// Kopiert ab *pos höchstens len Bytes nach out und rückt *pos vor. Liegt *pos vor dem ältesten noch
// vorhandenen Byte (überschrieben) oder hinter dem Ende (Log geleert), beginnt es beim ältesten vollständigen
// Zeilenanfang. Rückgabe: Anzahl Bytes, 0 am Ende.
size_t diag_log_read(uint64_t *pos, char *out, size_t len);
// Position hinter dem letzten Byte und des ältesten noch vorhandenen
uint64_t diag_log_end(void);
uint64_t diag_log_start(void);
void diag_log_clear(void);

size_t diag_log_size(void);
uint32_t diag_boot_count(void);   // Starts seit dem Einschalten
bool diag_log_kept(void);         // Log vom vorigen Lauf übernommen
const char *diag_reset_reason(void);
