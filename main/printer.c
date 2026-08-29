/*
 * printer.c - Thermal Printer TCP Client Implementation
 * ESP32 Printer Client System
 *
 * Runs as a FreeRTOS task. Reads orders (BY POINTER) from a queue, streams the
 * raw ESC/POS byte payload to the thermal printer via TCP 9100 `copies` times,
 * and posts results back. Payload is binary and may contain 0x00 bytes, so the
 * explicit content_len is always used (never strlen).
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  PAPER DETECTION: the one path that used to lose receipts silently.
 * ─────────────────────────────────────────────────────────────────────────
 *  Sending bytes to a printer that is out of paper succeeds at every level we
 *  can see: TCP connects, send() returns, the backend gets `/done` and marks
 *  the job printed. Nothing ever prints and nobody finds out. So before (and
 *  after) each payload we ask the printer with the ESC/POS real-time query
 *  DLE EOT 4 and report `/failed` on paper-out — the backend keeps such a job
 *  pending and re-sends it, so the receipt comes out by itself once the roll
 *  is replaced. See query_paper_status().
 *
 *  Two rules that matter more than the detection itself:
 *   - Unknown status FAILS OPEN. Plenty of printers never answer DLE EOT; if
 *     we treated silence as failure, those printers would print nothing at
 *     all. Silence means "print anyway".
 *   - Better a duplicate than a lost receipt. A half-printed receipt plus a
 *     reprint is fine; staff bin the short one. A missing receipt means a
 *     customer never gets their food.
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

/* ── Socket timeouts ────────────────────────────────────────────────── */
#define PRINTER_IO_TIMEOUT_MS   5000  /* connect / bulk payload transfer      */
#define PAPER_QUERY_TIMEOUT_MS   800  /* DLE EOT is real-time: ms, not seconds */

/* ════════════════════════════════════════════════════════════════════ */
/*  Paper sensor query: ESC/POS  DLE EOT 4                               */
/* ════════════════════════════════════════════════════════════════════ */

typedef enum {
    PAPER_OK = 0,     /* sensors report paper present                       */
    PAPER_NEAR_END,   /* roll running low — still prints, just warn         */
    PAPER_OUT,        /* paper-end sensor tripped — do not print            */
    PAPER_UNKNOWN,    /* no/invalid reply — FAIL OPEN, print anyway         */
} paper_status_t;

static const char *paper_status_str(paper_status_t s)
{
    switch (s) {
        case PAPER_OK:       return "ok";
        case PAPER_NEAR_END: return "near_end";
        case PAPER_OUT:      return "out";
        default:             return "unknown";
    }
}

