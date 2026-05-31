// RED tests for Sprint 4 pair_nvs component — host-side, no ESP-IDF.
//
// These tests are expected to FAIL (link error or assertion) until
// components/pair_nvs/pair_nvs.cpp is implemented by Wave 2 (Coder).
//
// Coverage:
//   - pair_nvs_is_valid_mac_for_persist: rejects placeholder, broadcast, zero; accepts valid MAC
//   - Round-trip in-memory: save + load yields same 6 bytes
//   - Load with empty NVS -> returns false
//   - pair_nvs_clear after save -> next load returns false
//   - Save with invalid MAC -> rejected, no NVS write
//   - Save with borderline valid MAC -> accepted
//
// Style mirrors components/reassembly/host_tests/test_reassembly.cpp
// (no Unity, exit(1) on failure, g_passed counter).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

// ---------------------------------------------------------------------------
// Host build: pull in pair_nvs_is_valid_mac_for_persist logic directly.
// The host build cannot link against ESP-IDF NVS, so we compile a thin
// in-memory shim for pair_nvs_{save,load,clear} and test the real
// pair_nvs_is_valid_mac_for_persist implementation.
//
// pair_nvs.cpp must guard with PAIR_NVS_HOST_BUILD to skip nvs_* calls.
// ---------------------------------------------------------------------------

