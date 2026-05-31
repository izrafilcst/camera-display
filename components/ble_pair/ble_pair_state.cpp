// Sprint 4 — ble_pair pure state machine
//
// No NimBLE dependency: take an event in, transition state out. The real
// ble_pair.cpp translates NimBLE callbacks into ble_pair_event_t calls.
// Keeping this layer pure lets the host tests exercise every transition
// without bringing up a BT stack.

#include "ble_pair.h"
#include "internal/ble_pair_state.h"
#include <cstring>

void ble_pair_sm_init(ble_pair_sm_t* sm) {
    if (!sm) return;
    sm->state = BLE_PAIR_IDLE;
    sm->err   = BLE_PAIR_ERR_NONE;
    std::memset(sm->tx_mac, 0, sizeof(sm->tx_mac));
}

// Helpers: set a terminal state, but never overwrite the first cause.
static inline void to_error(ble_pair_sm_t* sm, ble_pair_err_t err) {
    sm->state = BLE_PAIR_ERROR;
    sm->err   = err;
}

void ble_pair_sm_feed(ble_pair_sm_t* sm,
                      ble_pair_event_t ev,
                      const uint8_t* payload, size_t plen) {
    if (!sm) return;

    // Terminal states absorb every subsequent event silently.
    if (sm->state == BLE_PAIR_DONE || sm->state == BLE_PAIR_ERROR) return;

    // Disconnect at any non-terminal point counts as an early disconnect.
    if (ev == EV_DISCONNECTED) {
        to_error(sm, BLE_PAIR_ERR_DISCONNECTED_EARLY);
        return;
    }

    switch (sm->state) {
    case BLE_PAIR_IDLE:
        if (ev == EV_START) sm->state = BLE_PAIR_SCANNING;
        return;

    case BLE_PAIR_SCANNING:
        if (ev == EV_ADV_MATCH)    { sm->state = BLE_PAIR_CONNECTING; return; }
        if (ev == EV_SCAN_TIMEOUT) { to_error(sm, BLE_PAIR_ERR_SCAN_TIMEOUT); return; }
        return;  // anything else: ignore

    case BLE_PAIR_CONNECTING:
        if (ev == EV_CONNECTED)      { sm->state = BLE_PAIR_DISCOVERING; return; }
        if (ev == EV_CONNECT_FAILED) { to_error(sm, BLE_PAIR_ERR_CONNECT_FAILED); return; }
        return;

    case BLE_PAIR_DISCOVERING:
        if (ev == EV_DISCOVERED)       { sm->state = BLE_PAIR_WRITING_PIN; return; }
        if (ev == EV_DISCOVER_FAILED)  { to_error(sm, BLE_PAIR_ERR_DISCOVER_FAILED); return; }
        return;

    case BLE_PAIR_WRITING_PIN:
        if (ev == EV_PIN_WRITE_OK)        { sm->state = BLE_PAIR_READING_TX_MAC; return; }
        if (ev == EV_PIN_WRITE_REJECTED)  { to_error(sm, BLE_PAIR_ERR_PIN_REJECTED); return; }
        return;

    case BLE_PAIR_READING_TX_MAC:
        if (ev == EV_TX_MAC_READ_OK) {
            if (!payload || plen != 6) {
                to_error(sm, BLE_PAIR_ERR_BAD_TX_MAC_LEN);
                return;
            }
            std::memcpy(sm->tx_mac, payload, 6);
            sm->state = BLE_PAIR_WRITING_RX_MAC;
            return;
        }
        if (ev == EV_TX_MAC_READ_BAD_LEN) {
            to_error(sm, BLE_PAIR_ERR_BAD_TX_MAC_LEN);
            return;
        }
        return;

    case BLE_PAIR_WRITING_RX_MAC:
        if (ev == EV_RX_MAC_WRITE_OK)         { sm->state = BLE_PAIR_DONE; return; }
        if (ev == EV_RX_MAC_WRITE_AUTH_FAIL)  { to_error(sm, BLE_PAIR_ERR_RX_MAC_AUTH); return; }
        return;

    case BLE_PAIR_DONE:
    case BLE_PAIR_ERROR:
        return;  // already handled above; keeps the compiler happy
    }
}

// ---------------------------------------------------------------------------
// Pretty-printers — used in logs and host tests.
// ---------------------------------------------------------------------------
const char* ble_pair_state_str(ble_pair_state_t s) {
    switch (s) {
    case BLE_PAIR_IDLE:           return "IDLE";
    case BLE_PAIR_SCANNING:       return "SCANNING";
    case BLE_PAIR_CONNECTING:     return "CONNECTING";
    case BLE_PAIR_DISCOVERING:    return "DISCOVERING";
    case BLE_PAIR_WRITING_PIN:    return "WRITING_PIN";
    case BLE_PAIR_READING_TX_MAC: return "READING_TX_MAC";
    case BLE_PAIR_WRITING_RX_MAC: return "WRITING_RX_MAC";
    case BLE_PAIR_DONE:           return "DONE";
    case BLE_PAIR_ERROR:          return "ERROR";
    }
    return "?";
}

const char* ble_pair_err_str(ble_pair_err_t e) {
    switch (e) {
    case BLE_PAIR_ERR_NONE:                return "NONE";
    case BLE_PAIR_ERR_SCAN_TIMEOUT:        return "SCAN_TIMEOUT";
    case BLE_PAIR_ERR_CONNECT_FAILED:      return "CONNECT_FAILED";
    case BLE_PAIR_ERR_DISCOVER_FAILED:     return "DISCOVER_FAILED";
    case BLE_PAIR_ERR_PIN_REJECTED:        return "PIN_REJECTED";
    case BLE_PAIR_ERR_RX_MAC_AUTH:         return "RX_MAC_AUTH";
    case BLE_PAIR_ERR_BAD_TX_MAC_LEN:      return "BAD_TX_MAC_LEN";
    case BLE_PAIR_ERR_STACK_INIT:          return "STACK_INIT";
    case BLE_PAIR_ERR_DISCONNECTED_EARLY:  return "DISCONNECTED_EARLY";
    }
    return "?";
}
