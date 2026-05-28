# Sprint 2 — Parser Hardening Requirements (Pre-merge Gate)

**Origin**: Sprint 1 security audit, item **S-15** + §4 attack-surface preview
**Audit commit**: `8c8a6ec`
**Status**: **BLOCKING** — Sprint 2 must not merge to `main` until all items below are implemented and a fragment-parser fuzz harness exists.

---

## Context

Sprint 2 introduces the first attacker-controlled input into the firmware: ESP-NOW packets carrying video fragments. Headers are entirely under the sender's control. Without input validation, a single malformed packet can corrupt heap state, crash the decode task, or — worst case — gain code execution via stack smashing.

The wire format (spec §5.5, plan Sprint 2 Task 2) is:

```c
struct esnow_hdr_t {            // 2 bytes
    uint8_t msg_type;
    uint8_t seq;
};
struct video_frag_hdr_t {       // 8 bytes
    uint16_t frame_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t jpeg_size;
    uint16_t payload_len;
};
struct video_frag0_extra_t {    // 4 bytes, only when frag_idx == 0
    uint32_t tx_emission_ms;
};
```

Every field above must be validated against compile-time bounds before being used as an index, length, or arithmetic operand. The Sprint 1 audit identified these requirements while reviewing the spec, before code lands — implementing them up-front is far cheaper than retrofitting.

---

## Mandatory requirements (each blocks Sprint 2 merge)

### REQ-1 — Compile-time `MAX_FRAGS_PER_FRAME`

The constant `MAX_FRAGS_PER_FRAME = 64` already exists in `wire_types.h` (per Sprint 2 plan Task 2). Verify and enforce:

```c
static_assert(MAX_FRAGS_PER_FRAME > 0 && MAX_FRAGS_PER_FRAME <= 64,
              "MAX_FRAGS_PER_FRAME must fit in the frags_bitmap (uint64_t)");
```

Reassembly uses a `uint64_t` bitmap (Sprint 2 plan Task 3 `slot_t::frags_bitmap`); 64 is the absolute upper bound. Any compile-time mismatch must fail the build.

### REQ-2 — Reject malformed fragmentation envelope

In `reassembly_push_frag`, all of these inputs MUST cause early return with no slot mutation and `s_stats.fragments_invalid++`:

| Condition | Reason |
|---|---|
| `len < sizeof(video_frag_hdr_t)` | Truncated header — already in plan |
| `h.frag_total == 0` | Divide/modulo-by-zero risk in any callers iterating offsets |
| `h.frag_idx >= h.frag_total` | Would write past end-of-slot |
| `h.jpeg_size > MAX_JPEG_SIZE` | Heap overflow if memcpy'd into slot |
| `h.payload_len > remaining bytes after headers` | OOB read from RX buffer |
| `h.payload_len == 0` (except as terminator) | Wastes a fragment slot; suspicious |
| `h.frag_idx == 0 && len < sizeof(video_frag_hdr_t) + sizeof(video_frag0_extra_t)` | Missing tx_emission_ms |
| `offset + h.payload_len > h.jpeg_size` | Per-fragment OOB write into slot |

The Sprint 2 plan Task 3 already covers most of these — this section enforces full coverage. A test case must exist for each row in the host-side Unity tests.

### REQ-3 — Cap `jpeg_size` against slot size

`reassembly_init(slots)` pre-allocates `slots × MAX_JPEG_SIZE` bytes. Any `h.jpeg_size > MAX_JPEG_SIZE` (currently 16 KiB) is rejected. Additionally:

- Define `MAX_JPEG_SIZE` in a single header (`wire_types.h`).
- Place a runtime `ESP_LOGW` at first occurrence of `jpeg_size` in the [12 KiB, 16 KiB] band — abnormal for nominal L0–L4 levels (~3–14 KiB per spec §5.5 audit doc).
- Cap is enforced before `memcpy` — never after.

### REQ-4 — Widen telemetry `seq` from `uint8_t` to `uint32_t`

Spec §5.5 currently defines `MSG_TELEMETRY` payload with `uint8_t seq`. A single-byte sequence number wraps every 256 packets — at 2 Hz that's 128 seconds before wraparound, trivially replayable.

**Action**: edit spec `docs/superpowers/specs/2026-05-26-receptor-design.md` §5.5 and the corresponding C struct in `wire_types.h` / `telemetry.h` to use:

