# Sprint 1 Code Review — Display Bring-up

**Date**: 2026-05-27
**Reviewer**: code-review swarm agent
**Commits reviewed**: b64a214..8e523dd (8 commits)
**Branch**: main
**Verdict**: APPROVE WITH NITS

---

## Findings

| # | Severity | File:line | Description | Suggested Fix |
|---|----------|-----------|-------------|---------------|
| F1 | minor | `components/display/include/lgfx_ili9341_config.h:32` | `cfg.use_lock = true` contradicts `cfg.bus_shared = false` and the architect spec (§5.3). `use_lock` enables a per-transaction SPI mutex; `bus_shared = false` is specifically set to avoid that overhead (~5-10 µs/frame). Having both means the mutex is acquired but the shared-bus arbitration logic is bypassed — net result is unnecessary lock overhead without protection benefit. | Set `cfg.use_lock = false` to match `bus_shared = false` and the no-mutex intent. Add a comment: `// bus_shared=false + use_lock=false: display owns SPI2 exclusively` |
| F2 | minor | `components/display/display.cpp:18-21` | `gpio_set_direction()` and `gpio_set_level()` return `esp_err_t` but the return values are silently discarded. If a pin number is invalid the GPIO driver logs an error but execution continues silently into `new LGFX_ILI9341_Red(...)`. | Wrap calls with `ESP_ERROR_CHECK_WITHOUT_ABORT()` or explicitly check and log: `if (gpio_set_direction(...) != ESP_OK) { ESP_LOGE(...); return false; }` |
| F3 | minor | `components/display/include/display.h:84-88` | `rgb565()` is declared in the public `display.h` header outside the `extern "C"` block, exposing an implementation-detail helper as part of the public API. A C translation unit including `display.h` will get an implicit `extern "C"` prototype mismatch at link time if it ever calls `rgb565` directly. The host test already forward-declares `rgb565` independently — the `display.h` declaration is redundant and leaks internals. | Move `rgb565` declaration to a new internal header `components/display/test_patterns_internal.h`, or make it `static` in `test_patterns.cpp` and update the host-test forward-declaration. The `display.h` public surface should not expose it. |
| F4 | minor | `main/app_main.cpp:34,37` | The 4-byte alignment is asserted (`assert`) AND then redundantly re-checked with a ternary in the log format string on line 37. The second check is dead code after a passing assert. More importantly, `assert()` in ESP-IDF production builds may be compiled out if `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y` is ever set; for safety-critical memory alignment in embedded DMA code, prefer a log-and-abort pattern that survives all optimisation levels. | Replace the two-step assert + ternary with a single: `if (((uintptr_t)psram_fb & 0x3u) != 0u) { ESP_LOGE(TAG, "psram fb misaligned: %p", ...); abort(); }` This is always compiled in and self-documents the error. |
| F5 | nit | `components/display/include/lgfx_ili9341_config.h:68` | The `Light_PWM` config has `cfg.invert = false` with a comment referring the reader to the panel `cfg.invert` comment above ("ver nota cfg.invert acima"). However the two fields are unrelated: `Panel_ILI9341::config.invert` controls pixel inversion (INVON/INVOFF command), while `Light_PWM::config.invert` controls PWM polarity for the backlight transistor. The cross-reference comment is misleading; they may need different values simultaneously. | Change the `Light_PWM` comment to: `// PWM polarity: false = HIGH turns on BL. Swap to true for PNP/P-channel circuits. Independent of panel invert above.` |
| F6 | nit | `components/display/display.cpp:46` | `s_lcd->setAddrWindow(0, 0, s_lcd->width(), s_lcd->height())` is called inside `startWrite()...endWrite()` on every frame, even though the window never changes between frames in the current usage. LovyanGFX batches `setAddrWindow` as a SPI command; calling it unconditionally adds ~8-12 bytes of SPI overhead per blit (~3-5 µs at 80 MHz). | Call `s_lcd->setAddrWindow(0, 0, 320, 240)` once at the end of `display_init()` so that blit only calls `startWrite() / writePixels() / endWrite()`. Add a comment noting the assumption. |
| F7 | nit | `main/app_main.cpp:48` | `(frame / 60u) % 4u` — the pattern switches every 60 frames. At ~30 fps (33 ms/frame + fill time) this is approximately 2 s per pattern, but with `vTaskDelay(pdMS_TO_TICKS(33))` the actual period depends on fill time. The comment says "every 2 s" which is only accurate if fill is negligible. Not a bug, but the comment is imprecise for a file that will be read during performance tuning. | Change comment to: `// ~2 s per pattern at 30 fps (fill time excluded from frame count)` |
| F8 | nit | `components/display/host_tests/CMakeLists.txt:11` | `-std=c++17` is passed as a bare string in `target_compile_options` rather than through `target_compile_features(test_patterns PRIVATE cxx_std_17)`. The bare string works but bypasses CMake's standard management and will cause a warning in CMake 3.25+. | Replace with: `target_compile_features(test_patterns PRIVATE cxx_std_17)` and remove `-std=c++17` from the options list. |

---

## Spec Adherence

All 8 plan tasks are implemented and in order:

