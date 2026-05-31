// Sprint 4 — ble_pair NimBLE dispatch (CENTRAL/client role).
//
// Drives the TX-spec handshake: scan -> connect -> discover -> write PIN ->
// read TX MAC -> write RX MAC -> done. Every NimBLE callback collapses to
// a ble_pair_sm_feed() call; terminal states release the wait via an
// EventGroup bit so the caller of ble_pair_run() unblocks.
//
// The pure state machine (ble_pair_state.cpp) is the only place that
// owns the protocol logic. This file is just glue.

#include "ble_pair.h"
#include "internal/ble_pair_state.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include <cstring>

static const char* TAG = "ble_pair";

// ---------------------------------------------------------------------------
// Wire constants — must match TX spec (see docs/.../specs §11)
// ---------------------------------------------------------------------------
#define PAIR_SVC_UUID16   0x1234
#define CHR_PIN_UUID16    0x1235
#define CHR_RX_MAC_UUID16 0x1236
#define CHR_TX_MAC_UUID16 0x1237

// ---------------------------------------------------------------------------
// Termination bits for the caller-side wait
// ---------------------------------------------------------------------------
#define BIT_DONE  (1u << 0)
#define BIT_ERROR (1u << 1)

// ---------------------------------------------------------------------------
// Module state — only valid while ble_pair_run is in flight.
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_done_eg     = nullptr;
static ble_pair_sm_t      s_sm          = {};
static uint32_t           s_pin         = 0;
static uint16_t           s_conn_handle = 0;
static uint16_t           s_h_pin       = 0;
static uint16_t           s_h_rx_mac    = 0;
static uint16_t           s_h_tx_mac    = 0;
static uint16_t           s_svc_start   = 0;
static uint16_t           s_svc_end     = 0;
static uint8_t            s_own_addr_type = 0;
static uint8_t            s_rx_mac_le[6] = {0};

// Forward declarations
static int  on_gap_event(struct ble_gap_event* event, void* arg);
static int  on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error* err,
                        const struct ble_gatt_svc* svc, void* arg);
static int  on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error* err,
                        const struct ble_gatt_chr* chr, void* arg);
static int  on_pin_written(uint16_t conn_handle, const struct ble_gatt_error* err,
                            struct ble_gatt_attr* attr, void* arg);
static int  on_tx_mac_read(uint16_t conn_handle, const struct ble_gatt_error* err,
                            struct ble_gatt_attr* attr, void* arg);
static int  on_rx_mac_written(uint16_t conn_handle, const struct ble_gatt_error* err,
                               struct ble_gatt_attr* attr, void* arg);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void terminate(bool ok) {
    if (!s_done_eg) return;
    xEventGroupSetBits(s_done_eg, ok ? BIT_DONE : BIT_ERROR);
}

static void log_transition(ble_pair_state_t before) {
    if (before != s_sm.state) {
        ESP_LOGI(TAG, "%s -> %s%s%s",
                 ble_pair_state_str(before),
                 ble_pair_state_str(s_sm.state),
                 s_sm.state == BLE_PAIR_ERROR ? " err=" : "",
                 s_sm.state == BLE_PAIR_ERROR ? ble_pair_err_str(s_sm.err) : "");
    }
    if (s_sm.state == BLE_PAIR_DONE)  terminate(true);
    if (s_sm.state == BLE_PAIR_ERROR) terminate(false);
}

static void feed(ble_pair_event_t ev, const uint8_t* p = nullptr, size_t plen = 0) {
    const ble_pair_state_t before = s_sm.state;
    ble_pair_sm_feed(&s_sm, ev, p, plen);
    log_transition(before);
}

// ---------------------------------------------------------------------------
// Adv parsing: match the device name in adv fields
// ---------------------------------------------------------------------------
static bool adv_has_name(const struct ble_gap_disc_desc* d, const char* target) {
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) return false;
    if (fields.name_len == 0 || !fields.name) return false;
    const size_t tlen = std::strlen(target);
    if (fields.name_len != tlen) return false;
    return std::memcmp(fields.name, target, tlen) == 0;
}

// ---------------------------------------------------------------------------
// Step pumps — each one initiates the next NimBLE op based on the new state
// ---------------------------------------------------------------------------
static void start_discovery(void) {
    ble_uuid16_t svc = BLE_UUID16_INIT(PAIR_SVC_UUID16);
    int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &svc.u, on_svc_disc, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "disc_svc_by_uuid failed rc=%d", rc);
        feed(EV_DISCOVER_FAILED);
    }
}