static void set_rcv_timeout_ms(int sock, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/*
 * Ask the printer about its paper sensors and read the one-byte answer.
 *
 * DLE EOT n (0x10 0x04 n) is a REAL-TIME command: the printer answers
 * immediately, even while it is busy printing something else. n=4 selects the
 * paper sensors. In the reply byte:
 *
 *     bit 1 = 1, bit 4 = 1, bit 0 = 0, bit 7 = 0   (fixed — our sanity check)
 *     bits 2,3 both set -> paper near end
 *     bits 5,6 both set -> paper end (out of paper)
 *
 * NOTE: bit assignments are the EPSON standard and most clones follow it, but
 * they are NOT universal. Verify against the actual machine: pull the roll
 * out, read once; put it back, read again; see which bits flipped.
 *
 * Anything unexpected — send fails, recv times out, reply violates the fixed
 * bits — returns PAPER_UNKNOWN, which callers MUST treat as "go ahead and
 * print". A printer that does not implement DLE EOT must keep working.
 */
static paper_status_t query_paper_status(int sock)
{
    static const uint8_t DLE_EOT_PAPER[3] = { 0x10, 0x04, 0x04 };

    /* Use a short timeout so a printer that ignores the query costs us ~800 ms,
     * not the full 5 s payload timeout — this runs on every single receipt. */
    set_rcv_timeout_ms(sock, PAPER_QUERY_TIMEOUT_MS);

    paper_status_t status = PAPER_UNKNOWN;
    uint8_t st = 0;
    int n = 0;

    if (send(sock, DLE_EOT_PAPER, sizeof(DLE_EOT_PAPER), 0) != (int)sizeof(DLE_EOT_PAPER)) {
        ESP_LOGD(TAG, "paper query: send failed (errno %d) -> unknown", errno);
        goto restore;
    }

    n = recv(sock, &st, 1, 0);
    if (n != 1) {
        ESP_LOGD(TAG, "paper query: no reply (n=%d, errno %d) -> unknown, "
                      "printer likely does not support DLE EOT", n, errno);
        goto restore;
    }

    /* Fixed-bit sanity check: bit1 and bit4 set, bit0 and bit7 clear. */
    if ((st & 0x93) != 0x12) {
        ESP_LOGW(TAG, "paper query: malformed status byte 0x%02X -> unknown", st);
        goto restore;
    }

    if ((st & 0x60) == 0x60) {
        status = PAPER_OUT;
    } else if ((st & 0x0C) == 0x0C) {
        status = PAPER_NEAR_END;
    } else {
        status = PAPER_OK;
    }
    ESP_LOGD(TAG, "paper query: status byte 0x%02X -> %s",
             st, paper_status_str(status));

restore:
    set_rcv_timeout_ms(sock, PRINTER_IO_TIMEOUT_MS);
    return status;
}

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
/*
 * `retry_locally` (optional) is set to false when retrying here would be
 * pointless — currently only paper-out. Paper does not reappear during a
 * 2-second backoff; the backend's re-send window (30 tries over ~half an hour)
 * is the right place to wait, and it only starts once we report `/failed`.
 */
static bool send_to_printer(const char *ip, uint16_t port,
                            const uint8_t *data, size_t len,
                            char *err_reason, size_t reason_len,
                            bool *retry_locally)
{
    int sock = -1;
    bool success = false;

    if (retry_locally) *retry_locally = true;

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

    /* Set send/recv timeout (the paper query lowers recv briefly and restores) */
    struct timeval tv = { .tv_sec = PRINTER_IO_TIMEOUT_MS / 1000, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Connect */
    ESP_LOGI(TAG, "Connecting to printer %s:%u ...", ip, port);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        snprintf(err_reason, reason_len, "connect_timeout");
        ESP_LOGE(TAG, "Printer connect failed: errno %d", errno);
        goto cleanup;
    }

    /* Ask BEFORE sending: if there is no paper, sending is worse than useless.
     * The bytes would vanish and we would report success for a receipt that
     * never existed. Report /failed instead and let the backend re-send. */
    paper_status_t paper = query_paper_status(sock);
    if (paper == PAPER_OUT) {
        snprintf(err_reason, reason_len, "paper_out");
        if (retry_locally) *retry_locally = false;
        ESP_LOGE(TAG, "缺纸 (paper out) — NOT sending; reporting /failed so the "
                      "backend re-sends once the roll is replaced");
        goto cleanup;
    }
    if (paper == PAPER_NEAR_END) {
        ESP_LOGW(TAG, "纸快用完 (paper near end) — printing anyway");
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

    ESP_LOGI(TAG, "Sent %d bytes to printer (paper before: %s)",
             total_sent, paper_status_str(paper));

    /* Ask AFTER sending too: the roll can run out mid-receipt. Reporting
     * /failed may cost a duplicate later, which is the trade we want. */
    paper = query_paper_status(sock);
    if (paper == PAPER_OUT) {
        snprintf(err_reason, reason_len, "paper_out_mid_print");
        if (retry_locally) *retry_locally = false;
        ESP_LOGE(TAG, "缺纸 (paper ran out during printing) — reporting /failed; "
                      "expect a short receipt plus a full reprint");
        goto cleanup;
    }

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

            bool retry_locally = true;
            if (send_to_printer(cfg->printer_ip, cfg->printer_port,
                                order->content, order->content_len,
                                reason, reason_len, &retry_locally)) {
                copy_ok = true;
                break;
            }

            ESP_LOGW(TAG, "Copy %d attempt %d failed: %s", copy, attempt, reason);

            /* Paper-out: stop hammering the printer and hand it to the backend
             * now, so its ~30-minute re-send window starts immediately. */
            if (!retry_locally) {
                ESP_LOGW(TAG, "Not retrying locally (%s) — backend will re-send",
                         reason);
                break;
            }
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
