#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

enum link_status_t : uint8_t {
    LINK_BOOT         = 0,  // no packet received since boot
    LINK_CONNECTED    = 1,  // last packet within LINK_FREEZE_MS
    LINK_FREEZE       = 2,  // [LINK_FREEZE_MS, LINK_DISCONNECT_MS]
    LINK_DISCONNECTED = 3,  // > LINK_DISCONNECT_MS
};

// Thresholds in ms (spec §7). enum-of-uint32_t form lets the values stay
// constexpr and external-linkage-free in both C and C++ TUs.
enum : uint32_t {
    LINK_FREEZE_MS     = 200,
    LINK_DISCONNECT_MS = 3000,
    LINK_IDLE_UNKNOWN  = 0xFFFFFFFFu,  // returned by link_state_idle_ms before first rx
};

bool link_state_init(void);

// Marks a valid packet as received at now_ms. Safe to call from ESP-NOW RX cb.
void link_state_mark_rx(uint32_t now_ms);

// Computes the current state for now_ms.
link_status_t link_state_query(uint32_t now_ms);

// Returns milliseconds since the last rx, or LINK_IDLE_UNKNOWN before
// the first packet. Wraparound-safe via unsigned subtraction (handles
// now_ms < last_rx_ms when the 32-bit ms counter wraps ~49 days).
uint32_t link_state_idle_ms(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
