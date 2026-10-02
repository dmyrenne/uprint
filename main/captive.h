#pragma once

#include <stdbool.h>

#include "esp_netif.h"

/*
 * Captive Portal für den eigenen Access Point: Handys und Laptops öffnen die Weboberfläche
 * nach dem Verbinden von selbst, ohne dass man http://192.168.4.1 eintippen muss.
 *   - ein DNS-Server beantwortet jede Anfrage mit der Adresse des Access Points
 *   - der Webserver leitet unbekannte Pfade am Access Point auf die Startseite um
 *     (die Prüfadressen der Betriebssysteme, z. B. /generate_204, /hotspot-detect.html)
 */

#define CAPTIVE_URL "http://192.168.4.1/"

// Mit dem Netzwerk-Interface des Access Points aufrufen
void captive_init(esp_netif_t *ap);

// Startet den DNS-Server, mehrfacher Aufruf schadet nicht
void captive_dns_start(void);

// Kam die Verbindung über den Access Point (und nicht über das WLAN)?
bool captive_via_ap(int sockfd);
