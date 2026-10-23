#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Stuft G-Code-Dateien als 3D-Druck oder 2D-Plot ein (#12). Reines C ohne ESP-IDF, damit es sich auf dem PC
// testen lässt. Gelesen wird nur der Anfang der Datei: Kennung von µplot in der ersten Zeile, sonst Slicer-Kopf,
// Temperaturbefehle und Extrusion.

#define GCODE_HEAD_BYTES 4096

typedef enum {
    GCODE_UNKNOWN,   // nichts Eindeutiges gefunden (z. B. G-Code aus anderen Plotter-Programmen)
    GCODE_PRINT,     // 3D-Druck: Slicer-Kopf, Temperaturen oder Extrusion
    GCODE_AXIDRAW,   // µplot für AxiDraw/NextDraw ("; uplot-axidraw 1")
    GCODE_PEN,       // µplot für einen Drucker mit Stifthalter ("; uplot-printer 1")
} gcode_kind_t;

typedef struct {
    gcode_kind_t kind;
    bool has_bounds;
    float bounds[4];   // x0 y0 x1 y1 in mm aus "; uplot-bounds", Ausdehnung der gezeichneten Linien
} gcode_info_t;

// head: die ersten len Bytes der Datei (eine abgeschnittene letzte Zeile wird nicht ausgewertet)
void gcode_classify(const char *head, size_t len, gcode_info_t *out);

// Endung .gcode, .gco oder .g (andere Dateien werden nicht gelesen)
bool gcode_file_name(const char *name);

// "print", "axidraw", "pen" oder "unknown" (für die API)
const char *gcode_kind_name(gcode_kind_t kind);

// Einstufung einer Datei mit Zwischenspeicher (Schlüssel: Speicher, Name, Größe, Änderungszeit). Ist sie dort nicht,
// wird der Anfang über path gelesen. Nicht threadsicher: nur aus dem Task des Webservers aufrufen.
void gcode_info_cached(int vol, const char *name, const char *path, uint32_t size, int64_t mtime,
                       gcode_info_t *out);
