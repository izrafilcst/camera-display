# Sprint 3 — Security Audit

- **Date**: 2026-05-30
- **Auditor**: security-auditor agent
- **Branch audited**: `main`
- **Commits in scope**:
  - `64ea05c` — test: red tests for sprint 3 link state machine
  - `0cd0805` — feat(link_state): atomic state machine with wrap-safe idle math
  - `f514aec` — feat(render,main): link status overlays, thumb capture, link_ui task
- **Scope**:
  - `components/link_state/include/link_state.h`
  - `components/link_state/link_state.cpp`
  - `components/render/include/render.h`
  - `components/render/render.cpp` (overlays + thumb additions)
  - `main/app_main.cpp` (link_ui_task, mark_rx wiring)
  - `main/CMakeLists.txt`, `components/{link_state,render}/CMakeLists.txt`

---

## 1. Executive Summary

Sprint 3 adds a passive, read-mostly link-state surface (atomic timestamp + 4 LCD draw paths). No new attacker-controlled parser was introduced — the parser remains the Sprint 2 reassembly path. However, the way `link_state_mark_rx` is wired in `app_main.cpp` quietly **broadens** what counts as "link liveness" beyond video traffic, which expands the surface an off-path attacker can use to keep the receiver in `CONNECTED` even when no usable video is being delivered. This is the dominant finding of the sprint.

There are no Critical findings. **One High** (S3-01 — link-liveness gating). **Two Medium** (S3-02 thumbnail TOCTOU on async DMA; S3-03 cross-call timestamp skew with unsigned underflow). The rest are Low / Info.

| Severity | Count |
|----------|------:|
| Critical | 0     |
| High     | 1     |
| Medium   | 2     |
| Low      | 4     |
| Info     | 3     |
| **Total**| **10**|

**Verdict**: APPROVED for merge **conditional** on S3-01 being patched (one-line move in `app_main.cpp:on_msg`) or explicitly accepted as a documented threat-model gap before Sprint 4.

---

## 2. Findings Table

