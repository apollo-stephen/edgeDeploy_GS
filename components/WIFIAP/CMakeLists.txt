idf_component_register(
    SRCS "wifi_ap.c"
    REQUIRES esp_wifi esp_common
    PRIV_REQUIRES esp_event esp_netif esp_hw_support
    INCLUDE_DIRS "include"
)
