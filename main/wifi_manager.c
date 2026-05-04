/*
 * wifi_manager.c - WiFi Station Manager Implementation
 * ESP32 Printer Client System
 *
 * Handles WiFi STA initialization, connection, and auto-reconnect.
 * Uses ESP-IDF v5.x event-driven model (esp_netif).
 */

#include "wifi_manager.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

/* Event group for WiFi connectivity state */
static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
#define MAX_RETRY_BEFORE_LOG_SUPPRESS  10

/* ── Event handler ──────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started, connecting...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_retry_count++;

            /* Log at a reduced rate after many retries to avoid log spam */
            if (s_retry_count <= MAX_RETRY_BEFORE_LOG_SUPPRESS ||
                (s_retry_count % 10) == 0) {
                ESP_LOGW(TAG, "WiFi disconnected (attempt #%d), reconnecting...",
                         s_retry_count);
            }

            /* Exponential backoff: 1s, 2s, 4s, 8s ... capped at 30s */
            int delay_s = (1 << (s_retry_count > 5 ? 5 : s_retry_count));
            if (delay_s > 30) delay_s = 30;
            vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
            esp_wifi_connect();
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── Apply optional static IP ───────────────────────────────────────── */
static void apply_static_ip(esp_netif_t *netif, const device_config_t *cfg)
{
    if (!cfg->use_static_ip) {
        ESP_LOGI(TAG, "Using DHCP (default)");
        return;
    }

    ESP_LOGI(TAG, "Applying static IP: %s", cfg->static_ip);

    /* Stop DHCP client before setting static IP */
    esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(cfg->static_ip, &ip_info.ip);
    esp_netif_str_to_ip4(cfg->gateway,   &ip_info.gw);
    esp_netif_str_to_ip4(cfg->netmask,   &ip_info.netmask);
    esp_netif_set_ip_info(netif, &ip_info);

    /* Set DNS */
    esp_netif_dns_info_t dns_info = {0};
    esp_netif_str_to_ip4(cfg->dns, (esp_ip4_addr_t *)&dns_info.ip.u_addr.ip4);
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(uint32_t timeout_ms)
{
    const device_config_t *cfg = config_get();

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_FAIL;
    }

    /* Initialize networking stack (v5.x API) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    /* Apply static IP if configured */
    apply_static_ip(sta_netif, cfg);

    /* Initialize WiFi with default config */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* Configure STA */
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password,  cfg->wifi_pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init complete. Connecting to SSID: %s", cfg->wifi_ssid);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", cfg->wifi_ssid);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect to AP SSID:%s within timeout", cfg->wifi_ssid);
        esp_wifi_stop();
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_init_ap(void)
{
    ESP_LOGI(TAG, "Starting WiFi AP Mode for Configuration...");

    /* Initialize networking stack */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create default AP netif */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    /* Set AP IP (192.168.4.1 default) */
    esp_netif_ip_info_t info_t;
    memset(&info_t, 0, sizeof(esp_netif_ip_info_t));
    esp_netif_dhcps_stop(ap_netif);
    
    esp_netif_str_to_ip4("192.168.4.1", &info_t.ip);
    esp_netif_str_to_ip4("192.168.4.1", &info_t.gw);
    esp_netif_str_to_ip4("255.255.255.0", &info_t.netmask);
    esp_netif_set_ip_info(ap_netif, &info_t);
    esp_netif_dhcps_start(ap_netif);

    /* Initialize WiFi */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    /* Configure AP */
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "PrinterBox_Setup",
            .ssid_len = strlen("PrinterBox_Setup"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started. SSID: PrinterBox_Setup, Pass: 12345678");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (s_wifi_event_group == NULL) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}
