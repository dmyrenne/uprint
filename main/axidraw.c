/*
 * Testmodus für AxiDraw und NextDraw (EiBotBoard, EBB).
 *
 * Geplottet wird noch nicht. Ein Knopf in der Weboberfläche startet einen festen Ablauf: Firmware
 * abfragen, Stift bewegen, optional ein kleines Quadrat fahren, die PRG-Taste beobachten. Alles,
 * was gesendet und empfangen wird, landet in einem Log, das sich als Textdatei herunterladen lässt.
 * So lässt sich an einem echten Gerät klären, wie es sich verhält, bevor der eigentliche
 * Plotter-Modus entsteht (siehe docs/axidraw.md).
 *
 * Protokoll: Befehle enden mit CR. Wie die Antwort aussieht, hängt von der Firmware ab (ebb_syntax_t):
 * - FW 2.x (Legacy, am Gerät mit 2.8.1 geprüft): Befehle werden mit "OK" bestätigt, die meisten Abfragen
 *   liefern eine Datenzeile und dann "OK". V, QG und QM liefern nur die Datenzeile, ohne "OK".
 * - FW 3.x (Beta, nur nach Dokumentation): nach CU,10,1 genau eine Zeile "Befehl[,Daten]", kein "OK".
 * Fehler beginnen mit "!" (bei FW 3.x "Befehl,!…").
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "axidraw.h"
#include "settings.h"
#include "usb_serial.h"
#include "variant.h"

static const char *TAG = "axidraw";

#define LOG_SIZE          (16 * 1024)
#define LOG_RESERVE       64      // Platz für den Hinweis, dass das Log voll ist
#define REPLY_SILENCE_MS  250     // Firmware unbekannt: nach der letzten Zeile so lange auf weitere warten
#define REPLY_TIMEOUT_MS  2000
#define IDLE_TIMEOUT_MS   10000
#define STEPS_PER_MM      80      // 2032 Schritte/Zoll bei 1/16-Mikroschritt
#define TEST_STEPS        (10 * STEPS_PER_MM)
#define TEST_RATE         1600    // Schritte/s
#define LIVE_BUTTON_S     15
#define LATCH_WAIT_S      8

// QG-Statusbyte (ab FW 2.6.2). Bit 6/7 sind bei FW 2.x nur Pin-Zustände (RB2/RB5), erst ab FW 3
// bedeuten sie "Spannung war weg" und "Endschalter ausgelöst".
#define QG_LIMIT          0x80
#define QG_POWER_LOST     0x40
#define QG_BUTTON         0x20    // seit der letzten QG/QB-Abfrage gedrückt; beide teilen sich das Flag
#define QG_PEN_UP         0x10
#define QG_BUSY           0x0f    // Befehl läuft, Motor 1/2 bewegt sich, FIFO nicht leer
#define QG_STATE          (QG_BUTTON | QG_PEN_UP | QG_BUSY)

typedef enum {
    EBB_UNKNOWN,   // Firmware nicht erkannt: sammeln bis "OK", Fehler oder kurze Stille
    EBB_LEGACY,    // FW 2.x: "OK" bzw. Daten + "OK"; V, QG, QM ohne "OK"
    EBB_FUTURE,    // FW 3.x nach CU,10,1 (Beta): genau eine Zeile "Befehl[,Daten]"
} ebb_syntax_t;

static SemaphoreHandle_t s_lock;
static char s_log[LOG_SIZE];
static size_t s_log_len;
static bool s_log_full;
static char s_prompt[sizeof(((axidraw_test_status_t *)0)->prompt)];
static bool s_running;
static bool s_done;
static bool s_req_start;
static bool s_req_motion;
static volatile bool s_abort;

// Nur im Drucker-Task benutzt
static int64_t s_t0;
static char s_version[96];
static int s_fw[3];
static ebb_syntax_t s_syntax;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Schreibt ins Test-Log (nur während eines Tests) und immer auf die Konsole
__attribute__((format(printf, 1, 2)))
static void tlog(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", msg);

    LOCK();
    if (s_running && !s_log_full) {
        int64_t t = now_ms() - s_t0;
        char line[sizeof(msg) + 24];
        int n = snprintf(line, sizeof(line), "[%4" PRId64 ".%03" PRId64 "] %s\n", t / 1000, t % 1000, msg);
        if (n > (int)sizeof(line) - 1) {
            n = sizeof(line) - 1;
        }
        if (s_log_len + n <= LOG_SIZE - LOG_RESERVE) {
            memcpy(s_log + s_log_len, line, n);
            s_log_len += n;
        } else {
            s_log_len += snprintf(s_log + s_log_len, LOG_SIZE - s_log_len, "… Log voll, Rest abgeschnitten\n");
            s_log_full = true;
        }
    }
    UNLOCK();
}

__attribute__((format(printf, 1, 2)))
static void prompt(const char *fmt, ...)
{
    char msg[sizeof(s_prompt)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOCK();
    strlcpy(s_prompt, msg, sizeof(s_prompt));
    UNLOCK();
}

static void section(const char *title)
{
    tlog("---- %s ----", title);
}

static bool alive(void)
{
    return !s_abort && usb_serial_connected();
}

// Wartet und protokolliert dabei unaufgeforderte Zeilen. false bei Abbruch oder Verbindungsverlust.
static bool wait_ms(int ms)
{
    char line[160];
    int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (!alive()) {
            return false;
        }
        if (usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(50)) > 0) {
            tlog("< %s (unaufgefordert)", line);
        }
    }
    return alive();
}

// Legacy: Antworten ohne abschließendes "OK", nur eine Datenzeile (bei FW 2.8.1 so beobachtet)
static bool legacy_without_ok(const char *name)
{
    return strcmp(name, "V") == 0 || strcmp(name, "QG") == 0 || strcmp(name, "QM") == 0;
}

// "QS,12,-3" → "12,-3", wenn die Zeile mit dem Befehlsnamen beginnt; sonst NULL
static const char *strip_name(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(line, name, n) != 0) {
        return NULL;
    }
    return line[n] == ',' ? line + n + 1 : line[n] == '\0' ? line + n : NULL;
}

// Sendet einen Befehl und sammelt die Antwort im Format der erkannten Firmware (s_syntax).
// reply bekommt die Daten ohne "OK" und ohne vorangestellten Befehlsnamen ("QM,0,0,0,0" → "0,0,0,0"),
// bei einem Fehler die Fehlerzeile ab "!". Rückgabe: Zahl der Antwortzeilen, -1 ohne Verbindung.
// quiet: nur Auffälliges protokollieren (für häufige Statusabfragen).
static int ebb_cmd(const char *cmd, char *reply, size_t reply_len, bool quiet)
{
    char buf[80];
    char line[160];
    char name[8];
    int n = snprintf(buf, sizeof(buf), "%s\r", cmd);
    size_t nl = strcspn(cmd, ",");
    strlcpy(name, cmd, nl < sizeof(name) ? nl + 1 : sizeof(name));
    if (reply) {
        reply[0] = '\0';
    }
    if (!quiet) {
        tlog("> %s", cmd);
    }
    if (usb_serial_write(buf, n) != ESP_OK) {
        tlog("> %s: Senden fehlgeschlagen", cmd);
        return -1;
    }
    // V hat in keinem Format ein "OK", auch wenn die Firmware noch unbekannt ist
    bool one_line = strcmp(name, "V") == 0 || (s_syntax == EBB_LEGACY && legacy_without_ok(name));
    bool complete = false, error = false;
    int lines = 0;
    int64_t start = now_ms();
    int64_t last = start;
    for (;;) {
        int64_t t = now_ms();
        if (t - start > REPLY_TIMEOUT_MS) {
            break;
        }
        // Nach einem Fehler kommt bei Legacy evtl. noch ein "OK", ohne Firmware ist das Ende unklar
        if ((error || s_syntax == EBB_UNKNOWN) && lines > 0 && t - last > REPLY_SILENCE_MS) {
            complete = true;
            break;
        }
        if (usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(50)) <= 0) {
            continue;
        }
        last = now_ms();
        if (s_syntax == EBB_FUTURE) {
            const char *data = strip_name(line, name);
            if (!data) {
                tlog("< %s (unerwartet auf %s)", line, name);
                continue;
            }
            lines++;
            if (!quiet || data[0] == '!') {
                tlog("< %s", line);
            }
            if (reply) {
                strlcpy(reply, data, reply_len);
            }
            complete = true;
            break;
        }
        lines++;
        if (!quiet || line[0] == '!') {
            tlog("< %s", line);
        }
        if (strcmp(line, "OK") == 0) {
            complete = true;
            break;
        }
        if (reply && !reply[0]) {
            const char *data = s_syntax == EBB_LEGACY ? strip_name(line, name) : NULL;
            strlcpy(reply, data ? data : line, reply_len);
        }
        if (line[0] == '!') {
            error = true;
        } else if (one_line) {
            complete = true;
            break;
        }
    }
    if (lines == 0) {
        tlog("> %s: keine Antwort", cmd);
    } else if (!complete) {
        tlog("> %s: Antwort unvollständig (%d Zeile(n), Ende fehlt)", cmd, lines);
    }
    return lines;
}

static int ebb(const char *cmd, char *reply, size_t reply_len)
{
    return ebb_cmd(cmd, reply, reply_len, false);
}

__attribute__((format(printf, 1, 2)))
static int ebbf(const char *fmt, ...)
{
    char cmd[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return ebb(cmd, NULL, 0);
}

// "EBBv13_and_above EB Firmware Version 2.5.3" → {2, 5, 3}
static bool parse_version(const char *s, int fw[3])
{
    const char *p = strstr(s, "Version ");
    fw[0] = fw[1] = fw[2] = 0;
    return p && sscanf(p + 8, "%d.%d.%d", &fw[0], &fw[1], &fw[2]) >= 2;
}

static bool fw_at_least(int major, int minor, int patch)
{
    int want[3] = {major, minor, patch};
    for (int i = 0; i < 3; i++) {
        if (s_fw[i] != want[i]) {
            return s_fw[i] > want[i];
        }
    }
    return true;
}

static const char *syntax_name(void)
{
    switch (s_syntax) {
    case EBB_LEGACY: return "Legacy (FW 2.x, getestet)";
    case EBB_FUTURE: return "CU,10,1 (FW 3.x, Beta)";
    default:         return "unbekannt (Ende der Antwort per Zeitablauf)";
    }
}

// Fragt die Firmware ab und legt fest, welches Antwortformat erwartet wird. Bei FW 3.x wird auf
// CU,10,1 umgestellt; ob das greift, zeigt die Antwort auf QT (beginnt dann mit "QT").
static void ebb_detect(void)
{
    char r[96];
    s_syntax = EBB_UNKNOWN;
    s_version[0] = '\0';
    ebb("V", r, sizeof(r));
    strlcpy(s_version, r, sizeof(s_version));
    if (!parse_version(r, s_fw)) {
        tlog("  → Firmwareversion nicht erkannt, Antwortformat %s", syntax_name());
        return;
    }
    if (!fw_at_least(3, 0, 0)) {
        s_syntax = EBB_LEGACY;
    } else if (strncmp(r, "V,", 2) == 0) {
        s_syntax = EBB_FUTURE;   // Board steht noch von einer früheren Verbindung auf CU,10,1
    } else {
        ebb("CU,10,1", NULL, 0);
        ebb("QT", r, sizeof(r));
        s_syntax = strip_name(r, "QT") ? EBB_FUTURE : EBB_LEGACY;
    }
    tlog("  → Firmware %d.%d.%d, LM %s, Antwortformat %s", s_fw[0], s_fw[1], s_fw[2],
         fw_at_least(2, 7, 0) ? "vorhanden" : "fehlt (erst ab 2.7.0)", syntax_name());
    if (fw_at_least(3, 0, 0)) {
        tlog("  → FW 3.x ist in µprint noch Beta: bitte dieses Log an den Entwickler schicken");
    }
}

// QG-Antwort: Statusbyte in Hex ("3E"), der Befehlsname ist schon entfernt
static bool parse_qg(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 16);
    if (end == s || v < 0 || v > 0xff) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool query_qg(int *out, bool quiet)
{
    char r[32];
    return ebb_cmd("QG", r, sizeof(r), quiet) > 0 && parse_qg(r, out);
}

// Wartet, bis alle Bewegungen fertig sind (QG, bei alter Firmware QM)
static bool wait_idle(void)
{
    int64_t end = now_ms() + IDLE_TIMEOUT_MS;
    while (now_ms() < end) {
        if (!alive()) {
            return false;
        }
        int qg;
        char r[32];
        if (query_qg(&qg, true)) {
            if (!(qg & QG_BUSY)) {
                return true;
            }
        } else if (ebb_cmd("QM", r, sizeof(r), true) > 0 && r[0] != '!') {
            const char *qm = strip_name(r, "QM") ? strip_name(r, "QM") : r;
            if (strcmp(qm, "0,0,0,0") == 0 || strcmp(qm, "0,0,0") == 0) {
                return true;
            }
        } else {
            tlog("Status nicht abfragbar, warte pauschal 3 s");
            return wait_ms(3000);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    tlog("Bewegung nach %d s nicht beendet", IDLE_TIMEOUT_MS / 1000);
    return alive();
}

static void log_qg(void)
{
    int qg;
    if (query_qg(&qg, false)) {
        tlog("  → QG %02X: Stift %s, Taste %s, %s", qg, qg & QG_PEN_UP ? "oben" : "unten",
             qg & QG_BUTTON ? "gedrückt" : "nicht gedrückt", qg & QG_BUSY ? "beschäftigt" : "ruhig");
        if (fw_at_least(3, 0, 0) && (qg & (QG_LIMIT | QG_POWER_LOST))) {
            tlog("  → QG %02X:%s%s", qg, qg & QG_POWER_LOST ? " Spannung war weg" : "",
                 qg & QG_LIMIT ? " Endschalter ausgelöst" : "");
        }
    }
}

// Rate für LM: Schritte/s × 2^31 / 25000 (Akkumulator wird alle 40 µs addiert)
static int32_t lm_rate(int steps_per_s)
{
    return (int32_t)((int64_t)steps_per_s * 0x80000000LL / 25000);
}

// Beschleunigung für LM: Schritte/s² × 2^31 / 25000²
static int32_t lm_accel(int steps_per_s2)
{
    return (int32_t)((int64_t)steps_per_s2 * 0x80000000LL / 25000 / 25000);
}

static bool test_connection(void)
{
    section("1. Verbindung");
    prompt("Verbindung wird geprüft …");
    tlog("Warte 1 s auf unaufgeforderte Meldungen");
    return wait_ms(1000);
}

static bool test_status(void)
{
    section("2. Firmware und Status");
    prompt("Firmware und Status werden abgefragt …");
    ebb_detect();
    const char *queries[] = {"QT", "QC", "QE", "QG", "QM", "QS", "QB"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]) && alive(); i++) {
        ebb(queries[i], NULL, 0);
    }
    if (fw_at_least(3, 0, 0)) {
        ebb("QU,2", NULL, 0);   // maximale FIFO-Tiefe
    }
    return alive();
}

static bool test_pen(void)
{
    section("3. Stift");
    prompt("Bitte auf den Stift schauen: Er sollte hoch, runter und wieder hoch fahren.");
    ebb("SP,1,500", NULL, 0);
    if (!wait_ms(1000)) {
        return false;
    }
    log_qg();
    ebb("SP,0,500", NULL, 0);
    if (!wait_ms(1500)) {
        return false;
    }
    log_qg();
    ebb("SP,1,500", NULL, 0);
    if (!wait_ms(1500)) {
        return false;
    }
    log_qg();
    return alive();
}

static bool test_motion(void)
{
    section("4. Bewegung");
    prompt("Der Wagen fährt mit Stift oben ein 10-mm-Quadrat, dann 10 mm nach rechts und zurück (zweimal).");
    ebb("EM,1,1", NULL, 0);
    ebb("CS", NULL, 0);

    // XM: A/B sind die Achsen, das EBB rechnet in Motorschritte um (A+B, A−B)
    int ms = TEST_STEPS * 1000 / TEST_RATE;
    tlog("XM: Quadrat, je %d Schritte in %d ms", TEST_STEPS, ms);
    ebbf("XM,%d,%d,0", ms, TEST_STEPS);
    ebbf("XM,%d,0,%d", ms, TEST_STEPS);
    ebbf("XM,%d,%d,0", ms, -TEST_STEPS);
    ebbf("XM,%d,0,%d", ms, -TEST_STEPS);
    if (!wait_idle()) {
        return false;
    }
    ebb("QS", NULL, 0);
    tlog("  → erwartet: 0,0 (zurück am Start)");

    if (!fw_at_least(2, 7, 0)) {
        tlog("LM übersprungen, Firmware zu alt");
    } else {
        // LM zählt Motorschritte: nach rechts heißt beide Motoren gleich weit
        int32_t rate = lm_rate(TEST_RATE);
        tlog("LM: konstant %d Schritte/s, hin und zurück", TEST_RATE);
        ebbf("LM,%" PRId32 ",%d,0,%" PRId32 ",%d,0", rate, TEST_STEPS, rate, TEST_STEPS);
        ebbf("LM,%" PRId32 ",%d,0,%" PRId32 ",%d,0", rate, -TEST_STEPS, rate, -TEST_STEPS);
        if (!wait_idle()) {
            return false;
        }
        ebb("QS", NULL, 0);

        // Rampe: auf halber Strecke von 0 auf TEST_RATE beschleunigen, dann wieder auf 0
        int half = TEST_STEPS / 2;
        int32_t acc = lm_accel(TEST_RATE * TEST_RATE / (2 * half));
        tlog("LM: mit Rampe 0 → %d → 0 Schritte/s, hin und zurück (Accel %" PRId32 ")", TEST_RATE, acc);
        ebbf("LM,0,%d,%" PRId32 ",0,%d,%" PRId32, half, acc, half, acc);
        ebbf("LM,%" PRId32 ",%d,%" PRId32 ",%" PRId32 ",%d,%" PRId32, rate, half, -acc, rate, half, -acc);
        ebbf("LM,0,%d,%" PRId32 ",0,%d,%" PRId32, -half, acc, -half, acc);
        ebbf("LM,%" PRId32 ",%d,%" PRId32 ",%" PRId32 ",%d,%" PRId32, rate, -half, -acc, rate, -half, -acc);
        if (!wait_idle()) {
            return false;
        }
        ebb("QS", NULL, 0);
        tlog("  → erwartet: 0,0");
    }
    ebb("EM,0,0", NULL, 0);
    return alive();
}

static bool test_button_live(void)
{
    section("5. PRG-Taste, live");
    int last = -1, presses = 0, qg;
    query_qg(&qg, true);   // gespeicherten Druck verwerfen (QG und QB teilen sich das Flag)
    int64_t end = now_ms() + LIVE_BUTTON_S * 1000;
    tlog("QG alle 100 ms, nur Änderungen werden protokolliert");
    while (now_ms() < end) {
        if (!alive()) {
            return false;
        }
        prompt("Jetzt die PRG-Taste ein paar Mal drücken (noch %d s).", (int)((end - now_ms() + 999) / 1000));
        if (!query_qg(&qg, true)) {
            tlog("QG nicht verfügbar, frage stattdessen QB ab");
            char r[16];
            while (now_ms() < end && alive()) {
                if (ebb_cmd("QB", r, sizeof(r), true) > 0 && strcmp(r, "1") == 0) {
                    presses++;
                    tlog("  QB: Taste gedrückt");
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            break;
        }
        // Bit 6/7 ignorieren: bei FW 2.x Pin-Zustände, bei FW 3.x Merker, die QG selbst löscht
        if (last < 0 || (qg & QG_STATE) != (last & QG_STATE)) {
            tlog("  QG %02X (Taste %s)", qg, qg & QG_BUTTON ? "gedrückt" : "nicht gedrückt");
            if ((qg & QG_BUTTON) && (last < 0 || !(last & QG_BUTTON))) {
                presses++;
            }
            last = qg;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    tlog("  → %d Tastendruck/-drücke erkannt", presses);
    return alive();
}

static bool test_button_latch(void)
{
    section("6. PRG-Taste, gespeichert?");
    log_qg();
    int64_t end = now_ms() + LATCH_WAIT_S * 1000;
    tlog("Keine Abfragen für %d s", LATCH_WAIT_S);
    while (now_ms() < end) {
        prompt("Die PRG-Taste jetzt EINMAL kurz drücken und wieder loslassen. Nachgefragt wird in %d s.",
               (int)((end - now_ms() + 999) / 1000));
        if (!wait_ms(200)) {
            return false;
        }
    }
    prompt("Auswertung …");
    int qg;
    if (query_qg(&qg, false)) {
        tlog("  → QG merkt sich den Druck: %s", qg & QG_BUTTON ? "ja" : "nein");
    }
    log_qg();   // erwartet: Taste nicht gedrückt, das vorige QG hat den Merker gelöscht
    return alive();
}

static void run_test(bool motion)
{
    settings_t cfg;
    char info[128];
    settings_get(&cfg);
    usb_serial_info(info, sizeof(info));
    tlog("µprint %s (%s), AxiDraw-Test", esp_app_get_description()->version, uprint_variant());
    tlog("Gerätename: %s", cfg.device_name[0] ? cfg.device_name : "–");
    tlog("USB: %s", info);
    tlog("Bewegungstest: %s", motion ? "ja" : "nein");

    bool ok = test_connection() && test_status() && test_pen() && (!motion || test_motion()) &&
              test_button_live() && test_button_latch();

    section("Ende");
    if (ok) {
        tlog("Test vollständig durchgelaufen");
        prompt("Test fertig. Bitte die Beobachtungen ankreuzen und das Log herunterladen.");
    } else if (!usb_serial_connected()) {
        tlog("Abbruch: USB-Verbindung verloren");
        prompt("Abgebrochen: USB-Verbindung verloren.");
    } else {
        tlog("Abbruch durch Nutzer");
        s_abort = false;
        ebb("ES", NULL, 0);   // laufende Bewegung stoppen
        ebb("SP,1", NULL, 0);
        ebb("EM,0,0", NULL, 0);
        prompt("Abgebrochen. Das Log bis hierhin lässt sich herunterladen.");
    }
}

esp_err_t axidraw_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void axidraw_link(char *msg, size_t len)
{
    // Kurz warten, falls das Board nach dem Öffnen des Ports noch etwas meldet
    vTaskDelay(pdMS_TO_TICKS(300));
    usb_serial_flush_rx();
    ebb_detect();
    if (s_syntax == EBB_FUTURE || (s_syntax == EBB_LEGACY && fw_at_least(3, 0, 0))) {
        snprintf(msg, len, "AxiDraw verbunden (EBB-Firmware %d.%d.%d, Beta: bitte AxiDraw-Test laufen lassen "
                 "und das Log schicken)", s_fw[0], s_fw[1], s_fw[2]);
    } else if (s_syntax == EBB_LEGACY) {
        snprintf(msg, len, "AxiDraw verbunden (EBB-Firmware %d.%d.%d), nur Testmodus", s_fw[0], s_fw[1], s_fw[2]);
    } else if (s_version[0]) {
        snprintf(msg, len, "Gerät verbunden, antwortet unerwartet: %.60s", s_version);
    } else {
        snprintf(msg, len, "Gerät verbunden, antwortet nicht auf V");
    }
}

void axidraw_poll(void)
{
    LOCK();
    bool start = s_req_start;
    bool motion = s_req_motion;
    s_req_start = false;
    UNLOCK();

    if (start) {
        run_test(motion);
        LOCK();
        s_running = false;
        s_done = true;
        UNLOCK();
        return;
    }
    char line[160];
    if (usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(100)) > 0) {
        ESP_LOGI(TAG, "<< %s", line);
    }
}

esp_err_t axidraw_test_start(bool motion)
{
    settings_t cfg;
    settings_get(&cfg);
    esp_err_t err = ESP_OK;
    LOCK();
    if (s_running || cfg.device_type != DEVICE_AXIDRAW || !usb_serial_connected()) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_running = true;
        s_req_start = true;
        s_req_motion = motion;
        s_abort = false;
        s_log_len = 0;
        s_log_full = false;
        s_t0 = now_ms();
        strlcpy(s_prompt, "Test startet …", sizeof(s_prompt));
    }
    UNLOCK();
    return err;
}

void axidraw_test_abort(void)
{
    LOCK();
    if (s_running) {
        s_abort = true;
    }
    UNLOCK();
}

void axidraw_test_status(axidraw_test_status_t *out)
{
    LOCK();
    out->running = s_running;
    out->done = s_done;
    strlcpy(out->prompt, s_prompt, sizeof(out->prompt));
    out->log_len = s_log_len;
    UNLOCK();
}

size_t axidraw_test_log(char *out, size_t len, size_t offset)
{
    LOCK();
    size_t n = 0;
    if (offset < s_log_len) {
        n = s_log_len - offset < len ? s_log_len - offset : len;
        memcpy(out, s_log + offset, n);
    }
    UNLOCK();
    return n;
}
