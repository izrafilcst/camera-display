// Sprint 2 — reassembly component implementation
// REQ-2: all 8 invalid-fragment conditions checked before any slot mutation
// REQ-3: MAX_JPEG_SIZE enforced; warn-log on [12K,16K) band
//
// Host-build: compile with -DREASSEMBLY_HOST_BUILD=1 to avoid ESP-IDF headers.

#include "reassembly.h"
#include "wire_types.h"
#include <cstring>
#include <cstdlib>

#ifndef REASSEMBLY_HOST_BUILD
#  include "esp_heap_caps.h"
#  include "esp_log.h"
   static const char* TAG = "reasm";
#  define REASM_MALLOC(sz)  heap_caps_malloc((sz), MALLOC_CAP_SPIRAM)
#  define REASM_FREE(p)     heap_caps_free(p)
#else
#  include <cstdio>
   // Stub log macros for host build
#  define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#  define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#  define REASM_MALLOC(sz)  malloc(sz)
#  define REASM_FREE(p)     free(p)
   static const char* TAG = "reasm";
#endif

// ---------------------------------------------------------------------------
// Internal slot structure
// ---------------------------------------------------------------------------
struct slot_t {
    bool      in_use;
    uint16_t  frame_id;
    uint16_t  jpeg_size;
    uint32_t  tx_emission_ms;
    uint32_t  first_seen_ms;
    uint8_t*  data;                              // MAX_JPEG_SIZE bytes
    uint64_t  frags_bitmap;                      // bit N = frag N received
    uint8_t   frag_total;
};

