// Sprint 2 — Video pipeline: ESP-NOW RX → reassembly → JPEG decode → display
// REQ-5: peer MAC loaded from Kconfig with placeholder warning at boot.

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "display.h"
#include "espnow_link.h"
#include "reassembly.h"
#include "decoder.h"
#include "render.h"
#include "wire_types.h"
#include "pinout.h"
#include <cstring>

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
        render_present();
    }
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Sprint 2 boot — free heap=%u",
             (unsigned)esp_get_free_heap_size());

    // REQ-5: parse Kconfig MAC and warn if placeholder
    uint8_t tx_mac[6];
    if (!parse_peer_mac(CONFIG_RECEIVER_PEER_MAC, tx_mac)) {
        ESP_LOGE(TAG, "CONFIG_RECEIVER_PEER_MAC parse failed: '%s'",
                 CONFIG_RECEIVER_PEER_MAC);
        return;
    }
    if (peer_mac_is_placeholder(tx_mac)) {
        ESP_LOGW(TAG, "*** PEER MAC IS PLACEHOLDER — set CONFIG_RECEIVER_PEER_MAC ***");
    }

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

    xTaskCreatePinnedToCore(decode_task, "decode", 8192, nullptr, 6, nullptr, 1);

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
}
