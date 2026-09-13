/*
 * web_config.h - Web Configuration UI Module
 * ESP32 Printer Client System
 *
 * Provides a local HTTP server for initial AP configuration
 * and STA mode status/management.
 */

#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEB_CONFIG_MODE_AP,
    WEB_CONFIG_MODE_STA
} web_config_mode_t;

/**
 * @brief Start the HTTP server for web configuration.
 *
 * @param mode        WEB_CONFIG_MODE_AP for setup form, 
 *                    WEB_CONFIG_MODE_STA for status and management.
 * @param order_queue Queue handle to post test prints (only used in STA mode).
 * @return ESP_OK on success
 */
esp_err_t web_config_server_start(web_config_mode_t mode, QueueHandle_t order_queue);

/* main.c 启动拉单任务后调用，让 /api/status 能如实报 poller_running。
 * ⚠ 这一位存在的理由：拉单任务曾被一个 if 静默跳过，而外面完全看不出来。 */
void web_config_set_poller_running(bool running);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CONFIG_H */
