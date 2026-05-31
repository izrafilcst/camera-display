# Sprint 4 — Security Audit

- **Date**: 2026-05-31
- **Auditor**: security-auditor agent
- **Branch audited**: `main`
- **Commits in scope**:
  - `660b401` — docs: sprint 4 implementation spec refined
  - `93d0fe4` — feat(s4): pair_nvs + ble_pair_state logic
  - `d9b5fef` — feat(s4): NimBLE pair_run + app_main boot branches + Kconfig
- **Scope**:
  - `components/pair_nvs/{include/pair_nvs.h, pair_nvs.cpp, CMakeLists.txt}`
  - `components/ble_pair/{include/ble_pair.h, include/internal/ble_pair_state.h, ble_pair_state.cpp, ble_pair.cpp, CMakeLists.txt, Kconfig}`
  - `main/{app_main.cpp, Kconfig.projbuild, CMakeLists.txt}`
  - `sdkconfig.defaults`

---

## 1. Executive Summary

Sprint 4 introduces the first **inbound RF authentication surface**: BLE CENTRAL handshake (`ble_pair_run`) and a persisted authorization token (NVS `pairing/tx_mac`). The receiver does not advertise — it is strictly a client — which closes the entire inbound-connect attack vector. That is the dominant positive of the sprint.

The dominant negative is structural rather than implementation-level: the V0 protocol is **trust-on-first-use with a 4-byte PIN over plaintext BLE** and the receiver permanently binds to whatever MAC answers the handshake. Every attacker-relevant finding below is either a consequence of that V0 decision (already documented, deferred to Sprint 8 per spec §6) or a small implementation drift around it (cleanup-path race, NVS reset semantics, reboot loop).

**No Critical** findings. **Two High** (S4-01 spoof-of-CAM-TX after legitimate paired record exists; S4-02 NimBLE host task lifecycle on init-failure leaks the controller). **Three Medium** (S4-03 missing explicit `nimble_port_freertos_deinit`; S4-04 reboot-loop power/wear amplification; S4-05 `nvs_flash_erase` on NO_FREE_PAGES wipes all NVS data). The rest are Low / Info.

| Severity | Count |
|----------|------:|
| Critical | 0     |
| High     | 2     |
| Medium   | 3     |
| Low      | 6     |
| Info     | 5     |
| **Total**| **16**|

**Verdict**: APPROVED for merge. S4-02 and S4-03 should be fixed in a small follow-up before Sprint 5 to keep the cleanup path robust under real-world BLE failures; S4-01, S4-07, S4-09, S4-11 are explicit V0 carve-outs and **must be re-evaluated as part of Sprint 8** (Secure Boot + Encrypted NVS + LE SC bonding).

---

## 2. Findings Table

