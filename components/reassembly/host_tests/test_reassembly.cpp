// RED tests for Sprint 2 reassembly component.
// These tests are expected to fail (link error or assertion failure) until
// components/reassembly/reassembly.cpp is implemented.
//
// Coverage:
//   REQ-1  — compile-time: wire_types.h static_assert (verified by build)
//   REQ-2  — all 8 malformed-fragment conditions (one test per row)
//   REQ-3  — jpeg_size > MAX_JPEG_SIZE rejected; warn band [12K,16K] accepted
//   Happy  — single-fragment frame, 2-frag frame, out-of-order, timeout, overrun

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Wire types (provides video_frag_hdr_t, video_frag0_extra_t, MAX_JPEG_SIZE, etc.)
// REQ-1: static_assert inside wire_types.h will fire at compile time if violated.
#include "wire_types.h"
#include "reassembly.h"

// ---------------------------------------------------------------------------
// Assertion macros (no Unity dependency — host build uses plain exit)
// ---------------------------------------------------------------------------
#define ASSERT_TRUE(expr, msg)                                                  \
    do {                                                                        \
        if (!(expr)) {                                                          \
            fprintf(stderr, "%s:%d FAIL [%s]: expected true\n",                \
                    __FILE__, __LINE__, (msg));                                 \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define ASSERT_FALSE(expr, msg)                                                 \
    do {                                                                        \
        if ((expr)) {                                                           \
            fprintf(stderr, "%s:%d FAIL [%s]: expected false\n",               \
                    __FILE__, __LINE__, (msg));                                 \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define ASSERT_EQ(got, expected, msg)                                           \
    do {                                                                        \
        if ((got) != (expected)) {                                              \
            fprintf(stderr, "%s:%d FAIL [%s]: got=%lld expected=%lld\n",       \
                    __FILE__, __LINE__, (msg),                                  \
                    (long long)(got), (long long)(expected));                   \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static int g_passed = 0;
#define PASS(name) do { fprintf(stdout, "  PASS: %s\n", (name)); g_passed++; } while(0)

// ---------------------------------------------------------------------------
// Helper: build a valid fragment payload vector
// frag_idx == 0 includes video_frag0_extra_t; others do not.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> make_frag(
        uint16_t frame_id,
        uint8_t  frag_idx,
        uint8_t  frag_total,
        uint16_t jpeg_size,
        uint8_t  payload_len,    // uint8_t per TX wire spec (was uint16_t)
        uint8_t  fill,
        uint32_t tx_ms = 0)
{
    std::vector<uint8_t> out;
    video_frag_hdr_t h{};
    h.frame_id   = frame_id;
    h.frag_idx   = frag_idx;
    h.frag_total = frag_total;
    h.jpeg_size  = jpeg_size;
    h.payload_len = payload_len;
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&h),
               reinterpret_cast<uint8_t*>(&h) + sizeof(h));
    if (frag_idx == 0) {
        video_frag0_extra_t e{};
        e.tx_emission_ms = tx_ms;
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&e),
                   reinterpret_cast<uint8_t*>(&e) + sizeof(e));
    }
    out.insert(out.end(), payload_len, fill);
    return out;
}

// ===========================================================================
// REQ-2: malformed fragment rejection tests (8 conditions)
// ===========================================================================

// REQ-2 row 1: len < sizeof(video_frag_hdr_t) → reject
static void test_req2_truncated_header() {
    reassembly_init(2);
    uint8_t tiny[3] = {0x01, 0x00, 0x00};  // only 3 bytes; header needs 8
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(tiny, sizeof(tiny), 0, &out);
    ASSERT_FALSE(result, "req2-row1: truncated header must be rejected");
    uint32_t inv = reassembly_stats()->fragments_invalid;
    ASSERT_EQ(inv, 1u, "req2-row1: fragments_invalid incremented");
    PASS("REQ-2 row1: len < sizeof(video_frag_hdr_t) rejected");
}

// REQ-2 row 2: h.frag_total == 0 → reject (divide/modulo-by-zero risk)
static void test_req2_frag_total_zero() {
    reassembly_init(2);
    auto f = make_frag(1, 0, 0 /*total=0*/, 100, 50, 0xAA);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row2: frag_total=0 must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row2: fragments_invalid incremented");
    PASS("REQ-2 row2: frag_total==0 rejected");
}

// REQ-2 row 3: h.frag_idx >= h.frag_total → reject (OOB bitmap write)
static void test_req2_frag_idx_gte_total() {
    reassembly_init(2);
    // idx=2, total=2 → idx >= total
    auto f = make_frag(1, 2 /*idx*/, 2 /*total*/, 200, 100, 0xBB);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row3: frag_idx>=frag_total must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row3: fragments_invalid incremented");
    PASS("REQ-2 row3: frag_idx >= frag_total rejected");
}

// REQ-2 row 4: h.jpeg_size > MAX_JPEG_SIZE → reject (heap overflow)
static void test_req2_jpeg_size_above_max() {
    reassembly_init(2);
    uint16_t oversized = static_cast<uint16_t>(MAX_JPEG_SIZE + 1);
    auto f = make_frag(1, 0, 1, oversized, 100, 0xCC);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row4: jpeg_size > MAX_JPEG_SIZE must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row4: fragments_invalid incremented");
    PASS("REQ-2 row4: jpeg_size > MAX_JPEG_SIZE rejected");
}

// REQ-2 row 5: h.payload_len > remaining bytes after headers → reject (OOB read)
static void test_req2_payload_len_exceeds_remaining() {
    reassembly_init(2);
    // Build a non-frag-0 packet where payload_len claims 200 but only 50
    // bytes follow the 7-byte video header. frag_idx>0 so no extra block.
    video_frag_hdr_t h{};
    h.frame_id   = 1;
    h.frag_idx   = 1;
    h.frag_total = 2;
    h.jpeg_size  = 800;
    h.payload_len = 200;       // claims 200 bytes
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&h),
               reinterpret_cast<uint8_t*>(&h) + sizeof(h));
    buf.insert(buf.end(), 50, 0xDD);  // only 50 bytes actual payload
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(buf.data(), buf.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row5: payload_len > remaining must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row5: fragments_invalid incremented");
    PASS("REQ-2 row5: payload_len > remaining bytes rejected");
}

