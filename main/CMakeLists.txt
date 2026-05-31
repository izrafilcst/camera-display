idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES display espnow_link reassembly decoder render link_state ble_pair pair_nvs
)
