idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES nvs_flash CAMERA WIFIAP HTTP_CAPTURE INFERENCE HEALTH
    INCLUDE_DIRS "."
)
