// RED tests for Sprint 4 ble_pair state machine — pure host-side, no BLE stack.
//
// The state machine has no dependency on NimBLE: it is fed events and
// reports state/err/tx_mac. The real ble_pair.cpp translates NimBLE
// callbacks into these events. Covering every transition (and a sample of
// the disallowed-event matrix) keeps regressions cheap.
//
// Style mirrors components/reassembly/host_tests/test_reassembly.cpp:
// no Unity, exit(1) on first failure, g_passed counter.

#include "ble_pair.h"
#include "internal/ble_pair_state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#define ASSERT_EQ(got, expected, msg)                                          \
    do {                                                                       \
        auto _g = (got);                                                       \
        auto _e = (expected);                                                  \
        if (_g != _e) {                                                        \
            std::fprintf(stderr, "%s:%d FAIL [%s]: got=%d expected=%d\n",     \
                         __FILE__, __LINE__, (msg), (int)_g, (int)_e);         \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

#define ASSERT_MEM_EQ(buf, exp, n, msg)                                        \
    do {                                                                       \
        if (memcmp((buf), (exp), (n)) != 0) {                                  \
            std::fprintf(stderr, "%s:%d FAIL [%s]: memcmp\n",                  \
                         __FILE__, __LINE__, (msg));                           \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static int g_passed = 0;
#define PASS(name) do { std::printf("  PASS: %s\n", (name)); g_passed++; } while (0)

// ---------------------------------------------------------------------------
// Helpers — drive the SM up to a given state via the happy path
// ---------------------------------------------------------------------------
static void drive_to_scanning(ble_pair_sm_t* sm) {
    ble_pair_sm_init(sm);
    ble_pair_sm_feed(sm, EV_START, nullptr, 0);
}

static void drive_to_connecting(ble_pair_sm_t* sm) {
    drive_to_scanning(sm);
    ble_pair_sm_feed(sm, EV_ADV_MATCH, nullptr, 0);
}

static void drive_to_discovering(ble_pair_sm_t* sm) {
    drive_to_connecting(sm);
    ble_pair_sm_feed(sm, EV_CONNECTED, nullptr, 0);
}

static void drive_to_writing_pin(ble_pair_sm_t* sm) {
    drive_to_discovering(sm);
    ble_pair_sm_feed(sm, EV_DISCOVERED, nullptr, 0);
}

static void drive_to_reading_tx_mac(ble_pair_sm_t* sm) {
    drive_to_writing_pin(sm);
    ble_pair_sm_feed(sm, EV_PIN_WRITE_OK, nullptr, 0);
}

static void drive_to_writing_rx_mac(ble_pair_sm_t* sm) {
    drive_to_reading_tx_mac(sm);
    const uint8_t tx_mac[6] = {0x84, 0xF7, 0x03, 0x12, 0x34, 0x56};
    ble_pair_sm_feed(sm, EV_TX_MAC_READ_OK, tx_mac, 6);
}

// ===========================================================================
// Initial state
// ===========================================================================
static void test_init_state_is_idle() {
    ble_pair_sm_t sm;
    ble_pair_sm_init(&sm);
    ASSERT_EQ(sm.state, BLE_PAIR_IDLE, "init: state=IDLE");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_NONE, "init: err=NONE");
    PASS("init: state=IDLE, err=NONE");
}

// ===========================================================================
// Happy-path transitions (one per edge)
// ===========================================================================
static void test_idle_start_to_scanning() {
    ble_pair_sm_t sm;
    drive_to_scanning(&sm);
    ASSERT_EQ(sm.state, BLE_PAIR_SCANNING, "IDLE x START -> SCANNING");
    PASS("IDLE x EV_START -> SCANNING");
}

static void test_scanning_adv_to_connecting() {
    ble_pair_sm_t sm;
    drive_to_scanning(&sm);
    ble_pair_sm_feed(&sm, EV_ADV_MATCH, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_CONNECTING, "SCANNING x ADV_MATCH -> CONNECTING");
    PASS("SCANNING x EV_ADV_MATCH -> CONNECTING");
}

static void test_connecting_connected_to_discovering() {
    ble_pair_sm_t sm;
    drive_to_connecting(&sm);
    ble_pair_sm_feed(&sm, EV_CONNECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_DISCOVERING, "CONNECTING x CONNECTED -> DISCOVERING");
    PASS("CONNECTING x EV_CONNECTED -> DISCOVERING");
}

static void test_discovering_discovered_to_writing_pin() {
    ble_pair_sm_t sm;
    drive_to_discovering(&sm);
    ble_pair_sm_feed(&sm, EV_DISCOVERED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_WRITING_PIN, "DISCOVERING x DISCOVERED -> WRITING_PIN");
    PASS("DISCOVERING x EV_DISCOVERED -> WRITING_PIN");
}

static void test_writing_pin_ok_to_reading_tx_mac() {
    ble_pair_sm_t sm;
    drive_to_writing_pin(&sm);
    ble_pair_sm_feed(&sm, EV_PIN_WRITE_OK, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_READING_TX_MAC, "WRITING_PIN x OK -> READING_TX_MAC");
    PASS("WRITING_PIN x EV_PIN_WRITE_OK -> READING_TX_MAC");
}

static void test_reading_tx_mac_ok_to_writing_rx_mac() {
    ble_pair_sm_t sm;
    drive_to_reading_tx_mac(&sm);
    const uint8_t tx_mac[6] = {0x84, 0xF7, 0x03, 0x12, 0x34, 0x56};
    ble_pair_sm_feed(&sm, EV_TX_MAC_READ_OK, tx_mac, 6);
    ASSERT_EQ(sm.state, BLE_PAIR_WRITING_RX_MAC, "READING_TX_MAC x READ_OK -> WRITING_RX_MAC");
    ASSERT_MEM_EQ(sm.tx_mac, tx_mac, 6, "tx_mac populated on READ_OK");
    PASS("READING_TX_MAC x EV_TX_MAC_READ_OK -> WRITING_RX_MAC + tx_mac populated");
}

static void test_writing_rx_mac_ok_to_done() {
    ble_pair_sm_t sm;
    drive_to_writing_rx_mac(&sm);
    ble_pair_sm_feed(&sm, EV_RX_MAC_WRITE_OK, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_DONE, "WRITING_RX_MAC x OK -> DONE");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_NONE, "DONE: err=NONE");
    PASS("WRITING_RX_MAC x EV_RX_MAC_WRITE_OK -> DONE");
}

// ===========================================================================
// Error paths (one per error code)
// ===========================================================================
static void test_scan_timeout() {
    ble_pair_sm_t sm;
    drive_to_scanning(&sm);
    ble_pair_sm_feed(&sm, EV_SCAN_TIMEOUT, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "SCAN_TIMEOUT -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_SCAN_TIMEOUT, "err=SCAN_TIMEOUT");
    PASS("SCANNING x EV_SCAN_TIMEOUT -> ERROR(SCAN_TIMEOUT)");
}

static void test_connect_failed() {
    ble_pair_sm_t sm;
    drive_to_connecting(&sm);
    ble_pair_sm_feed(&sm, EV_CONNECT_FAILED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "CONNECT_FAILED -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_CONNECT_FAILED, "err=CONNECT_FAILED");
    PASS("CONNECTING x EV_CONNECT_FAILED -> ERROR(CONNECT_FAILED)");
}

static void test_discover_failed() {
    ble_pair_sm_t sm;
    drive_to_discovering(&sm);
    ble_pair_sm_feed(&sm, EV_DISCOVER_FAILED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "DISCOVER_FAILED -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_DISCOVER_FAILED, "err=DISCOVER_FAILED");
    PASS("DISCOVERING x EV_DISCOVER_FAILED -> ERROR(DISCOVER_FAILED)");
}

static void test_pin_rejected() {
    ble_pair_sm_t sm;
    drive_to_writing_pin(&sm);
    ble_pair_sm_feed(&sm, EV_PIN_WRITE_REJECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "PIN_REJECTED -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_PIN_REJECTED, "err=PIN_REJECTED");
    PASS("WRITING_PIN x EV_PIN_WRITE_REJECTED -> ERROR(PIN_REJECTED)");
}

static void test_tx_mac_bad_len() {
    ble_pair_sm_t sm;
    drive_to_reading_tx_mac(&sm);
    const uint8_t junk[8] = {1,2,3,4,5,6,7,8};
    ble_pair_sm_feed(&sm, EV_TX_MAC_READ_BAD_LEN, junk, 8);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "BAD_TX_MAC_LEN -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_BAD_TX_MAC_LEN, "err=BAD_TX_MAC_LEN");
    PASS("READING_TX_MAC x EV_TX_MAC_READ_BAD_LEN -> ERROR(BAD_TX_MAC_LEN)");
}

static void test_rx_mac_auth_fail() {
    ble_pair_sm_t sm;
    drive_to_writing_rx_mac(&sm);
    ble_pair_sm_feed(&sm, EV_RX_MAC_WRITE_AUTH_FAIL, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "RX_MAC_AUTH_FAIL -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_RX_MAC_AUTH, "err=RX_MAC_AUTH");
    PASS("WRITING_RX_MAC x EV_RX_MAC_WRITE_AUTH_FAIL -> ERROR(RX_MAC_AUTH)");
}

// EV_DISCONNECTED is special: it fires from many states. Test the few that
// matter and make sure each lands in ERROR(DISCONNECTED_EARLY).
static void test_disconnect_from_connecting() {
    ble_pair_sm_t sm;
    drive_to_connecting(&sm);
    ble_pair_sm_feed(&sm, EV_DISCONNECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "DISCONNECT @ CONNECTING -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_DISCONNECTED_EARLY, "err=DISCONNECTED_EARLY");
    PASS("CONNECTING x EV_DISCONNECTED -> ERROR(DISCONNECTED_EARLY)");
}

static void test_disconnect_from_writing_pin() {
    ble_pair_sm_t sm;
    drive_to_writing_pin(&sm);
    ble_pair_sm_feed(&sm, EV_DISCONNECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "DISCONNECT @ WRITING_PIN -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_DISCONNECTED_EARLY, "err=DISCONNECTED_EARLY");
    PASS("WRITING_PIN x EV_DISCONNECTED -> ERROR(DISCONNECTED_EARLY)");
}

static void test_disconnect_from_writing_rx_mac() {
    ble_pair_sm_t sm;
    drive_to_writing_rx_mac(&sm);
    ble_pair_sm_feed(&sm, EV_DISCONNECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "DISCONNECT @ WRITING_RX_MAC -> ERROR");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_DISCONNECTED_EARLY, "err=DISCONNECTED_EARLY");
    PASS("WRITING_RX_MAC x EV_DISCONNECTED -> ERROR(DISCONNECTED_EARLY)");
}

// ===========================================================================
// Terminal behaviour: DONE absorbs further events
// ===========================================================================
static void test_done_is_terminal() {
    ble_pair_sm_t sm;
    drive_to_writing_rx_mac(&sm);
    ble_pair_sm_feed(&sm, EV_RX_MAC_WRITE_OK, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_DONE, "first DONE");
    // Now feed garbage events — state must not change
    ble_pair_sm_feed(&sm, EV_DISCONNECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_DONE, "DONE absorbs DISCONNECTED");
    ble_pair_sm_feed(&sm, EV_SCAN_TIMEOUT, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_DONE, "DONE absorbs SCAN_TIMEOUT");
    ble_pair_sm_feed(&sm, EV_PIN_WRITE_REJECTED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_DONE, "DONE absorbs PIN_REJECTED");
    PASS("DONE is terminal: subsequent events are absorbed");
}

// ERROR is also terminal — subsequent events should not overwrite the err code
static void test_error_is_terminal() {
    ble_pair_sm_t sm;
    drive_to_scanning(&sm);
    ble_pair_sm_feed(&sm, EV_SCAN_TIMEOUT, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "ERROR after SCAN_TIMEOUT");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_SCAN_TIMEOUT, "err=SCAN_TIMEOUT");
    // Pumping further events must not overwrite the err code
    ble_pair_sm_feed(&sm, EV_CONNECT_FAILED, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_ERROR, "ERROR absorbs CONNECT_FAILED");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_SCAN_TIMEOUT, "err code unchanged");
    PASS("ERROR is terminal: err code is preserved");
}

// ===========================================================================
// Disallowed events: ignored, state unchanged, no err code, no tx_mac mutation
// ===========================================================================
static void test_disallowed_pin_ok_from_scanning() {
    ble_pair_sm_t sm;
    drive_to_scanning(&sm);
    ble_pair_sm_feed(&sm, EV_PIN_WRITE_OK, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_SCANNING, "disallowed PIN_OK from SCANNING ignored");
    ASSERT_EQ(sm.err,   BLE_PAIR_ERR_NONE, "no err set");
    PASS("SCANNING x EV_PIN_WRITE_OK (disallowed) ignored");
}

static void test_disallowed_adv_match_from_writing_pin() {
    ble_pair_sm_t sm;
    drive_to_writing_pin(&sm);
    ble_pair_sm_feed(&sm, EV_ADV_MATCH, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_WRITING_PIN, "disallowed ADV_MATCH from WRITING_PIN ignored");
    PASS("WRITING_PIN x EV_ADV_MATCH (disallowed) ignored");
}

static void test_disallowed_start_from_connecting() {
    ble_pair_sm_t sm;
    drive_to_connecting(&sm);
    ble_pair_sm_feed(&sm, EV_START, nullptr, 0);
    ASSERT_EQ(sm.state, BLE_PAIR_CONNECTING, "disallowed START from CONNECTING ignored");
    PASS("CONNECTING x EV_START (disallowed) ignored");
}

// ===========================================================================
// tx_mac stays zeroed until READING_TX_MAC x READ_OK
// ===========================================================================
static void test_tx_mac_zero_until_read_ok() {
    ble_pair_sm_t sm;
    drive_to_reading_tx_mac(&sm);
    const uint8_t zero[6] = {};
    ASSERT_MEM_EQ(sm.tx_mac, zero, 6, "tx_mac zeroed before READ_OK");
    PASS("tx_mac zero before EV_TX_MAC_READ_OK");
}

// ===========================================================================
// Helpers: state_str and err_str return non-null for every enum value
// ===========================================================================
static void test_state_str_non_null() {
    for (int s = BLE_PAIR_IDLE; s <= BLE_PAIR_ERROR; ++s) {
        const char* str = ble_pair_state_str(static_cast<ble_pair_state_t>(s));
        if (!str || !*str) {
            std::fprintf(stderr, "state_str(%d) returned null/empty\n", s);
            std::exit(1);
        }
    }
    PASS("ble_pair_state_str returns non-null for all enum values");
}

static void test_err_str_non_null() {
    for (int e = BLE_PAIR_ERR_NONE; e <= BLE_PAIR_ERR_DISCONNECTED_EARLY; ++e) {
        const char* str = ble_pair_err_str(static_cast<ble_pair_err_t>(e));
        if (!str || !*str) {
            std::fprintf(stderr, "err_str(%d) returned null/empty\n", e);
            std::exit(1);
        }
    }
    PASS("ble_pair_err_str returns non-null for all enum values");
}

// ===========================================================================
// main
// ===========================================================================
int main() {
    std::printf("\n=== ble_pair state machine ===\n");

    std::printf("\n-- Initial state --\n");
    test_init_state_is_idle();

    std::printf("\n-- Happy-path transitions (one per edge) --\n");
    test_idle_start_to_scanning();
    test_scanning_adv_to_connecting();
    test_connecting_connected_to_discovering();
    test_discovering_discovered_to_writing_pin();
    test_writing_pin_ok_to_reading_tx_mac();
    test_reading_tx_mac_ok_to_writing_rx_mac();
    test_writing_rx_mac_ok_to_done();

    std::printf("\n-- Error paths --\n");
    test_scan_timeout();
    test_connect_failed();
    test_discover_failed();
    test_pin_rejected();
    test_tx_mac_bad_len();
    test_rx_mac_auth_fail();
    test_disconnect_from_connecting();
    test_disconnect_from_writing_pin();
    test_disconnect_from_writing_rx_mac();

    std::printf("\n-- Terminal states --\n");
    test_done_is_terminal();
    test_error_is_terminal();

    std::printf("\n-- Disallowed events --\n");
    test_disallowed_pin_ok_from_scanning();
    test_disallowed_adv_match_from_writing_pin();
    test_disallowed_start_from_connecting();

    std::printf("\n-- Field invariants --\n");
    test_tx_mac_zero_until_read_ok();

    std::printf("\n-- Helpers --\n");
    test_state_str_non_null();
    test_err_str_non_null();

    std::printf("\nAll %d tests passed.\n", g_passed);
    return 0;
}
