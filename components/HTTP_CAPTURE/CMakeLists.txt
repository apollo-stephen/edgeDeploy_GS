idf_component_register(
    SRCS "http_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server CAMERA WIFIAP INFERENCE esp_system heap
    PRIV_REQUIRES esp_timer
)
