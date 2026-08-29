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
 *  bytes (GS v 0): 0x00-0xFF, with lots of 0x00 for white pixels. Every job
 *  carries a `contentEncoding` field saying how those bytes were packed into
 *  the JSON string, so we never have to guess:
 *
 *   "base64"  - what we ask for (?encoding=base64). Pure ASCII, so cJSON's
 *               valuestring is safe and mbedtls_base64_decode() gives us the
 *               exact original bytes. ~4.2x smaller on the wire than latin1.
 *
 *   "latin1"  - legacy shape, and what an older backend that ignores the query
 *               param still sends. Raw bytes latin1-decoded into a string; over
 *               the wire the JSON is UTF-8, so:
 *                 - a raw byte 0x00-0x7F -> 1 byte (or a JSON escape)
 *                 - a raw byte 0x80-0xFF -> 2 UTF-8 bytes (U+0080..U+00FF)
 *               To recover the EXACT bytes we JSON-unescape, UTF-8-decode to
 *               code points, and take (codepoint & 0xFF). cJSON's valuestring
 *               is useless here (an embedded 0x00 arrives as a 6-byte JSON
 *               escape and would truncate the C string), so we decode from
 *               the raw response buffer with an explicit length. Kept only as
 *               a fallback so this firmware works against both backends.
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
#include "mbedtls/base64.h"   /* bundled with ESP-IDF, no extra dependency */

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
 * Sizing note: we poll with `?limit=1&encoding=base64`, so a response carries
 * ONE job whose `content` is base64 (4/3 of the raw raster) instead of the old
 * latin1-in-JSON form (~5.6x, because every white-pixel 0x00 became the 6-byte
 * JSON escape). Measured across 535 real receipts, base64 responses run
 * ~137 KB median and ~210 KB at the top end, so 256 KB has comfortable margin
 * and no longer needs an allowance for 6x expansion.
 *
 * PSRAM is a hard requirement in practice: 137-210 KB never fits inside
 * HTTP_RESP_INTERNAL_MAX (48 KB), the cap we fall back to when there is no
 * PSRAM (a large internal buffer starves the TLS handshake -> -0x7F00). A board
 * without PSRAM can reach the backend but cannot receive a real receipt.
 */
#define HTTP_RESP_PREFERRED    (256 * 1024)  /* big buffer: ONLY from PSRAM        */
#define HTTP_RESP_INTERNAL_MAX (48 * 1024)   /* cap without PSRAM, to spare TLS heap */
#define HTTP_RESP_MIN          (8 * 1024)

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
/*  content decode: base64 (preferred) / latin1-in-JSON (fallback)       */
/*                            ->  exact raw ESC/POS bytes                */
/* ════════════════════════════════════════════════════════════════════ */

/*
 * base64 path — the one we normally take. `content` is pure ASCII here, so
 * cJSON's valuestring is safe (no embedded NUL can truncate it) and mbedtls
 * gives us the original bytes back verbatim: 0x00, 0x1B, 0x1D and 0x80-0xFF
 * all survive untouched.
 *
 * Caller owns *out on success and must free() it.
 */
static bool decode_base64_content(const cJSON *c_item,
                                  uint8_t **out, size_t *out_len)
{
    if (!cJSON_IsString(c_item) || !c_item->valuestring) return false;

    const unsigned char *src = (const unsigned char *)c_item->valuestring;
    size_t src_len = strlen(c_item->valuestring);

    /* Ask how big the plaintext is: with dst=NULL/dlen=0 mbedtls writes the
     * required length into `need` and returns BUFFER_TOO_SMALL. */
    size_t need = 0;
    (void)mbedtls_base64_decode(NULL, 0, &need, src, src_len);

    size_t cap = need ? need : 1;
    uint8_t *buf = big_malloc(cap);
    if (!buf) {
        ESP_LOGE(TAG, "OOM decoding base64 content (%u bytes)", (unsigned)need);
        return false;
    }

    size_t got = 0;
    int rc = mbedtls_base64_decode(buf, cap, &got, src, src_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 decode failed (-0x%04X), b64_len=%u",
                 (unsigned)(-rc), (unsigned)src_len);
        free(buf);
        return false;
    }

    *out     = buf;
    *out_len = got;
    return true;
}

/* ── latin1 fallback (old backends) ─────────────────────────────────── */

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
 * Find the next `"content"` key starting at *cursor, point *val at the first
 * char of its string value (just after the opening quote), and advance *cursor
 * past the closing quote so the following call lands on the next element.
 *
 * This only locates; nothing is allocated. Call it for EVERY array element —
 * including base64 ones whose value we take from cJSON — so the k-th
 * `"content"` key stays aligned with the k-th element.
 */
static bool next_content_value(const char **cursor, const char *bufend,
                               const char **val)
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
        scan_decode(p, bufend, NULL, &end); /* locate the closing quote */

        *val    = p;
        *cursor = end;
        return true;
    }
    return false;
}

/*
 * Decode a value located by next_content_value() into a freshly heap-allocated
 * byte buffer. Caller owns *out and must free() it.
 */
