/*
 * printer.c - Thermal Printer TCP Client Implementation
 * ESP32 Printer Client System
 *
 * Runs as a FreeRTOS task. Reads orders (BY POINTER) from a queue, streams the
 * raw ESC/POS byte payload to the thermal printer via TCP 9100 `copies` times,
 * and posts results back. Payload is binary and may contain 0x00 bytes, so the
 * explicit content_len is always used (never strlen).
 */

#include "printer.h"
#include "config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "PRINTER";

/* ── Order lifecycle helpers ────────────────────────────────────────── */
/* Wrap an already-allocated content buffer in a new order (no copy). The
 * order takes ownership of `content` and frees it in print_order_free(). */
print_order_t *print_order_adopt(const char *order_id, uint8_t *content,
                                 size_t content_len, int copies)
{
    print_order_t *order = calloc(1, sizeof(print_order_t));
    if (!order) {
        return NULL;
    }
    order->content     = content;
    order->content_len = content_len;
    order->copies      = (copies > 0) ? copies : 1;
    if (order_id) {
        strncpy(order->order_id, order_id, MAX_ORDER_ID_LEN);
        order->order_id[MAX_ORDER_ID_LEN] = '\0';
    }
    return order;
}

print_order_t *print_order_alloc(const char *order_id,
                                 size_t content_len, int copies)
{
    /* Always allocate at least 1 byte so content is non-NULL. */
    uint8_t *buf = malloc(content_len ? content_len : 1);
    if (!buf) {
        return NULL;
    }
    print_order_t *order = print_order_adopt(order_id, buf, content_len, copies);
    if (!order) {
        free(buf);
    }
    return order;
}

void print_order_free(print_order_t *order)
{
    if (!order) return;
    if (order->content) free(order->content);
    free(order);
}

/* Task parameters stored for the printer task */
typedef struct {
    QueueHandle_t order_queue;
    QueueHandle_t result_queue;
} printer_task_params_t;

static printer_task_params_t s_params;

/* ── Send data to printer via TCP ───────────────────────────────────── */
static bool send_to_printer(const char *ip, uint16_t port,
                            const uint8_t *data, size_t len,
                            char *err_reason, size_t reason_len)
{
    int sock = -1;
    bool success = false;

    /* Resolve address */
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };

    if (inet_pton(AF_INET, ip, &dest.sin_addr) <= 0) {
        snprintf(err_reason, reason_len, "invalid_ip");
        ESP_LOGE(TAG, "Invalid printer IP: %s", ip);
        return false;
    }

    /* Create socket */
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        snprintf(err_reason, reason_len, "socket_create_failed");
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        return false;
    }

    /* Set send/recv timeout to 5 seconds */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Connect */
    ESP_LOGI(TAG, "Connecting to printer %s:%u ...", ip, port);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        snprintf(err_reason, reason_len, "connect_timeout");
        ESP_LOGE(TAG, "Printer connect failed: errno %d", errno);
        goto cleanup;
    }

    /* Send data */
    int total_sent = 0;
    while (total_sent < (int)len) {
        int sent = send(sock, data + total_sent, len - total_sent, 0);
        if (sent < 0) {
            snprintf(err_reason, reason_len, "send_failed");
            ESP_LOGE(TAG, "Send failed: errno %d", errno);
            goto cleanup;
        }
        total_sent += sent;
    }

    ESP_LOGI(TAG, "Sent %d bytes to printer", total_sent);
    success = true;

cleanup:
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
    return success;
}

