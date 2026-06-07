idf_component_register(
    SRCS "main.c" "config.c" "http_client.c" "printer.c" "web_config.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls json esp_http_server esp_timer driver wifi_manager
)
