idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES nvs_flash WIFIAP
    INCLUDE_DIRS "."
)
