idf_component_register(
    SRCS "espnow_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash
)
