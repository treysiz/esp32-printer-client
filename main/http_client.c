/*
 * http_client.c - Backend "WiFi 拉单" (WiFi pull) HTTP Client Implementation
 * ESP32 Printer Client System
 *
 * Polls the backend printer-api over HTTP(S) with Bearer auth, decodes the raw
 * ESC/POS payload of each job, enqueues it for the printer task, and reports
 * done/failed back to the backend.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  THE ONE THING THAT MATTERS MOST: decoding `content` (see decode section).
 * ─────────────────────────────────────────────────────────────────────────
 *  The backend renders the whole receipt to an image and emits ESC/POS raster
 *  bytes (GS v 0). Those raw bytes (0x00–0xFF, lots of 0x00 for white pixels)
 *  are placed into JSON by latin1-decoding them to a string. Over the wire the
 *  JSON is UTF-8, so:
 *    - a raw byte 0x00–0x7F  -> 1 byte (or a JSON escape like \n, )
 *    - a raw byte 0x80–0xFF  -> 2 UTF-8 bytes (latin1 char U+0080..U+00FF)
 *  To recover the EXACT original bytes we JSON-unescape, UTF-8-decode to code
 *  points, and take (codepoint & 0xFF). We do NOT use cJSON's valuestring for
 *  `content`, because an embedded 0x00 (\u0000) would truncate it. Instead we
 *  decode straight from the raw response buffer with an explicit length.
 */

#include "http_client.h"
#include "config.h"
#include "printer.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "HTTP_CLIENT";

/* ── Tunables ───────────────────────────────────────────────────────── */
#define HEARTBEAT_INTERVAL_MS   30000   /* §2.1 every 30 s                */
#define POLL_INTERVAL_MS         4000   /* §2.2 every 3~5 s               */
#define HTTP_TIMEOUT_MS          8000
#define AUTH_BACKOFF_MS         30000   /* slow down on 401/403           */

/*
 * Max size of a single /jobs response buffer.
 *
 * Sizing note: `content` is a raster bitmap whose white pixels are 0x00, and
 * the backend JSON-encodes every 0x00 as the 6-byte sequence "\u0000". A
 * receipt that is, say, 12 KB of raster (mostly white) therefore expands to
 * ~70 KB of JSON. /jobs returns ALL pending jobs in one response, so a burst
 * (e.g. after a brief outage) can be several hundred KB. The buffer must be
 * generous: we allocate from PSRAM when present (big_malloc) and fall back to
 * internal RAM, shrinking on failure (see task start). PSRAM is strongly
 * recommended for production so job bursts and 80mm receipts always fit.
 */
#define HTTP_RESP_PREFERRED  (256 * 1024)
#define HTTP_RESP_MIN        (24 * 1024)

/* ── Internal state ─────────────────────────────────────────────────── */
typedef struct {
    QueueHandle_t order_queue;
    QueueHandle_t result_queue;

    char  *resp_buf;          /* shared /jobs response accumulation buffer */
    int    resp_cap;          /* actual allocated size of resp_buf          */
    bool   online;            /* last request reachable & authorized       */

    /* Order dedup ring buffer */
    char recent_orders[ORDER_DEDUP_SIZE][MAX_ORDER_ID_LEN + 1];
    int  dedup_index;

    SemaphoreHandle_t http_mutex;  /* one outbound HTTP request at a time  */
} http_state_t;

static http_state_t s_http = {0};

/* ── Small utilities ────────────────────────────────────────────────── */

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Prefer PSRAM for big buffers, fall back to internal RAM. */
static void *big_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    return p;
}