// Stub for peer_mac_is_placeholder (the real one lives in espnow_link.cpp
// but that pulls in ESP-IDF headers).  The host test re-declares the same
// logic here; pair_nvs.cpp must call the version from espnow_link in device
// builds but may call this via a weak symbol or #ifdef in host builds.
static bool peer_mac_is_placeholder_stub(const uint8_t mac[6]) {
    static const uint8_t kPlaceholder[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    static const uint8_t kBroadcast[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t kZero[6]        = {0x00,0x00,0x00,0x00,0x00,0x00};
    return memcmp(mac, kPlaceholder, 6) == 0 ||
           memcmp(mac, kBroadcast,   6) == 0 ||
           memcmp(mac, kZero,        6) == 0;
}

// In-memory NVS shim -------------------------------------------------------
static bool  s_has_entry   = false;
static uint8_t s_stored_mac[6] = {};

// These are the symbols that pair_nvs.cpp must expose when built with
// PAIR_NVS_HOST_BUILD=1.  In host mode, pair_nvs.cpp replaces nvs_* calls
// with the stubs below (or the test binary links pair_nvs_host_stub.cpp).
// For now we include a local re-implementation so the tests compile.
// When pair_nvs.cpp is written, this block becomes the thing we test against.

extern "C" {

bool pair_nvs_is_valid_mac_for_persist(const uint8_t mac[6]);
bool pair_nvs_init(void);
bool pair_nvs_load_tx_mac(uint8_t out[6]);
bool pair_nvs_save_tx_mac(const uint8_t mac[6]);
void pair_nvs_clear(void);

} // extern "C"

// ---------------------------------------------------------------------------
// Assertion macros
// ---------------------------------------------------------------------------
#define ASSERT_TRUE(expr, msg)                                                \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::fprintf(stderr, "%s:%d FAIL [%s]: expected true\n",         \
                         __FILE__, __LINE__, (msg));                          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define ASSERT_FALSE(expr, msg)                                               \
    do {                                                                      \
        if ((expr)) {                                                         \
            std::fprintf(stderr, "%s:%d FAIL [%s]: expected false\n",        \
                         __FILE__, __LINE__, (msg));                          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define ASSERT_EQ(got, expected, msg)                                         \
    do {                                                                      \
        if ((got) != (expected)) {                                            \
            std::fprintf(stderr, "%s:%d FAIL [%s]: got=%lld expected=%lld\n",\
                         __FILE__, __LINE__, (msg),                           \
                         (long long)(got), (long long)(expected));            \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define ASSERT_MEM_EQ(buf, expected_buf, len, msg)                            \
    do {                                                                      \
        if (memcmp((buf), (expected_buf), (len)) != 0) {                      \
            std::fprintf(stderr, "%s:%d FAIL [%s]: memory mismatch\n",        \
                         __FILE__, __LINE__, (msg));                           \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static int g_passed = 0;
#define PASS(name) do { std::printf("  PASS: %s\n", (name)); g_passed++; } while(0)

// ---------------------------------------------------------------------------
// Test 1: placeholder MAC (AA:BB:CC:DD:EE:FF) is invalid for persist
// ---------------------------------------------------------------------------
static void test_placeholder_rejected(void) {
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    ASSERT_FALSE(pair_nvs_is_valid_mac_for_persist(mac),
                 "placeholder AA:BB:CC:DD:EE:FF must be invalid");
    PASS("placeholder AA:BB:CC:DD:EE:FF is invalid for persist");
}

// ---------------------------------------------------------------------------
// Test 2: broadcast MAC (FF:FF:FF:FF:FF:FF) is invalid
// ---------------------------------------------------------------------------
static void test_broadcast_rejected(void) {
    const uint8_t mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_FALSE(pair_nvs_is_valid_mac_for_persist(mac),
                 "broadcast FF:FF:FF:FF:FF:FF must be invalid");
    PASS("broadcast FF:FF:FF:FF:FF:FF is invalid for persist");
}

// ---------------------------------------------------------------------------
// Test 3: zero MAC (00:00:00:00:00:00) is invalid
// ---------------------------------------------------------------------------
static void test_zero_rejected(void) {
    const uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_FALSE(pair_nvs_is_valid_mac_for_persist(mac),
                 "zero MAC 00:00:00:00:00:00 must be invalid");
    PASS("zero 00:00:00:00:00:00 is invalid for persist");
}

// ---------------------------------------------------------------------------
// Test 4: valid TX MAC (84:F7:03:12:34:56) is accepted
// ---------------------------------------------------------------------------
static void test_valid_mac_accepted(void) {
    const uint8_t mac[6] = {0x84, 0xF7, 0x03, 0x12, 0x34, 0x56};
    ASSERT_TRUE(pair_nvs_is_valid_mac_for_persist(mac),
                "84:F7:03:12:34:56 must be valid for persist");
    PASS("84:F7:03:12:34:56 accepted as valid");
}

// ---------------------------------------------------------------------------
// Test 5: another valid MAC (3C:DC:75:62:1F:3C — the RX's own MAC)
// ---------------------------------------------------------------------------
static void test_rx_own_mac_accepted(void) {
    const uint8_t mac[6] = {0x3C, 0xDC, 0x75, 0x62, 0x1F, 0x3C};
    ASSERT_TRUE(pair_nvs_is_valid_mac_for_persist(mac),
                "3C:DC:75:62:1F:3C must be valid for persist");
    PASS("3C:DC:75:62:1F:3C (RX MAC) accepted as valid");
}

// ---------------------------------------------------------------------------
// Test 6: multicast bit (LSB of first octet) — valid for ESP-NOW peer but
// confirm our validator does NOT reject it (pair_nvs only gates placeholders)
// ---------------------------------------------------------------------------
static void test_multicast_mac_accepted(void) {
    const uint8_t mac[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    // pair_nvs_is_valid_mac_for_persist gates placeholder/broadcast/zero only,
    // not multicast bit — that's a concern for the BLE layer, not NVS.
    ASSERT_TRUE(pair_nvs_is_valid_mac_for_persist(mac),
                "multicast MAC 01:00:5E:... should pass NVS validation");
    PASS("multicast MAC passes NVS validation (not a placeholder)");
}

// ---------------------------------------------------------------------------
// Test 7: load from empty NVS returns false
// ---------------------------------------------------------------------------
static void test_load_empty_returns_false(void) {
    pair_nvs_init();
    pair_nvs_clear();   // ensure clean state
    uint8_t out[6] = {};
    ASSERT_FALSE(pair_nvs_load_tx_mac(out), "load from empty NVS must return false");
    PASS("load from empty NVS returns false");
}

// ---------------------------------------------------------------------------
// Test 8: save valid MAC then load returns same bytes
// ---------------------------------------------------------------------------
static void test_save_load_roundtrip(void) {
    pair_nvs_init();
    pair_nvs_clear();
    const uint8_t mac_in[6] = {0x84, 0xF7, 0x03, 0x12, 0x34, 0x56};
    ASSERT_TRUE(pair_nvs_save_tx_mac(mac_in), "save valid MAC must return true");
    uint8_t mac_out[6] = {};
    ASSERT_TRUE(pair_nvs_load_tx_mac(mac_out), "load after save must return true");
    ASSERT_MEM_EQ(mac_out, mac_in, 6, "loaded MAC must equal saved MAC");
    PASS("save+load roundtrip returns identical 6 bytes");
}

// ---------------------------------------------------------------------------
// Test 9: clear after save makes next load return false
// ---------------------------------------------------------------------------
static void test_clear_after_save(void) {
    pair_nvs_init();
    const uint8_t mac[6] = {0x84, 0xF7, 0x03, 0x12, 0x34, 0x56};
    pair_nvs_save_tx_mac(mac);
    pair_nvs_clear();
    uint8_t out[6] = {};
    ASSERT_FALSE(pair_nvs_load_tx_mac(out), "load after clear must return false");
    PASS("clear wipes pairing state: load returns false");
}

// ---------------------------------------------------------------------------
// Test 10: save with placeholder MAC is rejected (no NVS write)
// ---------------------------------------------------------------------------
static void test_save_placeholder_rejected(void) {
    pair_nvs_init();
    pair_nvs_clear();
    const uint8_t bad[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    ASSERT_FALSE(pair_nvs_save_tx_mac(bad), "save placeholder MAC must return false");
    uint8_t out[6] = {};
    ASSERT_FALSE(pair_nvs_load_tx_mac(out),
                 "load after rejected save must still return false");
    PASS("save with placeholder MAC is rejected and NVS not written");
}

// ---------------------------------------------------------------------------
// Test 11: save with broadcast MAC is rejected
// ---------------------------------------------------------------------------
static void test_save_broadcast_rejected(void) {
    pair_nvs_init();
    pair_nvs_clear();
    const uint8_t bad[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_FALSE(pair_nvs_save_tx_mac(bad), "save broadcast MAC must return false");
    PASS("save with broadcast MAC is rejected");
}

// ---------------------------------------------------------------------------
// Test 12: save with zero MAC is rejected
// ---------------------------------------------------------------------------
static void test_save_zero_rejected(void) {
    pair_nvs_init();
    pair_nvs_clear();
    const uint8_t bad[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_FALSE(pair_nvs_save_tx_mac(bad), "save zero MAC must return false");
    PASS("save with zero MAC is rejected");
}

// ---------------------------------------------------------------------------
// Test 13: pair_nvs_init is idempotent (calling twice must not corrupt state)
// ---------------------------------------------------------------------------
static void test_init_idempotent(void) {
    pair_nvs_init();
    const uint8_t mac[6] = {0x84, 0xF7, 0x03, 0x12, 0x34, 0x56};
    pair_nvs_save_tx_mac(mac);
    pair_nvs_init();   // second init
    uint8_t out[6] = {};
    ASSERT_TRUE(pair_nvs_load_tx_mac(out), "load after second init must succeed");
    ASSERT_MEM_EQ(out, mac, 6, "data intact after second init");
    pair_nvs_clear();
    PASS("pair_nvs_init is idempotent: data survives second init");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    std::printf("=== pair_nvs host tests ===\n");

    test_placeholder_rejected();
    test_broadcast_rejected();
    test_zero_rejected();
    test_valid_mac_accepted();
    test_rx_own_mac_accepted();
    test_multicast_mac_accepted();
    test_load_empty_returns_false();
    test_save_load_roundtrip();
    test_clear_after_save();
    test_save_placeholder_rejected();
    test_save_broadcast_rejected();
    test_save_zero_rejected();
    test_init_idempotent();

    std::printf("\nAll %d tests passed.\n", g_passed);
    return 0;
}
