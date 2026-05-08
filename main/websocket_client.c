/*
 * websocket_client.c - WebSocket Client Manager Implementation
 * ESP32 Printer Client System
 *
 * Uses esp_websocket_client (ESP-IDF v5.x component).
 * Handles connection, registration, message parsing, deduplication,
 * result forwarding, heartbeat, and exponential-backoff reconnect.
 */

#include "websocket_client.h"
#include "config.h"
#include "printer.h"
#include "wifi_manager.h"
#include "wifi_status.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "WS_CLIENT";

/* ── Internal state ─────────────────────────────────────────────────── */
typedef struct {
    QueueHandle_t order_queue;
    QueueHandle_t result_queue;
    esp_websocket_client_handle_t client;
    bool connected;
    int reconnect_delay_ms;

    /* Order deduplication ring buffer */
    char recent_orders[ORDER_DEDUP_SIZE][MAX_ORDER_ID_LEN + 1];
    int  dedup_index;
} ws_state_t;

static ws_state_t s_ws = {0};

/* ── Deduplication helpers ──────────────────────────────────────────── */

/**
 * @brief Check if an order_id was recently processed.
 * @return true if duplicate
 */
static bool is_duplicate_order(const char *order_id)
{
    for (int i = 0; i < ORDER_DEDUP_SIZE; i++) {
        if (strcmp(s_ws.recent_orders[i], order_id) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Record an order_id in the dedup ring buffer.
 */
static void record_order_id(const char *order_id)
{
    strncpy(s_ws.recent_orders[s_ws.dedup_index],
            order_id, MAX_ORDER_ID_LEN);
    s_ws.recent_orders[s_ws.dedup_index][MAX_ORDER_ID_LEN] = '\0';
    s_ws.dedup_index = (s_ws.dedup_index + 1) % ORDER_DEDUP_SIZE;
}

/* ── Send registration message ──────────────────────────────────────── */
static void send_register(void)
{
    const device_config_t *cfg = config_get();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create registration JSON");
        return;
    }

    cJSON_AddStringToObject(root, "type",             "register");
    cJSON_AddStringToObject(root, "store_id",         cfg->store_id);
    cJSON_AddStringToObject(root, "device_id",        cfg->device_id);
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize registration JSON");
        return;
    }

    ESP_LOGI(TAG, "Sending registration: %s", json_str);
    esp_websocket_client_send_text(s_ws.client, json_str,
                                   (int)strlen(json_str), pdMS_TO_TICKS(3000));
    free(json_str);
}

/* ── Send print result to server ────────────────────────────────────── */
static void send_print_result(const print_result_t *result)
{
    if (result == NULL) return;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create result JSON");
        return;
    }

    cJSON_AddStringToObject(root, "type", "print_result");
    cJSON_AddStringToObject(root, "order_id", result->order_id);
    cJSON_AddStringToObject(root, "status",
                            result->status == PRINT_STATUS_SUCCESS ?
                            "success" : "failed");

    if (result->status == PRINT_STATUS_FAILED && result->reason[0] != '\0') {
        cJSON_AddStringToObject(root, "reason", result->reason);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize result JSON");
        return;
    }

    ESP_LOGI(TAG, "Sending result: %s", json_str);
    if (s_ws.connected && s_ws.client != NULL) {
        esp_websocket_client_send_text(s_ws.client, json_str,
                                       (int)strlen(json_str), pdMS_TO_TICKS(3000));
    } else {
        ESP_LOGW(TAG, "WebSocket not connected, result dropped for [%s]",
                 result->order_id);
    }

    free(json_str);
}

