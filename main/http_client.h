/*
 * http_client.h - Backend "WiFi 拉单" (WiFi pull) HTTP Client
 * ESP32 Printer Client System
 *
 * Replaces the old WebSocket push client with an HTTP polling client that
 * speaks the backend printer-api contract:
 *
 *   POST <base>/printer-api/heartbeat        every 30 s   (online keep-alive)
 *   GET  <base>/printer-api/jobs             every ~4 s   (pull pending jobs)
 *   POST <base>/printer-api/jobs/:id/done                 (print succeeded)
 *   POST <base>/printer-api/jobs/:id/failed               (print failed)
 *
 * Every request carries:  Authorization: Bearer <api_token>
 *
 * The `content` field of a job is the RAW ESC/POS raster bitmap, transported
 * as a latin1-in-JSON string. It is decoded back to exact bytes (see
 * http_client.c) and never treated as text — this is what keeps Chinese menu
 * names printing correctly.
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Remember the last N job IDs to avoid re-printing a job that is still
 * 'pending' in a poll that races with our done/failed report. */
#define ORDER_DEDUP_SIZE  20

/**
 * @brief Start the backend HTTP polling client tasks.
 *
 * Spawns:
 *   - a poll task: heartbeat + GET /jobs loop, decodes payloads and enqueues
 *     print_order_t pointers onto order_queue.
 *   - a result task: drains result_queue and reports done/failed to backend.
 *
 * @param order_queue   Queue of `print_order_t *` to push jobs onto.
 * @param result_queue  Queue of `print_result_t` to read print outcomes from.
 */
esp_err_t http_client_task_start(QueueHandle_t order_queue,
                                 QueueHandle_t result_queue);

/**
 * @brief True if the backend was reachable & authorized on the last request.
 *        Drives the "云端状态 / Cloud Status" indicator in the web UI.
 */
bool http_client_is_connected(void);

/**
 * @brief One-off test of backend reachability + auth with an EXPLICIT base URL
 *        and Bearer token (independent of the saved config / poll task). Sends
 *        POST <base>/printer-api/heartbeat. Returns the HTTP status (200 = OK)
 *        or -1 on transport/precondition error; `err` receives a short reason
 *        such as "bad_token", "not_wifi_provider", "unreachable", "bad_url".
 */
int http_client_test_backend(const char *base_url, const char *token,
                             char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
