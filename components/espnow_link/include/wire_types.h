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
// Video fragment header — 8 bytes, after esnow_hdr_t for MSG_VIDEO_FRAG.
// Wire layout (offsets are from the start of the ESP-NOW packet, including
// the 2-byte esnow_hdr_t):
//   offset  2: frame_id    (2)
//   offset  4: frag_idx    (1)
//   offset  5: frag_total  (1)
//   offset  6: jpeg_size   (2)
//   offset  8: payload_len (2, little-endian)
//   offset 10: tx_emission_ms (4 bytes, ONLY when frag_idx == 0)
//   offset 10 (frag N>0) or 14 (frag 0): jpeg_data
//
// GROUND TRUTH: camera-drone/components/frag_codec/src/frag_codec.c writes
// payload_len with write_u16_le() (2 bytes), and frag_codec.h declares
// VIDEO_FRAG_HDR_BYTES == 8. The TX code is authoritative — it is what ships
// bytes on the wire.
//
// This was previously declared as uint8_t (7-byte header), following the
// offset table in camera-drone/docs/RX_HANDOFF_PROMPT.md, which contradicted
// itself: the table summed to 7 bytes while the same document's summary said
// 8 bytes. Only 8 closes with the 236/240 payload caps below
// (2 + 8 + 4 + 236 = 250 and 2 + 8 + 240 = 250). The 1-byte skew corrupted
// 100% of every reassembled frame and shifted tx_emission_ms by one byte
// (yielding a timestamp 256x too large). Both sides' unit tests passed
// because each tested against its own layout.
// See docs/DIAG-video-24fps-espnow.md §1.
// ---------------------------------------------------------------------------
struct __attribute__((packed)) video_frag_hdr_t {
    uint16_t frame_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t jpeg_size;     // total JPEG size when fully reassembled
    uint16_t payload_len;   // bytes of JPEG in THIS fragment (<=236 in frag 0, <=240 otherwise)
};
static_assert(sizeof(video_frag_hdr_t) == 8, "video_frag_hdr_t must be 8 bytes per TX frag_codec.h");

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

// Header sizes and payload caps must close against the 250-byte MTU. These two
// asserts are what would have caught the 1-byte payload_len skew at compile
// time: with a 7-byte video header the sums below land on 249, not 250.
static_assert(sizeof(esnow_hdr_t) + sizeof(video_frag_hdr_t)
              + sizeof(video_frag0_extra_t) + FRAG_DATA_MAX_0 == ESPNOW_MTU,
              "frag 0 layout must fill exactly ESPNOW_MTU: 2 + 8 + 4 + 236 = 250");
static_assert(sizeof(esnow_hdr_t) + sizeof(video_frag_hdr_t)
              + FRAG_DATA_MAX_N == ESPNOW_MTU,
              "frag N>0 layout must fill exactly ESPNOW_MTU: 2 + 8 + 240 = 250");