static void start_chr_disc(void) {
    int rc = ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start, s_svc_end,
                                      on_chr_disc, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "disc_all_chrs failed rc=%d", rc);
        feed(EV_DISCOVER_FAILED);
    }
}

static void start_pin_write(void) {
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>( s_pin        & 0xFFu);
    buf[1] = static_cast<uint8_t>((s_pin >> 8)  & 0xFFu);
    buf[2] = static_cast<uint8_t>((s_pin >> 16) & 0xFFu);
    buf[3] = static_cast<uint8_t>((s_pin >> 24) & 0xFFu);
    int rc = ble_gattc_write_flat(s_conn_handle, s_h_pin, buf, sizeof(buf),
                                   on_pin_written, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "write_flat(PIN) failed rc=%d", rc);
        feed(EV_PIN_WRITE_REJECTED);
    }
}

static void start_tx_mac_read(void) {
    int rc = ble_gattc_read(s_conn_handle, s_h_tx_mac, on_tx_mac_read, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "read(TX_MAC) failed rc=%d", rc);
        feed(EV_TX_MAC_READ_BAD_LEN);
    }
}

static void start_rx_mac_write(void) {
    int rc = ble_gattc_write_flat(s_conn_handle, s_h_rx_mac,
                                   s_rx_mac_le, sizeof(s_rx_mac_le),
                                   on_rx_mac_written, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "write_flat(RX_MAC) failed rc=%d", rc);
        feed(EV_RX_MAC_WRITE_AUTH_FAIL);
    }
}

// ---------------------------------------------------------------------------
// NimBLE callbacks
// ---------------------------------------------------------------------------
static int on_gap_event(struct ble_gap_event* event, void* /*arg*/) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (adv_has_name(&event->disc, "CAM-TX")) {
            ESP_LOGI(TAG, "CAM-TX advertising spotted");
            ble_gap_disc_cancel();
            feed(EV_ADV_MATCH);
            // Connect using the advertiser's address from the disc event
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr,
                                      10000, nullptr, on_gap_event, nullptr);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_connect failed rc=%d", rc);
                feed(EV_CONNECT_FAILED);
            }
        }
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        // Scan ended without us matching anything
        if (s_sm.state == BLE_PAIR_SCANNING) feed(EV_SCAN_TIMEOUT);
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            feed(EV_CONNECTED);
            start_discovery();
        } else {
            feed(EV_CONNECT_FAILED);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        feed(EV_DISCONNECTED);
        break;

    default:
        break;
    }
    return 0;
}

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error* err,
                       const struct ble_gatt_svc* svc, void* /*arg*/) {
    if (conn_handle != s_conn_handle) return 0;
    if (err->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (s_svc_start == 0 || s_svc_end == 0) {
            feed(EV_DISCOVER_FAILED);
        } else {
            start_chr_disc();
        }
    } else {
        ESP_LOGW(TAG, "svc disc err status=%d", err->status);
        feed(EV_DISCOVER_FAILED);
    }
    return 0;
}

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error* err,
                       const struct ble_gatt_chr* chr, void* /*arg*/) {
    if (conn_handle != s_conn_handle) return 0;
    if (err->status == 0 && chr) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            const uint16_t u16 = chr->uuid.u16.value;
            if (u16 == CHR_PIN_UUID16)    s_h_pin    = chr->val_handle;
            if (u16 == CHR_RX_MAC_UUID16) s_h_rx_mac = chr->val_handle;
            if (u16 == CHR_TX_MAC_UUID16) s_h_tx_mac = chr->val_handle;
        }
    } else if (err->status == BLE_HS_EDONE) {
        if (s_h_pin && s_h_rx_mac && s_h_tx_mac) {
            feed(EV_DISCOVERED);
            start_pin_write();
        } else {
            ESP_LOGW(TAG, "missing chrs: pin=%u rx=%u tx=%u",
                     s_h_pin, s_h_rx_mac, s_h_tx_mac);
            feed(EV_DISCOVER_FAILED);
        }
    } else {
        ESP_LOGW(TAG, "chr disc err status=%d", err->status);
        feed(EV_DISCOVER_FAILED);
    }
    return 0;
}

static int on_pin_written(uint16_t conn_handle, const struct ble_gatt_error* err,
                          struct ble_gatt_attr* /*attr*/, void* /*arg*/) {
    if (conn_handle != s_conn_handle) return 0;
    if (err->status == 0) {
        feed(EV_PIN_WRITE_OK);
        start_tx_mac_read();
    } else {
        ESP_LOGW(TAG, "PIN write rejected status=%d", err->status);
        feed(EV_PIN_WRITE_REJECTED);
    }
    return 0;
}

