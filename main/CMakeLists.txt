idf_component_register(
    SRCS "main.c" "config.c" "wifi_manager.c" "websocket_client.c" "printer.c" "web_config.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client json esp_websocket_client esp_http_server driver
)
