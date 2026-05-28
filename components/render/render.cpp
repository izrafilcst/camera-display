// Sprint 2 — render component
// Double framebuffer in PSRAM + DMA swap via LovyanGFX (display component).

#include "render.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "render";

static uint16_t*       s_fb[2]     = {nullptr, nullptr};
static int             s_back_idx  = 0;
static SemaphoreHandle_t s_mutex   = nullptr;

bool render_init(void) {
    s_fb[0] = display_alloc_framebuffer_psram();
    s_fb[1] = display_alloc_framebuffer_psram();

    if (!s_fb[0] || !s_fb[1]) {
        ESP_LOGE(TAG, "framebuffer PSRAM alloc failed");
        return false;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex create failed");
        return false;
    }

    s_back_idx = 0;
    ESP_LOGI(TAG, "render ready: fb[0]=%p fb[1]=%p", s_fb[0], s_fb[1]);
    return true;
}

uint16_t* render_back_buffer(void) {
    return s_fb[s_back_idx];
}

void render_present(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Wait for previous DMA to finish before overwriting its source buffer
    display_wait_dma();
    // Blit the back buffer (becomes "front" while DMA runs)
    display_blit_full(s_fb[s_back_idx]);
    // Flip: next decode writes to the buffer not currently in DMA
    s_back_idx ^= 1;
    xSemaphoreGive(s_mutex);
}

void render_deinit(void) {
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
    }
    if (s_fb[0]) { display_free_framebuffer(s_fb[0]); s_fb[0] = nullptr; }
    if (s_fb[1]) { display_free_framebuffer(s_fb[1]); s_fb[1] = nullptr; }
}
