idf_component_register(
    SRCS "decoder.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_jpeg esp_timer log
)
