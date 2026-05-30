// Sprint 3 — link_state component
//
// Thread model: link_state_mark_rx is invoked from the ESP-NOW RX callback
// (Core 0, Wi-Fi context). link_state_query / link_state_idle_ms are read
// from link_ui_task (Core 1) and the periodic stats loop.
//
// Memory ordering: s_has_rx uses release/acquire so the s_last_rx_ms
// store is guaranteed to be visible to any reader that observes
// has_rx == true. Without that, the reviewer (Sprint 3 F-02) showed a
// real race: on the very first packet, a Core 1 reader could see
// has_rx=true before the new last_rx_ms propagated, compute
// idle = now - 0 > 3000 ms (true after Wi-Fi init), and flicker to
// DISCONNECTED for one 100 ms tick. The release/acquire pair fixes it.

#include "link_state.h"
#include <atomic>

namespace {
std::atomic<uint32_t> s_last_rx_ms{0};
std::atomic<bool>     s_has_rx{false};
}

bool link_state_init(void) {
    s_last_rx_ms.store(0, std::memory_order_relaxed);
    s_has_rx.store(false, std::memory_order_release);
    return true;
}

void link_state_mark_rx(uint32_t now_ms) {
    // last_rx must be visible before has_rx flips to true.
    s_last_rx_ms.store(now_ms, std::memory_order_relaxed);
    s_has_rx.store(true, std::memory_order_release);
}

uint32_t link_state_idle_ms(uint32_t now_ms) {
    if (!s_has_rx.load(std::memory_order_acquire)) return LINK_IDLE_UNKNOWN;
    // Unsigned subtraction wraps correctly modulo 2^32; the result is the
    // shortest forward distance from last_rx to now in ms, which is what we
    // want as long as the gap is < 2^31 ms (~24.8 days).
    return now_ms - s_last_rx_ms.load(std::memory_order_relaxed);
}

link_status_t link_state_query(uint32_t now_ms) {
    if (!s_has_rx.load(std::memory_order_acquire)) return LINK_BOOT;
    const uint32_t idle = now_ms - s_last_rx_ms.load(std::memory_order_relaxed);
    if (idle > LINK_DISCONNECT_MS) return LINK_DISCONNECTED;
    if (idle > LINK_FREEZE_MS)     return LINK_FREEZE;
    return LINK_CONNECTED;
}
