/*
 * wifi_status.c - WiFi Status Query
 * ESP32 PrinterBox / ESP-IDF v5.2+
 */

#include "wifi_status.h"
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>



wifi_quality_t wifi_rssi_to_quality(int8_t rssi)
{
    if (rssi > -65)  return WIFI_QUALITY_GOOD;
    if (rssi > -75)  return WIFI_QUALITY_NORMAL;
    if (rssi > -85)  return WIFI_QUALITY_WEAK;
    return WIFI_QUALITY_POOR;
}

const char *wifi_quality_to_str(wifi_quality_t q)
{
    switch (q) {
    case WIFI_QUALITY_GOOD:   return "good";
    case WIFI_QUALITY_NORMAL: return "normal";
    case WIFI_QUALITY_WEAK:   return "weak";
    case WIFI_QUALITY_POOR:   return "poor";
    default:                  return "none";
    }
}

esp_err_t wifi_status_get(wifi_status_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    info->connected = wifi_mgr_is_connected();

    /* Mode string */
    switch (wifi_mgr_get_mode()) {
    case WIFI_MGR_MODE_AP:   strncpy(info->mode, "AP",    sizeof(info->mode)-1); break;
    case WIFI_MGR_MODE_STA:  strncpy(info->mode, "STA",   sizeof(info->mode)-1); break;
    case WIFI_MGR_MODE_APSTA:strncpy(info->mode, "APSTA", sizeof(info->mode)-1); break;
    default:                 strncpy(info->mode, "NONE",  sizeof(info->mode)-1); break;
    }

    if (info->connected) {
        /* SSID + RSSI from current association */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            strncpy(info->ssid, (char *)ap.ssid, sizeof(info->ssid) - 1);
            info->rssi = ap.rssi;
        }

        /* IP */
        esp_netif_t *nif = wifi_mgr_get_sta_netif();
        if (nif) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
                esp_ip4addr_ntoa(&ip.ip, info->ip, sizeof(info->ip));
            }
        }

        info->quality = wifi_rssi_to_quality(info->rssi);
    } else {
        info->quality = WIFI_QUALITY_NONE;
    }

    strncpy(info->quality_str, wifi_quality_to_str(info->quality),
            sizeof(info->quality_str) - 1);

    return ESP_OK;
}