| ID    | Sev    | Category                   | Location                                          | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                                | Mitigation                                                                                                                                                                                                                                                                                       |
|-------|--------|----------------------------|---------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| S3-01 | High   | Link-state spoof / DoS-mask | `main/app_main.cpp:47-50` (on_msg)               | `link_state_mark_rx(now_ms)` is called for **every** valid `esnow_hdr_t` (any `msg_type`) **before** the `if (msg_type != MSG_VIDEO_FRAG) return` filter. Combined with espnow_link's per-`msg_type` replay window (`s_last_seq[256]` indexed by type), an attacker can keep the link state pinned to `LINK_CONNECTED` by injecting `MSG_JOYSTICK` (0x30) or `MSG_COMMAND` (0x40) — types that currently have **no** replay/seq enforcement on the RX path (only `MSG_TELEMETRY` is checked). Net effect: the operator's UI says "link OK" while zero video frames decode, defeating the FREEZE / DISCONNECTED indicator that is the entire point of Sprint 3. The attacker doesn't even need the real peer MAC if `peer_mac_is_placeholder` is still in effect (V0 default), and the cost is one packet every <200 ms. | Gate `link_state_mark_rx` on `msg_type == MSG_VIDEO_FRAG` (single-line move below the filter). Optionally also accept `MSG_TELEMETRY` *after* the replay check passes — but **never** before message-type validation. Document the policy in `link_state.h` next to the `mark_rx` declaration.   |
| S3-02 | Medium | Async DMA / lifetime        | `components/render/render.cpp:128-129`            | `lcd->pushImage(120, 140, 80, 60, s_thumb)` may queue an async DMA transfer from `s_thumb` and return before bytes leave SRAM. LovyanGFX's `endWrite()` blocks for the bus release but the SPI DMA descriptor lifetime depends on the underlying bus; meanwhile, `xSemaphoreGive(s_mutex)` runs **after** `endWrite`. If a future change introduces a non-blocking blit, `render_capture_thumb` from `decode_task` could rewrite `s_thumb` while the prior DISCONNECTED screen is still being DMA'd. Today the `endWrite` waits, so this is latent. | Either (a) copy `s_thumb` to a local stack buffer (4800 B; fits in default 4096 link_ui_task stack only if increased to 8192 — currently sized at 4096 in `xTaskCreatePinnedToCore`), or (b) explicitly call `display_wait_dma()` after `pushImage` and before releasing the mutex, mirroring the pattern used elsewhere. Add a comment "DO NOT make pushImage non-blocking without copying s_thumb". |
| S3-03 | Medium | Race / unsigned underflow   | `components/link_state/link_state.cpp:35,40` + `main/app_main.cpp:47,96-103` | `link_state_mark_rx` stores `s_last_rx_ms` with `memory_order_relaxed` from Core 0 (Wi-Fi RX context). `link_ui_task` on Core 1 reads `now_ms = esp_timer_get_time()/1000` **once**, then queries the atomic. With relaxed ordering, the query can observe a `last_rx_ms` value strictly greater than the cached `now_ms` (because mark_rx ran between the timestamp sample and the load). The unsigned subtraction then yields `~0xFFFFFFFE` (≈49 days), trips `idle > LINK_DISCONNECT_MS`, and the UI flickers to `DISCONNECTED` for one 100 ms tick. Same applies in the stats loop in `app_main.cpp`. Not exploitable, but produces user-visible incorrect overlays under heavy traffic. | Re-sample `now_ms` *after* the atomic load and clamp: `if ((int32_t)(now_ms - last) < 0) return 0;`. Or do a single `esp_timer_get_time()` call inside `link_state_query` / `link_state_idle_ms` and pass nothing from the caller (small API change). Document the invariant in `link_state.h`. |
| S3-04 | Low    | SPI saturation / DoS        | `main/app_main.cpp:93-115` (link_ui_task)         | At 10 Hz, `LINK_DISCONNECTED` repaints `fillScreen` (153,600 B at ~40 MHz SPI → ~30 ms) + text every 100 ms. Continuous flap (attacker oscillating link by sending exactly one MSG_VIDEO_FRAG every 200–3000 ms) drives a full-screen redraw 10 times/sec — **75 %+ of the SPI bus** spent on the offline screen alone, contending with any decode task that resumes briefly. The decoder is starved and the FPS gauge stays at zero even when intermittent frames arrive. | Add a `prev_state` / `prev_since_bucket` check and skip the redraw when the screen content hasn't materially changed (e.g., only repaint when `since_ms/1000` increments). Move `fillScreen` to once-per-state-entry; thereafter only repaint the time field via a partial `fillRect`. Cap link_ui_task redraw cadence to 2 Hz (vTaskDelay 500 ms) for the DISCONNECTED screen; keep 10 Hz only for the FREEZE blink. |
| S3-05 | Low    | UI correctness (stale badge) | `components/render/render.cpp:95-112` (render_show_freeze) | The FREEZE badge is painted when `blink == true` and **not erased** when `blink == false`. The expected blink-off-half is achieved only because the next decoder frame overwrites the region — but during FREEZE the decoder is by definition not delivering frames, so the badge appears **constantly on** instead of blinking. Visual bug masquerading as a security indicator: the operator cannot distinguish FREEZE from "stale display". | When `blink == false`, repaint the badge region with the underlying frame pixel data (read from `s_fb[s_back_idx ^ 1]` via `pushImage` of a 66×16 strip) or with a fixed background color. Alternatively, draw the badge as a translucent / inverted rectangle that toggles between two visible states (e.g., red↔dark-red) so both phases are positively rendered. |
| S3-06 | Low    | Static buffer in BSS        | `components/render/render.cpp:30-31`              | `static uint16_t s_thumb[80*60]` is 9,600 B allocated in BSS for the entire program lifetime. Trivially small but represents memory-policy drift: every Sprint adds a few KB of "static UI scratch" that never gets accounted in the 8 MB PSRAM budget review. The thumb is also **never zeroized on `render_init`** — first DISCONNECTED screen before any frame has decoded would show garbage if `s_thumb_valid` weren't gating it (gating exists, so no leak in practice). | Move `s_thumb` into PSRAM via `heap_caps_calloc(80*60, sizeof(uint16_t), MALLOC_CAP_SPIRAM)` allocated in `render_init`, freed in `render_deinit`. Memset to 0 on init for defense-in-depth even though gated by `s_thumb_valid`. |
| S3-07 | Low    | TOCTOU on link_ui state     | `main/app_main.cpp:97,103`                        | `link_ui_task` calls `link_state_query(now_ms)` then `link_state_idle_ms(now_ms)`. Between the two calls, `s_last_rx_ms` can update from the Wi-Fi callback. The result is benign (the idle_ms reported by the overlay may be 100 ms stale) but compounds with S3-03: a fresh `mark_rx` between the two reads can produce `idle_ms = 0` while `state == LINK_DISCONNECTED`, printing "offline: 0 s" on the disconnected screen. | Snapshot both fields atomically by adding `link_state_snapshot(uint32_t now_ms, link_status_t* out_state, uint32_t* out_idle_ms)` that does a single atomic load. |
| S3-08 | Info   | reinterpret_cast on void*   | `components/render/render.cpp:34-38` (get_lcd)    | `reinterpret_cast<lgfx::LGFX_Device*>(display_get_lgfx_ptr())` is sound today because `s_lcd` is constructed as `LGFX_ILI9341_Red` (derives from `lgfx::LGFX_Device`) in `display.cpp:8` and the void* is the same pointer value. Note: if a future change makes `display_get_lgfx_ptr` return a pointer to a wrapper struct (instead of the LGFX object itself), the cast becomes UB silently. The null-check in `get_lcd` only catches uninitialized state, not type drift. | Add a `static_assert(std::is_base_of_v<lgfx::LGFX_Device, LGFX_ILI9341_Red>)` next to `s_lcd`'s declaration in `display.cpp` (already covered indirectly by inheritance), and add a one-line comment in `render.cpp` documenting the invariant. |
| S3-09 | Info   | Wrap-arithmetic CTI         | `components/link_state/link_state.cpp:35,40`      | The user asked about constant-time issues in `uint32_t` wrap arithmetic. **None present and none required.** `s_last_rx_ms` is not a secret; timing side-channels are not in the threat model for this IoT receiver. The subtraction is a single AGU op on Xtensa with no branch dependent on secret data. | None — explicitly out of scope. Documented here so future audits don't relitigate.                                                                                                                                                                              |
| S3-10 | Info   | Stack budget for link_ui     | `main/app_main.cpp:169`                           | `xTaskCreatePinnedToCore(link_ui_task, "link_ui", 4096, ...)` — 4 KB stack. The deepest call path uses `lcd->printf(...)` which pulls in vsnprintf (~512 B). Safe today, but if S3-02's "copy s_thumb to stack" mitigation is taken (9,600 B), the stack must grow to ≥ 16 KB. Pre-emptively documenting. | If S3-02 is implemented by stack-copying, raise `link_ui_task` stack to 12288 and add `uxTaskGetStackHighWaterMark` logging. Prefer the PSRAM-copy variant in S3-06. |