static slot_t  s_slots[4];
static int     s_slots_count = 0;
static reassembly_stats_t s_stats = {};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool reassembly_init(int slots) {
    if (slots < 1 || slots > 4) return false;

    // Free any previously allocated data buffers
    for (int i = 0; i < s_slots_count; ++i) {
        if (s_slots[i].data) {
            REASM_FREE(s_slots[i].data);
            s_slots[i].data = nullptr;
        }
    }

    s_slots_count = slots;
    memset(s_slots, 0, sizeof(s_slots));
    memset(&s_stats, 0, sizeof(s_stats));

    for (int i = 0; i < slots; ++i) {
        s_slots[i].data = static_cast<uint8_t*>(REASM_MALLOC(MAX_JPEG_SIZE));
        if (!s_slots[i].data) {
            // Partial init — clean up what was allocated
            for (int j = 0; j < i; ++j) {
                REASM_FREE(s_slots[j].data);
                s_slots[j].data = nullptr;
            }
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static slot_t* find_slot(uint16_t fid) {
    for (int i = 0; i < s_slots_count; ++i) {
        if (s_slots[i].in_use && s_slots[i].frame_id == fid)
            return &s_slots[i];
    }
    return nullptr;
}

static slot_t* alloc_slot(uint32_t now_ms) {
    // First pass: look for a free slot
    for (int i = 0; i < s_slots_count; ++i) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    // No free slot: evict the oldest in_use slot
    slot_t* victim = &s_slots[0];
    for (int i = 1; i < s_slots_count; ++i) {
        if (s_slots[i].first_seen_ms < victim->first_seen_ms)
            victim = &s_slots[i];
    }
    // Eviction accounting
    s_stats.frames_dropped_overrun++;
    victim->in_use       = false;
    victim->frags_bitmap = 0;
    (void)now_ms;
    return victim;
}

// Calculate byte offset of fragment frag_idx in the assembled JPEG buffer.
// Wire contract (TX spec):
//   offset(0)   = 0
//   offset(N>0) = 236 + (N - 1) * 240
// Fragment 0 always carries up to 236 bytes (the 4 bytes of tx_emission_ms
// eat into the 240-byte data budget); subsequent fragments carry up to 240.
// Only the LAST fragment may be shorter. The receiver doesn't have to know
// payload_len to place the data — the position is fully determined by
// frag_idx, so offsets are correct regardless of arrival order.
static size_t calc_offset(uint8_t frag_idx) {
    if (frag_idx == 0) return 0;
    return FRAG_DATA_MAX_0 + (static_cast<size_t>(frag_idx) - 1u) * FRAG_DATA_MAX_N;
}

// ---------------------------------------------------------------------------
// push_frag — the central validation + reassembly function
// REQ-2: every invalid condition must increment fragments_invalid and return false
//         before any slot is created or mutated.
// ---------------------------------------------------------------------------
bool reassembly_push_frag(const uint8_t* payload, size_t len,
                          uint32_t now_ms, reassembled_frame_t* out) {
    s_stats.fragments_received++;

    // REQ-2 row 1: truncated header
    if (len < sizeof(video_frag_hdr_t)) {
        s_stats.fragments_invalid++;
        return false;
    }

    video_frag_hdr_t h;
    memcpy(&h, payload, sizeof(h));
    const uint8_t* p  = payload + sizeof(h);
    size_t remain     = len - sizeof(h);

    // REQ-2 row 2: frag_total == 0 (divide-by-zero / never-complete risk)
    if (h.frag_total == 0) {
        s_stats.fragments_invalid++;
        return false;
    }

    // REQ-2 row 3: frag_idx >= frag_total (OOB bitmap write)
    if (h.frag_idx >= h.frag_total) {
        s_stats.fragments_invalid++;
        return false;
    }

    // Extra check: frag_total > MAX_FRAGS_PER_FRAME (bitmap overflow)
    if (h.frag_total > static_cast<uint8_t>(MAX_FRAGS_PER_FRAME)) {
        s_stats.fragments_invalid++;
        return false;
    }

    // REQ-2 row 4 / REQ-3: jpeg_size > MAX_JPEG_SIZE (heap overflow)
    if (h.jpeg_size > static_cast<uint16_t>(MAX_JPEG_SIZE)) {
        s_stats.fragments_invalid++;
        return false;
    }

    // REQ-3: warn log for [12K,16K) band
    if (h.jpeg_size >= 12 * 1024) {
        ESP_LOGW(TAG, "jpeg_size=%u in high band [12K,16K] frame_id=%u",
                 (unsigned)h.jpeg_size, (unsigned)h.frame_id);
    }

    // REQ-2 row 6: payload_len == 0 (empty/suspicious fragment)
    if (h.payload_len == 0) {
        s_stats.fragments_invalid++;
        return false;
    }

    // Wire-contract cap on payload_len: frag 0 carries at most 236 bytes,
    // other fragments at most 240. Anything larger is a malformed packet
    // (uint8_t allows up to 255 but TX never emits more than the cap).
    const size_t per_frag_cap = (h.frag_idx == 0) ? FRAG_DATA_MAX_0 : FRAG_DATA_MAX_N;
    if (h.payload_len > per_frag_cap) {
        s_stats.fragments_invalid++;
        return false;
    }

    // REQ-2 row 7: frag_idx == 0 and not enough bytes for extra header
    uint32_t tx_ms = 0;
    if (h.frag_idx == 0) {
        if (remain < sizeof(video_frag0_extra_t)) {
            s_stats.fragments_invalid++;
            return false;
        }
        video_frag0_extra_t e;
        memcpy(&e, p, sizeof(e));
        tx_ms   = e.tx_emission_ms;
        p      += sizeof(e);
        remain -= sizeof(e);
    }

    // REQ-2 row 5: payload_len > remaining bytes after all headers
    if (h.payload_len > remain) {
        s_stats.fragments_invalid++;
        return false;
    }

    // Locate or allocate slot
    slot_t* s = find_slot(h.frame_id);
    if (!s) {
        s = alloc_slot(now_ms);
        s->in_use          = true;
        s->frame_id        = h.frame_id;
        s->jpeg_size       = h.jpeg_size;
        s->frag_total      = h.frag_total;
        s->frags_bitmap    = 0;
        s->first_seen_ms   = now_ms;
        s->tx_emission_ms  = 0;
    }

    if (h.frag_idx == 0) {
        s->tx_emission_ms = tx_ms;
    }

    // Offset is a pure function of frag_idx (TX wire formula).
    const size_t offset = calc_offset(h.frag_idx);

    // REQ-2 row 8: offset + payload_len > jpeg_size (OOB write into slot).
    // Also catches the case where frag 0 carries more bytes than fit in the
    // declared frame, since offset(0)==0 makes this reduce to payload>jpeg.
    if (offset + h.payload_len > h.jpeg_size) {
        s_stats.fragments_invalid++;
        return false;
    }

    // Safe to copy
    memcpy(s->data + offset, p, h.payload_len);
    s->frags_bitmap |= (uint64_t{1} << h.frag_idx);

    // Check completeness: bits 0..(frag_total-1) all set
    uint64_t mask = (h.frag_total == 64)
                    ? ~uint64_t{0}
                    : ((uint64_t{1} << h.frag_total) - 1);

    if ((s->frags_bitmap & mask) == mask) {
        out->frame_id       = s->frame_id;
        out->jpeg_size      = s->jpeg_size;
        out->tx_emission_ms = s->tx_emission_ms;
        out->jpeg_data      = s->data;
        out->opaque         = s;
        s_stats.frames_completed++;
        // Slot stays in_use until reassembly_release()
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------
void reassembly_release(reassembled_frame_t* out) {
    if (!out || !out->opaque) return;
    slot_t* s       = static_cast<slot_t*>(out->opaque);
    s->in_use       = false;
    s->frags_bitmap = 0;
    out->opaque     = nullptr;
    out->jpeg_data  = nullptr;
}

// ---------------------------------------------------------------------------
// GC — drop slots older than SKIP_DROP_TIMEOUT_MS
// ---------------------------------------------------------------------------
void reassembly_gc(uint32_t now_ms) {
    for (int i = 0; i < s_slots_count; ++i) {
        slot_t& s = s_slots[i];
        if (s.in_use && (now_ms - s.first_seen_ms) > SKIP_DROP_TIMEOUT_MS) {
            s_stats.frames_dropped_timeout++;
            s.in_use       = false;
            s.frags_bitmap = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
const reassembly_stats_t* reassembly_stats(void) {
    return &s_stats;
}
