// Sprint 2 — render component
// Sprint 3 — added thumb capture + link status overlays (FREEZE / DISCONNECTED).
//
// Concurrency model:
//   - render_present (decode task, Core 1) flips s_back_idx and starts DMA.
//   - render_show_freeze / render_show_disconnected (link_ui_task, Core 1)
//     draw on the LCD directly via LovyanGFX.
//   - render_capture_thumb reads the just-blitted front buffer.
// All four take s_mutex so concurrent LCD or back_idx access is serialized.

#include "render.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static const char* TAG = "render";

static uint16_t*         s_fb[2]    = {nullptr, nullptr};
static int               s_back_idx = 0;
static SemaphoreHandle_t s_mutex    = nullptr;

// Thumbnail of the last presented frame (80x60, 4× downsample).
static constexpr int THUMB_W = 80;
static constexpr int THUMB_H = 60;
static uint16_t s_thumb[THUMB_W * THUMB_H];
static bool     s_thumb_valid = false;
static bool     s_has_presented = false;

static inline lgfx::LGFX_Device* get_lcd(void) {
    // display_get_lgfx_ptr returns void* to keep display.h a pure-C interface
    // (Sprint 1 review F3). Cast back to the LovyanGFX type here in C++ land.
    return reinterpret_cast<lgfx::LGFX_Device*>(display_get_lgfx_ptr());
}

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

    s_back_idx      = 0;
    s_thumb_valid   = false;
    s_has_presented = false;
    ESP_LOGI(TAG, "render ready: fb[0]=%p fb[1]=%p", s_fb[0], s_fb[1]);
    return true;
}

uint16_t* render_back_buffer(void) {
    return s_fb[s_back_idx];
}

void render_present(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_wait_dma();
    display_blit_full(s_fb[s_back_idx]);
    s_back_idx ^= 1;
    s_has_presented = true;
    xSemaphoreGive(s_mutex);
}

void render_capture_thumb(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_has_presented) {
        xSemaphoreGive(s_mutex);
        return;
    }
    // The just-blitted frame is now the "front" (index s_back_idx ^ 1 because
    // render_present already flipped). The decoder is forbidden from touching
    // it until the next render_present swap.
    const uint16_t* src = s_fb[s_back_idx ^ 1];
    for (int y = 0; y < THUMB_H; ++y) {
        const uint16_t* row = &src[(y * 4) * 320];
        for (int x = 0; x < THUMB_W; ++x) {
            s_thumb[y * THUMB_W + x] = row[x * 4];
        }
    }
    s_thumb_valid = true;
    xSemaphoreGive(s_mutex);
}

void render_show_freeze(void) {
    auto* lcd = get_lcd();
    if (!lcd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_wait_dma();
    // Blink the badge at ~2 Hz (250 ms half-period).
    const bool blink = (esp_timer_get_time() / 250000) & 1;
    if (blink) {
        lcd->startWrite();
        lcd->fillRect(250, 4, 66, 16, 0xF800);          // red badge
        lcd->setTextColor(0xFFFF, 0xF800);
        lcd->setTextSize(2);
        lcd->setCursor(254, 6);
        lcd->print("FREEZE");
        lcd->endWrite();
    }
    xSemaphoreGive(s_mutex);
}

void render_show_disconnected(uint32_t since_ms) {
    auto* lcd = get_lcd();
    if (!lcd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_wait_dma();
    lcd->startWrite();
    lcd->fillScreen(0x1082);                            // very dark gray
    lcd->setTextColor(0xFFFF);
    lcd->setTextSize(2);
    lcd->setCursor(60, 80);
    lcd->print("AGUARDANDO LINK");
    lcd->setTextSize(1);
    lcd->setCursor(110, 110);
    lcd->printf("offline: %lu s", (unsigned long)(since_ms / 1000));
    if (s_thumb_valid) {
        lcd->pushImage(120, 140, THUMB_W, THUMB_H, s_thumb);
        lcd->setCursor(120, 205);
        lcd->print("ultimo frame");
    }
    lcd->endWrite();
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