static bool base_url_is_configured(const char *url)
{
    if (url == NULL || url[0] == '\0') return false;
    if (strcmp(url, DEFAULT_SERVER_URL) == 0) return false;
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

/* ── Dedup helpers ──────────────────────────────────────────────────── */
static bool is_duplicate_order(const char *order_id)
{
    for (int i = 0; i < ORDER_DEDUP_SIZE; i++) {
        if (strcmp(s_http.recent_orders[i], order_id) == 0) return true;
    }
    return false;
}

static void record_order_id(const char *order_id)
{
    strncpy(s_http.recent_orders[s_http.dedup_index], order_id, MAX_ORDER_ID_LEN);
    s_http.recent_orders[s_http.dedup_index][MAX_ORDER_ID_LEN] = '\0';
    s_http.dedup_index = (s_http.dedup_index + 1) % ORDER_DEDUP_SIZE;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  content decode: latin1-in-JSON  ->  exact raw ESC/POS bytes          */
/* ════════════════════════════════════════════════════════════════════ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decode a JSON string starting at `p` (the first char AFTER the opening
 * quote) up to the next unescaped quote. Each decoded code point is reduced
 * to a single byte via (cp & 0xFF) — exactly the original ESC/POS byte.
 *
 * If `out` is NULL we only count (and locate the end). Returns the number of
 * output bytes; *endp (if non-NULL) is set to the char just after the closing
 * quote.
 */
static size_t scan_decode(const char *p, const char *bufend,
                          uint8_t *out, const char **endp)
{
    size_t n = 0;

    while (p < bufend) {
        unsigned char c = (unsigned char)*p;

        if (c == '"') {                 /* closing quote */
            p++;
            break;
        }

        uint32_t cp;

        if (c == '\\') {                /* escape sequence */
            p++;
            if (p >= bufend) break;
            char e = *p++;
            switch (e) {
                case '"':  cp = 0x22; break;
                case '\\': cp = 0x5C; break;
                case '/':  cp = 0x2F; break;
                case 'b':  cp = 0x08; break;
                case 'f':  cp = 0x0C; break;
                case 'n':  cp = 0x0A; break;
                case 'r':  cp = 0x0D; break;
                case 't':  cp = 0x09; break;
                case 'u': {
                    cp = 0;
                    for (int i = 0; i < 4 && p < bufend; i++) {
                        int h = hexval(*p++);
                        if (h < 0) { h = 0; }
                        cp = (cp << 4) | (uint32_t)h;
                    }
                    break;
                }
                default:   cp = (unsigned char)e; break;
            }
        } else if (c < 0x80) {          /* plain ASCII */
            cp = c;
            p++;
        } else {                        /* UTF-8 multi-byte */
            int extra;
            if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            else                         { cp = c;        extra = 0; }
            p++;
            for (int i = 0; i < extra && p < bufend; i++) {
                if (((unsigned char)*p & 0xC0) != 0x80) break; /* defensive */
                cp = (cp << 6) | ((unsigned char)*p & 0x3F);
                p++;
            }
        }

        if (out) out[n] = (uint8_t)(cp & 0xFF);
        n++;
    }

    if (endp) *endp = p;
    return n;
}

/*
 * Find the next `"content"` key starting at *cursor, decode its string value
 * into a freshly heap-allocated byte buffer, and advance *cursor past it.
 * Returns true on success (caller owns *out and must free()).
 */
static bool next_content(const char **cursor, const char *bufend,
                         uint8_t **out, size_t *out_len)
{
    const char *p = *cursor;

    /* Locate the key token. The exact bytes "content" preceded by '{' / ','
     * / whitespace only ever occur as a JSON key (quotes inside string values
     * are escaped), so a forward search is safe. */
    while (p < bufend) {
        const char *hit = NULL;
        for (const char *q = p; q + 9 <= bufend; q++) {
            if (memcmp(q, "\"content\"", 9) == 0) { hit = q; break; }
        }
        if (!hit) return false;
        p = hit + 9;

        /* skip ws, expect ':' */
        while (p < bufend && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= bufend || *p != ':') continue;
        p++;
        while (p < bufend && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= bufend || *p != '"') continue;
        p++; /* now at first char of the value */

        const char *end = NULL;
        size_t len = scan_decode(p, bufend, NULL, &end); /* pass 1: size */

        uint8_t *buf = big_malloc(len ? len : 1);
        if (!buf) {
            ESP_LOGE(TAG, "OOM decoding content (%u bytes)", (unsigned)len);
            return false;
        }
        scan_decode(p, bufend, buf, &end);               /* pass 2: decode */

        *out = buf;
        *out_len = len;
        *cursor = end;
        return true;
    }
    return false;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  HTTP request helper                                                   */
/* ════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  *buf;
    int    cap;
    int    len;
    bool   overflow;
} resp_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
        if (!ctx || !ctx->buf) return ESP_OK;
        if (ctx->overflow) return ESP_OK;
        if (ctx->len + evt->data_len > ctx->cap) {
            ctx->overflow = true;
            return ESP_OK;
        }
        memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
        ctx->len += evt->data_len;
    }
    return ESP_OK;
}

/* Build "<base without trailing '/'><path>" into url. */
static void build_url(const char *base, const char *path, char *url, size_t url_sz)
{
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') blen--;
    snprintf(url, url_sz, "%.*s%s", (int)blen, base, path);
}

/*
 * Perform a request. `resp`/`resp_cap` may be NULL/0 if the body is not
 * needed. Returns the HTTP status code (>0) on transport success, or -1 on
 * transport failure. Serialized by the HTTP mutex.
 */
static int http_do(esp_http_client_method_t method, const char *path,
                   char *resp, int resp_cap, int *resp_len, bool *overflow)
{
    const device_config_t *cfg = config_get();

    char url[256];
    build_url(cfg->server_url, path, url, sizeof(url));

    char bearer[MAX_TOKEN_LEN + 16];
    snprintf(bearer, sizeof(bearer), "Bearer %s", cfg->api_token);

    resp_ctx_t ctx = { .buf = resp, .cap = resp_cap, .len = 0, .overflow = false };

    esp_http_client_config_t hcfg = {
        .url               = url,
        .method            = method,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* enables HTTPS */
        .keep_alive_enable = false,
    };

    int status = -1;

    xSemaphoreTake(s_http.http_mutex, portMAX_DELAY);

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (client) {
        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "ngrok-skip-browser-warning", "true");
        if (method == HTTP_METHOD_POST) {
            esp_http_client_set_post_field(client, NULL, 0);
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            status = esp_http_client_get_status_code(client);
            if (resp_len) *resp_len = ctx.len;
            if (overflow) *overflow = ctx.overflow;
            if (resp && resp_cap > 0 && ctx.len < resp_cap) {
                resp[ctx.len] = '\0'; /* convenience NUL (body may hold 0x00) */
            }
        } else {
            ESP_LOGW(TAG, "%s %s transport error: %s",
                     method == HTTP_METHOD_POST ? "POST" : "GET",
                     path, esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

    xSemaphoreGive(s_http.http_mutex);
    return status;
}

/* One-off backend reachability + auth test using EXPLICIT url/token (does not
 * touch the saved config or the running poll state). Powers the web UI
 * "test backend" button so the user can confirm before saving.
 * Returns the HTTP status (200 = OK), or -1 on transport/precondition error;
 * `err` gets a short reason. Serialized with the poll task if it is running. */
int http_client_test_backend(const char *base_url, const char *token,
                             char *err, size_t err_len)
{
    if (!base_url || !(strncmp(base_url, "http://", 7) == 0 ||
                       strncmp(base_url, "https://", 8) == 0)) {
        if (err && err_len) snprintf(err, err_len, "bad_url");
        return -1;
    }
    if (!token || token[0] == '\0') {
        if (err && err_len) snprintf(err, err_len, "no_token");
        return -1;
    }

    char url[256];
    build_url(base_url, "/printer-api/heartbeat", url, sizeof(url));
    char bearer[MAX_TOKEN_LEN + 16];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    esp_http_client_config_t hcfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    bool locked = false;
    if (s_http.http_mutex) { xSemaphoreTake(s_http.http_mutex, portMAX_DELAY); locked = true; }

    int status = -1;
    esp_http_client_handle_t cl = esp_http_client_init(&hcfg);
    if (cl) {
        esp_http_client_set_header(cl, "Authorization", bearer);
        esp_http_client_set_header(cl, "ngrok-skip-browser-warning", "true");
        esp_http_client_set_post_field(cl, NULL, 0);
        esp_err_t e = esp_http_client_perform(cl);
        if (e == ESP_OK) {
            status = esp_http_client_get_status_code(cl);
            ESP_LOGI(TAG, "test_backend %s -> HTTP %d", url, status);
        } else {
            ESP_LOGW(TAG, "test_backend %s transport error: %s", url, esp_err_to_name(e));
            if (err && err_len) snprintf(err, err_len, "unreachable");
        }
        esp_http_client_cleanup(cl);
    } else if (err && err_len) {
        snprintf(err, err_len, "init_fail");
    }

    if (locked) xSemaphoreGive(s_http.http_mutex);

    if (err && err_len) {
        if (status == 200)      err[0] = '\0';
        else if (status == 401) snprintf(err, err_len, "bad_token");
        else if (status == 403) snprintf(err, err_len, "not_wifi_provider");
        else if (status > 0)    snprintf(err, err_len, "http_%d", status);
    }
    return status;
}

/* Map a status code to online/auth state; log auth problems clearly. */
static bool status_is_ok(int status, const char *what)
{
    if (status == 200) return true;
    if (status == 401) {
        ESP_LOGE(TAG, "%s -> 401: missing/invalid Bearer token. Check API_TOKEN.", what);
    } else if (status == 403) {
        ESP_LOGE(TAG, "%s -> 403: token's printer is not provider='wifi'.", what);
    } else if (status > 0) {
        ESP_LOGW(TAG, "%s -> HTTP %d", what, status);
    }
    return false;
}

/* ── Heartbeat (§2.1) ───────────────────────────────────────────────── */
static void do_heartbeat(void)
{
    int status = http_do(HTTP_METHOD_POST, "/printer-api/heartbeat",
                         NULL, 0, NULL, NULL);
    if (status_is_ok(status, "heartbeat")) {
        s_http.online = true;
        ESP_LOGD(TAG, "Heartbeat OK");
    } else {
        s_http.online = false;
    }
}

/* ── Enqueue / fail a single decoded job ────────────────────────────── */
static void enqueue_failed_result(const char *order_id, const char *reason)
{
    print_result_t r;
    memset(&r, 0, sizeof(r));
    strncpy(r.order_id, order_id, MAX_ORDER_ID_LEN);
    r.status = PRINT_STATUS_FAILED;
    strncpy(r.reason, reason, sizeof(r.reason) - 1);
    xQueueSend(s_http.result_queue, &r, pdMS_TO_TICKS(500));
}

static void handle_job(const char *id, int copies, uint8_t *content, size_t len)
{
    /* Dedup: skip jobs already in flight / just finished. */
    if (is_duplicate_order(id)) {
        ESP_LOGW(TAG, "Duplicate job [%s] ignored", id);
        free(content);
        return;
    }

    /* Hand the decoded buffer straight to the order (no second copy). */
    print_order_t *order = print_order_adopt(id, content, len, copies);
    if (!order) {
        ESP_LOGE(TAG, "OOM allocating order [%s]", id);
        free(content);
        /* Let the backend know so it can retry / surface the failure. */
        enqueue_failed_result(id, "oom");
        return;
    }

    if (xQueueSend(s_http.order_queue, &order, pdMS_TO_TICKS(500)) == pdTRUE) {
        record_order_id(id);
        ESP_LOGI(TAG, "Job [%s] enqueued (%u bytes, %d copies)",
                 id, (unsigned)len, order->copies);
    } else {
        ESP_LOGE(TAG, "Order queue full, job [%s] dropped", id);
        print_order_free(order);
        enqueue_failed_result(id, "queue_full");
    }
}

/* ── Poll jobs (§2.2) ───────────────────────────────────────────────── */
static void do_poll_jobs(void)
{
    int resp_len = 0;
    bool overflow = false;
    int status = http_do(HTTP_METHOD_GET, "/printer-api/jobs",
                         s_http.resp_buf, s_http.resp_cap, &resp_len, &overflow);

    if (!status_is_ok(status, "jobs")) {
        s_http.online = false;
        return;
    }
    s_http.online = true;

    if (overflow) {
        ESP_LOGE(TAG, "/jobs response exceeded the %d-byte buffer; skipping "
                      "batch. Enable PSRAM and/or raise HTTP_RESP_PREFERRED "
                      "(white-pixel 0x00 bytes expand 6x as \\u0000 in JSON).",
                 s_http.resp_cap);
        return;
    }
    if (resp_len <= 0) return;

    cJSON *root = cJSON_ParseWithLength(s_http.resp_buf, (size_t)resp_len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse /jobs JSON");
        return;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return; /* no jobs */
    }

    /* Parallel raw cursor used ONLY to extract binary-safe `content`. cJSON
     * preserves document order, so the k-th "content" key aligns with the
     * k-th array element. */
    const char *content_cursor = s_http.resp_buf;
    const char *buf_end = s_http.resp_buf + resp_len;

    cJSON *elem = NULL;
    cJSON_ArrayForEach(elem, data) {
        cJSON *id_item     = cJSON_GetObjectItemCaseSensitive(elem, "id");
        cJSON *copies_item = cJSON_GetObjectItemCaseSensitive(elem, "copies");

        /* Always advance the content cursor for every element to stay aligned,
         * even if we end up skipping the job. */
        uint8_t *content = NULL;
        size_t   clen = 0;
        bool got_content = next_content(&content_cursor, buf_end, &content, &clen);

        if (!cJSON_IsString(id_item) || !id_item->valuestring) {
            ESP_LOGW(TAG, "job element missing 'id'");
            if (got_content) free(content);
            continue;
        }
        if (!got_content) {
            ESP_LOGW(TAG, "job [%s] missing/undecodable 'content'", id_item->valuestring);
            continue;
        }

        int copies = (cJSON_IsNumber(copies_item) && copies_item->valueint > 0)
                         ? copies_item->valueint : 1;

        handle_job(id_item->valuestring, copies, content, clen);
    }

    cJSON_Delete(root);
}

/* ── Poll task (heartbeat + jobs) ───────────────────────────────────── */
static void poll_task(void *arg)
{
    const device_config_t *cfg = config_get();
    int64_t last_hb = -HEARTBEAT_INTERVAL_MS; /* force an immediate heartbeat */

    ESP_LOGI(TAG, "Poll task started, backend: %s", cfg->server_url);

    while (1) {
        /* Wait for WiFi. */
        EventGroupHandle_t wifi_eg = wifi_mgr_get_event_group();
        if (wifi_eg) {
            xEventGroupWaitBits(wifi_eg, WIFI_CONNECTED_BIT,
                                pdFALSE, pdTRUE, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!base_url_is_configured(cfg->server_url) || cfg->api_token[0] == '\0') {
            ESP_LOGE(TAG, "Backend URL or API token not configured "
                          "(url='%s', token=%s)",
                     cfg->server_url, cfg->api_token[0] ? "set" : "EMPTY");
            s_http.online = false;
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        int64_t now = now_ms();
        if (now - last_hb >= HEARTBEAT_INTERVAL_MS) {
            do_heartbeat();
            last_hb = now;
        }

        bool was_online = s_http.online;
        do_poll_jobs();

        /* If auth is failing, back off to avoid hammering the backend. */
        if (!s_http.online && was_online == false) {
            vTaskDelay(pdMS_TO_TICKS(AUTH_BACKOFF_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        }
    }
}

/* ── Result reporter task (done/failed §2.3/§2.4) ───────────────────── */
static void result_task(void *arg)
{
    print_result_t result;
    char path[128];

    ESP_LOGI(TAG, "Result reporter task started");

    while (1) {
        if (xQueueReceive(s_http.result_queue, &result, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        const char *verb = (result.status == PRINT_STATUS_SUCCESS) ? "done" : "failed";
        snprintf(path, sizeof(path), "/printer-api/jobs/%s/%s", result.order_id, verb);

        int status = http_do(HTTP_METHOD_POST, path, NULL, 0, NULL, NULL);
        if (status == 200) {
            ESP_LOGI(TAG, "Reported %s for job [%s]", verb, result.order_id);
        } else if (status == 404) {
            ESP_LOGW(TAG, "Job [%s] not found when reporting %s (already done?)",
                     result.order_id, verb);
        } else {
            ESP_LOGW(TAG, "Failed to report %s for [%s] (HTTP %d) — retrying once",
                     verb, result.order_id, status);
            vTaskDelay(pdMS_TO_TICKS(2000));
            http_do(HTTP_METHOD_POST, path, NULL, 0, NULL, NULL);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */
esp_err_t http_client_task_start(QueueHandle_t order_queue,
                                 QueueHandle_t result_queue)
{
    if (order_queue == NULL || result_queue == NULL) {
        ESP_LOGE(TAG, "http_client_task_start: NULL queue");
        return ESP_ERR_INVALID_ARG;
    }

    s_http.order_queue  = order_queue;
    s_http.result_queue = result_queue;
    s_http.online       = false;
    s_http.dedup_index  = 0;
    memset(s_http.recent_orders, 0, sizeof(s_http.recent_orders));

    s_http.http_mutex = xSemaphoreCreateMutex();
    if (!s_http.http_mutex) {
        ESP_LOGE(TAG, "Failed to create HTTP mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate the response buffer, shrinking if the preferred size won't fit
     * (PSRAM-first via big_malloc). This avoids a boot loop on boards with
     * little free RAM; we just cap the max receipt size we can handle. */
    s_http.resp_cap = 0;
    for (int sz = HTTP_RESP_PREFERRED; sz >= HTTP_RESP_MIN; sz -= (16 * 1024)) {
        s_http.resp_buf = big_malloc(sz);
        if (s_http.resp_buf) { s_http.resp_cap = sz; break; }
    }
    if (!s_http.resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate /jobs response buffer (>= %d bytes)",
                 HTTP_RESP_MIN);
        return ESP_ERR_NO_MEM;
    }
    if (s_http.resp_cap < HTTP_RESP_PREFERRED) {
        ESP_LOGW(TAG, "Response buffer is %d bytes (< preferred %d). Large "
                      "receipts may not fit - consider enabling PSRAM.",
                 s_http.resp_cap, HTTP_RESP_PREFERRED);
    }

    BaseType_t ret = xTaskCreate(poll_task, "http_poll", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create poll_task");
        return ESP_FAIL;
    }

    ret = xTaskCreate(result_task, "http_result", 6144, NULL, 4, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create result_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP client tasks created (resp buf %d bytes)", s_http.resp_cap);
    return ESP_OK;
}

bool http_client_is_connected(void)
{
    return s_http.online;
}