| ID    | Sev    | Category                       | Location                                                                 | Description | Mitigation |
|-------|--------|--------------------------------|--------------------------------------------------------------------------|-------------|------------|
| S4-01 | High   | RF authentication (V0 spoof)   | `components/ble_pair/ble_pair.cpp` `on_gap_event` + adv match            | Receiver pairs with **any** advertiser broadcasting GAP local name `"CAM-TX"`. No address-type allowlist, no LE Secure Connections, no out-of-band confirmation. After the static PIN (default `1234`, plaintext) is accepted, the receiver writes its own MAC and **permanently persists the spoofer's MAC** in NVS. The receiver will then accept ESP-NOW from that MAC across every reboot until `CONFIG_RECEIVER_FORCE_PAIR_AGAIN` is toggled and reflashed. Combined with Sprint 2 S2-05 (ESP-NOW unencrypted V0), this is the end-to-end video-deception surface. | V0 acceptance per spec §6 / spec §11.7. Track as `audit/S4-01`, **gate Sprint 8**: (a) `ble_gap_security_initiate` + LE Secure Connections + MITM-protected bonding; (b) PIN moves to numeric-comparison with LCD confirmation; (c) reject inbound advertisements whose address type is `BLE_ADDR_RANDOM_NRPA` unless the bond record matches. Interim defense-in-depth: print the bound TX MAC at boot so operator can spot drift. |
| S4-02 | High   | NimBLE controller leak on init failure | `components/ble_pair/ble_pair.cpp:ble_pair_run` error path        | When `nimble_port_init()` returns non-OK, the function deletes `s_done_eg` and returns. It does **not** call `nimble_port_deinit()` — but `nimble_port_init` can fail *partway through* (BT controller enabled but host stack not registered). The BT controller, NimBLE memory pool, and internal mutexes stay allocated. The next branch in `app_main.cpp` proceeds to `espnow_link_init` → `esp_wifi_init` while the BT controller is still half-up; on ESP32-S3 shared-radio this can cause `ESP_ERR_INVALID_STATE` or hard-fault in dual-mode coexistence. Also makes a future "retry pairing" path impossible. | Always call `nimble_port_deinit()` (and `esp_bt_controller_disable() + esp_bt_controller_deinit()`) on the failure branch. Wrap NimBLE bring-up in a `goto cleanup` or RAII guard. Add a smoke test that aborts `nimble_port_init` mid-way (via mock) and verifies that the followup `esp_wifi_init()` returns OK. |
| S4-03 | Medium | NimBLE cleanup ordering        | `components/ble_pair/ble_pair.cpp` cleanup block                          | `ble_pair_run` calls only `nimble_port_stop(); nimble_port_deinit();` — **`nimble_port_freertos_deinit()` is missing** from the top-level cleanup. The call is referenced in `host_task` after `nimble_port_run`, but that path only runs if `nimble_port_stop` triggers an orderly exit and the scheduler picks `host_task` before `nimble_port_deinit` tears down its prerequisites. Per spec §1.9 the documented correct sequence is `stop → freertos_deinit → deinit`. Skipping the explicit call leaves the `"nimble_host"` task in the scheduler (race) or relies on indirect cleanup that has changed between IDF minor releases (5.1 vs 5.2 vs 6.x). | Replace with the explicit three-call sequence: `nimble_port_stop(); nimble_port_freertos_deinit(); nimble_port_deinit();`. Remove the call from `host_task` (becomes dead code). Add `ESP_LOGI` of heap before/after to confirm the budget. |
| S4-04 | Medium | DoS via reboot loop            | `main/app_main.cpp` pairing failure branch                                | If pairing fails (TX absent, PIN rejected, RF noise) the receiver `vTaskDelay(5000); esp_restart();` and re-attempts indefinitely. Each loop: ~5 s + bootloader ~1 s + NimBLE init ~200 ms + scan up to `CONFIG_BLE_SCAN_TIMEOUT_S=30` s = **~36 s/cycle, ~100 reboots/hour**. Risks: (a) **NVS wear** — every reboot calls `pair_nvs_init` which on `ESP_ERR_NVS_NO_FREE_PAGES` erases NVS; sustained failure wears the pairing namespace pages; (b) **Battery exhaustion** on portable builds — NimBLE scan TX power dominates the cycle budget; (c) `esp_reset_reason` becomes `ESP_RST_SW` permanently, masking real hardware reset causes. | Add an exponential-backoff counter persisted in RTC slow memory (`RTC_DATA_ATTR uint8_t s_pair_fail_count`), cap reboots to 3, then enter a "BLE pairing failed — power cycle to retry" idle screen via `render_show_disconnected`. After N=3, block on `vTaskDelay(portMAX_DELAY)`. Clear counter only if `esp_reset_reason() == ESP_RST_POWERON`. |
| S4-05 | Medium | Destructive NVS reset semantics | `components/pair_nvs/pair_nvs.cpp:pair_nvs_init`                          | `pair_nvs_init` calls `nvs_flash_erase()` on `ESP_ERR_NVS_NO_FREE_PAGES` and `ESP_ERR_NVS_NEW_VERSION_FOUND`. `nvs_flash_erase` wipes **the entire default NVS partition**, not just the `pairing` namespace. Today this only affects `pairing` keys, but the moment Sprint 5+ adds a stats/config namespace, that data is collateral damage. An attacker who can force `NO_FREE_PAGES` (via flash poisoning or many failed `nvs_set_blob` calls if such a path is added) gets a **free factory-reset**, which forces a re-pair — precondition for S4-01. | Scope the recovery: try `nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME)` only after logging namespace contents, OR move to a dedicated `pairing` NVS partition with its own `nvs_flash_init_partition` so the erase only wipes pairing state. Document why current behavior is acceptable for V0. |
| S4-06 | Low    | NVS blob length not strict on input | `components/pair_nvs/pair_nvs.cpp:pair_nvs_load_tx_mac`              | `size_t sz = 6;` then `nvs_get_blob(..., tmp, &sz)`. ESP-IDF semantics return `ESP_ERR_NVS_INVALID_LENGTH` if stored is larger; if stored is smaller, succeeds and updates `sz`. The post-check `sz != 6` catches that — good. However, future patterns that query size first with `sz=0` then read with attacker-influenced `sz` would be vulnerable. The defensive placeholder re-check catches the most common corruption. Latent. | Tighten to `if (err != ESP_OK || sz != 6) return false;` with `sz` initialized to **buffer capacity** (`sizeof(tmp)`) and `static_assert(sizeof(tmp) == 6)`. Document `pair_nvs_load_tx_mac` is the only sanctioned reader. |
| S4-07 | Low    | PIN in BLE TX queue + flash    | `components/ble_pair/ble_pair.cpp:start_pin_write`                        | The 4-byte LE PIN buffer is built on the stack, handed to `ble_gattc_write_flat`, which `memcpy`s into a NimBLE mbuf chain that lives in heap until air. Anyone with a heap dump can recover it. Also: `CONFIG_RECEIVER_PAIRING_PIN` is in **flash** as `sdkconfig.h #define`. | V0 acceptance. Sprint 8: (a) generate PIN at first boot, store hashed in NVS, show cleartext once on LCD; (b) explicitly zero `buf[4]` after `ble_gattc_write_flat`; (c) PIN length ≥ 8 digits. |
| S4-08 | Low    | `CONFIG_RECEIVER_PEER_MAC` bypasses pairing | `main/app_main.cpp` override branch                          | When `CONFIG_RECEIVER_PEER_MAC` is non-placeholder, BLE pairing is skipped. Intended (build-time override). Audit-relevant: attacker with build-system access (CI compromise) can pre-burn a MAC and disable BLE pairing. Only signal is the WARN log. | Acceptable V0. (a) Verify warning is at WARN (yes); (b) default to placeholder `AA:BB:CC:DD:EE:FF` in release configs (verified in `components/espnow_link/Kconfig`); (c) Add CI lint that fails build if non-placeholder in release. |
| S4-09 | Low    | NVS readback not authenticated | `components/pair_nvs/pair_nvs.cpp:pair_nvs_load_tx_mac`                   | Attacker with physical flash access can flip `pairing/tx_mac` and `pairing/paired=1`. On next boot receiver loads it (passes placeholder check) and trusts. No HMAC, no signature, no Secure Boot dependency. | V0 acceptance — spec §6 Sprint 8 item. Concrete: store MAC together with HMAC keyed by eFuse-derived secret, verified in `pair_nvs_load_tx_mac`. |
| S4-10 | Low    | `s_rx_mac_le` / `s_sm.tx_mac` in BSS | `components/ble_pair/ble_pair.cpp` static state                     | RX MAC and TX MAC persist in BSS across firmware lifetime. Core dump exposes them. RX MAC also reachable via `esp_read_mac`, so no new disclosure. TX MAC is same value as NVS so leaking via core dump only matters if attacker lacks flash access (which is precondition for S4-09). Net: redundant. | Acceptable V0. Defense-in-depth: clear `s_sm` and `s_rx_mac_le` before `ble_pair_run` returns. Add `volatile_memset_secure` helper for future secret fields. |
| S4-11 | Low    | Adv match without address-type check | `components/ble_pair/ble_pair.cpp:adv_has_name + ble_gap_connect`    | Match is only on GAP local name `"CAM-TX"`. The advertiser's `addr.type` is forwarded to `ble_gap_connect` as-is. Attacker can use random non-resolvable address to dodge MAC-based forensics. No RSSI floor. | V0 acceptance. Sprint 8: (a) reject `BLE_ADDR_RANDOM_NRPA` unless bond record matches; (b) `RSSI > CONFIG_BLE_MIN_RSSI_DBM` floor (default e.g. `-70 dBm`); (c) log address type + full address of every match attempt. |
| S4-12 | Info   | State machine bypass via reordered events | `components/ble_pair/ble_pair_state.cpp` switch/case             | The switch/case explicitly ignores events not allowed for the current state. **Correct posture** — disallowed events are silently dropped. Walked every state × event manually and confirmed no early-exit. Host tests cover this. | None — design works. Document the invariant in `ble_pair_state.h` ("disallowed events are silently dropped; this is the security property"). |
| S4-13 | Info   | Stack canaries cover NimBLE host task | `sdkconfig.defaults` `CONFIG_COMPILER_STACK_CHECK`                    | S-02's `CONFIG_COMPILER_STACK_CHECK=y` + `MODE_NORM=y` are **compile-time global** — `-fstack-protector-strong` applies to every TU. NimBLE host task inherits protection. | None — works as intended. |
| S4-14 | Info   | `link_state_mark_rx` still gated on decode success | `main/app_main.cpp:decode_task`                          | Sprint 3 audit S3-01 mitigation is intact: `mark_rx` called only after `decoder_decode_to_rgb565` returns `dt >= 0`. Confirmed Sprint 4 did not regress this. | None — regression check passed. |
| S4-15 | Info   | `host_tests/` not built by IDF      | `components/{ble_pair,pair_nvs}/host_tests/Makefile`                       | Host tests live in `components/*/host_tests/` and are excluded from IDF build (`idf_component_register` SRCS only enumerates sibling .cpp). No risk of linking `PAIR_NVS_HOST_BUILD` shim into firmware. | None. |
| S4-16 | Info   | cppcheck not installed on audit host | (no path)                                                              | Static-analysis pass attempted, cppcheck not in agent environment. Manual review covered the same surface. | Install `cppcheck` in CI. See §5 for command. |

