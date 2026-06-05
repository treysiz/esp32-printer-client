idf_component_register(
    SRCS "wifi_manager.c" "wifi_scan.c" "wifi_http.c" "wifi_portal.c" "wifi_status.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server json lwip driver
)
