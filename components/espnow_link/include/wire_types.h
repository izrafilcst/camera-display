#pragma once
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Message type discriminants (first byte of ESP-NOW payload after esnow_hdr_t)
// ---------------------------------------------------------------------------
enum : uint8_t {
    MSG_VIDEO_FRAG   = 0x10,
    MSG_TELEMETRY    = 0x20,
    MSG_JOYSTICK     = 0x30,
    MSG_COMMAND      = 0x40,
};

// ---------------------------------------------------------------------------
// ESP-NOW envelope header — 2 bytes, present in every message.
// The TX writes its own ESP-NOW-layer sequence counter into the second byte;
// it wraps every 256 packets and is NOT used for replay protection on this
// side. Per-msg-type replay protection (REQ-4) lives in the message body
// (e.g. telemetry_rx_to_tx::seq is uint32_t).
// ---------------------------------------------------------------------------
struct __attribute__((packed)) esnow_hdr_t {
    uint8_t  msg_type;   // MSG_*
    uint8_t  seq;        // ESP-NOW layer seq from TX (wraps 255->0); informational
};

// ---------------------------------------------------------------------------
// Video fragment header — 7 bytes, after esnow_hdr_t for MSG_VIDEO_FRAG.
// Wire layout (offsets are from the start of the ESP-NOW packet, including
// the 2-byte esnow_hdr_t):
//   offset 2: frame_id   (2)
//   offset 4: frag_idx   (1)
//   offset 5: frag_total (1)
//   offset 6: jpeg_size  (2)
//   offset 8: payload_len(1)   <-- uint8_t per TX spec, NOT uint16_t
//   offset 9: tx_emission_ms (4 bytes, ONLY when frag_idx == 0)
//   offset 9 or 13: jpeg_data
// ---------------------------------------------------------------------------
struct __attribute__((packed)) video_frag_hdr_t {
    uint16_t frame_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t jpeg_size;     // total JPEG size when fully reassembled
    uint8_t  payload_len;   // bytes of JPEG in THIS fragment (<=236 in frag 0, <=240 otherwise)
};
static_assert(sizeof(video_frag_hdr_t) == 7, "video_frag_hdr_t must be 7 bytes per TX spec");

// ---------------------------------------------------------------------------
// Fragment-0 extra — 4 bytes, immediately after video_frag_hdr_t when frag_idx == 0
// ---------------------------------------------------------------------------
struct __attribute__((packed)) video_frag0_extra_t {
    uint32_t tx_emission_ms;
};

// ---------------------------------------------------------------------------
// Telemetry struct (RX -> TX, 2 Hz) — REQ-4: seq widened to uint32_t
// ---------------------------------------------------------------------------
struct __attribute__((packed)) telemetry_rx_to_tx {
    uint8_t  msg_type;             // MSG_TELEMETRY = 0x20
    uint8_t  reserved;             // 0x00
    uint32_t seq;                  // REQ-4: widened from uint8_t; replay window 32
    uint8_t  requested_level;      // 0..4 (L0..L4)
    uint8_t  current_level_seen;
    uint16_t frames_received_1s;
    uint16_t frames_dropped_1s;
    uint16_t fragments_lost_1s;
    int8_t   rssi_avg_dbm;
    int8_t   rssi_min_dbm;
    uint16_t latency_p50_ms;
    uint16_t latency_p99_ms;
    uint8_t  rx_battery_pct;       // 0..100, 0xFF = N/A
    uint8_t  flags;                // bit0=menu_open, bit1=link_freeze
    uint32_t rx_uptime_ms;
};

// ---------------------------------------------------------------------------
// Compile-time constraints — REQ-1
// ---------------------------------------------------------------------------
static constexpr size_t ESPNOW_MTU             = 250;
static constexpr size_t MAX_FRAGS_PER_FRAME    = 64;
// TX wire contract: fragment 0 carries 236 bytes of JPEG (the 4-byte
// tx_emission_ms eats into the payload), every subsequent fragment carries
// up to 240 bytes. Only the last fragment may be shorter (runt).
static constexpr size_t FRAG_DATA_MAX_0        = 236;
static constexpr size_t FRAG_DATA_MAX_N        = 240;
// Effective per-frame ceiling per the wire formula:
//   236 + (MAX_FRAGS_PER_FRAME - 1) * 240 = 236 + 63 * 240 = 15356
static constexpr size_t MAX_JPEG_SIZE          = FRAG_DATA_MAX_0
                                                 + (MAX_FRAGS_PER_FRAME - 1) * FRAG_DATA_MAX_N;
static constexpr uint32_t SKIP_DROP_TIMEOUT_MS = 30;

// REQ-1: frags_bitmap is uint64_t; 64 fragments is the hard ceiling.
static_assert(MAX_FRAGS_PER_FRAME > 0 && MAX_FRAGS_PER_FRAME <= 64,
              "MAX_FRAGS_PER_FRAME must fit in frags_bitmap (uint64_t)");
static_assert(MAX_JPEG_SIZE == 15356, "MAX_JPEG_SIZE must match TX wire formula");