---

## 3. Validation against Sprint 4 spec

| Requirement (spec §1, §4, §6)                                     | Implementation                                                            | Status        |
|-------------------------------------------------------------------|---------------------------------------------------------------------------|---------------|
| Pure state machine, no NimBLE dep                                 | `ble_pair_state.cpp` includes only `<cstring>` + headers                  | OK            |
| Terminal states absorb events                                     | Early-return at top of `ble_pair_sm_feed`                                 | OK            |
| `EV_DISCONNECTED` → `DISCONNECTED_EARLY` only if non-terminal     | Gated after the DONE/ERROR check                                          | OK            |
| `nimble_port_deinit` before `esp_wifi_init`                       | `ble_pair.cpp` cleanup then `espnow_link_init`                            | OK (but see S4-03) |
| `nimble_port_freertos_deinit` in cleanup                          | Only in `host_task` continuation; not in top-level cleanup                | **S4-03**     |
| Cleanup on `nimble_port_init` failure                             | Deletes EG, does NOT call `nimble_port_deinit`                            | **S4-02**     |
| Heap logged in 3 points                                           | Only at boot                                                              | Partial — spec §1.5 unmet (Info; not security-critical) |
| `pair_nvs_load_tx_mac` re-checks placeholder defensively          | `pair_nvs.cpp` defensive re-check                                         | OK            |
| `pair_nvs_save_tx_mac` rejects placeholder MAC                    | `pair_nvs.cpp` `is_valid_mac_for_persist`                                 | OK            |
| Kconfig precedence: SMOKE > FORCE > PEER_MAC override > NVS > BLE | `app_main.cpp`                                                            | OK            |
| `RTC_FAST_ATTR s_last_pair_err`                                   | **Not implemented** — spec §1.8 deferred                                  | Info (not a security finding) |

