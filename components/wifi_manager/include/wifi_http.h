/*
 * wifi_http.h - WiFi HTTP REST Endpoints
 * ESP32 PrinterBox System
 *
 * Registers HTTP handlers for:
 *   GET  /wifi_scan    – JSON array of nearby APs
 *   GET  /wifi_status  – current WiFi connection status
 *   POST /wifi_connect – connect to an AP with ssid/password
 */

#ifndef WIFI_HTTP_H
#define WIFI_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register all WiFi-related HTTP URI handlers on the given server.
 * @param  server  Running httpd_handle_t.
 * @return ESP_OK on success.
 */
esp_err_t wifi_http_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_HTTP_H */
