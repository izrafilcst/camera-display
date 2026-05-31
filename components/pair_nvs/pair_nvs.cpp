// Sprint 4 — pair_nvs implementation
//
// Persists the discovered TX MAC after a successful BLE pairing handshake.
// Two builds share the same source:
//   - Device build (default): uses ESP-IDF nvs_flash to persist into the
//     `pairing` NVS namespace.
//   - Host build (PAIR_NVS_HOST_BUILD=1): replaces nvs_* with a single
//     in-memory slot so the logic can be unit-tested without ESP-IDF.

#include "pair_nvs.h"
#include <cstring>

// Inline the placeholder check rather than depending on espnow_link.cpp —
// keeps the host build hermetic and avoids a circular component dep.
static bool pn_is_placeholder(const uint8_t mac[6]) {
    static const uint8_t k_placeholder[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    static const uint8_t k_broadcast[6]   = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t k_zero[6]        = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    return std::memcmp(mac, k_placeholder, 6) == 0 ||
           std::memcmp(mac, k_broadcast,   6) == 0 ||
           std::memcmp(mac, k_zero,        6) == 0;
}

bool pair_nvs_is_valid_mac_for_persist(const uint8_t mac[6]) {
    return !pn_is_placeholder(mac);
}

#ifdef PAIR_NVS_HOST_BUILD

// ---------------------------------------------------------------------------
// Host-side in-memory shim — no ESP-IDF dependency
// ---------------------------------------------------------------------------
namespace {
bool    s_has_entry = false;
uint8_t s_blob[6]   = {};
bool    s_inited    = false;
}

bool pair_nvs_init(void) {
    s_inited = true;
    return true;
}

bool pair_nvs_load_tx_mac(uint8_t out[6]) {
    if (!s_inited || !s_has_entry || !out) return false;
    std::memcpy(out, s_blob, 6);
    return true;
}

bool pair_nvs_save_tx_mac(const uint8_t mac[6]) {
    if (!s_inited || !mac) return false;
    if (!pair_nvs_is_valid_mac_for_persist(mac)) return false;
    std::memcpy(s_blob, mac, 6);
    s_has_entry = true;
    return true;
}

void pair_nvs_clear(void) {
    s_has_entry = false;
    std::memset(s_blob, 0, 6);
}

#else  // !PAIR_NVS_HOST_BUILD — device build, real NVS

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char* TAG = "pair_nvs";

bool pair_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), wiping", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pair_nvs_load_tx_mac(uint8_t out[6]) {
    if (!out) return false;

    nvs_handle_t h;
    if (nvs_open(PAIR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t paired = 0;
    if (nvs_get_u8(h, PAIR_NVS_KEY_PAIRED, &paired) != ESP_OK || paired != 1) {
        nvs_close(h);
        return false;
    }

    size_t sz = 6;
    uint8_t tmp[6] = {};
    esp_err_t err = nvs_get_blob(h, PAIR_NVS_KEY_TX_MAC, tmp, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != 6) return false;

    // Defense in depth — never hand back a placeholder, even if NVS got
    // corrupted. Callers treat that as "force re-pair".
    if (!pair_nvs_is_valid_mac_for_persist(tmp)) return false;

    std::memcpy(out, tmp, 6);
    return true;
}

bool pair_nvs_save_tx_mac(const uint8_t mac[6]) {
    if (!mac || !pair_nvs_is_valid_mac_for_persist(mac)) return false;

    nvs_handle_t h;
    if (nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

    bool ok = nvs_set_blob(h, PAIR_NVS_KEY_TX_MAC, mac, 6) == ESP_OK
           && nvs_set_u8(h,  PAIR_NVS_KEY_PAIRED, 1) == ESP_OK
           && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) ESP_LOGE(TAG, "save failed");
    return ok;
}

void pair_nvs_clear(void) {
    nvs_handle_t h;
    if (nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, PAIR_NVS_KEY_TX_MAC);
    nvs_erase_key(h, PAIR_NVS_KEY_PAIRED);
    nvs_commit(h);
    nvs_close(h);
}

#endif  // PAIR_NVS_HOST_BUILD
