/*
 * wifi_manager.c - Production-Grade WiFi Manager
 * ESP32 PrinterBox / ESP-IDF v5.2+ / ESP32-S3
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>
#include "esp_mac.h"

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_eg       = NULL;
static esp_netif_t       *s_sta_nif  = NULL;
static esp_netif_t       *s_ap_nif   = NULL;
static wifi_mgr_mode_t    s_mode     = WIFI_MGR_MODE_NONE;
static wifi_mgr_config_t  s_cfg;
static bool                s_inited  = false;

#define MAX_RETRY 10
static int  s_retry   = 0;
static bool s_sta_on  = false;
static bool s_manual  = false;

static const int BK[] = {1,3,5,10};
#define BK_N (sizeof(BK)/sizeof(BK[0]))

static TimerHandle_t s_rtimer = NULL;

static void wifi_evt(void*,esp_event_base_t,int32_t,void*);
static void ip_evt(void*,esp_event_base_t,int32_t,void*);
static void rtimer_cb(TimerHandle_t);
static void do_static_ip(void);
static void do_fallback(void);

static void setup_ap_netif(void)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_dhcps_stop(s_ap_nif);
    esp_netif_str_to_ip4("192.168.4.1",   &ip.ip);
    esp_netif_str_to_ip4("192.168.4.1",   &ip.gw);
    esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
    esp_netif_set_ip_info(s_ap_nif, &ip);
    esp_netif_dhcps_start(s_ap_nif);
}

static void fill_ap_cfg(wifi_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strncpy((char*)c->ap.ssid, s_cfg.ap_ssid, sizeof(c->ap.ssid)-1);
    c->ap.ssid_len = strlen(s_cfg.ap_ssid);
    strncpy((char*)c->ap.password, s_cfg.ap_pass, sizeof(c->ap.password)-1);
    c->ap.channel = s_cfg.ap_channel;
    c->ap.max_connection = s_cfg.ap_max_conn;
    c->ap.authmode = strlen(s_cfg.ap_pass) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
}

wifi_mgr_config_t wifi_mgr_default_config(void)
{
    wifi_mgr_config_t c = {0};
    strncpy(c.ap_ssid, "PrinterBox_Setup", sizeof(c.ap_ssid)-1);
    strncpy(c.ap_pass, "12345678", sizeof(c.ap_pass)-1);
    c.ap_channel = 1; c.ap_max_conn = 4;
    return c;
}

esp_err_t wifi_mgr_init(const wifi_mgr_config_t *cfg)
{
    if (s_inited) return ESP_OK;
    s_cfg = cfg ? *cfg : wifi_mgr_default_config();
    s_eg = xEventGroupCreate();
    if (!s_eg) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_nif = esp_netif_create_default_wifi_sta();
    s_ap_nif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_evt, NULL, NULL));

    s_rtimer = xTimerCreate("wrc", pdMS_TO_TICKS(1000), pdFALSE, NULL, rtimer_cb);
    s_inited = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_mgr_start_ap(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    setup_ap_netif();
    wifi_config_t ac; fill_ap_cfg(&ac);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ac));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_mode = WIFI_MGR_MODE_AP;
    ESP_LOGI(TAG, "AP started SSID:%s", s_cfg.ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_start_sta(uint32_t timeout_ms)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    do_static_ip();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_sta_on = true; s_mode = WIFI_MGR_MODE_STA; s_manual = false;
    esp_err_t e = wifi_mgr_connect_from_nvs();
    if (e != ESP_OK) return e;
    EventBits_t b = xEventGroupWaitBits(s_eg, WIFI_CONNECTED_BIT|WIFI_FAIL_BIT,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (b & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_mgr_start_apsta(uint32_t timeout_ms)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    setup_ap_netif();
    wifi_config_t ac; fill_ap_cfg(&ac);
    do_static_ip();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ac));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_sta_on = true; s_mode = WIFI_MGR_MODE_APSTA; s_manual = false;
    esp_err_t e = wifi_mgr_connect_from_nvs();
    if (e != ESP_OK) { ESP_LOGW(TAG,"APSTA STA fail"); return ESP_ERR_TIMEOUT; }
    EventBits_t b = xEventGroupWaitBits(s_eg, WIFI_CONNECTED_BIT|WIFI_FAIL_BIT,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (b & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_mgr_connect_from_nvs(void)
{
    char ssid[33]={0}, pass[65]={0};
    nvs_handle_t h;
    esp_err_t e = nvs_open("printer_cfg", NVS_READONLY, &h);
    if (e != ESP_OK) { ESP_LOGE(TAG,"NVS open fail"); return e; }
    size_t l = sizeof(ssid);
    e = nvs_get_str(h, "wifi_ssid", ssid, &l);
    if (e != ESP_OK || !strlen(ssid)) { nvs_close(h); return ESP_ERR_NOT_FOUND; }
    l = sizeof(pass);
    nvs_get_str(h, "wifi_pass", pass, &l);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS SSID=%s", ssid);
    return wifi_mgr_connect(ssid, pass);
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!ssid || !strlen(ssid)) return ESP_ERR_INVALID_ARG;
    wifi_config_t sc = {0};
    strncpy((char*)sc.sta.ssid, ssid, sizeof(sc.sta.ssid)-1);
    if (password && strlen(password)) {
        strncpy((char*)sc.sta.password, password, sizeof(sc.sta.password)-1);
        sc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT|WIFI_FAIL_BIT);
    s_retry = 0; s_manual = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sc));
    if (s_sta_on) { esp_wifi_disconnect(); vTaskDelay(pdMS_TO_TICKS(50)); esp_wifi_connect(); }
    return ESP_OK;
}

esp_err_t wifi_mgr_disconnect(void)
{
    s_manual = true;
    if (s_rtimer) xTimerStop(s_rtimer, 0);
    esp_wifi_disconnect();
    xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return s_eg ? (xEventGroupGetBits(s_eg) & WIFI_CONNECTED_BIT) != 0 : false;
}
wifi_mgr_mode_t wifi_mgr_get_mode(void) { return s_mode; }
EventGroupHandle_t wifi_mgr_get_event_group(void) { return s_eg; }
esp_netif_t *wifi_mgr_get_sta_netif(void) { return s_sta_nif; }
esp_netif_t *wifi_mgr_get_ap_netif(void)  { return s_ap_nif; }

/* ── internals ─────────────────────────────────────────────────────── */
static void do_static_ip(void)
{
    if (!s_cfg.use_static_ip || !s_sta_nif) return;
    esp_netif_dhcpc_stop(s_sta_nif);
    esp_netif_ip_info_t ip={0};
    esp_netif_str_to_ip4(s_cfg.static_ip, &ip.ip);
    esp_netif_str_to_ip4(s_cfg.gateway, &ip.gw);
    esp_netif_str_to_ip4(s_cfg.netmask, &ip.netmask);
    esp_netif_set_ip_info(s_sta_nif, &ip);
    if (strlen(s_cfg.dns)) {
        esp_netif_dns_info_t d={0};
        esp_netif_str_to_ip4(s_cfg.dns,(esp_ip4_addr_t*)&d.ip.u_addr.ip4);
        d.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_nif, ESP_NETIF_DNS_MAIN, &d);
    }
}