/* ── Print one full payload (all copies) with retries ───────────────── */
/* Returns true if every copy was sent successfully. */
static bool print_one_copy_set(const device_config_t *cfg,
                               const print_order_t *order,
                               char *reason, size_t reason_len)
{
    for (int copy = 1; copy <= order->copies; copy++) {
        bool copy_ok = false;

        for (int attempt = 1; attempt <= PRINT_MAX_RETRIES; attempt++) {
            ESP_LOGI(TAG, "Order [%s] copy %d/%d attempt %d/%d (%u bytes)",
                     order->order_id, copy, order->copies,
                     attempt, PRINT_MAX_RETRIES,
                     (unsigned)order->content_len);

            if (send_to_printer(cfg->printer_ip, cfg->printer_port,
                                order->content, order->content_len,
                                reason, reason_len)) {
                copy_ok = true;
                break;
            }

            ESP_LOGW(TAG, "Copy %d attempt %d failed: %s", copy, attempt, reason);
            if (attempt < PRINT_MAX_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
            }
        }

        if (!copy_ok) {
            return false; /* `reason` already holds the failure cause */
        }

        /* Brief gap between copies so the printer can cut/feed cleanly. */
        if (copy < order->copies) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    return true;
}

/* ── Printer task main loop ─────────────────────────────────────────── */
static void printer_task(void *arg)
{
    printer_task_params_t *params = (printer_task_params_t *)arg;
    const device_config_t *cfg = config_get();

    print_order_t *order = NULL;   /* received BY POINTER from the queue */
    print_result_t result;

    ESP_LOGI(TAG, "Printer task started (printer=%s:%u, max_retry=%d)",
             cfg->printer_ip, cfg->printer_port, PRINT_MAX_RETRIES);

    while (1) {
        /* Block waiting for an order pointer (indefinitely) */
        if (xQueueReceive(params->order_queue, &order, portMAX_DELAY) != pdTRUE
            || order == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200)); /* Failsafe delay if queue errors */
            continue;
        }

        ESP_LOGI(TAG, ">>> Received order [%s], %u bytes, %d cop%s",
                 order->order_id, (unsigned)order->content_len,
                 order->copies, order->copies == 1 ? "y" : "ies");

        memset(&result, 0, sizeof(result));
        strncpy(result.order_id, order->order_id, MAX_ORDER_ID_LEN);

        char reason[64] = {0};
        bool printed = print_one_copy_set(cfg, order, reason, sizeof(reason));

        if (printed) {
            result.status = PRINT_STATUS_SUCCESS;
            result.reason[0] = '\0';
            ESP_LOGI(TAG, "Order [%s] printed successfully", order->order_id);
        } else {
            result.status = PRINT_STATUS_FAILED;
            strncpy(result.reason, reason, sizeof(result.reason) - 1);
            ESP_LOGE(TAG, "Order [%s] FAILED: %s", order->order_id, reason);
        }

        /* Done with the payload — release it before reporting. */
        print_order_free(order);
        order = NULL;

        /* Post result to the result queue (non-blocking, drop if full) */
        if (xQueueSend(params->result_queue, &result, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Result queue full, dropping result for [%s]",
                     result.order_id);
        }

        /* Small delay to prevent tight loop and yield execution */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t printer_task_start(QueueHandle_t order_queue,
                             QueueHandle_t result_queue)
{
    if (order_queue == NULL || result_queue == NULL) {
        ESP_LOGE(TAG, "printer_task_start: NULL queue handle");
        return ESP_ERR_INVALID_ARG;
    }

    s_params.order_queue  = order_queue;
    s_params.result_queue = result_queue;

    BaseType_t ret = xTaskCreate(
        printer_task,
        "printer_task",
        8192,           /* Stack size (bytes) increased to prevent overflow */
        &s_params,
        5,              /* Priority */
        NULL
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create printer task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Printer task created");
    return ESP_OK;
}

bool printer_test_connection(const char *ip, uint16_t port)
{
    if (ip == NULL || strlen(ip) == 0) return false;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };

    if (inet_pton(AF_INET, ip, &dest.sin_addr) <= 0) {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    /* Set 2 seconds timeout for connect */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int err = connect(sock, (struct sockaddr *)&dest, sizeof(dest));

    if (sock >= 0) {
        close(sock);
    }

    return (err == 0);
}
