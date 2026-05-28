#pragma once
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Completed frame descriptor
// ---------------------------------------------------------------------------
struct reassembled_frame_t {
    uint16_t      frame_id;
    uint16_t      jpeg_size;
    uint32_t      tx_emission_ms;
    const uint8_t* jpeg_data;   // points into internal slot; valid until reassembly_release
    void*          opaque;      // slot handle, pass to reassembly_release
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * Initialise the slot pool.
 * @param slots  Number of reassembly slots (1..4).
 * @return true on success.
 */
bool reassembly_init(int slots);

/**
 * Process one incoming fragment (payload begins at video_frag_hdr_t, no esnow_hdr_t).
 * @param payload  Raw bytes from ESP-NOW after stripping esnow_hdr_t.
 * @param len      Byte count of payload.
 * @param now_ms   Current monotonic time in milliseconds.
 * @param out      Filled when return value is true.
 * @return true if this fragment completed a frame; out is valid.
 */
bool reassembly_push_frag(const uint8_t* payload, size_t len,
                          uint32_t now_ms, reassembled_frame_t* out);

/**
 * Release the slot referenced by out->opaque.
 * The out->jpeg_data pointer becomes invalid after this call.
 */
void reassembly_release(reassembled_frame_t* out);

/**
 * Free slots whose first_seen age exceeds SKIP_DROP_TIMEOUT_MS.
 * Call periodically from the RX context.
 */
void reassembly_gc(uint32_t now_ms);

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
struct reassembly_stats_t {
    uint32_t frames_completed;
    uint32_t frames_dropped_timeout;
    uint32_t frames_dropped_overrun;
    uint32_t fragments_received;
    uint32_t fragments_invalid;
};

const reassembly_stats_t* reassembly_stats(void);
