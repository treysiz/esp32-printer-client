/*
 * wifi_manager.h - WiFi Station Manager
 * ESP32 Printer Client System
 *
 * Manages WiFi STA connection with auto-reconnect.
 * Supports optional static IP configuration.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_event.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi as Station (STA) and connect to the configured AP.
 * @param timeout_ms  Time to wait for connection in milliseconds.
 * @return ESP_OK if connected successfully within timeout, ESP_FAIL otherwise.
 */
esp_err_t wifi_manager_init(uint32_t timeout_ms);

/**
 * @brief Initialize and start WiFi in AP mode for configuration.
 *        SSID: PrinterBox_Setup, Password: 12345678, IP: 192.168.4.1
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init_ap(void);

/**
 * @brief Check whether WiFi is currently connected and has an IP.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Event group bit indicating WiFi is connected with IP.
 *        Other tasks can wait on this before starting network operations.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT  BIT0

/**
 * @brief Get the WiFi event group handle.
 *        External modules can use xEventGroupWaitBits() on WIFI_CONNECTED_BIT.
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