---

## 4. Attack-Surface Analysis (new in Sprint 4)

| Vector                                                                 | Pre-S4 surface | New surface in S4                                                          | Mitigation present?                                            |
|------------------------------------------------------------------------|----------------|----------------------------------------------------------------------------|----------------------------------------------------------------|
| Spoof `CAM-TX` advertiser, claim trust                                 | n/a            | `adv_has_name` matches local name only                                     | No — S4-01 (V0 accepted)                                       |
| PIN brute-force                                                        | n/a            | RX writes PIN, TX rejects on mismatch                                      | TX-side responsibility; RX has no rate-limit                   |
| MITM passive PIN capture                                               | n/a            | Plaintext over LE 1M PHY                                                   | No — S4-07 (V0 accepted)                                       |
| Persistent backdoor via NVS modification                               | n/a            | `pair_nvs_load_tx_mac` blindly trusts a non-placeholder value              | No — S4-09 (V0 accepted, requires flash access)                |
| Reboot loop induced DoS                                                | n/a            | `esp_restart` every ~35 s under no-TX condition                            | No — S4-04                                                     |
| NimBLE host task lifecycle leak after failed init                      | n/a            | Cleanup skips `nimble_port_deinit` on `nimble_port_init` failure           | No — S4-02                                                     |
| Stale FreeRTOS host task after stop                                    | n/a            | Cleanup missing explicit `nimble_port_freertos_deinit`                     | No — S4-03                                                     |
| Build-time override (`CONFIG_RECEIVER_PEER_MAC`)                       | inherited      | Same path, now gates BLE bypass                                            | Logged at WARN — S4-08                                         |
| Address-type spoof via RANDOM_NRPA                                     | n/a            | `ble_gap_connect` blindly forwards addr.type                               | No — S4-11 (V0 accepted)                                       |

