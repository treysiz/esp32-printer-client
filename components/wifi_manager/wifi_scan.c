/*
 * wifi_scan.c - WiFi AP Scan
 * ESP32 PrinterBox / ESP-IDF v5.2+
 */

#include "wifi_scan.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WIFI_SCAN";

/* Compare for qsort: descending RSSI */
static int cmp_rssi(const void *a, const void *b)
{
    const wifi_scan_ap_t *x = (const wifi_scan_ap_t *)a;
    const wifi_scan_ap_t *y = (const wifi_scan_ap_t *)b;
    return (int)y->rssi - (int)x->rssi;
}

esp_err_t wifi_scan_start(wifi_scan_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    /* Blocking scan with internal timeout (safe, ~3s max) */
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        ESP_LOGI(TAG, "No APs found");
        return ESP_OK;
    }

    /* Cap to reasonable amount to avoid large malloc */
    uint16_t max_rec = (num > 30) ? 30 : num;
    wifi_ap_record_t *records = calloc(max_rec, sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "malloc failed for %u records", max_rec);
        esp_wifi_scan_get_ap_records(&max_rec, NULL); /* clear internal list */
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(&max_rec, records);

    /* Deduplicate + filter hidden */
    uint16_t out = 0;
    for (uint16_t i = 0; i < max_rec && out < WIFI_SCAN_MAX_AP; i++) {
        /* Skip hidden SSIDs */
        if (records[i].ssid[0] == '\0') continue;

        /* Skip duplicates (keep strongest) */
        bool dup = false;
        for (uint16_t j = 0; j < out; j++) {
            if (strcmp(result->aps[j].ssid, (char *)records[i].ssid) == 0) {
                dup = true;
                /* Update RSSI if this one is stronger */
                if (records[i].rssi > result->aps[j].rssi) {
                    result->aps[j].rssi = records[i].rssi;
                    result->aps[j].authmode = records[i].authmode;
                }
                break;
            }
        }
        if (dup) continue;

        strncpy(result->aps[out].ssid, (char *)records[i].ssid,
                sizeof(result->aps[out].ssid) - 1);
        result->aps[out].rssi     = records[i].rssi;
        result->aps[out].authmode = records[i].authmode;
        out++;
    }
    result->count = out;
    free(records);

    /* Sort by RSSI descending */
    qsort(result->aps, result->count, sizeof(wifi_scan_ap_t), cmp_rssi);

    ESP_LOGI(TAG, "Scan done: %u unique APs", result->count);
    return ESP_OK;
}
