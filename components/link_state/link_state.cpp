// Sprint 3 — link_state component
//
// Thread model: link_state_mark_rx is invoked from the ESP-NOW RX callback
// (Core 0, Wi-Fi context). link_state_query / link_state_idle_ms are read
// from link_ui_task (Core 1) and the periodic stats loop. std::atomic with
// memory_order_relaxed is sufficient because the only invariant we need is
// "reads see a self-consistent uint32_t value" — the timestamp is monotonic
// in practice and a momentarily stale read just causes a one-iteration
// delay in the state transition, which is invisible to the user.

#include "link_state.h"
#include <atomic>

namespace {
std::atomic<uint32_t> s_last_rx_ms{0};
std::atomic<bool>     s_has_rx{false};
}

bool link_state_init(void) {
    s_last_rx_ms.store(0, std::memory_order_relaxed);
    s_has_rx.store(false, std::memory_order_relaxed);
    return true;
}

void link_state_mark_rx(uint32_t now_ms) {
    s_last_rx_ms.store(now_ms, std::memory_order_relaxed);
    s_has_rx.store(true, std::memory_order_relaxed);
}

uint32_t link_state_idle_ms(uint32_t now_ms) {
    if (!s_has_rx.load(std::memory_order_relaxed)) return LINK_IDLE_UNKNOWN;
    // Unsigned subtraction wraps correctly modulo 2^32; the result is the
    // shortest forward distance from last_rx to now in ms, which is what we
    // want as long as the gap is < 2^31 ms (~24.8 days).
    return now_ms - s_last_rx_ms.load(std::memory_order_relaxed);
}

link_status_t link_state_query(uint32_t now_ms) {
    if (!s_has_rx.load(std::memory_order_relaxed)) return LINK_BOOT;
    const uint32_t idle = now_ms - s_last_rx_ms.load(std::memory_order_relaxed);
    if (idle > LINK_DISCONNECT_MS) return LINK_DISCONNECTED;
    if (idle > LINK_FREEZE_MS)     return LINK_FREEZE;
    return LINK_CONNECTED;
}
