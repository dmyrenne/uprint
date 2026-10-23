#include "gcode_kind.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG_AXIDRAW "; uplot-axidraw 1"
#define TAG_PEN "; uplot-printer 1"
#define TAG_BOUNDS "; uplot-bounds"
// µplot vor der Kennung (Vorlage aus brianlow/plotter): Drucker-Plots beginnen mit diesem Abschnitt
#define TAG_PEN_OLD "; -- setup"

// Kopfzeilen der verbreiteten Slicer; PrusaSlicer schreibt danach oft viele KB Vorschaubilder, deshalb reicht
// der Anfang nicht immer bis zu den ersten Temperaturbefehlen
static const char *const SLICERS[] = {
    "PrusaSlicer", "SuperSlicer", "OrcaSlicer", "BambuStudio", "Slic3r", "Cura", ";FLAVOR:", "Simplify3D",
    "ideaMaker", "KISSlicer",
};

static bool line_is(const char *line, size_t len, const char *tag)
{
    size_t n = strlen(tag);
    return len >= n && strncmp(line, tag, n) == 0 && (len == n || isspace((unsigned char)line[n]));
}

// Wert eines Parameters (z. B. 'E') in einer Befehlszeile ohne Kommentar
static bool has_param(const char *p, const char *end, char key)
{
    for (; p < end; p++) {
        if (toupper((unsigned char)*p) == key && (p[-1] == ' ' || p[-1] == '\t')) {
            return true;
        }
    }
    return false;
}

// 3D-Druck: Temperaturbefehl oder Fahrt mit Extrusion
static bool print_command(const char *line, size_t len)
{
    const char *p = line, *end = line + len;
    const char *semi = memchr(line, ';', len);
    if (semi) {
        end = semi;
    }
    if (p < end && toupper((unsigned char)*p) == 'N') {   // Zeilennummer überspringen
        while (p < end && !isspace((unsigned char)*p)) {
            p++;
        }
        while (p < end && isspace((unsigned char)*p)) {
            p++;
        }
    }
    if (end - p < 2) {
        return false;
    }
    char letter = (char)toupper((unsigned char)p[0]);
    char *num_end;
    long code = strtol(p + 1, &num_end, 10);
    if (num_end == p + 1 || num_end > end) {
        return false;
    }
    if (letter == 'M') {
        return code == 104 || code == 109 || code == 140 || code == 190;
    }
    return letter == 'G' && (code == 0 || code == 1) && has_param(num_end, end, 'E');
}

void gcode_classify(const char *head, size_t len, gcode_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = GCODE_UNKNOWN;
    const char *p = head, *end = head + len;
    if (end - p >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) {   // UTF-8-BOM
        p += 3;
    }
    for (int n = 0; p < end; n++) {
        const char *nl = memchr(p, '\n', end - p);
        if (!nl) {
            break;   // letzte Zeile abgeschnitten
        }
        size_t l = nl - p;
        if (l && p[l - 1] == '\r') {
            l--;
        }
        if (n == 0 && line_is(p, l, TAG_AXIDRAW)) {
            out->kind = GCODE_AXIDRAW;
        } else if (n == 0 && (line_is(p, l, TAG_PEN) || line_is(p, l, TAG_PEN_OLD))) {
            out->kind = GCODE_PEN;
        } else if (line_is(p, l, TAG_BOUNDS)) {
            char buf[96];
            size_t m = l < sizeof(buf) - 1 ? l : sizeof(buf) - 1;
            memcpy(buf, p, m);
            buf[m] = '\0';
            float *b = out->bounds;
            out->has_bounds = sscanf(buf + strlen(TAG_BOUNDS), "%f %f %f %f", &b[0], &b[1], &b[2], &b[3]) == 4;
        } else if (out->kind == GCODE_UNKNOWN) {
            if (p[0] == ';') {
                for (size_t i = 0; i < sizeof(SLICERS) / sizeof(SLICERS[0]); i++) {
                    const char *s = SLICERS[i];
                    size_t sl = strlen(s);
                    for (size_t k = 0; k + sl <= l; k++) {
                        if (memcmp(p + k, s, sl) == 0) {
                            out->kind = GCODE_PRINT;
                            break;
                        }
                    }
                }
            } else if (print_command(p, l)) {
                out->kind = GCODE_PRINT;
            }
        }
        // µplot: Kennung und Ausdehnung stehen in den ersten Zeilen, danach gibt es nichts mehr zu finden
        if ((out->kind == GCODE_AXIDRAW || out->kind == GCODE_PEN) && n >= 4) {
            break;
        }
        p = nl + 1;
    }
}

bool gcode_file_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".gcode") == 0 || strcasecmp(dot, ".gco") == 0 || strcasecmp(dot, ".g") == 0);
}

const char *gcode_kind_name(gcode_kind_t kind)
{
    switch (kind) {
    case GCODE_PRINT:
        return "print";
    case GCODE_AXIDRAW:
        return "axidraw";
    case GCODE_PEN:
        return "pen";
    default:
        return "unknown";
    }
}

// ---------- Zwischenspeicher ----------

#define CACHE_SIZE 64

typedef struct {
    bool used;
    int vol;
    uint32_t name_hash;
    uint32_t size;
    int64_t mtime;
    gcode_info_t info;
} cache_entry_t;

static cache_entry_t s_cache[CACHE_SIZE];
static unsigned s_next;   // nächster zu ersetzender Eintrag (reihum)

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h = (h ^ (unsigned char)*s++) * 16777619u;
    }
    return h;
}

void gcode_info_cached(int vol, const char *name, const char *path, uint32_t size, int64_t mtime,
                       gcode_info_t *out)
{
    uint32_t h = hash(name);
    for (int i = 0; i < CACHE_SIZE; i++) {
        cache_entry_t *e = &s_cache[i];
        if (e->used && e->vol == vol && e->name_hash == h && e->size == size && e->mtime == mtime) {
            *out = e->info;
            return;
        }
    }
    static char head[GCODE_HEAD_BYTES];
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        n = fread(head, 1, sizeof(head), f);
        fclose(f);
    }
    gcode_classify(head, n, out);
    // Nicht lesbare Dateien nicht festhalten, beim nächsten Mal neu versuchen
    cache_entry_t *e = &s_cache[s_next++ % CACHE_SIZE];
    *e = (cache_entry_t){.used = f != NULL, .vol = vol, .name_hash = h, .size = size, .mtime = mtime, .info = *out};
}
