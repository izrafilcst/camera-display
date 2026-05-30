# Sprint 3 — Tratamento de Link (Implementation Spec)

**Origin**: refines `docs/superpowers/plans/2026-05-26-sprint-3-link-handling.md` against spec §7.
**Status**: implemented in `64ea05c..f514aec`.

This document captures architectural decisions taken during implementation
that diverge from or fill gaps in the plan. The plan stays authoritative
for task ordering and acceptance criteria.

---

## 1. Concurrency primitive for `s_last_rx_ms`

**Decision**: `std::atomic<uint32_t> s_last_rx_ms` with `std::memory_order_relaxed`
for all loads and stores. `s_has_rx` is also `std::atomic<bool>`.

**Why**: writes happen in the ESP-NOW RX callback (Core 0, Wi-Fi context).
Reads happen in `link_ui_task` (Core 1) and the stats loop (Core 0). On a
multi-core LX7 the relaxed atomic is mapped to a plain aligned 32-bit
load/store, which is already atomic on the hardware. The only invariant we
need is "reads see a self-consistent uint32_t value" — and a brief stale
read just delays the state transition by one 100 ms tick, which is
imperceptible relative to the 200 ms FREEZE threshold.

**Alternatives considered**:
- `volatile uint32_t`: works but compiler is free to widen or torn-read on
  some targets; standard does not guarantee atomicity.
- `taskENTER_CRITICAL`: overkill for two counters with no inter-field
  consistency requirement; also unavailable in plain ISR contexts.
- Sequential consistency: would impose an unnecessary memory barrier on the
  RX hot path with no observable benefit.

## 2. Wraparound-safe idle computation

**Decision**: compute idle as plain unsigned subtraction `now_ms - last_rx_ms`.
Drop the plan's `now_ms >= t ? (now_ms - t) : 0` ternary.

**Why**: `esp_timer_get_time() / 1000` is read as `uint32_t` (we cast it that
way) and wraps every ~49.7 days. The ternary returns 0 for any post-wrap
query, falsely reporting "fresh" and blocking the FREEZE transition forever
until the next `mark_rx`. The unsigned subtraction wraps modulo 2^32 and
yields the correct forward distance as long as the true gap is < 2^31 ms
(~24.8 days), which is far beyond any realistic FREEZE / DISCONNECT window.

A dedicated host test (`test_wraparound_idle_computation`) pins this in:
mark_rx at `0xFFFFFFF0`, query at `0x00000010`, expect idle 32 ms.

## 3. `display_get_lgfx_ptr` stays `void*`

**Decision**: do NOT change the return type to `lgfx::LGFX_Device*`.

**Why**: Sprint 1 review finding F3 explicitly stripped C++ types from
`display.h` to keep it a pure-C interface. The plan's suggestion to add
`namespace lgfx { class LGFX_Device; }` to `display.h` would re-introduce
that coupling. Instead, `render.cpp` has a local helper:

```cpp
static inline lgfx::LGFX_Device* get_lcd(void) {
    return reinterpret_cast<lgfx::LGFX_Device*>(display_get_lgfx_ptr());
}
```

This contains the C-to-C++ bridge to a single translation unit.

## 4. LCD concurrency: shared `s_mutex`

**Decision**: `render_show_freeze`, `render_show_disconnected`, and
`render_capture_thumb` all acquire the same `s_mutex` that `render_present`
already uses, and call `display_wait_dma()` before drawing.

**Why**: the plan notes that decode and UI tasks are "mutually exclusive in
V0" (decode only renders while CONNECTED, UI only while ≠ CONNECTED) but
also flags that transitions can collide. Reusing the existing mutex closes
that gap with zero extra state. `display_wait_dma()` makes sure no DMA
SPI transfer is mid-flight when the UI starts a new transaction.

## 5. Thumbnail capture timing & buffer

**Decision**: `render_capture_thumb` is called by the decode task
immediately after `render_present()`, reads `s_fb[s_back_idx ^ 1]`, and is
guarded by both `s_mutex` and a `s_has_presented` latch.

**Why**:
- After `render_present` flips `s_back_idx`, the just-blitted buffer
  becomes the *front* and lives at `s_fb[s_back_idx ^ 1]`. The decoder is
  contractually forbidden from touching that buffer until the next swap.
- `s_has_presented` makes the very first capture (before any frame) a
  silent no-op instead of copying garbage PSRAM.
- The mutex prevents `render_present` from swapping back_idx mid-copy.

## 6. `link_state_mark_rx` placement in `on_msg`

**Decision**: call `link_state_mark_rx(now_ms)` for *any* valid message
type, before the `MSG_VIDEO_FRAG` filter.

**Why**: spec §7 defines link liveness in terms of *traffic*, not
*video frames*. Future telemetry, heartbeat, or control messages should
also reset the FREEZE clock. The Sprint 2 audit's S2-15 follow-up
(replay window per msg_type) already constrains what counts as "valid"
upstream of this callback.

**Trade-off**: an attacker with a known peer MAC could keep the state
`CONNECTED` indefinitely by spraying valid telemetry. Mitigated by REQ-5
(Kconfig peer MAC) — without the right MAC, `esp_now_recv_cb` never
fires. Tracked as INFO in this sprint's audit, not a blocker.

## 7. `link_ui_task` placement and cadence

**Decision**: Core 1, priority 4 (below decode at 6, render-equivalent), poll
at 10 Hz (`vTaskDelay(pdMS_TO_TICKS(100))`).

**Why**:
- Same core as `decode_task` so the FreeRTOS scheduler arbitrates them
  cooperatively rather than via cross-core IPI.
- Lower priority than decode so video frames never starve waiting for the
  overlay to repaint.
- 10 Hz polling is fast enough that the FREEZE transition is invisible
  (200 ms threshold ⇒ at most 100 ms detection latency).

---

## What the plan covered and we kept as-is

- File structure (`components/link_state/...`, `host_tests/test_link_state.cpp`).
- Threshold constants (`LINK_FREEZE_MS = 200`, `LINK_DISCONNECT_MS = 3000`).
- Status enum names and integer values (`LINK_BOOT=0..LINK_DISCONNECTED=3`).
- `render_capture_thumb` after `render_present` in the decode loop.
- `link_ui_task` switch on state, log transitions with `last`.
- Acceptance criteria (state transitions, FREEZE icon, DISCONNECTED screen,
  reconnect < 2 s).

## What the plan got wrong (corrected here)

- The `now >= t ? t - now : 0` ternary breaks at the 32-bit ms wrap. See §2.
- The proposed `lgfx::LGFX_Device*` forward declaration in `display.h`
  contradicts Sprint 1 F3. See §3.
- The plan's `render_show_freeze` / `..._disconnected` did not show mutex
  acquisition; they need it. See §4.
