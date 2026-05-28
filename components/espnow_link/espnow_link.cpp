// Sprint 2 — espnow_link component
// REQ-4: replay window for MSG_TELEMETRY (seq uint32_t)
// REQ-5: peer_mac_is_placeholder + Kconfig warning at boot

#include "espnow_link.h"
#include "wire_types.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>

static const char* TAG = "espnow_link";
static espnow_msg_cb_t s_cb = nullptr;

// ---------------------------------------------------------------------------
// REQ-4: replay-protection state (per msg_type)
// ---------------------------------------------------------------------------
#define REPLAY_WINDOW 32
static uint32_t s_last_seq[256] = {};  // indexed by msg_type

static bool check_and_update_seq(uint8_t msg_type, uint32_t seq) {
    uint32_t last = s_last_seq[msg_type];
    if (seq > last) {
        s_last_seq[msg_type] = seq;
        return true;  // fresh packet
    }
    // Tolerate out-of-order within REPLAY_WINDOW
    if ((last - seq) <= REPLAY_WINDOW) return true;
    // Too old — likely replay
    ESP_LOGW(TAG, "replay or stale seq=%u last=%u type=0x%02X", seq, last, msg_type);
    return false;
}

// ---------------------------------------------------------------------------
// ESP-NOW RX callback
// ---------------------------------------------------------------------------
static void on_rx(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < static_cast<int>(sizeof(esnow_hdr_t))) return;

    esnow_hdr_t h;
    memcpy(&h, data, sizeof(h));

    int8_t rssi = (info && info->rx_ctrl) ? info->rx_ctrl->rssi : 0;

    // REQ-4: for MSG_TELEMETRY, extract uint32_t seq from payload and replay-check.
    // payload begins immediately after the 2-byte esnow_hdr_t that was already stripped.
    // telemetry_rx_to_tx layout: msg_type(1) + reserved(1) + seq(4) + ...
    // So seq sits at payload[2..5] (offset 2 within the payload pointer).
    if (h.msg_type == MSG_TELEMETRY) {
        const uint8_t* payload = data + sizeof(h);
        size_t plen = static_cast<size_t>(len) - sizeof(h);
        // seq is at offset 2 within payload (after msg_type + reserved in telemetry body)
        static_assert(offsetof(telemetry_rx_to_tx, seq) == 2,
                      "seq must be at byte 2 of telemetry_rx_to_tx");
        if (plen < 2 + sizeof(uint32_t)) {
            return;  // too short for seq field
        }
        uint32_t seq;
        memcpy(&seq, payload + 2, sizeof(seq));
        if (!check_and_update_seq(h.msg_type, seq)) return;
    }

    if (s_cb) s_cb(h.msg_type, data + sizeof(h),
                   static_cast<size_t>(len) - sizeof(h), rssi);
}

// ---------------------------------------------------------------------------
// REQ-5: placeholder detection
// ---------------------------------------------------------------------------
bool peer_mac_is_placeholder(const uint8_t mac[6]) {
    static const uint8_t placeholder[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    static const uint8_t broadcast[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t zero[6]        = {0x00,0x00,0x00,0x00,0x00,0x00};
    return memcmp(mac, placeholder, 6) == 0 ||
           memcmp(mac, broadcast,   6) == 0 ||
           memcmp(mac, zero,        6) == 0;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool espnow_link_init(uint8_t channel, espnow_msg_cb_t cb) {
    s_cb = cb;

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(on_rx);
    memset(s_last_seq, 0, sizeof(s_last_seq));

    ESP_LOGI(TAG, "esp-now ready, channel=%u", channel);
    return true;
}

// ---------------------------------------------------------------------------
// Peer management
// ---------------------------------------------------------------------------
bool espnow_link_add_peer(const uint8_t mac[6]) {
    // REQ-5: warn if placeholder
    if (peer_mac_is_placeholder(mac)) {
        ESP_LOGW(TAG, "*** PEER MAC IS PLACEHOLDER — set CONFIG_RECEIVER_PEER_MAC ***");
    }
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;           // 0 = current channel
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;       // V0: no encryption
    return esp_now_add_peer(&p) == ESP_OK;
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------
bool espnow_link_send(const uint8_t* mac, uint8_t msg_type,
                      const uint8_t* payload, size_t len) {
    if (sizeof(esnow_hdr_t) + len > ESPNOW_MTU) return false;

    uint8_t buf[ESPNOW_MTU];
    esnow_hdr_t h{msg_type, 0x00};
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), payload, len);
    return esp_now_send(mac, buf, sizeof(h) + len) == ESP_OK;
}