### Combined risk (S4-01 × S2-05)

The Sprint 4 spec §6 puts S4-01 (CAM-TX spoof) as a V0 acceptance, but combined with Sprint 2 S2-05 (ESP-NOW unencrypted) the failure mode is **persistent** rather than transient. An attacker who wins the BLE handshake **once** controls the video pipeline across all subsequent reboots without further BLE exposure (ESP-NOW is unencrypted and only MAC-filtered). The operator cannot tell from the LCD whether the connected TX is the legitimate one. The remediation must happen at both layers in Sprint 8: bonded LE SC for the handshake **and** PMK/LMK for the ESP-NOW link.

---

## 5. Static-analysis pass

`cppcheck` not installed on audit host. Local command:

```bash
cppcheck --enable=warning,style,performance,portability \
         --inline-suppr \
         --std=c++17 \
         --suppress=missingIncludeSystem \
         -I components/pair_nvs/include \
         -I components/ble_pair/include \
         -I components/ble_pair/include/internal \
         -I components/espnow_link/include \
         components/pair_nvs/pair_nvs.cpp \
         components/ble_pair/ble_pair_state.cpp \
         components/ble_pair/ble_pair.cpp \
         main/app_main.cpp
```

Predicted findings from manual read: design-intended only. Not a gate. Add to CI before Sprint 5.

---

## 6. Backlog for Sprint 5+

1. **Land S4-02 and S4-03** before any code that calls `ble_pair_run` more than once (re-pair button). Current cleanup is not idempotent.
2. **`pair_nvs_namespace_isolation`** — once Sprint 5 adds a second NVS namespace, S4-05 becomes a real bug.
3. **Backchannel auth** — Sprint 5 RX→TX telemetry under unencrypted ESP-NOW + spoofed TX MAC (S4-09 / S4-01) is directly exploitable. Track as `audit/S5-pre-1`.
4. **`pair_nvs` reuse beyond MAC** — destructive `nvs_flash_erase` wipes everything else too.
5. **Bond key migration plan** — when Sprint 8 adds LE Secure Connections, existing `pairing/tx_mac` records must either wipe or migrate.
6. **Reboot loop cap** — S4-04 backoff before any battery-powered build.

