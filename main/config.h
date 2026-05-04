/*
 * config.h - NVS Configuration Manager
 * ESP32 Printer Client System
 *
 * Stores and retrieves device configuration from NVS.
 * Falls back to compile-time defaults when NVS is empty.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Firmware Version ───────────────────────────────────────────────── */
#define FIRMWARE_VERSION "1.0.0"

/* ── Compile-time Defaults (used when NVS is empty) ─────────────────── */
#define DEFAULT_WIFI_SSID       "YourWiFi"
#define DEFAULT_WIFI_PASS       "YourPassword"
#define DEFAULT_STORE_ID        "store_001"
#define DEFAULT_DEVICE_ID       "printer_001"
#define DEFAULT_PRINTER_IP      "192.168.1.100"
#define DEFAULT_PRINTER_PORT    9100
#define DEFAULT_SERVER_URL      "ws://your-server.com:3001/printer"

/* Static IP defaults (disabled by default) */
#define DEFAULT_USE_STATIC_IP   false
#define DEFAULT_STATIC_IP       "192.168.1.200"
#define DEFAULT_GATEWAY         "192.168.1.1"
#define DEFAULT_NETMASK         "255.255.255.0"
#define DEFAULT_DNS             "8.8.8.8"

/* ── Maximum field lengths ──────────────────────────────────────────── */
#define MAX_SSID_LEN        32
#define MAX_PASS_LEN        64
#define MAX_STORE_ID_LEN    32
#define MAX_DEVICE_ID_LEN   32
#define MAX_IP_LEN          16
#define MAX_URL_LEN         128

/* ── Configuration Structure ────────────────────────────────────────── */
typedef struct {
    /* WiFi */
    char wifi_ssid[MAX_SSID_LEN + 1];
    char wifi_pass[MAX_PASS_LEN + 1];

    /* Device identity */
    char store_id[MAX_STORE_ID_LEN + 1];
    char device_id[MAX_DEVICE_ID_LEN + 1];

    /* Printer */
    char printer_ip[MAX_IP_LEN + 1];
    uint16_t printer_port;

    /* Server */
    char server_url[MAX_URL_LEN + 1];

    /* Static IP (optional) */
    bool use_static_ip;
    char static_ip[MAX_IP_LEN + 1];
    char gateway[MAX_IP_LEN + 1];
    char netmask[MAX_IP_LEN + 1];
    char dns[MAX_IP_LEN + 1];
} device_config_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * @brief Initialize the NVS subsystem. Must be called before load/save.
 */
esp_err_t config_init(void);

/**
 * @brief Load configuration from NVS into the global config struct.
 *        Falls back to compile-time defaults if keys are missing.
 * @param[out] cfg  Pointer to config struct to populate
 */
esp_err_t config_load(device_config_t *cfg);

/**
 * @brief Save the current configuration to NVS.
 * @param[in] cfg  Pointer to config struct to persist
 */
esp_err_t config_save(const device_config_t *cfg);

/**
 * @brief Check if the current configuration is valid (e.g. WiFi SSID is set).
 */
bool config_is_valid(void);

/**
 * @brief Clear all configuration from NVS and restart the device.
 */
void config_clear(void);

/**
 * @brief Get a read-only pointer to the global configuration.
 *        Valid only after config_load() has been called.
 */
const device_config_t *config_get(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
