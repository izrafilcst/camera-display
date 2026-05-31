#pragma once
#include <cstdint>
#include <cstdbool>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_PAIR_IDLE            = 0,
    BLE_PAIR_SCANNING        = 1,
    BLE_PAIR_CONNECTING      = 2,
    BLE_PAIR_DISCOVERING     = 3,
    BLE_PAIR_WRITING_PIN     = 4,
    BLE_PAIR_READING_TX_MAC  = 5,
    BLE_PAIR_WRITING_RX_MAC  = 6,
    BLE_PAIR_DONE            = 7,
    BLE_PAIR_ERROR           = 8,
} ble_pair_state_t;

typedef enum {
    BLE_PAIR_ERR_NONE                = 0,
    BLE_PAIR_ERR_SCAN_TIMEOUT        = 1,
    BLE_PAIR_ERR_CONNECT_FAILED      = 2,
    BLE_PAIR_ERR_DISCOVER_FAILED     = 3,
    BLE_PAIR_ERR_PIN_REJECTED        = 4,
    BLE_PAIR_ERR_RX_MAC_AUTH         = 5,
    BLE_PAIR_ERR_BAD_TX_MAC_LEN      = 6,
    BLE_PAIR_ERR_STACK_INIT          = 7,
    BLE_PAIR_ERR_DISCONNECTED_EARLY  = 8,
} ble_pair_err_t;

bool        ble_pair_run(uint32_t pin, uint8_t tx_mac_out[6], ble_pair_err_t* err_out);
void        ble_pair_get_rx_mac(uint8_t out[6]);
const char* ble_pair_state_str(ble_pair_state_t s);
const char* ble_pair_err_str(ble_pair_err_t e);

#ifdef __cplusplus
}
#endif
