// Sprint 2 — Video pipeline: ESP-NOW RX → reassembly → JPEG decode → display
// REQ-5: peer MAC loaded from Kconfig with placeholder warning at boot.
// CONFIG_RECEIVER_SMOKE_TEST swaps the network path for a Sprint 1 demo loop
// so the display can be validated standalone before a transmitter is wired up.

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "display.h"
#include "espnow_link.h"
#include "reassembly.h"
#include "decoder.h"
#include "render.h"
#include "link_state.h"
#include "ble_pair.h"
#include "pair_nvs.h"
#include "wire_types.h"
#include "pinout.h"
#include <cstring>

#if CONFIG_RECEIVER_SMOKE_TEST
// Test-pattern helpers live in components/display/test_patterns.cpp. They're
// not part of the public display.h API (Sprint 1 review F3), so forward-declare
// here only inside the smoke-test build.
extern void pattern_color_bars(uint16_t* buf);
extern void pattern_gradient(uint16_t* buf);
extern void pattern_checker(uint16_t* buf, int cell_px);
extern void pattern_tearing_stripes(uint16_t* buf, uint32_t frame_counter);
#endif

static const char* TAG = "main";

// ---------------------------------------------------------------------------
// REQ-5: peer MAC from Kconfig
// Change CONFIG_RECEIVER_PEER_MAC to your transmitter's real MAC before flashing.
// ---------------------------------------------------------------------------
static bool parse_peer_mac(const char* str, uint8_t out[6]) {
    unsigned v[6];
    int n = sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
                   &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

// Queue entry for completed reassembled frames
struct ready_frame_t {
    reassembled_frame_t frame;
};

static QueueHandle_t s_frame_q = nullptr;

// ---------------------------------------------------------------------------
// ESP-NOW RX callback (runs in Wi-Fi task context, Core 0)
// ---------------------------------------------------------------------------
static void on_msg(uint8_t msg_type, const uint8_t* payload, size_t len, int8_t rssi) {
    (void)rssi;
    if (msg_type != MSG_VIDEO_FRAG) return;

    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    // Liveness is refreshed in decode_task only after a frame fully decodes
    // (audit S3-01 follow-up): even gating on MSG_VIDEO_FRAG is too generous
    // because that msg_type has no replay window — an attacker could keep
    // CONNECTED by replaying any captured fragment. Marking on successful
    // JPEG decode requires producing a valid frame, which neither header
    // spoofing nor fragment replay can do.

    reassembled_frame_t out{};

    if (reassembly_push_frag(payload, len, now_ms, &out)) {
        ready_frame_t rf;
        rf.frame = out;
        if (xQueueSend(s_frame_q, &rf, 0) != pdTRUE) {
            // Queue full — drop frame; counter tracked externally via stats
            reassembly_release(&out);
        }
    }
    // Periodic GC: evict slots older than 30 ms
    reassembly_gc(now_ms);
}

// ---------------------------------------------------------------------------
// Decode task (Core 1, priority 6)
// ---------------------------------------------------------------------------
static void decode_task(void*) {
    while (true) {
        ready_frame_t rf{};
        if (xQueueReceive(s_frame_q, &rf, portMAX_DELAY) != pdTRUE) continue;

        uint16_t* back = render_back_buffer();
        int64_t dt = decoder_decode_to_rgb565(
                rf.frame.jpeg_data, rf.frame.jpeg_size, back, 320, 240);
        reassembly_release(&rf.frame);

        if (dt < 0) {
            ESP_LOGW(TAG, "decode failed — skipping frame");
            continue;
        }
        // Successful decode is the strongest available evidence of real
        // traffic from a cooperating transmitter — refresh liveness here.
        link_state_mark_rx(static_cast<uint32_t>(esp_timer_get_time() / 1000));
        render_present();
        render_capture_thumb();
    }
}

// ---------------------------------------------------------------------------
// Link UI task (Core 1, priority 4): polls link state at 10 Hz and paints
// FREEZE / DISCONNECTED overlays. Stays silent while LINK_CONNECTED to leave
// the LCD to the decode pipeline.
// ---------------------------------------------------------------------------
static void link_ui_task(void*) {
    link_status_t last = LINK_BOOT;
    while (true) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const link_status_t st = link_state_query(now_ms);
        switch (st) {
            case LINK_FREEZE:
                render_show_freeze();
                break;
            case LINK_DISCONNECTED:
                render_show_disconnected(link_state_idle_ms(now_ms));
                break;
            case LINK_BOOT:
            case LINK_CONNECTED:
            default:
                break;
        }
        if (st != last) {
            ESP_LOGI(TAG, "link %d -> %d", last, st);
            last = st;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Sprint 4 boot — free heap=%u",
             (unsigned)esp_get_free_heap_size());

#if CONFIG_RECEIVER_SMOKE_TEST
    // -----------------------------------------------------------------------
    // Smoke test mode: display + render only. Skip ESP-NOW / decode / link UI.
    // -----------------------------------------------------------------------
    ESP_LOGW(TAG, "SMOKE TEST MODE — bypassing espnow/decode/link_ui");
    if (!display_init(LCD_SPI_HZ_TARGET)) {
        ESP_LOGE(TAG, "display_init failed");
        return;
    }
    if (!render_init()) {
        ESP_LOGE(TAG, "render_init failed");
        return;
    }
    uint32_t frame = 0;
    while (true) {
        uint16_t* back = render_back_buffer();
        const uint32_t which = (frame / 60u) % 4u;   // ~2 s per pattern at 30 fps
        switch (which) {
            case 0: pattern_color_bars(back);           break;
            case 1: pattern_gradient(back);             break;
            case 2: pattern_checker(back, 20);          break;
            case 3: pattern_tearing_stripes(back, frame); break;
        }
        render_present();
        if ((frame % 30u) == 0u) {
            ESP_LOGI(TAG, "smoke: frame=%u pattern=%u free_psram=%u",
                     (unsigned)frame, (unsigned)which,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        vTaskDelay(pdMS_TO_TICKS(33));
        frame++;
    }
#else
    // -----------------------------------------------------------------------
    // Boot rota — precedência (spec §11.5):
    //   1. SMOKE_TEST (acima, já filtrado pelo ifdef)
    //   2. FORCE_PAIR_AGAIN -> wipe NVS e re-pareia
    //   3. CONFIG_RECEIVER_PEER_MAC não-placeholder -> override sem pareamento
    //   4. NVS paired                                -> Fase 2 direto
    //   5. caso contrário                            -> BLE pair (Fase 1)
    // -----------------------------------------------------------------------
    if (!pair_nvs_init()) {
        ESP_LOGE(TAG, "pair_nvs_init failed");
        return;
    }

#if CONFIG_RECEIVER_FORCE_PAIR_AGAIN
    ESP_LOGW(TAG, "FORCE_PAIR_AGAIN — wiping pairing NVS");
    pair_nvs_clear();
#endif

    uint8_t tx_mac[6] = {};
    bool have_tx_mac = false;

    // Kconfig override (útil para teste com ESP-NOW sem TX BLE)
    uint8_t override_mac[6];
    if (parse_peer_mac(CONFIG_RECEIVER_PEER_MAC, override_mac)
        && !peer_mac_is_placeholder(override_mac)) {
        ESP_LOGW(TAG, "using CONFIG_RECEIVER_PEER_MAC override (no BLE pairing)");
        std::memcpy(tx_mac, override_mac, 6);
        have_tx_mac = true;
    }

    // Tenta NVS pareado
    if (!have_tx_mac && pair_nvs_load_tx_mac(tx_mac)) {
        ESP_LOGI(TAG, "using saved peer: %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_mac[0], tx_mac[1], tx_mac[2], tx_mac[3], tx_mac[4], tx_mac[5]);
        have_tx_mac = true;
    }

    // Sem NVS e sem override → roda Fase 1 (BLE pair)
    if (!have_tx_mac) {
        ESP_LOGI(TAG, "no paired TX in NVS — running BLE pairing handshake");
        ble_pair_err_t pair_err;
        if (!ble_pair_run(CONFIG_RECEIVER_PAIRING_PIN, tx_mac, &pair_err)) {
            ESP_LOGE(TAG, "pairing failed: %s — rebooting in 5s",
                     ble_pair_err_str(pair_err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        if (!pair_nvs_save_tx_mac(tx_mac)) {
            ESP_LOGE(TAG, "pair_nvs_save_tx_mac rejected MAC — rebooting in 5s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        ESP_LOGI(TAG, "paired: %02X:%02X:%02X:%02X:%02X:%02X — saved to NVS",
                 tx_mac[0], tx_mac[1], tx_mac[2], tx_mac[3], tx_mac[4], tx_mac[5]);
    }

    link_state_init();

    // Init order: Wi-Fi first to claim GDMA channels before SPI2 (see spec §1)
    if (!espnow_link_init(6, on_msg)) {
        ESP_LOGE(TAG, "espnow_link_init failed");
        return;
    }
    espnow_link_add_peer(tx_mac);

    if (!display_init(LCD_SPI_HZ_TARGET)) {
        ESP_LOGE(TAG, "display_init failed");
        return;
    }
    if (!render_init()) {
        ESP_LOGE(TAG, "render_init failed");
        return;
    }
    if (!decoder_init()) {
        ESP_LOGE(TAG, "decoder_init failed");
        return;
    }
    if (!reassembly_init(2)) {
        ESP_LOGE(TAG, "reassembly_init failed");
        return;
    }

    s_frame_q = xQueueCreate(4, sizeof(ready_frame_t));
    if (!s_frame_q) {
        ESP_LOGE(TAG, "frame queue create failed");
        return;
    }

    xTaskCreatePinnedToCore(decode_task,  "decode",  8192, nullptr, 6, nullptr, 1);
    xTaskCreatePinnedToCore(link_ui_task, "link_ui", 4096, nullptr, 4, nullptr, 1);

    // Stats loop: log reassembly metrics every second
    uint32_t last_completed = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const auto* s = reassembly_stats();
        uint32_t fps = s->frames_completed - last_completed;
        last_completed = s->frames_completed;
        ESP_LOGI(TAG,
                 "fps=%u drops_timeout=%u drops_overrun=%u frags_invalid=%u free_psram=%u",
                 (unsigned)fps,
                 (unsigned)s->frames_dropped_timeout,
                 (unsigned)s->frames_dropped_overrun,
                 (unsigned)s->fragments_invalid,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
#endif  // CONFIG_RECEIVER_SMOKE_TEST
}
