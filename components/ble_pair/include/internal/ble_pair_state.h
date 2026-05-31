#pragma once
#include "ble_pair.h"
#include <cstddef>

typedef enum {
    EV_START,
    EV_ADV_MATCH,
    EV_SCAN_TIMEOUT,
    EV_CONNECTED,
    EV_CONNECT_FAILED,
    EV_DISCOVERED,
    EV_DISCOVER_FAILED,
    EV_PIN_WRITE_OK,
    EV_PIN_WRITE_REJECTED,
    EV_TX_MAC_READ_OK,         // payload holds the 6 MAC bytes
    EV_TX_MAC_READ_BAD_LEN,
    EV_RX_MAC_WRITE_OK,
    EV_RX_MAC_WRITE_AUTH_FAIL,
    EV_DISCONNECTED,
} ble_pair_event_t;

typedef struct {
    ble_pair_state_t state;
    uint8_t          tx_mac[6];
    ble_pair_err_t   err;
} ble_pair_sm_t;

void ble_pair_sm_init(ble_pair_sm_t* sm);
void ble_pair_sm_feed(ble_pair_sm_t* sm,
                      ble_pair_event_t ev,
                      const uint8_t* payload, size_t plen);
