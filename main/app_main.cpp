#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "pinout.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "camera-display Sprint 1 boot, free heap=%u",
             (unsigned)esp_get_free_heap_size());

    if (!display_init(LCD_SPI_HZ_TARGET)) {
        ESP_LOGE(TAG, "display init failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Single framebuffer in internal DRAM (153.6 KB fits in ~320 KB SRAM).
    // DMA | INTERNAL ensures DMA engine can access it directly.
    uint16_t* fb = static_cast<uint16_t*>(
        heap_caps_malloc(320u * 240u * sizeof(uint16_t),
                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!fb) {
        ESP_LOGE(TAG, "fb alloc failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // PSRAM smoke test — validates allocator before entering demo loop.
    uint16_t* psram_fb = display_alloc_framebuffer_psram();
    if (psram_fb) {
        // Architect §5.4: assert 4-byte alignment on PSRAM buffer.
        assert(((uintptr_t)psram_fb & 0x3u) == 0u);
        ESP_LOGI(TAG, "psram fb ok at %p (aligned=%d, free spiram=%u)",
                 static_cast<void*>(psram_fb),
                 (((uintptr_t)psram_fb & 0x3u) == 0u) ? 1 : 0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        display_free_framebuffer(psram_fb);
    } else {
        ESP_LOGE(TAG, "psram fb alloc failed");
    }

    // Demo loop: cycle through 4 test patterns at ~30 fps, switching every 2 s.
    uint32_t frame = 0;
    while (true) {
        int64_t t0 = esp_timer_get_time();
        switch ((frame / 60u) % 4u) {
            case 0: pattern_color_bars(fb);               break;
            case 1: pattern_gradient(fb);                 break;
            case 2: pattern_checker(fb, 20);              break;
            case 3: pattern_tearing_stripes(fb, frame);   break;
        }
        int64_t t1 = esp_timer_get_time();
        display_blit_full(fb);
        display_wait_dma();
        int64_t t2 = esp_timer_get_time();
        if ((frame % 30u) == 0u) {
            ESP_LOGI(TAG, "frame %u: fill=%lld us blit=%lld us",
                     (unsigned)frame,
                     (long long)(t1 - t0),
                     (long long)(t2 - t1));
        }
        ++frame;
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps demo
    }
}
