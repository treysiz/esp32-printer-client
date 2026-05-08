/*
 * wifi_portal.h - Captive Portal (DNS Hijack + HTTP Redirect)
 * ESP32 PrinterBox System
 *
 * When AP mode is active, all DNS queries resolve to 192.168.4.1
 * and unknown HTTP requests are 302-redirected to the config page.
 * This causes phones/laptops to auto-open the config page.
 */

#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start the captive-portal DNS server (UDP 53).
 *         All queries return 192.168.4.1.
 * @return ESP_OK on success.
 */
esp_err_t wifi_portal_dns_start(void);

/**
 * @brief  Stop the captive-portal DNS server.
 */
esp_err_t wifi_portal_dns_stop(void);

/**
 * @brief  Register the HTTP 302 catch-all redirect handler.
 *         Must be called AFTER all normal URI handlers are registered,
 *         because this uses the wildcard URI.
 * @param  server  Running httpd_handle_t.
 * @return ESP_OK on success.
 */
esp_err_t wifi_portal_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PORTAL_H */
