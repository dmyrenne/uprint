/*
 * Übersetzer G-Code → EBB-Befehle für AxiDraw/NextDraw (siehe plot.h und docs/axidraw.md).
 *
 * Jede Strecke bekommt ein Trapezprofil (beschleunigen, fahren, bremsen), ausgegeben als bis zu drei
 * LM-Befehle. Die Vorausschau über PLOT_LOOKAHEAD Strecken bestimmt, wie schnell eine Ecke durchfahren
 * werden darf (Verfahren wie bei Grbl: Rückwärts- und Vorwärtsdurchlauf, Eckgeschwindigkeit aus dem
 * Winkel). Vor jedem Stiftwechsel wird auf Stillstand gebremst.
 *
 * LM rechnet je Motor: Rate und Beschleunigung werden alle 40 µs auf einen Akkumulator addiert, ein
 * Überlauf über 2^31 ist ein Schritt. Damit beide Motoren gleichzeitig ankommen, bekommt jeder Motor
 * eine Beschleunigung, mit der er seine (ganzzahligen) Schritte genau in der Phasendauer fährt. Er
 * zielt dabei eine halbe Schritt weiter, damit Rundungsfehler ihn nie mit Rate fast 0 vor dem
 * letzten Schritt hängen lassen; das LM endet ohnehin nach der vorgegebenen Schrittzahl.
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plot.h"

#define V_MIN          2.0f        // mm/s an Stillstandsstellen, damit keine Rate gegen 0 läuft
#define MOTOR_MAX      24000.0f    // Schritte/s, LM erlaubt höchstens 25 000
#define LM_MIN_S       0.003f      // kürzeste LM-Dauer: Das EBB puffert nur einen Befehl, der nächste
                                   // muss über USB ankommen, bevor der laufende fertig ist
#define LM_MAX_S       0.5f        // längste LM-Dauer: begrenzt die Wartezeit auf "OK" und beim Abbrechen
#define LM_TICK_HZ     25000.0f
#define LM_ONE         2147483648.0f

// Stift-Servos, Werte aus der AxiDraw-Software (axidraw_conf.py, pen_handling.py). Position in 83,3 ns
// für 0 % und 100 %, Ausgang an Port B, Zahl der PWM-Kanäle (SC,8).
typedef struct {
    int min, max;
    int pin;        // für SP; 0 = Vorgabe der Firmware (RB1)
    int channels;
} servo_t;

static const servo_t SERVOS[] = {
    [PLOT_SERVO_STANDARD] = {9855, 27831, 0, 8},
    [PLOT_SERVO_BRUSHLESS] = {5400, 12600, 2, 1},
};

int32_t plot_lm_rate(float steps_per_s)
{
    return (int32_t)lroundf(steps_per_s * (LM_ONE / LM_TICK_HZ));
}

int32_t plot_lm_accel(float steps_per_s2)
{
    return (int32_t)lroundf(steps_per_s2 * (LM_ONE / LM_TICK_HZ / LM_TICK_HZ));
}

__attribute__((format(printf, 2, 3)))
static void emitf(plot_t *p, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->emit(buf, p->ctx);
}

__attribute__((format(printf, 2, 3)))
static plot_result_t fail(plot_t *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error, sizeof(p->error), fmt, ap);
    va_end(ap);
    return PLOT_ERROR;
}

static const servo_t *servo(const plot_t *p)
{
    return &SERVOS[p->cfg.servo == PLOT_SERVO_BRUSHLESS ? PLOT_SERVO_BRUSHLESS : PLOT_SERVO_STANDARD];
}

static int servo_pos(const plot_t *p, int pct)
{
    if (pct > 100) {
        pct = 100;
    }
    return servo(p)->min + (servo(p)->max - servo(p)->min) * pct / 100;
}

// ---------- Ausgabe einer Strecke ----------

// Ein LM für eine Phase: Geschwindigkeit vs → ve (mm/s) in t Sekunden, s1/s2 Motorschritte.
// k1/k2: Motorschritte/s je mm/s Bahngeschwindigkeit.
static void emit_lm(plot_t *p, int32_t s1, int32_t s2, float k1, float k2, float vs, float t)
{
    int32_t steps[2] = {s1, s2};
    float k[2] = {k1, k2};
    int32_t rate[2] = {0, 0}, acc[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (steps[i] == 0) {
            continue;
        }
        float target = fabsf((float)steps[i]) + 0.5f;
        float r0 = vs * k[i];
        float a = 2.0f * (target - r0 * t) / (t * t);
        if (r0 + a * t < 0.0f) {
            r0 = target / t;   // sollte nicht vorkommen; dann gleichförmig
            a = 0.0f;
        }
        rate[i] = plot_lm_rate(r0);
        acc[i] = plot_lm_accel(a);
    }
    emitf(p, "LM,%ld,%ld,%ld,%ld,%ld,%ld", (long)rate[0], (long)steps[0], (long)acc[0],
          (long)rate[1], (long)steps[1], (long)acc[1]);
}

static void emit_seg(plot_t *p, const plot_seg_t *s, float v0, float v1)
{
    float a = p->cfg.accel;
    float vc = s->v_max;
    float d_acc = (vc * vc - v0 * v0) / (2.0f * a);
    float d_dec = (vc * vc - v1 * v1) / (2.0f * a);
    if (d_acc + d_dec > s->len) {
        // Dreieck: Höchstgeschwindigkeit wird nicht erreicht
        vc = sqrtf((2.0f * a * s->len + v0 * v0 + v1 * v1) / 2.0f);
        vc = fmaxf(vc, fmaxf(v0, v1));
        d_acc = fmaxf(0.0f, (vc * vc - v0 * v0) / (2.0f * a));
        d_dec = fmaxf(0.0f, s->len - d_acc);
    }
    float d_cruise = fmaxf(0.0f, s->len - d_acc - d_dec);
    float dist[3] = {d_acc, d_cruise, d_dec};
    float vs[3] = {v0, vc, vc};
    float ve[3] = {vc, vc, v1};
    for (int i = 0; i < 3; i++) {
        if (dist[i] > 1e-6f && 2.0f * dist[i] / (vs[i] + ve[i]) < LM_MIN_S) {
            // Eine Phase wäre zu kurz: die ganze Strecke als ein LM von v0 nach v1. Fahrbar, weil die
            // Planung |v1² − v0²| ≤ 2·a·len garantiert; mindestens LM_MIN_S lang wegen v_max in queue_move.
            dist[0] = s->len;
            dist[1] = dist[2] = 0.0f;
            ve[0] = v1;
            break;
        }
    }

    // Motorschritte/s je mm/s Bahngeschwindigkeit: Anteil des Motors an der Bahn
    float k1 = fabsf((float)s->m1) / s->len;
    float k2 = fabsf((float)s->m2) / s->len;

    int last = 0;
    for (int i = 0; i < 3; i++) {
        if (dist[i] > 1e-6f) {
            last = i;
        }
    }
    float cum = 0.0f;
    int32_t done1 = 0, done2 = 0;
    for (int i = 0; i <= last; i++) {
        if (dist[i] <= 1e-6f) {
            continue;
        }
        // Lange Phasen in Stücke von höchstens LM_MAX_S teilen, Geschwindigkeit linear weiter
        float t_phase = 2.0f * dist[i] / (vs[i] + ve[i]);
        int pieces = (int)ceilf(t_phase / LM_MAX_S);
        for (int j = 0; j < pieces; j++) {
            float a0 = vs[i] + (ve[i] - vs[i]) * j / pieces;
            float a1 = vs[i] + (ve[i] - vs[i]) * (j + 1) / pieces;
            float t = t_phase / pieces;
            cum += (a0 + a1) / 2.0f * t;
            // das letzte Stück der letzten Phase fährt den Rest
            float f = i == last && j == pieces - 1 ? 1.0f : fminf(1.0f, cum / s->len);
            int32_t t1 = (int32_t)lroundf(s->m1 * f), t2 = (int32_t)lroundf(s->m2 * f);
            int32_t n1 = t1 - done1, n2 = t2 - done2;
            if (n1 == 0 && n2 == 0) {
                continue;
            }
            done1 = t1;
            done2 = t2;
            emit_lm(p, n1, n2, k1, k2, a0, t);
        }
    }
    // Ausgegebene Position mitführen (für Abbrechen): x = (m1 + m2) / 2, y = (m1 − m2) / 2
    p->ex += (s->m1 + s->m2) / 2;
    p->ey += (s->m1 - s->m2) / 2;
}

// ---------- Vorausschau ----------

static plot_seg_t *seg_at(plot_t *p, int i)
{
    return &p->seg[(p->head + i) % PLOT_LOOKAHEAD];
}

// Plant die Eintrittsgeschwindigkeiten im Puffer. Am Ende des Puffers wird Stillstand angenommen,
// so bleibt jeder Plan fahrbar, auch wenn nichts mehr kommt. Die erste Strecke ist schon
// festgelegt (ihr Vorgänger wurde mit dieser Austrittsgeschwindigkeit ausgegeben).
static void plan(plot_t *p)
{
    float a2 = 2.0f * p->cfg.accel;
    float next = V_MIN;
    for (int i = p->count - 1; i >= 1; i--) {
        plot_seg_t *s = seg_at(p, i);
        s->v_entry = fminf(s->v_entry_max, sqrtf(next * next + a2 * s->len));
        next = s->v_entry;
    }
    for (int i = 0; i + 1 < p->count; i++) {
        plot_seg_t *s = seg_at(p, i), *n = seg_at(p, i + 1);
        float reach = sqrtf(s->v_entry * s->v_entry + a2 * s->len);
        if (n->v_entry > reach) {
            n->v_entry = reach;
        }
    }
}

static void emit_oldest(plot_t *p)
{
    plot_seg_t *s = seg_at(p, 0);
    float v1 = p->count > 1 ? seg_at(p, 1)->v_entry : V_MIN;
    emit_seg(p, s, s->v_entry, v1);
    p->head = (p->head + 1) % PLOT_LOOKAHEAD;
    p->count--;
    if (p->count > 0) {
        seg_at(p, 0)->v_entry_max = seg_at(p, 0)->v_entry;   // festgelegt
    }
}

// Alles ausgeben und auf Stillstand bremsen
static void flush(plot_t *p)
{
    plan(p);
    while (p->count > 0) {
        emit_oldest(p);
    }
    p->prev_valid = false;
}

// Eckgeschwindigkeit aus dem Winkel zwischen zwei Richtungen (Grbl: junction deviation)
static float junction_speed(const plot_t *p, float ux, float uy)
{
    float cos_theta = -(p->prev_ux * ux + p->prev_uy * uy);
    if (cos_theta > 0.999999f) {
        return V_MIN;   // Umkehr
    }
    if (cos_theta < -0.999999f) {
        return INFINITY;   // gerade weiter
    }
    float sin_half = sqrtf(0.5f * (1.0f - cos_theta));
    return fmaxf(V_MIN, sqrtf(p->cfg.accel * p->cfg.cornering * sin_half / (1.0f - sin_half)));
}

static void queue_move(plot_t *p, float x, float y, bool rapid)
{
    int32_t tx = (int32_t)lroundf(x * p->cfg.steps_per_mm);
    int32_t ty = (int32_t)lroundf(y * p->cfg.steps_per_mm);
    int32_t dx = tx - p->sx, dy = ty - p->sy;
    if (dx == 0 && dy == 0) {
        return;
    }
    float len_steps = sqrtf((float)dx * dx + (float)dy * dy);
    plot_seg_t s = {
        .m1 = dx + dy,
        .m2 = dx - dy,
        .len = len_steps / p->cfg.steps_per_mm,
        .ux = dx / len_steps,
        .uy = dy / len_steps,
    };
    float v = rapid ? p->cfg.speed_travel : fminf(p->feed, p->cfg.speed_draw);
    // Kein Motor schneller als MOTOR_MAX
    float m = fmaxf(abs(s.m1), abs(s.m2)) / s.len;   // Motorschritte je mm Bahn
    s.v_max = fmaxf(V_MIN, fminf(fminf(v, MOTOR_MAX / m), s.len / LM_MIN_S));
    s.v_entry_max = p->prev_valid ? fminf(s.v_max, junction_speed(p, s.ux, s.uy)) : V_MIN;
    if (p->prev_valid && p->count > 0) {
        s.v_entry_max = fminf(s.v_entry_max, seg_at(p, p->count - 1)->v_max);
    }
    s.v_entry = s.v_entry_max;

    if (p->count == PLOT_LOOKAHEAD) {
        plan(p);
        emit_oldest(p);
    }
    p->seg[(p->head + p->count) % PLOT_LOOKAHEAD] = s;
    p->count++;
    p->sx = tx;
    p->sy = ty;
    p->prev_ux = s.ux;
    p->prev_uy = s.uy;
    p->prev_valid = true;
}

static void set_pen(plot_t *p, float z)
{
    int want = z >= 0.5f ? 1 : 0;   // Zwischenwerte vorerst gerundet (Druckstufen später)
    if (want == p->pen) {
        return;
    }
    flush(p);
    int ms = want ? p->cfg.pen_up_ms : p->cfg.pen_down_ms;
    if (servo(p)->pin) {
        emitf(p, "SP,%d,%d,%d", want, ms, servo(p)->pin);
    } else {
        emitf(p, "SP,%d,%d", want, ms);   // RB1, so auch am Gerät mit FW 2.8.1 getestet
    }
    p->pen = want;
}

// ---------- G-Code ----------

typedef struct {
    int g[4], ng;
    int m;
    bool has_x, has_y, has_z, has_f, has_e, has_p, has_s;
    float x, y, z, f, p, s;
} words_t;

static bool parse_words(const char *line, words_t *w)
{
    memset(w, 0, sizeof(*w));
    w->m = -1;
    const char *c = line;
    while (*c) {
        if (*c == ';') {
            break;
        }
        if (*c == '(') {
            const char *end = strchr(c, ')');
            if (!end) {
                break;
            }
            c = end + 1;
            continue;
        }
        if (isspace((unsigned char)*c)) {
            c++;
            continue;
        }
        char letter = (char)toupper((unsigned char)*c++);
        char *end;
        float v = strtof(c, &end);
        if (end == c) {
            return false;
        }
        c = end;
        switch (letter) {
        case 'G':
            if (w->ng < 4) {
                w->g[w->ng++] = (int)lroundf(v * 10.0f);   // G28.1 → 281, G1 → 10
            }
            break;
        case 'M': w->m = (int)v; break;
        case 'X': w->has_x = true; w->x = v; break;
        case 'Y': w->has_y = true; w->y = v; break;
        case 'Z': w->has_z = true; w->z = v; break;
        case 'F': w->has_f = true; w->f = v; break;
        case 'E': w->has_e = true; break;
        case 'P': w->has_p = true; w->p = v; break;
        case 'S': w->has_s = true; w->s = v; break;
        default: break;   // N, T usw. sind für den Plotter bedeutungslos
        }
    }
    return true;
}

static plot_result_t motion(plot_t *p, const words_t *w, bool rapid)
{
    if (w->has_e) {
        return fail(p, "Zeile %lu: Extrusion (E) – das ist 3D-G-Code", (unsigned long)p->lines);
    }
    if (w->has_f && w->f > 0.0f) {
        p->feed = w->f * p->unit / 60.0f;
    }
    float x = p->x, y = p->y;
    if (w->has_x) {
        x = p->absolute ? w->x * p->unit : x + w->x * p->unit;
    }
    if (w->has_y) {
        y = p->absolute ? w->y * p->unit : y + w->y * p->unit;
    }
    bool xy = w->has_x || w->has_y;
    // Stift hoch vor der Bewegung, runter erst danach
    if (w->has_z && w->z >= 0.5f) {
        set_pen(p, w->z);
    }
    if (xy) {
        queue_move(p, x, y, rapid);
        p->x = x;
        p->y = y;
    }
    if (w->has_z && w->z < 0.5f) {
        set_pen(p, w->z);
    }
    return PLOT_OK;
}

void plot_begin(plot_t *p, const plot_config_t *cfg, plot_emit_fn emit, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    p->emit = emit;
    p->ctx = ctx;
    p->absolute = true;
    p->unit = 1.0f;
    p->feed = cfg->speed_draw;
    p->pen = -1;
    emitf(p, "EM,1,1");
    if (cfg->pen_up_pct >= 0 && cfg->pen_down_pct >= 0) {
        emitf(p, "SC,4,%d", servo_pos(p, cfg->pen_up_pct));
        emitf(p, "SC,5,%d", servo_pos(p, cfg->pen_down_pct));
    }
    emitf(p, "SC,8,%d", servo(p)->channels);
    set_pen(p, 1.0f);
}

plot_result_t plot_line(plot_t *p, const char *line)
{
    words_t w;
    p->lines++;
    if (!parse_words(line, &w)) {
        return fail(p, "Zeile %lu: nicht lesbar", (unsigned long)p->lines);
    }
    if (w.ng == 0 && w.m < 0) {
        // Nur Koordinaten (modale Bewegung) kommen bei µplot nicht vor
        return w.has_x || w.has_y || w.has_z ? fail(p, "Zeile %lu: Bewegung ohne G0/G1", (unsigned long)p->lines)
                                             : PLOT_IGNORED;
    }
    plot_result_t res = PLOT_IGNORED;
    for (int i = 0; i < w.ng; i++) {
        switch (w.g[i]) {
        case 0:
        case 10:
            res = motion(p, &w, w.g[i] == 0);
            break;
        case 200: p->unit = 25.4f; res = PLOT_OK; break;
        case 210: p->unit = 1.0f; res = PLOT_OK; break;
        case 900: p->absolute = true; res = PLOT_OK; break;
        case 910: p->absolute = false; res = PLOT_OK; break;
        case 40: {
            // Verweilen: P in ms, S in s
            int ms = w.has_p ? (int)w.p : w.has_s ? (int)(w.s * 1000.0f) : 0;
            if (ms > 0) {
                flush(p);
                for (; ms > 0; ms -= (int)(LM_MAX_S * 1000.0f)) {
                    emitf(p, "SM,%d,0,0", ms < (int)(LM_MAX_S * 1000.0f) ? ms : (int)(LM_MAX_S * 1000.0f));
                }
            }
            res = PLOT_OK;
            break;
        }
        case 20:
        case 30:
            return fail(p, "Zeile %lu: Kreisbögen (G2/G3) werden nicht unterstützt", (unsigned long)p->lines);
        default:
            break;   // G28 (keine Referenzfahrt), G92 usw.: ignorieren
        }
        if (res == PLOT_ERROR) {
            return res;
        }
    }
    switch (w.m) {
    case 18:
    case 84:
        flush(p);
        emitf(p, "EM,0,0");
        return PLOT_OK;
    case 2:
    case 30:
        flush(p);
        return PLOT_OK;
    case 104:
    case 109:
    case 140:
    case 190:
        return fail(p, "Zeile %lu: Temperaturbefehl – das ist 3D-G-Code", (unsigned long)p->lines);
    default:
        break;
    }
    return res;
}

void plot_finish(plot_t *p)
{
    flush(p);
    set_pen(p, 1.0f);
}

void plot_hold(plot_t *p)
{
    flush(p);
    p->held_pen = p->pen;
    set_pen(p, 1.0f);
}

void plot_continue(plot_t *p)
{
    if (p->held_pen == 0) {
        set_pen(p, 0.0f);
    }
}

void plot_cancel(plot_t *p)
{
    // Puffer verwerfen: Die Position ist dann die nach dem letzten ausgegebenen LM
    p->count = 0;
    p->prev_valid = false;
    p->sx = p->ex;
    p->sy = p->ey;
    set_pen(p, 1.0f);
    queue_move(p, 0.0f, 0.0f, true);
    flush(p);
    p->x = p->y = 0.0f;
    emitf(p, "EM,0,0");
}