/* ── Process incoming message from server ───────────────────────────── */
static void handle_incoming_message(const char *data, int len)
{
    if (data == NULL || len <= 0) {
        ESP_LOGW(TAG, "Empty message received");
        return;
    }

    /* Parse JSON with safety */
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed: %.100s", data);
        return;
    }

    /* Extract "type" field */
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == NULL) {
        ESP_LOGW(TAG, "Message missing 'type' field");
        cJSON_Delete(root);
        return;
    }

    const char *msg_type = type_item->valuestring;

    if (strcmp(msg_type, "print") == 0) {
        /* ── Handle print order ─────────────────────────────────────── */
        cJSON *order_id_item = cJSON_GetObjectItemCaseSensitive(root, "order_id");
        cJSON *content_item  = cJSON_GetObjectItemCaseSensitive(root, "content");

        if (!cJSON_IsString(order_id_item) || order_id_item->valuestring == NULL) {
            ESP_LOGE(TAG, "Print message missing 'order_id'");
            cJSON_Delete(root);
            return;
        }

        if (!cJSON_IsString(content_item) || content_item->valuestring == NULL) {
            ESP_LOGE(TAG, "Print message missing 'content'");
            cJSON_Delete(root);
            return;
        }

        const char *order_id = order_id_item->valuestring;
        const char *content  = content_item->valuestring;

        /* Deduplication check */
        if (is_duplicate_order(order_id)) {
            ESP_LOGW(TAG, "Duplicate order [%s] ignored", order_id);
            cJSON_Delete(root);
            return;
        }

        /* Build order struct */
        print_order_t order;
        memset(&order, 0, sizeof(order));
        strncpy(order.order_id, order_id, MAX_ORDER_ID_LEN);
        strncpy(order.content,  content,  MAX_PRINT_CONTENT_LEN);

        /* Enqueue for printing (non-blocking) */
        if (xQueueSend(s_ws.order_queue, &order, pdMS_TO_TICKS(500)) == pdTRUE) {
            record_order_id(order_id);
            ESP_LOGI(TAG, "Order [%s] enqueued for printing (%d bytes)",
                     order_id, (int)strlen(content));
        } else {
            ESP_LOGE(TAG, "Order queue full! Order [%s] dropped!", order_id);

            /* Report failure to server */
            print_result_t fail_result;
            memset(&fail_result, 0, sizeof(fail_result));
            strncpy(fail_result.order_id, order_id, MAX_ORDER_ID_LEN);
            fail_result.status = PRINT_STATUS_FAILED;
            strncpy(fail_result.reason, "queue_full", sizeof(fail_result.reason) - 1);
            send_print_result(&fail_result);
        }

    } else if (strcmp(msg_type, "ping") == 0) {
        /* ── Handle server ping → respond with pong ─────────────────── */
        cJSON *pong = cJSON_CreateObject();
        if (pong) {
            cJSON_AddStringToObject(pong, "type", "pong");
            char *pong_str = cJSON_PrintUnformatted(pong);
            cJSON_Delete(pong);
            if (pong_str) {
                esp_websocket_client_send_text(s_ws.client, pong_str,
                                               (int)strlen(pong_str),
                                               pdMS_TO_TICKS(1000));
                free(pong_str);
            }
        }
        ESP_LOGD(TAG, "Responded to server ping");

    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", msg_type);
    }

    cJSON_Delete(root);
}

/* ── WebSocket event handler ────────────────────────────────────────── */
static void ws_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket CONNECTED");
        s_ws.connected = true;
        s_ws.reconnect_delay_ms = 1000; /* Reset backoff on success */
        send_register();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket DISCONNECTED");
        s_ws.connected = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ws_data->op_code == 0x01 || ws_data->op_code == 0x00) {
            /* Text frame (or continuation) */
            if (ws_data->data_ptr && ws_data->data_len > 0) {
                handle_incoming_message(ws_data->data_ptr, ws_data->data_len);
            }
        } else if (ws_data->op_code == 0x0A) {
            /* Pong frame (response to our ping) */
            ESP_LOGD(TAG, "Received pong from server");
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket ERROR");
        s_ws.connected = false;
        break;

    default:
        break;
    }
}

/* ── Result forwarder task ──────────────────────────────────────────── */
static void result_forward_task(void *arg)
{
    ws_state_t *state = (ws_state_t *)arg;
    print_result_t result;

    ESP_LOGI(TAG, "Result forwarder task started");

    while (1) {
        if (xQueueReceive(state->result_queue, &result,
                          pdMS_TO_TICKS(500)) == pdTRUE) {
            send_print_result(&result);
        }
    }
}