---

## 7. Conclusion

Sprint 4 delivers a working BLE-CENTRAL pairing handshake that correctly avoids accepting inbound connections, correctly persists only non-placeholder MACs, and correctly gates Wi-Fi bring-up on a successful pair. The pure state machine is the high point — disallowed events are silently dropped, terminal states absorb everything after, host tests cover the matrix. The two implementation findings that need attention (S4-02, S4-03) are cleanup-path issues that today are masked by the firmware never re-pairing within one boot cycle; they become real the moment Sprint 5 adds a "re-pair" path. S4-04 (reboot loop) is operational/wear with a simple RTC-counter fix.

The four V0-accepted threats (S4-01 TOFU, S4-07 plaintext PIN, S4-09 unauthenticated NVS, S4-11 address-type spoof) are tracked correctly in spec §6 and must be reopened together as Sprint 8 bonding/encryption work; they are not independent.

**Recommendation**: APPROVE merge. Open follow-ups:
- `audit/S4-02` (High, before any retry-pairing path)
- `audit/S4-03` (Medium, fold into S4-02 PR — three-line change)
- `audit/S4-04` (Medium, before any battery-powered build)
- `audit/S4-05` (Medium, before second NVS namespace lands)
- `audit/S4-01`, `audit/S4-07`, `audit/S4-09`, `audit/S4-11` (V0, gated by Sprint 8)

Lows (S4-06, S4-08, S4-10) and Infos (S4-12 .. S4-16) tracked as backlog with no Sprint 5 gating.

---

## 8. Resolution log

| Finding | Disposition | Commit / location |
|---|---|---|
| S4-01 (High) — CAM-TX spoof (V0 TOFU) | OPEN — V0 acceptance, gated by Sprint 8 (LE SC + bonding) | backlog (Sprint 8) |
| S4-02 (High) — NimBLE leak on init failure | OPEN — pending follow-up patch | TBD |
| S4-03 (Med) — missing `nimble_port_freertos_deinit` | OPEN — pending follow-up patch (fold into S4-02 PR) | TBD |
| S4-04 (Med) — reboot loop DoS / wear | OPEN — RTC backoff counter | TBD |
| S4-05 (Med) — destructive `nvs_flash_erase` | OPEN — track before second NVS namespace lands | backlog (Sprint 5+) |
| S4-06 (Low) — NVS blob length init pattern | OPEN — defense-in-depth tweak | backlog |
| S4-07 (Low) — PIN in BLE TX queue + flash | OPEN — V0 acceptance, gated by Sprint 8 | backlog (Sprint 8) |
| S4-08 (Low) — `CONFIG_RECEIVER_PEER_MAC` override bypass | OPEN — CI lint TBD; WARN log present | backlog |
| S4-09 (Low) — NVS readback not authenticated | OPEN — V0 acceptance, gated by Sprint 8 | backlog (Sprint 8) |
| S4-10 (Low) — BSS MAC residue | OPEN — defense-in-depth zeroize on exit | backlog |
| S4-11 (Low) — adv match without addr-type check | OPEN — V0 acceptance, gated by Sprint 8 | backlog (Sprint 8) |
| S4-12 (Info) — state-machine bypass posture | RESOLVED by design — host tests cover | n/a |
| S4-13 (Info) — stack canaries cover NimBLE | RESOLVED — confirmed by sdkconfig.defaults | n/a |
| S4-14 (Info) — S3-01 fix still in place | RESOLVED — confirmed in app_main.cpp | n/a |
| S4-15 (Info) — host_tests not linked in firmware | RESOLVED — `idf_component_register` scope | n/a |
| S4-16 (Info) — cppcheck not installed | OPEN — add to CI | backlog |
