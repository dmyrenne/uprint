#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "captive.h"

static const char *TAG = "captive";

#define DNS_PORT    53
#define DNS_MAX_LEN 512

static esp_netif_t *s_ap;
static TaskHandle_t s_dns_task;

static uint32_t ap_ip(void)
{
    esp_netif_ip_info_t info;
    if (!s_ap || esp_netif_get_ip_info(s_ap, &info) != ESP_OK) {
        return 0;
    }
    return info.ip.addr;
}

void captive_init(esp_netif_t *ap)
{
    // Keine DHCP-Option 114 (RFC 8910): Sie muss auf eine HTTPS-API mit application/captive+json zeigen,
    // eine einfache HTTP-Seite werten Android und iOS als Fehler. Erkannt wird das Portal über DNS und
    // die Weiterleitung. Den DHCP-Server hier auch nicht anfassen: Nach esp_netif_dhcps_stop() startet
    // ESP-IDF ihn beim Öffnen des Access Points nicht mehr, Geräte bekommen dann keine Adresse.
    s_ap = ap;
}

// Antwort auf eine Anfrage im selben Puffer bauen. Rückgabe: Länge oder 0 (verwerfen).
// Typ A bekommt die Adresse des Access Points, alles andere (z. B. AAAA) eine leere Antwort.
static int dns_answer(uint8_t *buf, int len, int cap, uint32_t ip)
{
    if (len < 12 || (buf[2] & 0x80) || (buf[2] & 0x78) || buf[4] != 0 || buf[5] != 1) {
        return 0;   // keine Standardabfrage mit genau einer Frage
    }
    int p = 12;
    while (p < len && buf[p]) {
        if (buf[p] & 0xC0) {
            return 0;   // Kompression in der Frage kommt nicht vor
        }
        p += buf[p] + 1;
    }
    if (p + 5 > len) {
        return 0;
    }
    p++;   // Null-Byte am Ende des Namens
    uint16_t qtype = buf[p] << 8 | buf[p + 1];
    uint16_t qclass = buf[p + 2] << 8 | buf[p + 3];
    p += 4;   // Ende der Frage, alles danach (z. B. EDNS) fällt weg

    buf[2] = 0x84 | (buf[2] & 0x01);   // Antwort, autoritativ, RD übernehmen
    buf[3] = 0x00;                     // kein Fehler
    buf[6] = buf[7] = 0;               // Antworten
    buf[8] = buf[9] = 0;               // Nameserver
    buf[10] = buf[11] = 0;             // Zusätze
    if (qtype != 1 || qclass != 1 || p + 16 > cap) {
        return p;
    }
    buf[7] = 1;
    const uint8_t rr[] = {
        0xC0, 0x0C,               // Name: Verweis auf die Frage
        0x00, 0x01, 0x00, 0x01,   // Typ A, Klasse IN
        0x00, 0x00, 0x00, 0x3C,   // TTL 60 s
        0x00, 0x04,
    };
    memcpy(buf + p, rr, sizeof(rr));
    memcpy(buf + p + sizeof(rr), &ip, 4);   // ip ist schon in Netzwerk-Byte-Reihenfolge
    return p + sizeof(rr) + 4;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS-Server: Port %d nicht verfügbar", DNS_PORT);
        if (sock >= 0) {
            close(sock);
        }
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS-Server läuft, alle Namen zeigen auf " CAPTIVE_URL);
    static uint8_t buf[DNS_MAX_LEN];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        n = dns_answer(buf, n, sizeof(buf), ap_ip());
        if (n > 0) {
            sendto(sock, buf, n, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

void captive_dns_start(void)
{
    if (!s_dns_task) {
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_dns_task);
    }
}

bool captive_via_ap(int sockfd)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(sockfd, (struct sockaddr *)&local, &len) != 0) {
        return false;
    }
    uint32_t ip = ap_ip();
    if (local.ss_family == AF_INET) {
        return ip && ((struct sockaddr_in *)&local)->sin_addr.s_addr == ip;
    }
#if LWIP_IPV6
    // Der Webserver lauscht auf IPv6, IPv4-Verbindungen kommen dann als ::ffff:a.b.c.d
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (struct sockaddr_in6 *)&local;
        return ip && IN6_IS_ADDR_V4MAPPED(&a->sin6_addr) && a->sin6_addr.un.u32_addr[3] == ip;
    }
#endif
    return false;
}