static bool decode_latin1_content(const char *val, const char *bufend,
                                  uint8_t **out, size_t *out_len)
{
    size_t len = scan_decode(val, bufend, NULL, NULL); /* pass 1: size */

    uint8_t *buf = big_malloc(len ? len : 1);
    if (!buf) {
        ESP_LOGE(TAG, "OOM decoding latin1 content (%u bytes)", (unsigned)len);
        return false;
    }
    scan_decode(val, bufend, buf, NULL);               /* pass 2: decode */

    *out     = buf;
    *out_len = len;
    return true;
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
    /* `limit=1` matches the backend default, but we say it explicitly so we
     * never inherit a future change to that default. `encoding=base64` keeps
     * the response ~4.2x smaller than the latin1-in-JSON form. Both params are
     * optional on the backend, so an older one just ignores them and answers
     * with contentEncoding="latin1" (or omits the field) — handled below.
     * build_url() concatenates verbatim, so '?' and '&' go out untouched. */
    int status = http_do(HTTP_METHOD_GET, "/printer-api/jobs?limit=1&encoding=base64",
                         s_http.resp_buf, s_http.resp_cap, &resp_len, &overflow);

    if (!status_is_ok(status, "jobs")) {
        s_http.online = false;
        return;
    }
    s_http.online = true;

    if (overflow) {
        ESP_LOGE(TAG, "/jobs response exceeded the %d-byte buffer; skipping "
                      "batch. A base64 receipt is ~137-210 KB, so this board "
                      "almost certainly has no PSRAM (see HTTP_RESP_PREFERRED).",
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

    /* Parallel raw cursor into the response, needed only by the latin1
     * fallback (cJSON's valuestring truncates at the first embedded NUL). We
     * advance it for EVERY element — base64 ones included — because cJSON
     * preserves document order, so the k-th "content" key must stay aligned
     * with the k-th array element. */
    const char *content_cursor = s_http.resp_buf;
    const char *buf_end = s_http.resp_buf + resp_len;

    cJSON *elem = NULL;
    cJSON_ArrayForEach(elem, data) {
        cJSON *id_item     = cJSON_GetObjectItemCaseSensitive(elem, "id");
        cJSON *copies_item = cJSON_GetObjectItemCaseSensitive(elem, "copies");
        cJSON *enc_item    = cJSON_GetObjectItemCaseSensitive(elem, "contentEncoding");
        cJSON *c_item      = cJSON_GetObjectItemCaseSensitive(elem, "content");

        /* Keep the raw cursor in step whether or not we end up using it. */
        const char *raw_val = NULL;
        bool located = next_content_value(&content_cursor, buf_end, &raw_val);

        if (!cJSON_IsString(id_item) || !id_item->valuestring) {
            ESP_LOGW(TAG, "job element missing 'id'");
            continue;
        }

        /* A backend that predates `contentEncoding` only ever spoke latin1. */
        const char *enc = (cJSON_IsString(enc_item) && enc_item->valuestring)
                              ? enc_item->valuestring : "latin1";

        uint8_t *content = NULL;
        size_t   clen    = 0;
        bool     got_content;

        if (strcmp(enc, "base64") == 0) {
            got_content = decode_base64_content(c_item, &content, &clen);
        } else {
            got_content = located &&
                          decode_latin1_content(raw_val, buf_end, &content, &clen);
        }

        if (!got_content) {
            ESP_LOGW(TAG, "job [%s] missing/undecodable 'content' (encoding=%s)",
                     id_item->valuestring, enc);
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

        /* Small buffer: /failed answers with `willRetry`, i.e. whether the job
         * stays pending for a later poll or the backend has given up on it.
         * Cheap to capture and the first thing you want when chasing a receipt
         * that never came out. */
        char body[192] = {0};
        int  body_len  = 0;

        int status = http_do(HTTP_METHOD_POST, path,
                             body, (int)sizeof(body) - 1, &body_len, NULL);
        if (status == 200) {
            const char *retry_note = "";
            if (result.status != PRINT_STATUS_SUCCESS && body_len > 0) {
                cJSON *r = cJSON_ParseWithLength(body, (size_t)body_len);
                cJSON *w = r ? cJSON_GetObjectItemCaseSensitive(r, "willRetry") : NULL;
                if (cJSON_IsBool(w)) {
                    retry_note = cJSON_IsTrue(w)
                        ? " willRetry=true (backend will re-send)"
                        : " willRetry=false (backend gave up; alert raised)";
                }
                cJSON_Delete(r);
            }
            ESP_LOGI(TAG, "Reported %s for job [%s]%s",
                     verb, result.order_id, retry_note);
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
    s_http.resp_buf = heap_caps_malloc(HTTP_RESP_PREFERRED, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_http.resp_buf) {
        s_http.resp_cap = HTTP_RESP_PREFERRED;        /* PSRAM available */
    } else {
        /* No PSRAM: keep the buffer SMALL so the TLS handshake (~40 KB heap)
         * and the rest of the system keep enough RAM. A large internal buffer
         * previously starved the heap -> mbedtls_ssl_setup -0x7F00 (alloc
         * failed) -> all HTTPS broke. Large/Chinese receipts need PSRAM. */
        for (int sz = HTTP_RESP_INTERNAL_MAX; sz >= HTTP_RESP_MIN; sz -= (8 * 1024)) {
            s_http.resp_buf = malloc(sz);
            if (s_http.resp_buf) { s_http.resp_cap = sz; break; }
        }
    }
    if (!s_http.resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate /jobs response buffer (>= %d bytes)",
                 HTTP_RESP_MIN);
        return ESP_ERR_NO_MEM;
    }
    if (s_http.resp_cap < HTTP_RESP_PREFERRED) {
        ESP_LOGE(TAG, "Response buffer only %d bytes (no PSRAM). A real receipt "
                      "is ~137-210 KB even as base64, so /jobs responses WILL "
                      "overflow and no job can be printed. PSRAM is required. "
                      "Free internal=%u",
                 s_http.resp_cap,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