```c
struct __attribute__((packed)) esnow_hdr_t {
    uint8_t  msg_type;
    uint8_t  reserved;   // alignment / future use
    uint32_t seq;
};
```

…or move `seq` into the per-message body (recommended; keeps `esnow_hdr_t` at 2 B). Replay protection:
- Track `last_seq_per_msg_type` in RAM
- Reject if `(received_seq - last_seq) > REPLAY_WINDOW` (suggest 32) AND `received_seq < last_seq`

### REQ-5 — Kconfig-gate the peer MAC with boot-time placeholder check

Sprint 2 plan Task 7 hardcodes `TX_MAC` as `{0x84,0xF7,0x03,0xAA,0xBB,0xCC}` in `app_main.cpp`. Replace with:

**Step 1**: Add to `components/espnow_link/Kconfig` (new file):

```kconfig
menu "ESP-NOW link"
    config RECEIVER_PEER_MAC
        string "Hardcoded peer MAC (V0)"
        default "AA:BB:CC:DD:EE:FF"
        help
            Six-byte MAC of the transmitter, colon-separated.
            MUST be changed before any production build.
            V0 contract: see docs/superpowers/specs/2026-05-26-receptor-design.md §10
endmenu
```

**Step 2**: At boot, log a warning if the value is still the placeholder:

```c
static bool peer_mac_is_placeholder(const uint8_t mac[6]) {
    static const uint8_t placeholder[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    static const uint8_t broadcast[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t zero[6]        = {0};
    return memcmp(mac, placeholder, 6) == 0 ||
           memcmp(mac, broadcast,   6) == 0 ||
           memcmp(mac, zero,        6) == 0;
}

if (peer_mac_is_placeholder(TX_MAC)) {
    ESP_LOGW(TAG, "*** PEER MAC IS PLACEHOLDER — set CONFIG_RECEIVER_PEER_MAC ***");
}
```

**Step 3**: Add a host-side test in `components/espnow_link/host_tests/test_peer_mac.cpp` that fails CI if the parsed Kconfig value matches any item in the placeholder list. Reference for the test file: `components/display/host_tests/test_patterns.cpp` already in tree.

---

## Optional but recommended (not blocking)

### OPT-A — Rate-limit slot creation per source MAC

Spec §4.2 keeps 2 slots × 16 KiB alive. An attacker spraying many `frame_id` values can churn both slots and freeze the pipeline. Mitigation:

- Track `slots_created_per_src_per_second` (one counter per peer MAC)
- If > N (suggest 30 — well above the legitimate 24 fps rate), reject new slots from that source for 1 s

This is OPT not REQ because the V0 single-peer link already restricts the threat model to a co-located adversary; promoting to REQ would be premature.

### OPT-B — Decode task in its own watchdog group

JPEG decoders (`esp_jpeg`, `TJpgDec`) have history of crash bugs on malformed input. Even non-adversarial RF interference can produce garbage JPEGs.

- Call `esp_task_wdt_add()` for `task_decode`
- Set timeout = 200 ms (5× the typical 41 ms frame budget)
- On WDT fire: `esp_restart()` with reason persisted in RTC slow memory

---

## Verification checklist (CI gate)

Before Sprint 2 PR can merge:

- [ ] REQ-1: `static_assert(MAX_FRAGS_PER_FRAME ≤ 64)` present in `wire_types.h`
- [ ] REQ-2: One Unity test per invalid-input row in `components/reassembly/host_tests/`
- [ ] REQ-3: `MAX_JPEG_SIZE` single-source-of-truth in `wire_types.h`; warn-log on 12–16 KiB band
- [ ] REQ-4: Spec §5.5 updated; `seq` widened in struct; replay window enforced
- [ ] REQ-5: `Kconfig` added; placeholder check at boot; CI test rejects placeholder MAC

Each item above corresponds to a commit message prefix `feat(s2-hardening):` or `fix(s2-hardening):` so the audit trail is easy to grep.

---

## References

- Sprint 1 audit (`docs/audits/sprint-1-security.md`) §2 row S-15 + §4
- Sprint 2 plan (`docs/superpowers/plans/2026-05-26-sprint-2-video-pipeline.md`) Tasks 2 and 3
- Spec (`docs/superpowers/specs/2026-05-26-receptor-design.md`) §4.2, §5.5