---

## 3. Validation against Sprint 3 spec

| Requirement (spec §7)                                     | Implementation                                                            | Status        |
|-----------------------------------------------------------|---------------------------------------------------------------------------|---------------|
| State machine: BOOT/CONNECTED/FREEZE/DISCONNECTED         | `link_status_t` enum, `link_state_query` chains `>` thresholds            | OK            |
| Thresholds 200 ms / 3000 ms                               | `LINK_FREEZE_MS = 200`, `LINK_DISCONNECT_MS = 3000`                       | OK            |
| Atomic / lock-free between Core 0 and Core 1              | `std::atomic<uint32_t>` + `std::atomic<bool>`, `memory_order_relaxed`     | OK (note S3-03) |
| Wrap-safe idle math (49-day rollover)                     | Comment on line 33-35, single unsigned sub                                | OK            |
| Thumbnail of last good frame                              | `render_capture_thumb`, 4× downsample 80×60, mutex-guarded                | OK (note S3-02) |
| FREEZE overlay (blinking)                                 | `render_show_freeze` 250 ms half-period                                   | Partial (S3-05) |
| DISCONNECTED screen with offline counter + thumb          | `render_show_disconnected`                                                | OK            |
| `link_ui_task` polls at 10 Hz, silent in CONNECTED        | `vTaskDelay(pdMS_TO_TICKS(100))`, `case LINK_CONNECTED: break;`           | OK (note S3-04) |
| Transition logged                                         | `ESP_LOGI(TAG, "link %d -> %d", last, st);`                               | OK            |
| `link_state_mark_rx` called on valid rx                   | Called on every msg_type with valid header — **too broad**                | **S3-01**     |