// REQ-2 row 6: h.payload_len == 0 → reject (empty/suspicious fragment)
static void test_req2_payload_len_zero() {
    reassembly_init(2);
    auto f = make_frag(1, 0, 1, 100, 0 /*payload_len=0*/, 0x00);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row6: payload_len=0 must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row6: fragments_invalid incremented");
    PASS("REQ-2 row6: payload_len==0 rejected");
}

// REQ-2 row 7: frag_idx==0 and len < sizeof(hdr) + sizeof(extra) → reject
static void test_req2_frag0_missing_extra() {
    reassembly_init(2);
    // Build packet that has hdr but not the extra 4 bytes
    video_frag_hdr_t h{};
    h.frame_id   = 1;
    h.frag_idx   = 0;
    h.frag_total = 2;
    h.jpeg_size  = 200;
    h.payload_len = 1;
    // Do NOT include video_frag0_extra_t — just one byte of "payload"
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&h),
               reinterpret_cast<uint8_t*>(&h) + sizeof(h));
    buf.push_back(0xEE);  // one lone byte — no extra, no real payload room
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(buf.data(), buf.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row7: frag0 without extra bytes must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row7: fragments_invalid incremented");
    PASS("REQ-2 row7: frag_idx==0 missing tx_emission_ms rejected");
}

// REQ-2 row 8: offset + payload_len > jpeg_size → reject (OOB write into slot)
static void test_req2_offset_plus_len_exceeds_jpeg_size() {
    reassembly_init(2);
    // Non-frag-0 attacker: frag_idx=1 sits at offset 236 by the wire formula.
    // jpeg_size=300 and payload_len=200 → 236+200 = 436 > 300 → must reject.
    auto f = make_frag(1, 1 /*idx*/, 2 /*total*/, 300, 200, 0xFF);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "req2-row8: offset+payload_len > jpeg_size must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "req2-row8: fragments_invalid incremented");
    PASS("REQ-2 row8: offset + payload_len > jpeg_size rejected");
}

// Wire-cap on payload_len: frag 0 must have payload_len <= 236.
static void test_req2_frag0_payload_above_cap() {
    reassembly_init(2);
    // jpeg_size large enough that the row-8 check wouldn't trip, but
    // payload_len=237 violates the frag-0 per-fragment cap.
    auto f = make_frag(1, 0, 2, 1000, 237, 0xAB, 7);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "frag 0 payload_len > 236 must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "frag 0 per-cap: fragments_invalid incremented");
    PASS("frag 0 payload_len > FRAG_DATA_MAX_0 (236) rejected");
}

