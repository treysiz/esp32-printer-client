/*
 * wifi_manager.h - Production-Grade WiFi Manager
 * ESP32 PrinterBox System
 *
 * Core WiFi lifecycle: init, event handling, STA connect, APSTA mode,
 * exponential-backoff reconnect, NVS-based credential storage.
 *
 * ESP-IDF v5.2+  /  ESP32-S3
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event-group bits ───────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT   BIT0   /* STA got IP */
#define WIFI_FAIL_BIT        BIT1   /* Exceeded max retries */

/* ── WiFi operating modes ───────────────────────────────────────────── */
typedef enum {
    WIFI_MGR_MODE_NONE = 0,
    WIFI_MGR_MODE_AP,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_APSTA,
} wifi_mgr_mode_t;

/* ── Configuration for the WiFi manager ─────────────────────────────── */
typedef struct {
    /* AP settings */
    char     ap_ssid[33];
    char     ap_pass[65];
    uint8_t  ap_channel;
    uint8_t  ap_max_conn;

    /* Whether to keep AP running after STA connects */
    bool     keep_ap_after_sta;

    /* Static IP (for STA) */
    bool     use_static_ip;
    char     static_ip[16];
    char     gateway[16];
    char     netmask[16];
    char     dns[16];
} wifi_mgr_config_t;

/**
 * @brief  Get default manager config.
 *         AP SSID = "PrinterBox_Setup", pass = "12345678", channel 1, 4 clients.
 */
wifi_mgr_config_t wifi_mgr_default_config(void);

/**
 * @brief  Initialize the WiFi subsystem (netif + driver + event handlers).
 *         Must be called once before any other wifi_mgr_* function.
 * @param  cfg  Manager configuration; pass NULL for defaults.
 * @return ESP_OK on success.
 */
esp_err_t wifi_mgr_init(const wifi_mgr_config_t *cfg);

/**
 * @brief  Start WiFi in AP-only mode (for initial setup).
 */
esp_err_t wifi_mgr_start_ap(void);

/**
 * @brief  Start WiFi in STA mode, connecting with NVS credentials.
 * @param  timeout_ms  Max time to wait for initial connection.
 * @return ESP_OK if connected within timeout; ESP_FAIL otherwise.
 */
esp_err_t wifi_mgr_start_sta(uint32_t timeout_ms);

/**
 * @brief  Start WiFi in APSTA mode: AP open + STA connecting.
 * @param  timeout_ms  Max time to wait for STA to connect.
 * @return ESP_OK if STA connected within timeout.
 *         ESP_ERR_TIMEOUT if timeout but AP is still running.
 */
esp_err_t wifi_mgr_start_apsta(uint32_t timeout_ms);

/**
 * @brief  Connect STA using credentials read from NVS.
 *         Called internally by start_sta / start_apsta; can also be called
 *         after saving new credentials via the web UI.
 */
esp_err_t wifi_mgr_connect_from_nvs(void);

/**
 * @brief  Connect STA with explicit SSID/password.
 */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

/**
 * @brief  Disconnect and stop STA.
 */
esp_err_t wifi_mgr_disconnect(void);

/**
 * @brief  Query current connection state.
 */
bool wifi_mgr_is_connected(void);

/**
 * @brief  Get the current WiFi mode.
 */
wifi_mgr_mode_t wifi_mgr_get_mode(void);

/**
 * @brief  Get the event group handle so external tasks can wait on
 *         WIFI_CONNECTED_BIT / WIFI_FAIL_BIT.
 */
EventGroupHandle_t wifi_mgr_get_event_group(void);

/**
 * @brief  Get the STA netif handle (for IP queries etc.).
 */
esp_netif_t *wifi_mgr_get_sta_netif(void);

/**
 * @brief  Get the AP netif handle.
 */
esp_netif_t *wifi_mgr_get_ap_netif(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
