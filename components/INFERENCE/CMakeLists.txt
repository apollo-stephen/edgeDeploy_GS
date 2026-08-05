idf_component_register(
    SRCS "inference.cpp"
    INCLUDE_DIRS "include"
    REQUIRES CAMERA esp32-camera modev1
    PRIV_REQUIRES esp_timer
)