// Wire-cap on payload_len: frag N > 0 must have payload_len <= 240.
static void test_req2_fragN_payload_above_cap() {
    reassembly_init(2);
    // frag_idx=1, payload_len=241 violates the non-frag-0 per-fragment cap.
    auto f = make_frag(2, 1, 2, 1000, 241, 0xCD);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "frag>0 payload_len > 240 must be rejected");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 1u,
              "frag N per-cap: fragments_invalid incremented");
    PASS("frag N payload_len > FRAG_DATA_MAX_N (240) rejected");
}

// ===========================================================================
// REQ-3: jpeg_size at boundary conditions
// ===========================================================================

// REQ-3: jpeg_size == MAX_JPEG_SIZE should be accepted at the upper boundary.
// We can't actually deliver this in a single frag (236 bytes max), so use
// frag_total=64 declaration. The test only validates header acceptance —
// reassembly completion isn't required here.
static void test_req3_jpeg_size_at_max_accepted() {
    reassembly_init(2);
    uint16_t jsz = static_cast<uint16_t>(MAX_JPEG_SIZE);  // 15356 with wire formula
    // Frag 0 of MAX_FRAGS_PER_FRAME — fills positions 0..235.
    auto f = make_frag(10, 0, 64, jsz, FRAG_DATA_MAX_0, 0x55);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    // First frag of a 64-frag frame doesn't complete; the call should
    // accept it (return false meaning "no completion yet" but no error).
    ASSERT_FALSE(result, "req3: first frag of MAX_JPEG_SIZE frame must not complete");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 0u,
              "req3: MAX_JPEG_SIZE header must be accepted (no invalid count)");
    PASS("REQ-3: jpeg_size==MAX_JPEG_SIZE accepted");
}

// REQ-3: jpeg_size in [12K,15K) band — accepted but should emit warn log
static void test_req3_jpeg_size_warn_band_accepted() {
    reassembly_init(2);
    uint16_t jsz = 13 * 1024;  // 13 KiB — in [12K, MAX_JPEG_SIZE) band
    auto f = make_frag(11, 0, 64, jsz, FRAG_DATA_MAX_0, 0x66);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_FALSE(result, "req3: warn-band single frag must not complete a 64-frag frame");
    ASSERT_EQ(reassembly_stats()->fragments_invalid, 0u,
              "req3: warn-band header must be accepted");
    PASS("REQ-3: jpeg_size in [12K,MAX_JPEG_SIZE) warn band accepted");
}

// ===========================================================================
// Happy-path and behavioural tests
// ===========================================================================

// Single-fragment frame completes immediately
static void test_happy_single_fragment() {
    reassembly_init(2);
    uint8_t data[100];
    memset(data, 0xAA, sizeof(data));
    auto f = make_frag(1, 0, 1, 100, 100, 0xAA, 1234);
    reassembled_frame_t out{};
    bool result = reassembly_push_frag(f.data(), f.size(), 0, &out);
    ASSERT_TRUE(result, "happy: single-frag frame completes");
    ASSERT_EQ(out.frame_id,        1u,    "happy: correct frame_id");
    ASSERT_EQ(out.jpeg_size,       100u,  "happy: correct jpeg_size");
    ASSERT_EQ(out.tx_emission_ms,  1234u, "happy: correct tx_emission_ms");
    ASSERT_EQ(out.jpeg_data[0],    0xAAu, "happy: data byte 0 correct");
    reassembly_release(&out);
    PASS("happy: single-fragment frame completes immediately");
}

