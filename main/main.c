/*
 * main.c - System Entry Point
 * ESP32 Printer Client System
 *
 * Initializes NVS, loads config, starts WiFi (via wifi_manager component),
 * creates FreeRTOS queues, and launches the WebSocket and Printer tasks.
 */

#include "config.h"
#include "wifi_manager.h"
#include "wifi_http.h"
#include "wifi_portal.h"
#include "http_client.h"
#include "printer.h"
#include "web_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "MAIN";

#define ORDER_QUEUE_DEPTH   10
#define RESULT_QUEUE_DEPTH  10
#define BOOT_BUTTON_GPIO    GPIO_NUM_0

/* ── BOOT Button Task ───────────────────────────────────────────────── */
static void boot_button_task(void *arg)
{
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    int press_duration_ms = 0;

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            press_duration_ms += 100;
            if (press_duration_ms >= 5000) {
                ESP_LOGW(TAG, "BOOT button held for 5s. Clearing config!");
                config_clear(); /* This function restarts the ESP32 */
            }
        } else {
            press_duration_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32 Printer Client v%s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "========================================");

    /* ── Step 1: Initialize NVS and load config ─────────────────────── */
    ESP_LOGI(TAG, "[1/5] Initializing NVS...");
    ESP_ERROR_CHECK(config_init());

    device_config_t cfg;
    ESP_ERROR_CHECK(config_load(&cfg));

    ESP_LOGI(TAG, "Config loaded:");
    ESP_LOGI(TAG, "  WiFi SSID  : %s", cfg.wifi_ssid);
    ESP_LOGI(TAG, "  Store ID   : %s", cfg.store_id);
    ESP_LOGI(TAG, "  Device ID  : %s", cfg.device_id);
    ESP_LOGI(TAG, "  Printer    : %s:%u", cfg.printer_ip, cfg.printer_port);
    ESP_LOGI(TAG, "  Server URL : %s", cfg.server_url);
    ESP_LOGI(TAG, "  Static IP  : %s", cfg.use_static_ip ? "enabled" : "disabled");

    /* Start BOOT button monitor task */
    xTaskCreate(boot_button_task, "boot_btn_task", 8192, NULL, 5, NULL);

    /* ── Step 2: Initialize WiFi manager ────────────────────────────── */
    ESP_LOGI(TAG, "[2/5] Initializing WiFi manager...");
    wifi_mgr_config_t wmcfg = wifi_mgr_default_config();

    /* Pass static IP settings from device config */
    wmcfg.use_static_ip = cfg.use_static_ip;
    if (cfg.use_static_ip) {
        strncpy(wmcfg.static_ip, cfg.static_ip, sizeof(wmcfg.static_ip) - 1);
        strncpy(wmcfg.gateway,   cfg.gateway,   sizeof(wmcfg.gateway) - 1);
        strncpy(wmcfg.netmask,   cfg.netmask,   sizeof(wmcfg.netmask) - 1);
        strncpy(wmcfg.dns,       cfg.dns,       sizeof(wmcfg.dns) - 1);
    }

    ESP_ERROR_CHECK(wifi_mgr_init(&wmcfg));

    if (!config_is_valid()) {
        /* ── NOT CONFIGURED: AP + Captive Portal ───────────────────────── */
        ESP_LOGI(TAG, "No valid config. Entering AP Setup Mode.");

        ESP_ERROR_CHECK(wifi_mgr_start_ap());
        ESP_ERROR_CHECK(wifi_portal_dns_start());
        ESP_ERROR_CHECK(web_config_server_start(WEB_CONFIG_MODE_AP, NULL));

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, " AP Mode active.");
        ESP_LOGI(TAG, " Connect to WiFi: PrinterBox_Setup");
        ESP_LOGI(TAG, " Browse to http://192.168.4.1");
        ESP_LOGI(TAG, "========================================");
    } else {
        /* ── CONFIGURED: APSTA mode (STA + AP fallback) ────────────────── */
        ESP_LOGI(TAG, "[3/5] Starting APSTA WiFi...");

        esp_err_t wifi_err = wifi_mgr_start_apsta(8000);

        if (wifi_err == ESP_OK) {
            ESP_LOGI(TAG, "STA connected successfully");
        } else {
            ESP_LOGW(TAG, "STA not connected yet, AP active for config");
            /* Captive portal for fallback */
            wifi_portal_dns_start();
        }

        /* ── Step 4: Create queues ──────────────────────────────────────── */
        ESP_LOGI(TAG, "[4/5] Creating message queues...");

        /* The order queue carries POINTERS to heap-allocated orders, so the
         * payload (large binary ESC/POS data) never gets copied by value. */
        QueueHandle_t order_queue = xQueueCreate(ORDER_QUEUE_DEPTH,
                                                  sizeof(print_order_t *));
        if (order_queue == NULL) {
            ESP_LOGE(TAG, "FATAL: Failed to create order queue");
            esp_restart();
        }

        QueueHandle_t result_queue = xQueueCreate(RESULT_QUEUE_DEPTH,
                                                   sizeof(print_result_t));
        if (result_queue == NULL) {
            ESP_LOGE(TAG, "FATAL: Failed to create result queue");
            esp_restart();
        }

        /* ── Step 5: Start worker tasks & web server ────────────────────── */
        ESP_LOGI(TAG, "[5/5] Starting worker tasks...");

        ESP_ERROR_CHECK(printer_task_start(order_queue, result_queue));

        if (wifi_mgr_is_connected()) {
            /* Non-fatal: if the client can't start (e.g. out of RAM for the
             * response buffer), keep the web config UI alive so the user can
             * still reconfigure rather than crash-looping. */
            esp_err_t hc_err = http_client_task_start(order_queue, result_queue);
            if (hc_err != ESP_OK) {
                ESP_LOGE(TAG, "HTTP client failed to start: %s",
                         esp_err_to_name(hc_err));
            }
        }

        ESP_ERROR_CHECK(web_config_server_start(
            wifi_mgr_is_connected() ? WEB_CONFIG_MODE_STA : WEB_CONFIG_MODE_AP,
            order_queue));

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, " System ready. Waiting for orders...");
        ESP_LOGI(TAG, "========================================");
    }

    /* Main task: periodic health log */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        ESP_LOGI(TAG, "[Health] WiFi=%s | Free heap=%lu bytes | Min free=%lu bytes",
                 wifi_mgr_is_connected() ? "OK" : "DISCONNECTED",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size());
    }
}
