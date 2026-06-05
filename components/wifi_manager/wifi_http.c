/*
 * wifi_http.c - WiFi REST API Handlers
 * ESP32 PrinterBox / ESP-IDF v5.2+
 *
 *   GET  /wifi_scan    → JSON array of APs
 *   GET  /wifi_status  → JSON status object
 *   POST /wifi_connect → connect with posted ssid/password
 */

#include "wifi_http.h"
#include "wifi_scan.h"
#include "wifi_status.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "WIFI_HTTP";

/* ── GET /wifi_scan ─────────────────────────────────────────────────── */
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_scan_result_t result;
    esp_err_t err = wifi_scan_start(&result);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc fail");
        return ESP_FAIL;
    }

    if (err == ESP_OK) {
        for (int i = 0; i < result.count; i++) {
            cJSON *obj = cJSON_CreateObject();
            if (!obj) break;
            cJSON_AddStringToObject(obj, "ssid", result.aps[i].ssid);
            cJSON_AddNumberToObject(obj, "rssi", result.aps[i].rssi);
            cJSON_AddItemToArray(arr, obj);
        }
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print fail");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

/* ── GET /wifi_status ───────────────────────────────────────────────── */
static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    wifi_status_info_t info;
    wifi_status_get(&info);

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc fail");
        return ESP_FAIL;
    }

    cJSON_AddBoolToObject(obj, "connected", info.connected);
    cJSON_AddStringToObject(obj, "mode", info.mode);
    cJSON_AddStringToObject(obj, "ssid", info.ssid);
    cJSON_AddNumberToObject(obj, "rssi", info.rssi);
    cJSON_AddStringToObject(obj, "quality", info.quality_str);
    cJSON_AddStringToObject(obj, "ip", info.ip);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print fail");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

/* ── POST /wifi_connect ─────────────────────────────────────────────── */
static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;

    if (remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (remaining > 0) {
        ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(root, "password");

    if (!j_ssid || !cJSON_IsString(j_ssid) || strlen(j_ssid->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }

    const char *ssid = j_ssid->valuestring;
    const char *pass = (j_pass && cJSON_IsString(j_pass)) ? j_pass->valuestring : "";

    ESP_LOGI(TAG, "WiFi connect request: SSID=%s", ssid);

    esp_err_t err = wifi_mgr_connect(ssid, pass);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Connect failed");
    }
    return ESP_OK;
}

/* ── Register all handlers ──────────────────────────────────────────── */
esp_err_t wifi_http_register_handlers(httpd_handle_t server)
{
    httpd_uri_t uri_scan = {
        .uri = "/wifi_scan", .method = HTTP_GET,
        .handler = handle_wifi_scan, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_scan);

    httpd_uri_t uri_status = {
        .uri = "/wifi_status", .method = HTTP_GET,
        .handler = handle_wifi_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_status);

    httpd_uri_t uri_conn = {
        .uri = "/wifi_connect", .method = HTTP_POST,
        .handler = handle_wifi_connect, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_conn);

    ESP_LOGI(TAG, "WiFi HTTP handlers registered");
    return ESP_OK;
}