// Two-fragment frame completes on second fragment.
// Wire-spec sizing: frag 0 carries 236 bytes (its max), frag 1 carries the
// remainder. Total jpeg_size = 236 + 100 = 336.
static void test_happy_two_fragment_in_order() {
    reassembly_init(2);
    auto f0 = make_frag(2, 0, 2, 336, 236, 0x11, 9999);
    auto f1 = make_frag(2, 1, 2, 336, 100, 0x22);
    reassembled_frame_t out{};
    bool r0 = reassembly_push_frag(f0.data(), f0.size(), 0, &out);
    ASSERT_FALSE(r0, "happy-2frag: first frag does not complete");
    bool r1 = reassembly_push_frag(f1.data(), f1.size(), 5, &out);
    ASSERT_TRUE(r1, "happy-2frag: second frag completes frame");
    ASSERT_EQ(out.jpeg_size,      336u,  "happy-2frag: jpeg_size correct");
    ASSERT_EQ(out.jpeg_data[0],   0x11u, "happy-2frag: first byte from frag 0");
    ASSERT_EQ(out.jpeg_data[236], 0x22u, "happy-2frag: first byte from frag 1 at wire offset 236");
    reassembly_release(&out);
    PASS("happy: two-fragment frame completes on second");
}

// Four fragments arriving out of order (2,0,3,1) still complete.
// Wire-spec sizing: frag 0=236, frags 1..2=240 (full), frag 3=100 (runt).
// Total jpeg_size = 236 + 240 + 240 + 100 = 816.
static void test_happy_out_of_order_four_frags() {
    reassembly_init(2);
    auto f0 = make_frag(20, 0, 4, 816, 236, 0xA0, 100);
    auto f1 = make_frag(20, 1, 4, 816, 240, 0xB0);
    auto f2 = make_frag(20, 2, 4, 816, 240, 0xC0);
    auto f3 = make_frag(20, 3, 4, 816, 100, 0xD0);
    reassembled_frame_t out{};
    // Arrive in order: 2,0,3,1
    ASSERT_FALSE(reassembly_push_frag(f2.data(), f2.size(), 0, &out), "ooo: frag2 no complete");
    ASSERT_FALSE(reassembly_push_frag(f0.data(), f0.size(), 0, &out), "ooo: frag0 no complete");
    ASSERT_FALSE(reassembly_push_frag(f3.data(), f3.size(), 0, &out), "ooo: frag3 no complete");
    bool done = reassembly_push_frag(f1.data(), f1.size(), 0, &out);
    ASSERT_TRUE(done, "ooo: last frag (1) completes frame");
    // Wire offsets per spec: 0, 236, 476, 716
    ASSERT_EQ(out.jpeg_data[0],   0xA0u, "ooo: byte 0 from frag0");
    ASSERT_EQ(out.jpeg_data[236], 0xB0u, "ooo: byte 236 from frag1");
    ASSERT_EQ(out.jpeg_data[476], 0xC0u, "ooo: byte 476 from frag2");
    ASSERT_EQ(out.jpeg_data[716], 0xD0u, "ooo: byte 716 from frag3");
    reassembly_release(&out);
    PASS("happy: four fragments arriving 2,0,3,1 complete correctly");
}

// Timeout drops incomplete slot
static void test_timeout_drops_incomplete_frame() {
    reassembly_init(2);
    // 2-frag frame with realistic wire sizes: 236 + 100 = 336.
    auto f0 = make_frag(3, 0, 2, 336, 100, 0xCC, 1);
    reassembled_frame_t out{};
    reassembly_push_frag(f0.data(), f0.size(), 0, &out);
    // Advance time past SKIP_DROP_TIMEOUT_MS (30 ms)
    reassembly_gc(31);
    ASSERT_EQ(reassembly_stats()->frames_dropped_timeout, 1u,
              "timeout: one frame dropped");
    PASS("timeout: incomplete slot dropped after 30 ms");
}

// Overrun evicts oldest slot when both are full
static void test_overrun_evicts_oldest_slot() {
    reassembly_init(2);
    uint8_t d[100];
    memset(d, 0xCC, sizeof(d));
    // 2-frag frames, wire-correct: 236 (frag 0 cap) + 100 = 336.
    auto f_a = make_frag(10, 0, 2, 336, 100, 0xCC, 0);
    auto f_b = make_frag(11, 0, 2, 336, 100, 0xCC, 0);
    auto f_c = make_frag(12, 0, 2, 336, 100, 0xCC, 0);
    reassembled_frame_t out{};
    reassembly_push_frag(f_a.data(), f_a.size(), 0,  &out);  // slot 0 = frame 10
    reassembly_push_frag(f_b.data(), f_b.size(), 1,  &out);  // slot 1 = frame 11
    // Both slots full; frame 12 should evict oldest (frame 10, t=0)
    reassembly_push_frag(f_c.data(), f_c.size(), 2,  &out);
    ASSERT_EQ(reassembly_stats()->frames_dropped_overrun, 1u,
              "overrun: one frame evicted");
    PASS("overrun: newer frame_id evicts oldest slot when full");
}