---

## 4. Attack-Surface Analysis (new in Sprint 3)

| Vector                                                                  | Pre-S3 surface | New surface in S3                                                          | Mitigation present?                                            |
|-------------------------------------------------------------------------|----------------|----------------------------------------------------------------------------|----------------------------------------------------------------|
| Off-path packet to mask link loss                                       | n/a            | `mark_rx` on any valid `esnow_hdr_t`                                       | **No** — S3-01                                                  |
| Replay of MSG_VIDEO_FRAG headers to spoof CONNECTED                      | n/a            | No replay window on VIDEO_FRAG                                            | Partial — Sprint 2 S2-* notes this; not addressed here          |
| Flap-induced SPI saturation via link_ui_task redraws                    | n/a            | 10 Hz `fillScreen` while DISCONNECTED                                      | **No** — S3-04                                                  |
| TOCTOU between query/idle reads                                         | n/a            | Two separate atomic reads + one cached `now_ms`                            | **No** — S3-03 / S3-07                                          |
| Thumbnail buffer corruption                                             | n/a            | Shared `s_thumb` across capture (decode task) and present (link_ui task)   | Mutex guarded; OK provided pushImage stays blocking — S3-02     |
| Stale FREEZE badge masquerading as link OK                              | n/a            | Blink-off relies on decoder repaint which is absent during FREEZE          | **No** — S3-05                                                  |
| Reused void* → LGFX_Device cast                                         | inherited      | Still in render.cpp                                                       | Null-check only; type-drift latent — S3-08                      |

### Combined risk (S3-01 × S2-05 ESP-NOW unencrypted V0)

The most operationally important combined risk: with Sprint 2's S2-05 (ESP-NOW unencrypted in V0) **plus** S3-01 (mark_rx on any msg_type), any RF-range attacker can produce a "looks-connected" UI on the receptor while no real video is being delivered. The legitimate transmitter being out of range — or jammed by the same attacker — becomes indistinguishable from "everything is fine". For a robot-control product this is a safety-relevant deception. **Fix S3-01 before Sprint 4 even if everything else slips.**

---

## 5. Static-analysis pass

`cppcheck` is **not installed** on the audit host. The equivalent command to run locally:

```bash
cppcheck --enable=warning,style,performance,portability \
         --inline-suppr \
         --std=c++17 \
         --suppress=missingIncludeSystem \
         -I components/link_state/include \
         -I components/render/include \
         -I components/display/include \
         components/link_state/link_state.cpp \
         components/render/render.cpp \
         main/app_main.cpp
```

Expected findings (predicted from manual read):

- `render.cpp:127` — `%lu` with `unsigned long` cast: OK across ESP32 (long = 32 bits).
- `link_state.cpp:35,40` — no warnings; clean atomic pattern.
- `app_main.cpp:97-103` — `link_state_query` then `link_state_idle_ms` reads of the same atomic — cppcheck may flag as "duplicate condition" but it's an intentional design (see S3-07).

