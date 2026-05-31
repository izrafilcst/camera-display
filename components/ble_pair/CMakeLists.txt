set(_ble_pair_srcs "ble_pair_state.cpp" "ble_pair.cpp")
set(_ble_pair_reqs nvs_flash log esp_event freertos esp_hw_support)
if(CONFIG_BT_NIMBLE_ENABLED)
    list(APPEND _ble_pair_reqs bt)
endif()
idf_component_register(
    SRCS ${_ble_pair_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include/internal"
    REQUIRES ${_ble_pair_reqs}
)