// ===========================================================================
// REQ-5 peer MAC placeholder test
// (The actual Kconfig test lives in espnow_link/host_tests/test_peer_mac.cpp)
// This test verifies the placeholder-detection logic in isolation.
// ===========================================================================
static bool peer_mac_is_placeholder(const uint8_t mac[6]) {
    static const uint8_t placeholder[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    static const uint8_t broadcast[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t zero[6]        = {};
    return memcmp(mac, placeholder, 6) == 0 ||
           memcmp(mac, broadcast,   6) == 0 ||
           memcmp(mac, zero,        6) == 0;
}

static void test_req5_placeholder_detected() {
    uint8_t ph[6]  = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    uint8_t bc[6]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t zr[6]  = {0x00,0x00,0x00,0x00,0x00,0x00};
    uint8_t ok[6]  = {0x84,0xF7,0x03,0x12,0x34,0x56};
    ASSERT_TRUE (peer_mac_is_placeholder(ph), "req5: placeholder MAC detected");
    ASSERT_TRUE (peer_mac_is_placeholder(bc), "req5: broadcast MAC detected");
    ASSERT_TRUE (peer_mac_is_placeholder(zr), "req5: zero MAC detected");
    ASSERT_FALSE(peer_mac_is_placeholder(ok), "req5: valid MAC not flagged");
    PASS("REQ-5: peer_mac_is_placeholder correctly identifies all 3 bad patterns");
}

// ===========================================================================
// REQ-4 smoke: telemetry_rx_to_tx seq field is 32-bit
// ===========================================================================
static void test_req4_telemetry_seq_is_u32() {
    // If seq is still uint8_t, sizeof(telemetry_rx_to_tx) would be smaller.
    // With uint32_t seq (4 bytes) instead of uint8_t seq (1 byte), the struct
    // grows by 3 bytes. Minimum expected size is > the old size with uint8_t.
    // Old: msg_type(1)+seq(1)+requested_level(1)+current_level_seen(1)+
    //      frames_received_1s(2)+frames_dropped_1s(2)+fragments_lost_1s(2)+
    //      rssi_avg_dbm(1)+rssi_min_dbm(1)+latency_p50_ms(2)+latency_p99_ms(2)+
    //      rx_battery_pct(1)+flags(1)+rx_uptime_ms(4) = 22 bytes
    // New: seq widened to uint32_t adds 3 bytes → 25 bytes minimum
    static_assert(sizeof(telemetry_rx_to_tx) >= 25,
                  "REQ-4: telemetry_rx_to_tx::seq must be uint32_t (struct too small)");
    PASS("REQ-4: telemetry_rx_to_tx::seq is uint32_t (size check passed)");
}

// ===========================================================================
// main
// ===========================================================================

int main() {
    fprintf(stdout, "\n=== REQ-2: malformed fragment rejection ===\n");
    test_req2_truncated_header();
    test_req2_frag_total_zero();
    test_req2_frag_idx_gte_total();
    test_req2_jpeg_size_above_max();
    test_req2_payload_len_exceeds_remaining();
    test_req2_payload_len_zero();
    test_req2_frag0_missing_extra();
    test_req2_offset_plus_len_exceeds_jpeg_size();
    test_req2_frag0_payload_above_cap();
    test_req2_fragN_payload_above_cap();

    fprintf(stdout, "\n=== REQ-3: jpeg_size boundary ===\n");
    test_req3_jpeg_size_at_max_accepted();
    test_req3_jpeg_size_warn_band_accepted();

    fprintf(stdout, "\n=== Happy path ===\n");
    test_happy_single_fragment();
    test_happy_two_fragment_in_order();
    test_happy_out_of_order_four_frags();

    fprintf(stdout, "\n=== Timeout and overrun ===\n");
    test_timeout_drops_incomplete_frame();
    test_overrun_evicts_oldest_slot();

    fprintf(stdout, "\n=== REQ-4 and REQ-5 smoke ===\n");
    test_req4_telemetry_seq_is_u32();
    test_req5_placeholder_detected();

    fprintf(stdout, "\nAll %d tests passed.\n", g_passed);
    return 0;
}
