/*
 * websocket_client.h - WebSocket Client Manager
 * ESP32 Printer Client System
 *
 * Manages WebSocket connection to cloud server.
 * Receives order messages and forwards print results.
 * Implements device registration, heartbeat, and order deduplication.
 */

#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Order deduplication settings ───────────────────────────────────── */
#define ORDER_DEDUP_SIZE  20   /* Remember last N order IDs */

/**
 * @brief Start the WebSocket client task.
 *
 * The task will:
 *   1. Wait for WiFi connection.
 *   2. Connect to the configured server URL.
 *   3. Send device registration.
 *   4. Listen for incoming order messages.
 *   5. De-duplicate and enqueue orders for printing.
 *   6. Forward print results back to server.
 *   7. Auto-reconnect with exponential backoff on disconnect.
 *
 * @param order_queue   Queue to push print_order_t messages into
 * @param result_queue  Queue to read print_result_t messages from
 */
esp_err_t websocket_client_task_start(QueueHandle_t order_queue,
                                      QueueHandle_t result_queue);

/**
 * @brief Get the current connection state of the WebSocket.
 * @return true if connected to cloud server.
 */
bool websocket_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_CLIENT_H */
