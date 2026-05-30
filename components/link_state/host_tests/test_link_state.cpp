// Sprint 3 — RED tests for link_state machine
// Mirror the harness style from components/reassembly/host_tests.

#include "link_state.h"

#include <cstdio>
#include <cstdlib>

static int g_fails = 0;

#define ASSERT_EQ(actual, expected, msg)                                      \
    do {                                                                      \
        auto _a = (actual);                                                   \
        auto _e = (expected);                                                 \
        if (_a != _e) {                                                       \
            std::fprintf(stderr, "%s:%d FAIL [%s]: got=%u expected=%u\n",     \
                         __FILE__, __LINE__, msg,                             \
                         (unsigned)_a, (unsigned)_e);                         \
            g_fails++;                                                        \
        }                                                                     \
    } while (0)

#define PASS(label)  std::printf("  PASS: %s\n", label)

// ---------------------------------------------------------------------------
// Boot: no packet received yet
// ---------------------------------------------------------------------------
static void test_boot_until_first_packet() {
    link_state_init();
    ASSERT_EQ(link_state_query(0),    LINK_BOOT, "boot: t=0 is LINK_BOOT");
    ASSERT_EQ(link_state_query(5000), LINK_BOOT, "boot: t=5000 still LINK_BOOT");
    ASSERT_EQ(link_state_idle_ms(0),  LINK_IDLE_UNKNOWN,
              "boot: idle is LINK_IDLE_UNKNOWN before first rx");
    PASS("boot: stays LINK_BOOT until first rx");
}

// ---------------------------------------------------------------------------
// Connected immediately after first packet
// ---------------------------------------------------------------------------
static void test_connected_after_first_packet() {
    link_state_init();
    link_state_mark_rx(100);
    ASSERT_EQ(link_state_query(150), LINK_CONNECTED, "connected at t=150 after rx@100");
    ASSERT_EQ(link_state_idle_ms(150), 50u, "idle 50 ms after rx@100");
    PASS("connected: state flips to CONNECTED on first rx");
}

// ---------------------------------------------------------------------------
// FREEZE boundary: 199 ms still CONNECTED, 201 ms is FREEZE
// ---------------------------------------------------------------------------
static void test_freeze_boundary() {
    link_state_init();
    link_state_mark_rx(0);
    ASSERT_EQ(link_state_query(199), LINK_CONNECTED, "199 ms idle still CONNECTED");
    ASSERT_EQ(link_state_query(201), LINK_FREEZE,    "201 ms idle is FREEZE");
    ASSERT_EQ(link_state_query(2999), LINK_FREEZE,   "2999 ms idle still FREEZE");
    PASS("freeze: boundary at 200 ms");
}

// ---------------------------------------------------------------------------
// DISCONNECTED boundary: 2999 FREEZE, 3001 DISCONNECTED
// ---------------------------------------------------------------------------
static void test_disconnected_boundary() {
    link_state_init();
    link_state_mark_rx(0);
    ASSERT_EQ(link_state_query(2999), LINK_FREEZE,       "2999 ms still FREEZE");
    ASSERT_EQ(link_state_query(3001), LINK_DISCONNECTED, "3001 ms is DISCONNECTED");
    PASS("disconnected: boundary at 3000 ms");
}

// ---------------------------------------------------------------------------
// Recovery from any state back to CONNECTED on new rx
// ---------------------------------------------------------------------------
static void test_recovery_to_connected() {
    link_state_init();
    link_state_mark_rx(0);
    ASSERT_EQ(link_state_query(5000), LINK_DISCONNECTED, "5000 ms idle -> DISCONNECTED");
    link_state_mark_rx(5100);
    ASSERT_EQ(link_state_query(5150), LINK_CONNECTED, "rx@5100 -> CONNECTED at t=5150");
    PASS("recovery: new rx restores CONNECTED");
}

// ---------------------------------------------------------------------------
// idle_ms returns LINK_IDLE_UNKNOWN before first rx, then real idle
// ---------------------------------------------------------------------------
static void test_idle_ms_semantics() {
    link_state_init();
    ASSERT_EQ(link_state_idle_ms(12345), LINK_IDLE_UNKNOWN, "idle before any rx");
    link_state_mark_rx(1000);
    ASSERT_EQ(link_state_idle_ms(1000), 0u,    "idle == 0 at the rx instant");
    ASSERT_EQ(link_state_idle_ms(1500), 500u,  "idle == 500 after 500 ms");
    PASS("idle_ms: sentinel before rx, real value after");
}

// ---------------------------------------------------------------------------
// Wrap edge case (NOT in plan): 32-bit ms counter wraps after ~49 days.
// rx near the wrap, query after wrap; idle must compute via unsigned subtraction.
// ---------------------------------------------------------------------------
static void test_wraparound_idle_computation() {
    link_state_init();
    // mark_rx 16 ms before wrap
    const uint32_t before_wrap = 0xFFFFFFF0u;
    const uint32_t after_wrap  = 0x00000010u;   // 32 ms past the wrap, total elapsed = 48
    link_state_mark_rx(before_wrap);
    // 0x10 - 0xFFFFFFF0 in uint32_t is 0x20 (= 32), so 32 ms idle
    ASSERT_EQ(link_state_idle_ms(after_wrap), 32u,
              "wrap: unsigned subtraction yields correct idle across wrap");
    ASSERT_EQ(link_state_query(after_wrap), LINK_CONNECTED,
              "wrap: still CONNECTED (32 ms idle < 200 ms threshold)");
    PASS("wrap: 32-bit ms wraparound handled by unsigned subtraction");
}

// ---------------------------------------------------------------------------
int main() {
    std::printf("\n=== link_state state machine ===\n");
    test_boot_until_first_packet();
    test_connected_after_first_packet();
    test_freeze_boundary();
    test_disconnected_boundary();
    test_recovery_to_connected();
    test_idle_ms_semantics();
    test_wraparound_idle_computation();

    if (g_fails == 0) {
        std::printf("\nAll 7 tests passed.\n");
        return EXIT_SUCCESS;
    }
    std::printf("\n%d test(s) failed.\n", g_fails);
    return EXIT_FAILURE;
}
