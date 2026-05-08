/*
 * wifi_status.h - WiFi Status Query
 * ESP32 PrinterBox System
 *
 * Provides a structured snapshot of the current WiFi state:
 * mode, SSID, RSSI, IP, quality grade.
 */

#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_QUALITY_GOOD = 0,   /* RSSI > -65  */
    WIFI_QUALITY_NORMAL,     /* -65 ~ -75   */
    WIFI_QUALITY_WEAK,       /* -75 ~ -85   */
    WIFI_QUALITY_POOR,       /* < -85       */
    WIFI_QUALITY_NONE,       /* Not connected */
} wifi_quality_t;

typedef struct {
    bool         connected;
    char         mode[8];        /* "AP", "STA", "APSTA" */
    char         ssid[33];
    int8_t       rssi;
    wifi_quality_t quality;
    char         quality_str[8]; /* "good","normal","weak","poor" */
    char         ip[16];
} wifi_status_info_t;

/**
 * @brief  Get a snapshot of the current WiFi status.
 * @param[out] info  Populated status structure.
 * @return ESP_OK on success.
 */
esp_err_t wifi_status_get(wifi_status_info_t *info);

/**
 * @brief  Convert RSSI to a quality enum.
 */
wifi_quality_t wifi_rssi_to_quality(int8_t rssi);

/**
 * @brief  Convert quality enum to string.
 */
const char *wifi_quality_to_str(wifi_quality_t q);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STATUS_H */
