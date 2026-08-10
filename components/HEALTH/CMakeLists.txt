idf_component_register(
    SRCS "health.c"
    INCLUDE_DIRS "include"
    REQUIRES INFERENCE heap
    PRIV_REQUIRES esp_timer
)
