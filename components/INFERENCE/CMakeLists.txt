idf_component_register(
    SRCS "inference.cpp"
    INCLUDE_DIRS "include"
    REQUIRES CAMERA esp32-camera modev2
    PRIV_REQUIRES esp_timer
)