A clean cppcheck run is **not a gate** for this sprint (none of the findings above are syntactic). It should be added to CI before Sprint 5.

---

## 6. Backlog for Sprint 4

Sprint 4's planned surface (per backlog: ACK/NACK feedback to TX, telemetry of rx quality, possible button input for `link_ui_task` toggle):

1. **Pre-condition: land S3-01 fix** before any code that uses `link_state` for control-flow decisions ships. Once a backchannel exists, an attacker who can pin `link_state == CONNECTED` can also influence the *transmitter's* behaviour through the feedback loop.
2. **MSG_JOYSTICK / MSG_COMMAND replay protection** — extend `check_and_update_seq` (currently MSG_TELEMETRY only) to every msg_type that carries a `seq` field, or document why each exempt type is safe.
3. **`link_state_snapshot` API** — needed for any task that wants a coherent (state, idle_ms) pair; pre-requisite for log/metrics export to TX. Closes S3-03 and S3-07.
4. **PSRAM-resident thumbnail + lock-free swap** — if Sprint 4 adds multiple thumbnails (e.g., last-good per source, or a snapshot ring), follow the S3-06 mitigation now to avoid expanding BSS surface.
5. **link_ui repaint suppression** — implement S3-04 before any battery-powered build, where SPI traffic translates directly to milliamps.
6. **Button input for forced reconnect**: GPIO 1/2 buttons (Sprint 1 S-09) — if Sprint 4 uses them to trigger `espnow_link_reinit` or similar, the debounce + edge detection becomes a new attack surface (long-press DoS). File a tracking item now.

---

## 7. Files audited

- `/home/linux/dev/droner6/camera-display/components/link_state/include/link_state.h`
- `/home/linux/dev/droner6/camera-display/components/link_state/link_state.cpp`
- `/home/linux/dev/droner6/camera-display/components/link_state/CMakeLists.txt`
- `/home/linux/dev/droner6/camera-display/components/render/include/render.h`
- `/home/linux/dev/droner6/camera-display/components/render/render.cpp`
- `/home/linux/dev/droner6/camera-display/components/render/CMakeLists.txt`
- `/home/linux/dev/droner6/camera-display/main/app_main.cpp`
- `/home/linux/dev/droner6/camera-display/main/CMakeLists.txt`

Cross-referenced with:
- `/home/linux/dev/droner6/camera-display/components/espnow_link/espnow_link.cpp` (for msg_type / replay context)
- `/home/linux/dev/droner6/camera-display/components/espnow_link/include/wire_types.h`
- `/home/linux/dev/droner6/camera-display/components/display/display.cpp`
- `/home/linux/dev/droner6/camera-display/components/display/include/display.h`
- `/home/linux/dev/droner6/camera-display/docs/audits/sprint-1-security.md`
- `/home/linux/dev/droner6/camera-display/docs/audits/sprint-2-security.md`

---

## 8. Conclusion

Sprint 3 is structurally sound — atomic state machine, mutex-guarded LCD access, wrap-safe arithmetic. The implementation faithfully delivers the FREEZE/DISCONNECTED indicators promised in the spec. The dominant finding (**S3-01**) is a single-line wiring mistake in `app_main.cpp` that converts the new link indicator into a deception surface; fixing it costs nothing and meaningfully strengthens the safety guarantee of the UI. The two Mediums (S3-02, S3-03) are latent and require small API/refactor work that fits naturally into Sprint 4's planned snapshot/feedback API.

**Recommendation**: APPROVE merge to `main`. Open three issues:
- `audit/S3-01` (High, must close before Sprint 4 merge)
- `audit/S3-02` (Medium, before any non-blocking-DMA change to LovyanGFX)
- `audit/S3-03` (Medium, fold into Sprint 4 `link_state_snapshot` work)

Lows (S3-04, S3-05, S3-06, S3-07) and Infos (S3-08, S3-09, S3-10) tracked as backlog with no Sprint 4 gating.
