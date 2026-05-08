/*
 * wifi_portal.c - Captive Portal (DNS Hijack + HTTP 302)
 * ESP32 PrinterBox / ESP-IDF v5.2+
 *
 * DNS server: all queries → 192.168.4.1
 * HTTP: unknown host requests → 302 redirect to http://192.168.4.1
 */

#include "wifi_portal.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "PORTAL";

/* ════════════════════════════════════════════════════════════════════ */
/*                     DNS  SERVER  (UDP 53)                          */
/* ════════════════════════════════════════════════════════════════════ */

#define DNS_PORT      53
#define DNS_BUF_SIZE  512

/* Minimal DNS response: return 192.168.4.1 for every A query */
static int  s_dns_sock = -1;
static bool s_dns_running = false;
static TaskHandle_t s_dns_task = NULL;

/*
 * Build a DNS response that answers every query with 192.168.4.1.
 * We simply copy the query and append an answer RR.
 */
static int build_dns_response(const uint8_t *query, int qlen,
                              uint8_t *resp, int resp_max)
{
    if (qlen < 12 || qlen > resp_max - 16) return -1;

    /* Copy entire query as base of response */
    memcpy(resp, query, qlen);

    /* Set response flags: QR=1, AA=1, RD=1, RA=0 */
    resp[2] = 0x81;  /* QR=1, Opcode=0, AA=1 */
    resp[3] = 0x80;  /* RA=1, RCode=0 */

    /* Answer count = 1 */
    resp[6] = 0x00;
    resp[7] = 0x01;

    /* Append answer RR at end of query */
    int pos = qlen;

    /* NAME: pointer to offset 12 (the question QNAME) */
    resp[pos++] = 0xC0;
    resp[pos++] = 0x0C;

    /* TYPE: A (1) */
    resp[pos++] = 0x00;
    resp[pos++] = 0x01;

    /* CLASS: IN (1) */
    resp[pos++] = 0x00;
    resp[pos++] = 0x01;

    /* TTL: 60 seconds */
    resp[pos++] = 0x00;
    resp[pos++] = 0x00;
    resp[pos++] = 0x00;
    resp[pos++] = 0x3C;

    /* RDLENGTH: 4 */
    resp[pos++] = 0x00;
    resp[pos++] = 0x04;

    /* RDATA: 192.168.4.1 */
    resp[pos++] = 192;
    resp[pos++] = 168;
    resp[pos++] = 4;
    resp[pos++] = 1;

    return pos;
}

static void dns_server_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_BUF_SIZE];
    uint8_t tx[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    while (s_dns_running) {
        int n = recvfrom(s_dns_sock, rx, sizeof(rx), 0,
                         (struct sockaddr *)&client, &clen);
        if (n <= 0) {
            if (!s_dns_running) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int rlen = build_dns_response(rx, n, tx, sizeof(tx));
        if (rlen > 0) {
            sendto(s_dns_sock, tx, rlen, 0,
                   (struct sockaddr *)&client, clen);
        }
    }

    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

esp_err_t wifi_portal_dns_start(void)
{
    if (s_dns_running) return ESP_OK;

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    /* Set receive timeout so task can check s_dns_running */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_FAIL;
    }

    s_dns_running = true;
    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task);
    return ESP_OK;
}

esp_err_t wifi_portal_dns_stop(void)
{
    s_dns_running = false;
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    s_dns_task = NULL;
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════ */
/*              HTTP  REDIRECT  (Captive Portal Detection)            */
/* ════════════════════════════════════════════════════════════════════ */

/*
 * Catch-all: if the Host header is NOT 192.168.4.1 → 302 redirect.
 * This handles the captive portal probe requests from phones/laptops.
 *
 * Known probe URLs:
 *   Android:  /generate_204, connectivitycheck.gstatic.com
 *   iOS:      /hotspot-detect.html
 *   Windows:  /ncsi.txt, msftconnecttest.com
 */
static esp_err_t handle_captive_redirect(httpd_req_t *req)
{
    /* Check Host header */
    char host[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        /* No Host header — redirect anyway */
    }

    /* If Host is our AP IP, let it through (will 404 naturally) */
    if (strcmp(host, "192.168.4.1") == 0 ||
        strncmp(host, "192.168.4.1:", 12) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
        return ESP_OK;
    }

    /* 302 redirect to our config page */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirecting to PrinterBox Setup...",
                    HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Captive redirect: %s%s -> 192.168.4.1",
             host, req->uri);
    return ESP_OK;
}

/* Well-known captive-portal probe paths */
static const char *s_probe_uris[] = {
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/ncsi.txt",
    "/connecttest.txt",
    "/redirect",
    "/success.txt",
    "/canonical.html",
    "/check_network_status.txt",
};
#define PROBE_URI_COUNT (sizeof(s_probe_uris)/sizeof(s_probe_uris[0]))

esp_err_t wifi_portal_http_register(httpd_handle_t server)
{
    /* Register known captive-portal probe URIs */
    for (int i = 0; i < (int)PROBE_URI_COUNT; i++) {
        httpd_uri_t u = {
            .uri     = s_probe_uris[i],
            .method  = HTTP_GET,
            .handler = handle_captive_redirect,
        };
        httpd_register_uri_handler(server, &u);
    }

    ESP_LOGI(TAG, "Captive portal HTTP redirects registered (%d URIs)",
             (int)PROBE_URI_COUNT);
    return ESP_OK;
}
