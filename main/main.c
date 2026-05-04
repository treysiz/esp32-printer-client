/*
 * main.c - System Entry Point
 * ESP32 Printer Client System
 *
 * Initializes NVS, loads config, starts WiFi, creates FreeRTOS queues,
 * and launches the WebSocket and Printer tasks.
 */

#include "config.h"
#include "wifi_manager.h"
#include "websocket_client.h"
#include "printer.h"
#include "web_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include <string.h>

static const char *TAG = "MAIN";

/* Queue depth — how many orders can be buffered */
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
    ESP_LOGI(TAG, "[1/4] Initializing NVS...");
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
    xTaskCreate(boot_button_task, "boot_btn_task", 2048, NULL, 5, NULL);

    if (!config_is_valid()) {
        /* ── NOT CONFIGURED: AP + Web Setup Mode ────────────────────────── */
        ESP_LOGI(TAG, "No valid configuration found. Entering Setup Mode.");
        
        ESP_ERROR_CHECK(wifi_manager_init_ap());
        ESP_ERROR_CHECK(web_config_server_start(WEB_CONFIG_MODE_AP, NULL));

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, " AP Mode active.");
        ESP_LOGI(TAG, " Connect to WiFi: PrinterBox_Setup");
        ESP_LOGI(TAG, " Browse to http://192.168.4.1");
        ESP_LOGI(TAG, "========================================");
    } else {
        /* ── CONFIGURED: STA + Tasks + Local Web Mode ───────────────────── */
        ESP_LOGI(TAG, "[2/4] Starting WiFi...");
        if (wifi_manager_init(5000) != ESP_OK) {
            ESP_LOGW(TAG, "========================================");
            ESP_LOGW(TAG, " 5s Timeout! Falling back to AP Mode!");
            ESP_LOGW(TAG, "========================================");
            ESP_ERROR_CHECK(wifi_manager_init_ap());
            ESP_ERROR_CHECK(web_config_server_start(WEB_CONFIG_MODE_AP, NULL));
        } else {
            /* ── Step 3: Create queues ──────────────────────────────────────── */
            ESP_LOGI(TAG, "[3/4] Creating message queues...");

            QueueHandle_t order_queue = xQueueCreate(ORDER_QUEUE_DEPTH,
                                                     sizeof(print_order_t));
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

            ESP_LOGI(TAG, "  Order queue  : depth=%d, item_size=%u bytes",
                     ORDER_QUEUE_DEPTH, (unsigned)sizeof(print_order_t));
            ESP_LOGI(TAG, "  Result queue : depth=%d, item_size=%u bytes",
                     RESULT_QUEUE_DEPTH, (unsigned)sizeof(print_result_t));

            /* ── Step 4: Start worker tasks & web server ────────────────────── */
            ESP_LOGI(TAG, "[4/4] Starting worker tasks...");

            ESP_ERROR_CHECK(printer_task_start(order_queue, result_queue));
            ESP_ERROR_CHECK(websocket_client_task_start(order_queue, result_queue));
            ESP_ERROR_CHECK(web_config_server_start(WEB_CONFIG_MODE_STA, order_queue));

            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, " System ready. Waiting for orders...");
            ESP_LOGI(TAG, " Local config available at device IP");
            ESP_LOGI(TAG, "========================================");
        }
    }

    /* ── Main task: periodic health log ─────────────────────────────── */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); /* Log every 60s */

        ESP_LOGI(TAG, "[Health] WiFi=%s | Free heap=%lu bytes | Min free=%lu bytes",
                 wifi_manager_is_connected() ? "OK" : "DISCONNECTED",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size());
    }
}
