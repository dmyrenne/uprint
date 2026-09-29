#pragma once

#include "esp_http_server.h"

/*
 * Schnittstelle für Slicer, kompatibel zu PrusaLink und OctoPrint (PrusaSlicer, SuperSlicer, OrcaSlicer, …).
 * Alle Aufrufe brauchen den API-Schlüssel aus den Einstellungen (X-Api-Key, "Authorization: Bearer" oder ?apikey=).
 *
 *   GET  /api/version                   Kennung und Fähigkeiten (upload-by-put)
 *   GET  /api/v1/storage                Speicherorte (PrusaLink)
 *   PUT  /api/v1/files/<speicher>/<datei>  Rohdaten, Print-After-Upload: ?1 startet den Druck (PrusaLink)
 *   POST /api/files/local               multipart/form-data mit file, print (OctoPrint)
 *   GET  /api/job, /api/printer         Druckstatus und Temperaturen (OctoPrint)
 *   POST /api/job                       {"command": "pause" | "cancel"} (OctoPrint)
 *
 * Braucht einen Server mit httpd_uri_match_wildcard.
 */
esp_err_t slicer_api_register(httpd_handle_t server);
