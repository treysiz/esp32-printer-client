/*
 * printer.h - Thermal Printer TCP Client
 * ESP32 Printer Client System
 *
 * Streams RAW ESC/POS bytes to a network thermal printer via TCP port 9100.
 * Operates as a FreeRTOS task consuming from an order queue.
 *
 * IMPORTANT: the print payload is BINARY (ESC/POS raster bitmap). It routinely
 * contains 0x00 bytes (white pixels), so it must never be treated as a
 * null-terminated C string. Orders carry an explicit length and are passed
 * through the queue BY POINTER (heap-allocated) to support large receipts.
 */

#ifndef PRINTER_H
#define PRINTER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Maximum sizes ──────────────────────────────────────────────────── */
#define MAX_ORDER_ID_LEN      64       /* fits a UUID (36 chars) comfortably */
#define PRINT_MAX_RETRIES     3

/* ── Order message passed via queue (BY POINTER) ────────────────────── */
/*
 * The order_queue holds `print_order_t *` elements. The producer (HTTP
 * client / web UI test) heap-allocates the struct and the content buffer;
 * the printer task takes ownership and frees both after printing.
 */
typedef struct {
    char     order_id[MAX_ORDER_ID_LEN + 1]; /* backend job UUID */
    uint8_t *content;        /* heap buffer of raw ESC/POS bytes (owned) */
    size_t   content_len;    /* number of bytes in `content` */
    int      copies;         /* how many times to send the payload (>=1) */
} print_order_t;

/**
 * @brief Allocate a print order with a content buffer of `content_len` bytes.
 *        Caller fills order->content then enqueues the POINTER. The printer
 *        task frees it. Returns NULL on allocation failure.
 */
print_order_t *print_order_alloc(const char *order_id,
                                 size_t content_len, int copies);

/**
 * @brief Wrap an existing heap buffer in a new order WITHOUT copying it. The
 *        order takes ownership of `content` and frees it in print_order_free().
 *        On failure returns NULL and does NOT free `content` (caller owns it).
 */
print_order_t *print_order_adopt(const char *order_id, uint8_t *content,
                                 size_t content_len, int copies);

/**
 * @brief Free a print order and its content buffer.
 */
void print_order_free(print_order_t *order);

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
