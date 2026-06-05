/*
 * wifi_scan.h - WiFi AP Scan Module
 * ESP32 PrinterBox System
 *
 * Provides non-blocking WiFi scanning with deduplication, hidden-SSID
 * filtering, and RSSI-sorted results.
 */

#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SCAN_MAX_AP  20

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode;   /* wifi_auth_mode_t */
} wifi_scan_ap_t;

typedef struct {
    wifi_scan_ap_t  aps[WIFI_SCAN_MAX_AP];
    uint16_t        count;
} wifi_scan_result_t;

/**
 * @brief  Perform a blocking WiFi scan (with short timeout to avoid WDT).
 *         Filters hidden SSIDs, deduplicates, sorts by RSSI descending.
 * @param[out] result  Populated scan result.
 * @return ESP_OK on success.
 */
esp_err_t wifi_scan_start(wifi_scan_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SCAN_H */