static void rtimer_cb(TimerHandle_t t) { (void)t; esp_wifi_connect(); }

static void sched_reconn(void)
{
    int i = s_retry-1; if(i<0) i=0; if(i>=(int)BK_N) i=BK_N-1;
    ESP_LOGW(TAG,"Reconnect in %ds (%d/%d)", BK[i], s_retry, MAX_RETRY);
    xTimerChangePeriod(s_rtimer, pdMS_TO_TICKS(BK[i]*1000), 0);
    xTimerStart(s_rtimer, 0);
}

static void do_fallback(void)
{
    ESP_LOGW(TAG,"Max retries exceeded, fallback to AP");
    xEventGroupSetBits(s_eg, WIFI_FAIL_BIT);
    if (s_mode == WIFI_MGR_MODE_STA) {
        esp_wifi_disconnect(); esp_wifi_stop();
        setup_ap_netif();
        wifi_config_t ac; fill_ap_cfg(&ac);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ac);
        esp_wifi_start();
        s_mode = WIFI_MGR_MODE_AP; s_sta_on = false;
    }
}

static void wifi_evt(void *a, esp_event_base_t b, int32_t id, void *d)
{
    switch(id) {
    case WIFI_EVENT_STA_START:
        s_sta_on = true; esp_wifi_connect(); break;
    case WIFI_EVENT_STA_DISCONNECTED:
        xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
        if (s_manual) break;
        s_retry++;
        if (s_retry > MAX_RETRY) do_fallback(); else sched_reconn();
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = d;
        ESP_LOGI(TAG,"AP client+" MACSTR, MAC2STR(e->mac)); break; }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = d;
        ESP_LOGI(TAG,"AP client-" MACSTR, MAC2STR(e->mac)); break; }
    default: break;
    }
}

static void ip_evt(void *a, esp_event_base_t b, int32_t id, void *d)
{
    ip_event_got_ip_t *e = d;
    ESP_LOGI(TAG,"Got IP:" IPSTR, IP2STR(&e->ip_info.ip));
    s_retry = 0;
    xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
    xEventGroupClearBits(s_eg, WIFI_FAIL_BIT);
    if (s_rtimer) xTimerStop(s_rtimer, 0);
}