| Task | Commit | Status |
|------|--------|--------|
| Task 1 — ESP-IDF skeleton | b64a214 | Done. `sdkconfig.defaults` includes the architect's USB-CDC fix from spec §1.1. |
| Task 2 — Central pinout | a6b2b67 | Done. `PIN_JOY_Y = 14` correctly applied per architect §5.1. Comment references spec. |
| Task 2.5 — Multimeter doc | 66104a8 | Done. Matches spec §1.3 exactly. Correctly inserted between Task 2 and Task 3. |
| Task 3 — Display skeleton | 247bfa3 | Done. API matches spec §2 signatures. |
| Task 4 — LovyanGFX init | 5457fab | Done. `cfg.invert` comment per architect §5.2 present. CS neutralization per spec §1.3 tip applied. |
| Task 5 — Test patterns + demo loop | 3625624 | Done. 22/22 host tests pass per commit message. |
| Task 6 — 80 MHz target | d819b10 | Done. `LCD_SPI_HZ_TARGET` used. hw validation noted as pending. |
| Task 7 — PSRAM allocator | f2d2c95 | Done. 4-byte alignment assert present per architect §5.4. Fallback path with warning log. |

### Architect adjustments verified

- `PIN_JOY_Y == 14` (GPIO 14, not 3): YES, `main/pinout.h:20`
- `cfg.invert` has required comment: YES, `lgfx_ili9341_config.h:54-58`
- 4-byte alignment assert in PSRAM smoke: YES, `app_main.cpp:34`

---

## Wins — Carry Into Future Sprints

1. **GPIO CS neutralization before SPI init** (`display.cpp:17-21`): the proactive `gpio_set_level(PIN_SD_CS, 1)` / `gpio_set_level(PIN_TOUCH_CS, 1)` before `s_lcd->init()` directly implements the architect's "tip adicional" from spec §1.3. This pattern should be applied to any future peripheral sharing the bus.

2. **Double-init guard** (`display.cpp:11-13`): the `s_lcd != nullptr` early return prevents accidental re-initialization, avoiding double-`new` and resource leak. The correct cleanup path (`delete s_lcd; s_lcd = nullptr;`) on init failure is also clean.

3. **PSRAM allocator fallback with explicit log** (`display.cpp:65`): the two-phase `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA` then `MALLOC_CAP_SPIRAM` fallback with `ESP_LOGW` gives Sprint 2 exactly the visibility needed to diagnose staging-buffer decisions without code changes.

4. **Host test harness** (`host_tests/`): 22 unit tests covering `rgb565`, `pattern_color_bars`, `pattern_gradient`, and `pattern_checker` with edge cases (cell_px=0 clamp, boundary pixels, channel precision) — all runnable on the host with `make test` before any hardware is available. This pattern should be extended to every component added in Sprint 2+.

5. **`display.h` as a pure-C interface**: the `extern "C"` wrapper and `void*` return for `display_get_lgfx_ptr` correctly insulate callers from LovyanGFX's C++ template types. This boundary will be important in Sprint 3 when the network task (Core 0, C linkage) needs to signal render readiness without touching display internals.

6. **Commit message quality**: every commit includes the reason (not just "what"), references architect spec sections, and notes hw validation status. The co-author attribution is consistent.

---

## Tech Debt

| Item | Accepted Trade-off | Revisit In |
|------|--------------------|------------|
| `vTaskDelay(pdMS_TO_TICKS(33))` sleep-based pacing | Correct for Sprint 1 demo loop. Will introduce jitter when fill time varies. Sprint 2 should replace with a deadline-based `xTaskDelayUntil()` pattern. | Sprint 2 |
| `display_blit_full` calls `setAddrWindow` every frame | Simpler for bring-up; wastes ~5 µs/frame. See F6. | Sprint 2 (before perf benchmarks) |
| Single framebuffer in DRAM (no double-buffer) | Intentional for Sprint 1 per plan. Tearing will be visible in `pattern_tearing_stripes`. Double-buffer is Sprint 2 scope. | Sprint 2 |
| `s_lcd` raw global pointer (not RAII-wrapped) | Acceptable for embedded singleton; no destructor path needed. Would need change only if dynamic display re-init is ever required (unlikely). | Sprint 5+ if needed |
| `display_alloc_framebuffer_psram` allocates fixed 320x240 size | Correct for this hardware. A `size_t pixels` parameter would be more general but adds interface complexity with no current benefit. | Sprint 3 if dirty-rect blitting is added |
| `idf.py build` not run in worktree | Noted in multiple commit messages. All commits are structural + logic; LovyanGFX dependency unavailable in CI worktree. This is acceptable for Sprint 1 but should be resolved before Sprint 2 merge by adding a CI step or docker build. | Sprint 2 setup |

---

## Summary

8 commits cleanly implement the 7+1 plan tasks. All three architect-mandated adjustments (GPIO 14, `cfg.invert` comment, alignment assert) are applied. The code is idiomatic ESP-IDF C++, avoids `any`-style casts except the intentional `void*` in the public API, and the host test suite provides meaningful coverage of the pure-logic layer.

The one noteworthy functional issue (F1: `use_lock=true` with `bus_shared=false`) wastes ~5-10 µs per frame but does not cause correctness bugs in Sprint 1. It should be fixed before Sprint 2 performance baseline measurements.
