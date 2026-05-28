#pragma once
#include <cstdint>
#include <cstddef>

// Callback invoked for each received message (esnow_hdr_t already stripped).
typedef void (*espnow_msg_cb_t)(uint8_t msg_type,
                                const uint8_t* payload, size_t len,
                                int8_t rssi);

// Initialise Wi-Fi STA on given channel and ESP-NOW. Registers cb for all messages.
bool espnow_link_init(uint8_t channel, espnow_msg_cb_t cb);

// Add a peer MAC (TX side) — V0: hardcoded single peer.
bool espnow_link_add_peer(const uint8_t mac[6]);

// Send a message to mac with msg_type envelope.
bool espnow_link_send(const uint8_t* mac, uint8_t msg_type,
                      const uint8_t* payload, size_t len);

// REQ-5: returns true if mac is a known placeholder (default Kconfig, broadcast, zero).
bool peer_mac_is_placeholder(const uint8_t mac[6]);
