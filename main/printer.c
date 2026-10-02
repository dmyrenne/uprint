/*
 * G-Code-Sender für Marlin (und Prusa-Firmware).
 *
 * Protokoll: Jede Zeile wird als "N<n> <befehl>*<xor-prüfsumme>" gesendet, danach
 * wird auf "ok" gewartet (immer nur eine Zeile unterwegs). Fordert der Drucker mit
 * "Resend: <n>" eine Zeile erneut an, wird ab <n> aus dem Verlauf wiederholt; das
 * darauf folgende "ok" gibt das Senden wieder frei.
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "printer.h"
#include "settings.h"
#include "storage.h"
#include "usb_serial.h"

static const char *TAG = "printer";

// Prusa/Marlin: MAX_CMD_SIZE 96 inkl. "N12345 " und "*123"
#define CMD_LIMIT        80
#define CMD_BUF          (CMD_LIMIT + 1)
#define HISTORY_LEN      32
#define INJECT_LEN       16
#define SHUTDOWN_MAX     10
#define SHUTDOWN_TAIL    4096   // so weit vor Dateiende wird die Endsequenz gesucht
#define LINE_BUF         192
#define START_WAIT_US    (5 * 1000000LL)
#define SYNC_WAIT_US     (2 * 1000000LL)
#define SYNC_ATTEMPTS    5
#define ACK_TIMEOUT_US   (30 * 1000000LL)

typedef struct {
    uint32_t n;
    char cmd[CMD_BUF];
} history_t;

// Beim Abbruch immer zuerst: Heizungen und Lüfter aus
static const char *const CANCEL_PREFIX[] = {"M104 S0", "M140 S0", "M107"};

// µplot schreibt ans Dateiende "; -- shutdown", gefolgt von Sicherheitshöhe, Parkposition, Motoren aus.
// Genau diese Sequenz wird beim Abbruch ausgeführt, damit die Werte aus dem µplot-Profil gelten.
#define SHUTDOWN_MARKER  "; -- shutdown"

// Mit Web-Handlern geteilt, geschützt durch s_lock
static SemaphoreHandle_t s_lock;
static printer_status_t s_st;
static bool s_req_start;
static bool s_req_cancel;
static bool s_req_pause;
static bool s_req_resume;

// Nur im Drucker-Task benutzt
static FILE *s_file;
static storage_vol_t s_job_vol;   // Speicher der offenen Datei, über storage_acquire() gehalten
static history_t s_hist[HISTORY_LEN];
static uint32_t s_last_n;      // höchste bisher vergebene Zeilennummer
static uint32_t s_next_n;      // als nächstes zu sendende Nummer (<= s_last_n: Wiederholung)
static bool s_ready;           // letzte Zeile bestätigt, nächste darf raus
static bool s_need_sync;
static int64_t s_last_io_us;
static char s_inject[INJECT_LEN][CMD_BUF];
static int s_inject_head;
static int s_inject_count;
static char s_shutdown[SHUTDOWN_MAX][CMD_BUF];   // Endsequenz aus der Datei, falls vorhanden
static int s_shutdown_count;

// Aus dem gesendeten G-Code mitgeführt, damit Pause und Abbruch absolut anheben können.
// (G91/G90 zum relativen Anheben würde bei Marlin auch einen relativen Extruder, M83, zurücksetzen.)
static bool s_abs_xyz;         // G90 aktiv
static bool s_z_known;
static float s_z;
static float s_feed;           // zuletzt gesetzter Vorschub (mm/min), 0 = unbekannt
static bool s_lifted;          // beim Pausieren angehoben
static float s_pause_z;        // Z vor dem Anheben

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

__attribute__((format(printf, 2, 3)))
static void set_state(printer_state_t state, const char *fmt, ...)
{
    char msg[sizeof(s_st.message)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    LOCK();
    s_st.state = state;
    strlcpy(s_st.message, msg, sizeof(s_st.message));
    UNLOCK();
    ESP_LOGI(TAG, "[%s] %s", printer_state_name(state), msg);
}

static printer_state_t get_state(void)
{
    LOCK();
    printer_state_t state = s_st.state;
    UNLOCK();
    return state;
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool contains_ci(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void close_job(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
        storage_release(s_job_vol);
    }
}

static void inject(const char *cmd)
{
    if (s_inject_count == INJECT_LEN) {
        ESP_LOGW(TAG, "Befehlspuffer voll, verwerfe \"%s\"", cmd);
        return;
    }
    strlcpy(s_inject[(s_inject_head + s_inject_count) % INJECT_LEN], cmd, CMD_BUF);
    s_inject_count++;
}

static void inject_pop(char *out)
{
    strlcpy(out, s_inject[s_inject_head], CMD_BUF);
    s_inject_head = (s_inject_head + 1) % INJECT_LEN;
    s_inject_count--;
}

__attribute__((format(printf, 1, 2)))
static void fail(const char *fmt, ...)
{
    char msg[sizeof(s_st.message)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    close_job();
    s_inject_count = 0;
    LOCK();
    s_st.finished = false;
    UNLOCK();
    set_state(PRINTER_ERROR, "%s", msg);
}

static void finish_job(void)
{
    close_job();
    LOCK();
    s_st.finished = true;
    s_st.duration_s = (uint32_t)((now_us() - s_st.started_us) / 1000000LL);
    s_st.file_pos = s_st.file_size;
    uint32_t minutes = s_st.duration_s / 60;
    char name[STORAGE_NAME_MAX];
    strlcpy(name, s_st.file, sizeof(name));
    UNLOCK();
    set_state(PRINTER_IDLE, "Druck fertig: %s (%" PRIu32 " min)", name, minutes);
}

static void transmit(uint32_t n, const char *cmd)
{
    char body[CMD_BUF + 16];
    char out[CMD_BUF + 32];
    snprintf(body, sizeof(body), "N%" PRIu32 " %s", n, cmd);
    uint8_t cs = 0;
    for (const char *p = body; *p; p++) {
        cs ^= (uint8_t)*p;
    }
    int len = snprintf(out, sizeof(out), "%s*%u\n", body, cs);

    ESP_LOGD(TAG, ">> %s", body);
    s_ready = false;
    s_last_io_us = now_us();
    if (usb_serial_write(out, len) != ESP_OK) {
        ESP_LOGW(TAG, "Senden von Zeile %" PRIu32 " fehlgeschlagen", n);
    }
}

// Entfernt Kommentar und Leerraum; liefert einen Zeiger in raw (leer = nichts zu senden)
static char *strip_gcode(char *raw)
{
    char *comment = strchr(raw, ';');
    if (comment) {
        *comment = '\0';
    }
    char *p = raw;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    char *end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return p;
}

// Sucht die µplot-Endsequenz am Dateiende; ohne Treffer bleibt s_shutdown leer
static void load_shutdown(const char *path)
{
    s_shutdown_count = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        fseek(f, size > SHUTDOWN_TAIL ? size - SHUTDOWN_TAIL : 0, SEEK_SET);
    }
    char raw[LINE_BUF];
    bool found = false;
    while (fgets(raw, sizeof(raw), f)) {
        if (strncmp(raw, SHUTDOWN_MARKER, strlen(SHUTDOWN_MARKER)) == 0) {
            found = true;          // bei mehreren Markern gilt der letzte
            s_shutdown_count = 0;
            continue;
        }
        if (!found) {
            continue;
        }
        char *cmd = strip_gcode(raw);
        // M2/M30 (Programmende) kennt Marlin nicht, sie sind hier ohnehin überflüssig
        if (*cmd == '\0' || strlen(cmd) > CMD_LIMIT || strcmp(cmd, "M2") == 0 || strcmp(cmd, "M30") == 0) {
            continue;
        }
        if (s_shutdown_count < SHUTDOWN_MAX) {
            strlcpy(s_shutdown[s_shutdown_count++], cmd, CMD_BUF);
        }
    }
    fclose(f);
    if (s_shutdown_count > 0) {
        ESP_LOGI(TAG, "µplot-Endsequenz gefunden (%d Befehle), wird beim Abbruch verwendet", s_shutdown_count);
    }
}

static bool gcode_has(const char *cmd, char letter)
{
    for (const char *p = cmd + 1; *p; p++) {
        if (toupper((unsigned char)*p) == letter) {
            return true;
        }
    }
    return false;
}

static bool gcode_param(const char *cmd, char letter, float *out)
{
    for (const char *p = cmd + 1; *p; p++) {
        if (toupper((unsigned char)*p) == letter) {
            char *end;
            float v = strtof(p + 1, &end);
            if (end != p + 1) {
                *out = v;
                return true;
            }
        }
    }
    return false;
}

// Verfolgt Z, Vorschub und Positionierungsart der Befehle aus der Datei
static void track_motion(const char *cmd)
{
    if (toupper((unsigned char)cmd[0]) != 'G') {
        return;
    }
    char *end;
    long g = strtol(cmd + 1, &end, 10);
    if (end == cmd + 1) {
        return;
    }
    float v;
    switch (g) {
    case 0:
    case 1:
        if (gcode_param(cmd, 'Z', &v)) {
            if (s_abs_xyz) {
                s_z = v;
                s_z_known = true;
            } else if (s_z_known) {
                s_z += v;
            }
        }
        if (gcode_param(cmd, 'F', &v) && v > 0) {
            s_feed = v;
        }
        break;
    case 28:
        // Referenzieren ohne Achsangabe (oder mit Z) setzt Z auf einen unbekannten Wert
        if (gcode_has(cmd, 'Z') || !(gcode_has(cmd, 'X') || gcode_has(cmd, 'Y'))) {
            s_z_known = false;
        }
        break;
    case 90:
        s_abs_xyz = true;
        break;
    case 91:
        s_abs_xyz = false;
        break;
    case 92:
        if (gcode_param(cmd, 'Z', &v)) {
            s_z = v;
            s_z_known = true;
        }
        break;
    }
}

static void reset_motion(void)
{
    s_abs_xyz = true;
    s_z_known = false;
    s_z = 0;
    s_feed = 0;
    s_lifted = false;
}

static void set_pause_message(const char *msg)
{
    LOCK();
    if (s_st.state == PRINTER_PAUSED) {
        strlcpy(s_st.message, msg, sizeof(s_st.message));
    }
    UNLOCK();
}

static void pause_lift(void)
{
    char cmd[CMD_BUF];
    settings_t cfg;
    settings_get(&cfg);
    if (cfg.pause_lift <= 0 || s_lifted) {
        return;
    }
    if (!s_z_known || !s_abs_xyz) {
        ESP_LOGW(TAG, "Z-Position unbekannt, pausiere ohne Anheben");
        set_pause_message("Pausiert (Z-Position unbekannt, nicht angehoben)");
        return;
    }
    s_pause_z = s_z;
    s_lifted = true;
    snprintf(cmd, sizeof(cmd), "G1 Z%.2f F600", s_z + cfg.pause_lift);
    inject(cmd);
    snprintf(cmd, sizeof(cmd), "Pausiert, Düse um %d mm angehoben", cfg.pause_lift);
    set_pause_message(cmd);
}

static void resume_lower(void)
{
    char cmd[CMD_BUF];
    if (!s_lifted) {
        return;
    }
    s_lifted = false;
    snprintf(cmd, sizeof(cmd), "G1 Z%.2f F600", s_pause_z);
    inject(cmd);
    if (s_feed > 0) {
        // Der Vorschub des Absenkens würde sonst für die folgenden Bewegungen gelten
        snprintf(cmd, sizeof(cmd), "G1 F%.0f", s_feed);
        inject(cmd);
    }
}

static void inject_cancel_sequence(void)
{
    char cmd[CMD_BUF];
    for (size_t i = 0; i < sizeof(CANCEL_PREFIX) / sizeof(CANCEL_PREFIX[0]); i++) {
        inject(CANCEL_PREFIX[i]);
    }
    if (s_shutdown_count > 0) {
        inject("G90");   // die Endsequenz aus µplot arbeitet mit absoluten Koordinaten
        for (int i = 0; i < s_shutdown_count; i++) {
            inject(s_shutdown[i]);
        }
        return;
    }
    // Allgemein: erst anheben (über den bisherigen Druck hinweg), dann parken
    settings_t cfg;
    settings_get(&cfg);
    if (s_z_known && s_abs_xyz) {
        float z = s_z + cfg.cancel_lift;
        if (s_lifted && s_pause_z + cfg.pause_lift > z) {
            z = s_pause_z + cfg.pause_lift;   // nicht wieder absenken, wenn die Pause höher angehoben hat
        }
        snprintf(cmd, sizeof(cmd), "G1 Z%.2f F600", z);
        inject(cmd);
    } else {
        inject("G91");
        snprintf(cmd, sizeof(cmd), "G1 Z%d F600", cfg.cancel_lift);
        inject(cmd);
        inject("G90");
    }
    snprintf(cmd, sizeof(cmd), "G1 X%d Y%d F3000", cfg.park_x, cfg.park_y);
    inject(cmd);
    inject("M84");
}

// Die Datei lässt sich nicht mehr lesen (z. B. SD-Karte entfernt), der Drucker selbst ist aber in Ordnung:
// wie beim Abbrechen Heizungen aus, anheben und parken, dann den Fehler anzeigen. Anders als bei fail()
// gehen die Befehle trotz Fehlerzustand noch raus.
static void abort_job(void)
{
    const char *msg = s_job_vol == STORAGE_SD ? "SD-Karte entfernt oder nicht lesbar, Druck abgebrochen"
                                              : "Lesefehler im internen Speicher, Druck abgebrochen";
    close_job();
    s_inject_count = 0;
    inject_cancel_sequence();
    LOCK();
    s_st.finished = false;
    UNLOCK();
    set_state(PRINTER_ERROR, "%s", msg);
}

// 1 = Zeile gelesen, 0 = Dateiende, -1 = Zeile zu lang, -2 = Lesefehler
static int read_file_line(char *out)
{
    char raw[LINE_BUF];
    while (fgets(raw, sizeof(raw), s_file)) {
        size_t len = strlen(raw);
        bool truncated = len == sizeof(raw) - 1 && raw[len - 1] != '\n';
        bool has_comment = strchr(raw, ';') != NULL;
        if (truncated) {
            // Rest der Zeile verwerfen; unkritisch, wenn es nur ein langer Kommentar war
            int c;
            while ((c = fgetc(s_file)) != EOF && c != '\n') {
            }
            if (!has_comment) {
                return -1;
            }
        }

        char *p = strip_gcode(raw);
        if (*p == '\0') {
            continue;
        }
        if (strlen(p) > CMD_LIMIT) {
            return -1;
        }

        strlcpy(out, p, CMD_BUF);
        long pos = ftell(s_file);
        LOCK();
        s_st.file_pos = pos > 0 ? (uint32_t)pos : 0;
        UNLOCK();
        return 1;
    }
    return ferror(s_file) ? -2 : 0;
}

static void send_next(void)
{
    if (s_next_n <= s_last_n) {
        const history_t *h = &s_hist[s_next_n % HISTORY_LEN];
        if (h->n != s_next_n) {
            fail("Zeile %" PRIu32 " kann nicht erneut gesendet werden", s_next_n);
            s_need_sync = true;
            return;
        }
        s_next_n++;
        transmit(h->n, h->cmd);
        return;
    }

    char cmd[CMD_BUF];
    if (s_inject_count > 0) {
        inject_pop(cmd);
    } else if (s_file && get_state() == PRINTER_PRINTING) {
        int r = read_file_line(cmd);
        if (r == 0) {
            finish_job();
            return;
        }
        if (r == -1) {
            fail("G-Code-Zeile zu lang (max. %d Zeichen)", CMD_LIMIT);
            return;
        }
        if (r < 0) {
            abort_job();
            return;
        }
        track_motion(cmd);
    } else {
        return;
    }

    uint32_t n = ++s_last_n;
    history_t *h = &s_hist[n % HISTORY_LEN];
    h->n = n;
    strlcpy(h->cmd, cmd, sizeof(h->cmd));
    s_next_n = n + 1;
    transmit(n, cmd);
}

static void parse_temp(const char *line, const char *key, float *value, float *target)
{
    const char *p = strstr(line, key);
    if (!p) {
        return;
    }
    p += strlen(key);
    char *end;
    float v = strtof(p, &end);
    if (end == p) {
        return;
    }
    *value = v;
    while (*end == ' ') {
        end++;
    }
    if (*end == '/') {
        *target = strtof(end + 1, NULL);
    }
}

static void parse_temps(const char *line)
{
    if (!strstr(line, "T:") && !strstr(line, "B:")) {
        return;
    }
    LOCK();
    parse_temp(line, "T:", &s_st.hotend, &s_st.hotend_target);
    parse_temp(line, "B:", &s_st.bed, &s_st.bed_target);
    UNLOCK();
}

static void handle_line(const char *line)
{
    s_last_io_us = now_us();
    if (*line == '\0') {
        return;
    }
    ESP_LOGD(TAG, "<< %s", line);
    parse_temps(line);

    if (starts_with(line, "ok")) {
        s_ready = true;
        return;
    }
    if (starts_with(line, "Resend:") || starts_with(line, "rs ")) {
        const char *p = strpbrk(line, "0123456789");
        if (p) {
            uint32_t n = strtoul(p, NULL, 10);
            if (n >= 1 && n <= s_last_n + 1) {
                ESP_LOGW(TAG, "Drucker fordert Zeile %" PRIu32 " erneut an", n);
                s_next_n = n;
            }
        }
        return;
    }
    if (starts_with(line, "start")) {
        // Drucker hat neu gestartet, Zeilennummern sind weg
        printer_state_t state = get_state();
        if (state == PRINTER_PRINTING || state == PRINTER_PAUSED) {
            fail("Drucker hat während des Drucks neu gestartet");
        }
        s_need_sync = true;
        return;
    }
    if (starts_with(line, "!!") ||
        (starts_with(line, "Error:") &&
         (contains_ci(line, "halted") || contains_ci(line, "kill") || contains_ci(line, "stopped")))) {
        fail("Drucker meldet: %s", line);
        return;
    }
    if (starts_with(line, "Error:")) {
        // z. B. Prüfsummenfehler; das zugehörige "Resend:" folgt
        ESP_LOGW(TAG, "%s", line);
    }
}

// Protokoll mit dem Drucker synchronisieren (Zeilennummern auf 0 setzen)
static bool sync_printer(void)
{
    char line[LINE_BUF];
    bool keep_error = get_state() == PRINTER_ERROR;
    if (!keep_error) {
        set_state(PRINTER_CONNECTING, "Verbinde mit Drucker …");
    }
    usb_serial_flush_rx();
    s_ready = false;

    // Die meisten Boards starten beim Öffnen des Ports neu und melden sich mit "start"
    int64_t deadline = now_us() + START_WAIT_US;
    while (now_us() < deadline && usb_serial_connected()) {
        if (usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(100)) > 0) {
            ESP_LOGI(TAG, "<< %s", line);
            if (starts_with(line, "start")) {
                break;
            }
        }
    }

    bool synced = false;
    for (int attempt = 0; attempt < SYNC_ATTEMPTS && !synced && usb_serial_connected(); attempt++) {
        usb_serial_write("M110 N0\n", 8);
        deadline = now_us() + SYNC_WAIT_US;
        while (now_us() < deadline && !synced) {
            if (usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(100)) > 0) {
                ESP_LOGI(TAG, "<< %s", line);
                parse_temps(line);
                synced = starts_with(line, "ok");
            }
        }
    }
    if (!synced) {
        return false;
    }

    // "ok" von wiederholten M110 abwarten und verwerfen
    deadline = now_us() + 500000;
    while (now_us() < deadline) {
        usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(50));
    }

    memset(s_hist, 0, sizeof(s_hist));
    s_last_n = 0;
    s_next_n = 1;
    s_inject_head = 0;
    s_inject_count = 0;
    s_ready = true;
    s_last_io_us = now_us();
    inject("M155 S2");   // Temperaturen alle 2 s automatisch melden

    if (!keep_error) {
        set_state(PRINTER_IDLE, "Drucker bereit");
    }
    return true;
}

static void process_requests(void)
{
    LOCK();
    bool start = s_req_start;
    bool cancel = s_req_cancel;
    bool pause = s_req_pause;
    bool resume = s_req_resume;
    s_req_start = false;
    s_req_cancel = false;
    s_req_pause = false;
    s_req_resume = false;
    char name[STORAGE_NAME_MAX];
    strlcpy(name, s_st.file, sizeof(name));
    storage_vol_t vol = s_st.vol;
    bool printing = s_st.state == PRINTER_PRINTING;
    UNLOCK();

    if (cancel) {
        // Beim Quittieren eines Fehlers keine noch laufende Abbruchsequenz verwerfen
        if (s_file) {
            close_job();
            s_inject_count = 0;
            inject_cancel_sequence();
        }
        return;
    }

    if (pause && s_file) {
        pause_lift();
    }
    if (resume && s_file) {
        resume_lower();
    }

    if (start && printing) {
        char path[STORAGE_PATH_MAX];
        storage_path(path, sizeof(path), vol, name);
        if (!storage_acquire(vol)) {
            set_state(PRINTER_IDLE, "%s nicht verfügbar", storage_label(vol));
            return;
        }
        s_file = fopen(path, "r");
        if (!s_file) {
            storage_release(vol);
            set_state(PRINTER_IDLE, "Datei nicht lesbar: %s", name);
            return;
        }
        s_job_vol = vol;
        struct stat st;
        uint32_t size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
        load_shutdown(path);
        reset_motion();
        LOCK();
        s_st.file_size = size;
        s_st.file_pos = 0;
        s_st.started_us = now_us();
        UNLOCK();
        ESP_LOGI(TAG, "Starte Druck: %s (%s, %" PRIu32 " Bytes)", name, storage_label(vol), size);
    }
}

static void check_timeout(void)
{
    if (!s_ready && s_next_n > 1 && now_us() - s_last_io_us > ACK_TIMEOUT_US) {
        // Doppelt gesendete Zeilen erkennt der Drucker an der Nummer
        s_next_n--;
        ESP_LOGW(TAG, "Keine Antwort, sende Zeile %" PRIu32 " erneut", s_next_n);
        s_ready = true;
    }
}

static void printer_task(void *arg)
{
    char line[LINE_BUF];
    uint32_t generation = 0;
    bool linked = false;

    for (;;) {
        if (!usb_serial_connected()) {
            if (linked) {
                linked = false;
                bool active = s_file != NULL;
                close_job();
                set_state(PRINTER_DISCONNECTED, active ? "USB-Verbindung verloren, Druck abgebrochen"
                                                       : "Kein Drucker verbunden");
            }
            process_requests();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!linked || generation != usb_serial_generation() || s_need_sync) {
            if (linked && generation != usb_serial_generation() && s_file) {
                fail("USB-Verbindung unterbrochen, Druck abgebrochen");
            }
            linked = true;
            generation = usb_serial_generation();
            s_need_sync = false;
            if (!sync_printer()) {
                ESP_LOGW(TAG, "Drucker antwortet nicht, neuer Versuch");
                s_need_sync = true;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }

        if (usb_serial_readline(line, sizeof(line), pdMS_TO_TICKS(20)) >= 0) {
            handle_line(line);
        }
        process_requests();
        if (s_file && !storage_ready(s_job_vol)) {
            abort_job();   // SD-Karte während des Drucks oder der Pause entfernt
        }
        // Im Fehlerzustand nur noch die Abbruchsequenz nach abort_job()
        if (s_ready && (get_state() != PRINTER_ERROR || s_inject_count > 0)) {
            send_next();
        }
        check_timeout();
    }
}

esp_err_t printer_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_st.state = PRINTER_DISCONNECTED;
    strlcpy(s_st.message, "Kein Drucker verbunden", sizeof(s_st.message));
    if (xTaskCreate(printer_task, "printer", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void printer_get_status(printer_status_t *out)
{
    LOCK();
    *out = s_st;
    UNLOCK();
    if (out->state == PRINTER_PRINTING || out->state == PRINTER_PAUSED) {
        out->elapsed_s = (uint32_t)((now_us() - out->started_us) / 1000000LL);
    } else {
        out->elapsed_s = 0;
    }
}

const char *printer_state_name(printer_state_t state)
{
    switch (state) {
    case PRINTER_DISCONNECTED: return "disconnected";
    case PRINTER_CONNECTING:   return "connecting";
    case PRINTER_IDLE:         return "idle";
    case PRINTER_PRINTING:     return "printing";
    case PRINTER_PAUSED:       return "paused";
    case PRINTER_ERROR:        return "error";
    }
    return "unknown";
}

esp_err_t printer_start(storage_vol_t vol, const char *name)
{
    esp_err_t err = ESP_OK;
    LOCK();
    if (s_st.state != PRINTER_IDLE) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_st.state = PRINTER_PRINTING;
        s_st.vol = vol;
        strlcpy(s_st.file, name, sizeof(s_st.file));
        s_st.file_size = 0;
        s_st.file_pos = 0;
        s_st.started_us = now_us();
        s_st.finished = false;
        s_st.duration_s = 0;
        strlcpy(s_st.message, "Druck läuft", sizeof(s_st.message));
        s_req_start = true;
    }
    UNLOCK();
    return err;
}

esp_err_t printer_pause(void)
{
    esp_err_t err = ESP_OK;
    LOCK();
    if (s_st.state != PRINTER_PRINTING) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        // Senden anhalten und Düse anheben (im Drucker-Task); Heizungen bleiben an
        s_st.state = PRINTER_PAUSED;
        strlcpy(s_st.message, "Pausiert", sizeof(s_st.message));
        s_req_pause = true;
    }
    UNLOCK();
    return err;
}

esp_err_t printer_resume(void)
{
    esp_err_t err = ESP_OK;
    LOCK();
    if (s_st.state != PRINTER_PAUSED) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_st.state = PRINTER_PRINTING;
        strlcpy(s_st.message, "Druck läuft", sizeof(s_st.message));
        s_req_resume = true;
    }
    UNLOCK();
    return err;
}

esp_err_t printer_cancel(void)
{
    esp_err_t err = ESP_OK;
    LOCK();
    printer_state_t state = s_st.state;
    if (state != PRINTER_PRINTING && state != PRINTER_PAUSED && state != PRINTER_ERROR) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_st.state = usb_serial_connected() ? PRINTER_IDLE : PRINTER_DISCONNECTED;
        strlcpy(s_st.message, state == PRINTER_ERROR ? "Fehler quittiert" : "Druck abgebrochen",
                sizeof(s_st.message));
        s_req_start = false;
        s_req_pause = false;
        s_req_resume = false;
        s_req_cancel = true;
    }
    UNLOCK();
    return err;
}

bool printer_is_using(storage_vol_t vol, const char *name)
{
    LOCK();
    bool using = (s_st.state == PRINTER_PRINTING || s_st.state == PRINTER_PAUSED) &&
                 s_st.vol == vol && strcmp(s_st.file, name) == 0;
    UNLOCK();
    return using;
}