static int on_tx_mac_read(uint16_t conn_handle, const struct ble_gatt_error* err,
                          struct ble_gatt_attr* attr, void* /*arg*/) {
    if (conn_handle != s_conn_handle) return 0;
    if (err->status != 0 || !attr || !attr->om) {
        feed(EV_TX_MAC_READ_BAD_LEN);
        return 0;
    }
    const uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len != 6) {
        ESP_LOGW(TAG, "TX MAC bad len=%u", (unsigned)len);
        feed(EV_TX_MAC_READ_BAD_LEN);
        return 0;
    }
    uint8_t mac[6];
    if (os_mbuf_copydata(attr->om, 0, 6, mac) != 0) {
        feed(EV_TX_MAC_READ_BAD_LEN);
        return 0;
    }
    feed(EV_TX_MAC_READ_OK, mac, 6);
    if (s_sm.state == BLE_PAIR_WRITING_RX_MAC) start_rx_mac_write();
    return 0;
}

static int on_rx_mac_written(uint16_t conn_handle, const struct ble_gatt_error* err,
                             struct ble_gatt_attr* /*attr*/, void* /*arg*/) {
    if (conn_handle != s_conn_handle) return 0;
    if (err->status == 0) {
        feed(EV_RX_MAC_WRITE_OK);
        // TX will disconnect; that's expected at this point.
    } else {
        ESP_LOGW(TAG, "RX MAC write rejected status=%d", err->status);
        feed(EV_RX_MAC_WRITE_AUTH_FAIL);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NimBLE host bootstrap
// ---------------------------------------------------------------------------
static void on_sync(void) {
    // Determine which address type we can use
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
        feed(EV_SCAN_TIMEOUT);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        feed(EV_SCAN_TIMEOUT);
        return;
    }

    // Start scan
    struct ble_gap_disc_params disc = {};
    disc.itvl              = 0;
    disc.window            = 0;
    disc.filter_policy     = 0;
    disc.limited           = 0;
    disc.passive           = 1;
    disc.filter_duplicates = 1;

    feed(EV_START);

    const int32_t duration_ms =
        CONFIG_BLE_SCAN_TIMEOUT_S > 0 ? CONFIG_BLE_SCAN_TIMEOUT_S * 1000 : 30000;
    rc = ble_gap_disc(s_own_addr_type, duration_ms, &disc, on_gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        feed(EV_SCAN_TIMEOUT);
    } else {
        ESP_LOGI(TAG, "scanning for CAM-TX (%lds)...", (long)(duration_ms / 1000));
    }
}

static void host_task(void* /*param*/) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ble_pair_get_rx_mac(uint8_t out[6]) {
    esp_read_mac(out, ESP_MAC_WIFI_STA);
}

bool ble_pair_run(uint32_t pin, uint8_t tx_mac_out[6], ble_pair_err_t* err_out) {
    if (!tx_mac_out || !err_out) return false;
    *err_out = BLE_PAIR_ERR_NONE;

    s_pin         = pin;
    s_conn_handle = 0;
    s_h_pin = s_h_rx_mac = s_h_tx_mac = 0;
    s_svc_start = s_svc_end = 0;
    ble_pair_sm_init(&s_sm);

    // Our own MAC (sent to the TX in chr 0x1236)
    ble_pair_get_rx_mac(s_rx_mac_le);

    s_done_eg = xEventGroupCreate();
    if (!s_done_eg) { *err_out = BLE_PAIR_ERR_STACK_INIT; return false; }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        vEventGroupDelete(s_done_eg);
        s_done_eg = nullptr;
        *err_out = BLE_PAIR_ERR_STACK_INIT;
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    nimble_port_freertos_init(host_task);

    EventBits_t bits = xEventGroupWaitBits(
        s_done_eg, BIT_DONE | BIT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

    // Tear down NimBLE — frees ~150 KB before Wi-Fi/ESP-NOW comes up.
    nimble_port_stop();
    nimble_port_deinit();

    vEventGroupDelete(s_done_eg);
    s_done_eg = nullptr;

    if (bits & BIT_DONE) {
        std::memcpy(tx_mac_out, s_sm.tx_mac, 6);
        return true;
    }
    *err_out = s_sm.err;
    return false;
}
