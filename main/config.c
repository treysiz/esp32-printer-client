/*
 * config.c - NVS Configuration Manager Implementation
 * ESP32 Printer Client System
 */

#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "CONFIG";
static const char *NVS_NAMESPACE = "printer_cfg";

/* Global config instance */
static device_config_t g_config;

/* ── Helper: read string from NVS, fall back to default ─────────────── */
static void nvs_read_str(nvs_handle_t h, const char *key,
                         char *out, size_t max_len, const char *def)
{
    size_t len = max_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        strncpy(out, def, max_len);
        out[max_len - 1] = '\0';
        ESP_LOGI(TAG, "Key '%s' not in NVS, using default: %s", key, def);
    }
}

/* ── Helper: read u16 from NVS, fall back to default ────────────────── */
static void nvs_read_u16(nvs_handle_t h, const char *key,
                         uint16_t *out, uint16_t def)
{
    esp_err_t err = nvs_get_u16(h, key, out);
    if (err != ESP_OK) {
        *out = def;
        ESP_LOGI(TAG, "Key '%s' not in NVS, using default: %u", key, def);
    }
}

/* ── Helper: read u8 (bool) from NVS, fall back to default ─────────── */
static void nvs_read_bool(nvs_handle_t h, const char *key,
                          bool *out, bool def)
{
    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(h, key, &val);
    if (err != ESP_OK) {
        *out = def;
        ESP_LOGI(TAG, "Key '%s' not in NVS, using default: %s",
                 key, def ? "true" : "false");
    } else {
        *out = (val != 0);
    }
}

/* ── Public: Initialize NVS ─────────────────────────────────────────── */
esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing and re-init...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* ── Public: Load config from NVS (with defaults) ───────────────────── */
esp_err_t config_load(device_config_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "config_load: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(device_config_t));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS namespace not found, loading all defaults");
        /* Populate with compile-time defaults */
        strncpy(cfg->wifi_ssid,  DEFAULT_WIFI_SSID,  MAX_SSID_LEN);
        strncpy(cfg->wifi_pass,  DEFAULT_WIFI_PASS,   MAX_PASS_LEN);
        strncpy(cfg->store_id,   DEFAULT_STORE_ID,    MAX_STORE_ID_LEN);
        strncpy(cfg->device_id,  DEFAULT_DEVICE_ID,   MAX_DEVICE_ID_LEN);
        strncpy(cfg->printer_ip, DEFAULT_PRINTER_IP,  MAX_IP_LEN);
        cfg->printer_port = DEFAULT_PRINTER_PORT;
        strncpy(cfg->server_url, DEFAULT_SERVER_URL,  MAX_URL_LEN);

        cfg->use_static_ip = DEFAULT_USE_STATIC_IP;
        strncpy(cfg->static_ip,  DEFAULT_STATIC_IP,   MAX_IP_LEN);
        strncpy(cfg->gateway,    DEFAULT_GATEWAY,      MAX_IP_LEN);
        strncpy(cfg->netmask,    DEFAULT_NETMASK,      MAX_IP_LEN);
        strncpy(cfg->dns,        DEFAULT_DNS,           MAX_IP_LEN);

        memcpy(&g_config, cfg, sizeof(device_config_t));
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    /* Read each field, falling back to default if missing */
    nvs_read_str(h, "wifi_ssid",  cfg->wifi_ssid,  sizeof(cfg->wifi_ssid),  DEFAULT_WIFI_SSID);
    nvs_read_str(h, "wifi_pass",  cfg->wifi_pass,   sizeof(cfg->wifi_pass),  DEFAULT_WIFI_PASS);
    nvs_read_str(h, "store_id",   cfg->store_id,    sizeof(cfg->store_id),   DEFAULT_STORE_ID);
    nvs_read_str(h, "device_id",  cfg->device_id,   sizeof(cfg->device_id),  DEFAULT_DEVICE_ID);
    nvs_read_str(h, "printer_ip", cfg->printer_ip,  sizeof(cfg->printer_ip), DEFAULT_PRINTER_IP);
    nvs_read_u16(h, "printer_pt", &cfg->printer_port, DEFAULT_PRINTER_PORT);
    nvs_read_str(h, "server_url", cfg->server_url,  sizeof(cfg->server_url), DEFAULT_SERVER_URL);

    nvs_read_bool(h, "use_sip",   &cfg->use_static_ip, DEFAULT_USE_STATIC_IP);
    nvs_read_str(h, "static_ip",  cfg->static_ip,   sizeof(cfg->static_ip),  DEFAULT_STATIC_IP);
    nvs_read_str(h, "gateway",    cfg->gateway,      sizeof(cfg->gateway),    DEFAULT_GATEWAY);
    nvs_read_str(h, "netmask",    cfg->netmask,      sizeof(cfg->netmask),    DEFAULT_NETMASK);
    nvs_read_str(h, "dns",        cfg->dns,           sizeof(cfg->dns),        DEFAULT_DNS);

    nvs_close(h);

    memcpy(&g_config, cfg, sizeof(device_config_t));

    ESP_LOGI(TAG, "Config loaded: SSID=%s store=%s device=%s printer=%s:%u",
             cfg->wifi_ssid, cfg->store_id, cfg->device_id,
             cfg->printer_ip, cfg->printer_port);
    return ESP_OK;
}

/* ── Public: Save config to NVS ─────────────────────────────────────── */
esp_err_t config_save(const device_config_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "config_save: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(h, "wifi_ssid",  cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass",  cfg->wifi_pass);
    nvs_set_str(h, "store_id",   cfg->store_id);
    nvs_set_str(h, "device_id",  cfg->device_id);
    nvs_set_str(h, "printer_ip", cfg->printer_ip);
    nvs_set_u16(h, "printer_pt", cfg->printer_port);
    nvs_set_str(h, "server_url", cfg->server_url);

    nvs_set_u8(h,  "use_sip",    cfg->use_static_ip ? 1 : 0);
    nvs_set_str(h, "static_ip",  cfg->static_ip);
    nvs_set_str(h, "gateway",    cfg->gateway);
    nvs_set_str(h, "netmask",    cfg->netmask);
    nvs_set_str(h, "dns",        cfg->dns);

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Configuration saved to NVS");
    }

    nvs_close(h);

    memcpy(&g_config, cfg, sizeof(device_config_t));
    return err;
}

/* ── Public: Get global config pointer ──────────────────────────────── */
const device_config_t *config_get(void)
{
    return &g_config;
}

/* ── Public: Check if config is valid ───────────────────────────────── */
bool config_is_valid(void)
{
    /* If SSID is empty or still the default, it's not valid */
    if (strlen(g_config.wifi_ssid) == 0) {
        return false;
    }
    if (strcmp(g_config.wifi_ssid, DEFAULT_WIFI_SSID) == 0) {
        return false;
    }
    return true;
}

/* ── Public: Clear NVS and restart ──────────────────────────────────── */
void config_clear(void)
{
    ESP_LOGW(TAG, "Erasing NVS and restarting...");
    nvs_flash_erase();
    esp_restart();
}

