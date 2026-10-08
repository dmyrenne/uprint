#pragma once

#include <stdbool.h>
#include <stdint.h>

// Übersetzer G-Code → EBB-Befehle (AxiDraw/NextDraw). Reines C ohne ESP-IDF, damit er sich auf dem
// PC testen lässt. Der Aufrufer füttert Zeile für Zeile, der Übersetzer gibt die EBB-Befehle über
// den Callback aus (ohne CR). Wegen der Vorausschau kommen die Befehle verzögert: Bewegungen werden
// erst ausgegeben, wenn der Puffer voll ist, der Stift wechselt oder plot_finish() kommt.
//
// Vertrag mit µplot (docs/axidraw.md): Nullpunkt = Stiftposition beim Einschalten, X nach rechts,
// Y vom Gerät weg, Einheit mm. Z 0 = Stift unten, Z 1 = Stift oben (Zwischenwerte sind für
// Druckstufen reserviert und werden vorerst auf unten/oben gerundet).

#define PLOT_LOOKAHEAD 32

typedef enum {
    PLOT_SERVO_STANDARD = 0,   // AxiDraw V3/SE/MiniKit: Hobby-Servo an RB1
    PLOT_SERVO_BRUSHLESS = 1,  // NextDraw und AxiDraw mit Upgrade-Kit: bürstenloser Servo an RB2
} plot_servo_t;

typedef struct {
    float steps_per_mm;       // 80 bei AxiDraw (2032 Schritte/Zoll, 1/16-Mikroschritt)
    float speed_draw;         // mm/s, Obergrenze für G1 (F aus der Datei wird darauf begrenzt)
    float speed_travel;       // mm/s, Obergrenze für G0
    float accel;              // mm/s²
    float cornering;          // mm, Abweichung an Ecken (wie "junction deviation" bei Grbl)
    plot_servo_t servo;
    int pen_up_pct;           // Servoposition 0–100 %, < 0: Werte aus der Firmware lassen
    int pen_down_pct;
    int pen_up_ms;            // Wartezeit nach dem Heben bzw. Senken
    int pen_down_ms;
} plot_config_t;

typedef void (*plot_emit_fn)(const char *cmd, void *ctx);

typedef enum {
    PLOT_OK,
    PLOT_IGNORED,   // Befehl ohne Bedeutung für den Plotter (M3, G28, Temperaturen …)
    PLOT_ERROR,     // nicht übersetzbar, Plot abbrechen; Text in plot_t.error
} plot_result_t;

typedef struct {
    int32_t m1, m2;           // Motorschritte (CoreXY: x + y, x − y)
    float len;                // mm
    float ux, uy;             // Richtung (Einheitsvektor)
    float v_max;              // mm/s
    float v_entry_max;        // aus dem Winkel zum Vorgänger
    float v_entry;            // geplant
} plot_seg_t;

typedef struct {
    plot_config_t cfg;
    plot_emit_fn emit;
    void *ctx;
    bool absolute;
    float unit;               // mm je G-Code-Einheit
    float x, y;               // Sollposition in mm (G-Code-Koordinaten)
    int32_t sx, sy;           // Position nach der letzten Strecke im Puffer, in Schritten (X/Y-Achse)
    int32_t ex, ey;           // Position nach dem letzten ausgegebenen LM
    float feed;               // mm/s aus dem letzten F
    int pen;                  // -1 unbekannt, 0 unten, 1 oben
    int held_pen;             // Stift vor plot_hold()
    plot_seg_t seg[PLOT_LOOKAHEAD];
    int head, count;
    float prev_ux, prev_uy;   // Richtung der letzten Bewegung im Puffer
    bool prev_valid;
    uint32_t lines;
    char error[96];
} plot_t;

// Setzt den Zustand zurück und gibt die Startbefehle aus (Motoren an, Stifthöhen, Stift hoch)
void plot_begin(plot_t *p, const plot_config_t *cfg, plot_emit_fn emit, void *ctx);

// Eine G-Code-Zeile (ohne Zeilenende, Kommentare erlaubt)
plot_result_t plot_line(plot_t *p, const char *line);

// Restliche Bewegungen ausgeben, Stift hoch. Motoren bleiben an (M84 schaltet sie ab).
void plot_finish(plot_t *p);

// Pause: Puffer ausfahren, Stift hoch. Die Motoren bleiben an, die Position stimmt also beim Fortsetzen.
void plot_hold(plot_t *p);
// Nach der Pause: Stift wieder runter, falls er vorher unten war
void plot_continue(plot_t *p);

// Abbrechen: Puffer verwerfen, Stift hoch, zurück auf den Nullpunkt, Motoren aus
void plot_cancel(plot_t *p);

// Hilfen für den Test auf dem PC: Rate und Beschleunigung für LM (Akkumulator alle 40 µs, 2^31 = 1 Schritt)
int32_t plot_lm_rate(float steps_per_s);
int32_t plot_lm_accel(float steps_per_s2);
