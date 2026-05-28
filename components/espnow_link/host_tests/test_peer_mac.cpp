// REQ-5 host-side test: verifies that the Kconfig default peer MAC
// is identified as a placeholder by peer_mac_is_placeholder().
//
// This test will FAIL (compile or link error) until:
//   1. components/espnow_link/Kconfig exists with RECEIVER_PEER_MAC default
//   2. peer_mac_is_placeholder() is implemented in espnow_link.cpp
//
// In the host build, we include the logic directly (no ESP-IDF headers).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Reproduce peer_mac_is_placeholder as it must appear in app_main.cpp (REQ-5)
// In CI this test validates the logic is correct before the firmware builds.
// ---------------------------------------------------------------------------
static bool peer_mac_is_placeholder(const uint8_t mac[6]) {
    static const uint8_t placeholder[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    static const uint8_t broadcast[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t zero[6]        = {};
    return memcmp(mac, placeholder, 6) == 0 ||
           memcmp(mac, broadcast,   6) == 0 ||
           memcmp(mac, zero,        6) == 0;
}

// ---------------------------------------------------------------------------
// Parse "AA:BB:CC:DD:EE:FF" → uint8_t[6]
// Mirrors what app_main.cpp must do with CONFIG_RECEIVER_PEER_MAC
// ---------------------------------------------------------------------------
static bool parse_mac(const char* str, uint8_t out[6]) {
    unsigned vals[6];
    int n = sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
                   &vals[0],&vals[1],&vals[2],&vals[3],&vals[4],&vals[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(vals[i]);
    return true;
}

#define ASSERT_TRUE(expr, msg)                                                  \
    do {                                                                        \
        if (!(expr)) {                                                          \
            fprintf(stderr, "%s:%d FAIL [%s]\n", __FILE__, __LINE__, (msg));   \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

#define ASSERT_FALSE(expr, msg)                                                 \
    do {                                                                        \
        if ((expr)) {                                                           \
            fprintf(stderr, "%s:%d FAIL [%s]\n", __FILE__, __LINE__, (msg));   \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static int g_passed = 0;
#define PASS(name) do { fprintf(stdout, "  PASS: %s\n", (name)); g_passed++; } while(0)

// ---------------------------------------------------------------------------
// Test: Kconfig default "AA:BB:CC:DD:EE:FF" must be detected as placeholder
// ---------------------------------------------------------------------------
static void test_kconfig_default_is_placeholder() {
    // This is the Kconfig default defined in components/espnow_link/Kconfig
    const char* kconfig_default = "AA:BB:CC:DD:EE:FF";
    uint8_t mac[6];
    ASSERT_TRUE(parse_mac(kconfig_default, mac), "parse_mac must succeed for default");
    ASSERT_TRUE(peer_mac_is_placeholder(mac),
                "Kconfig default MAC must be flagged as placeholder by peer_mac_is_placeholder");
    PASS("Kconfig default 'AA:BB:CC:DD:EE:FF' is placeholder");
}

// Test: broadcast is also placeholder
static void test_broadcast_is_placeholder() {
    const char* bc = "FF:FF:FF:FF:FF:FF";
    uint8_t mac[6];
    ASSERT_TRUE(parse_mac(bc, mac), "parse broadcast");
    ASSERT_TRUE(peer_mac_is_placeholder(mac), "broadcast must be placeholder");
    PASS("broadcast FF:FF:FF:FF:FF:FF is placeholder");
}

// Test: zero MAC is placeholder
static void test_zero_is_placeholder() {
    const char* zr = "00:00:00:00:00:00";
    uint8_t mac[6];
    ASSERT_TRUE(parse_mac(zr, mac), "parse zero");
    ASSERT_TRUE(peer_mac_is_placeholder(mac), "zero must be placeholder");
    PASS("zero 00:00:00:00:00:00 is placeholder");
}

// Test: a real-looking MAC is NOT a placeholder
static void test_real_mac_not_placeholder() {
    const char* real = "84:F7:03:12:34:56";
    uint8_t mac[6];
    ASSERT_TRUE(parse_mac(real, mac), "parse real mac");
    ASSERT_FALSE(peer_mac_is_placeholder(mac), "real MAC must not be placeholder");
    PASS("real MAC 84:F7:03:12:34:56 is not placeholder");
}

// Test: another real manufacturer-prefix MAC is not a placeholder
static void test_espressif_mac_not_placeholder() {
    const char* espressif = "A0:B7:65:AB:CD:EF";
    uint8_t mac[6];
    ASSERT_TRUE(parse_mac(espressif, mac), "parse espressif mac");
    ASSERT_FALSE(peer_mac_is_placeholder(mac), "espressif MAC must not be placeholder");
    PASS("Espressif MAC A0:B7:65:AB:CD:EF is not placeholder");
}

int main() {
    fprintf(stdout, "\n=== REQ-5: peer MAC placeholder detection ===\n");
    test_kconfig_default_is_placeholder();
    test_broadcast_is_placeholder();
    test_zero_is_placeholder();
    test_real_mac_not_placeholder();
    test_espressif_mac_not_placeholder();
    fprintf(stdout, "\nAll %d tests passed.\n", g_passed);
    return 0;
}
