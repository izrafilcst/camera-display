idf_component_register(
    SRCS "ble_pair_state.cpp" "ble_pair.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include/internal"
    REQUIRES bt nvs_flash log esp_event freertos
)
