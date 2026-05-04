/*
 * printer.h - Thermal Printer TCP Client
 * ESP32 Printer Client System
 *
 * Sends pre-formatted text to a network thermal printer via TCP port 9100.
 * Operates as a FreeRTOS task consuming from an order queue.
 */

#ifndef PRINTER_H
#define PRINTER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Maximum sizes ──────────────────────────────────────────────────── */
#define MAX_ORDER_ID_LEN      64
#define MAX_PRINT_CONTENT_LEN 2048
#define PRINT_MAX_RETRIES     3

/* ── Order message passed via queue ─────────────────────────────────── */
typedef struct {
    char order_id[MAX_ORDER_ID_LEN + 1];
    char content[MAX_PRINT_CONTENT_LEN + 1];
} print_order_t;

/* ── Print result passed back via queue ─────────────────────────────── */
typedef enum {
    PRINT_STATUS_SUCCESS = 0,
    PRINT_STATUS_FAILED,
} print_status_t;

typedef struct {
    char order_id[MAX_ORDER_ID_LEN + 1];
    print_status_t status;
    char reason[64];
} print_result_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * @brief Start the printer task.
 *        Creates a FreeRTOS task that waits on the order queue.
 * @param order_queue   Queue handle for incoming print_order_t
 * @param result_queue  Queue handle for outgoing print_result_t
 */
esp_err_t printer_task_start(QueueHandle_t order_queue,
                             QueueHandle_t result_queue);

/**
 * @brief Synchronously test if the printer is reachable.
 *        Attempts a TCP connect with a 2-second timeout.
 * @param ip    Printer IP address
 * @param port  Printer port (usually 9100)
 * @return true if connect succeeds, false otherwise.
 */
bool printer_test_connection(const char *ip, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* PRINTER_H */