/* ── Main WebSocket client task ─────────────────────────────────────── */
static void ws_client_task(void *arg)
{
    ws_state_t *state = (ws_state_t *)arg;
    const device_config_t *cfg = config_get();

    ESP_LOGI(TAG, "WebSocket task started, server: %s", cfg->server_url);

    /* Initialize dedup buffer */
    memset(state->recent_orders, 0, sizeof(state->recent_orders));
    state->dedup_index = 0;
    state->reconnect_delay_ms = 1000;

    while (1) {
        /* Wait for WiFi to be connected */
        EventGroupHandle_t wifi_eg = wifi_mgr_get_event_group();
        if (wifi_eg) {
            xEventGroupWaitBits(wifi_eg, WIFI_CONNECTED_BIT,
                                pdFALSE, pdTRUE, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "WiFi ready, connecting WebSocket...");

        /* Configure WebSocket client */
        esp_websocket_client_config_t ws_cfg = {
            .uri                    = cfg->server_url,
            .reconnect_timeout_ms   = 0,     /* We manage reconnect ourselves */
            .network_timeout_ms     = 10000,
            .ping_interval_sec      = 30,    /* Built-in ping/pong heartbeat */
            .pingpong_timeout_sec   = 10,
            .disable_auto_reconnect = true,  /* We handle reconnect with backoff */
        };

        state->client = esp_websocket_client_init(&ws_cfg);
        if (state->client == NULL) {
            ESP_LOGE(TAG, "Failed to init WebSocket client");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Register event handler */
        esp_websocket_register_events(state->client,
                                      WEBSOCKET_EVENT_ANY,
                                      ws_event_handler, NULL);

        /* Start connection */
        esp_err_t err = esp_websocket_client_start(state->client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
            esp_websocket_client_destroy(state->client);
            state->client = NULL;
            vTaskDelay(pdMS_TO_TICKS(state->reconnect_delay_ms));

            /* Exponential backoff: 1s → 2s → 4s → ... → 30s max */
            state->reconnect_delay_ms *= 2;
            if (state->reconnect_delay_ms > 30000) {
                state->reconnect_delay_ms = 30000;
            }
            continue;
        }

        /* Stay connected — monitor for disconnect */
        while (esp_websocket_client_is_connected(state->client)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* Disconnected — clean up and reconnect */
        ESP_LOGW(TAG, "WebSocket lost, reconnecting in %d ms...",
                 state->reconnect_delay_ms);

        state->connected = false;
        esp_websocket_client_stop(state->client);
        esp_websocket_client_destroy(state->client);
        state->client = NULL;

        vTaskDelay(pdMS_TO_TICKS(state->reconnect_delay_ms));

        /* Exponential backoff */
        state->reconnect_delay_ms *= 2;
        if (state->reconnect_delay_ms > 30000) {
            state->reconnect_delay_ms = 30000;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t websocket_client_task_start(QueueHandle_t order_queue,
                                      QueueHandle_t result_queue)
{
    if (order_queue == NULL || result_queue == NULL) {
        ESP_LOGE(TAG, "websocket_client_task_start: NULL queue");
        return ESP_ERR_INVALID_ARG;
    }

    s_ws.order_queue  = order_queue;
    s_ws.result_queue = result_queue;
    s_ws.connected    = false;
    s_ws.client       = NULL;

    /* Create the main WebSocket task */
    BaseType_t ret = xTaskCreate(
        ws_client_task,
        "ws_client_task",
        6144,           /* Stack: 6KB (JSON parsing needs more) */
        &s_ws,
        5,
        NULL
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ws_client_task");
        return ESP_FAIL;
    }

    /* Create the result forwarder task */
    ret = xTaskCreate(
        result_forward_task,
        "ws_result_fwd",
        3072,
        &s_ws,
        4,
        NULL
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create result_forward_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebSocket client tasks created");
    return ESP_OK;
}

bool websocket_client_is_connected(void)
{
    return s_ws.connected;
}
