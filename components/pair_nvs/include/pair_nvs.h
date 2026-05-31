#pragma once
#include <cstdint>
#include <cstdbool>

#ifdef __cplusplus
extern "C" {
#endif

// Namespace constant — exposed for tests; production callers use the API
#define PAIR_NVS_NAMESPACE  "pairing"
#define PAIR_NVS_KEY_TX_MAC "tx_mac"
#define PAIR_NVS_KEY_PAIRED "paired"

bool pair_nvs_init(void);
bool pair_nvs_load_tx_mac(uint8_t out[6]);   // returns false if not paired / invalid
bool pair_nvs_save_tx_mac(const uint8_t mac[6]);
void pair_nvs_clear(void);                    // wipes `paired` and `tx_mac`

// Logic-only entry point — testable on host without ESP-IDF NVS.
// Validates that mac is non-placeholder before allowing persist.
bool pair_nvs_is_valid_mac_for_persist(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
