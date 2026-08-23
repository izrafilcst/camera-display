#include "display.h"
#include "lgfx_ili9341_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cassert>
#include <cstdint>
#include <cstddef>   // size_t — usado tambem pelo blit fatiado da Task 8

static const char* TAG = "display";
static LGFX_ILI9341_Red* s_lcd = nullptr;
static uint32_t s_spi_hz = 0;

bool display_init(uint32_t spi_hz) {
    if (s_lcd != nullptr) {
        if (spi_hz != s_spi_hz) {
            ESP_LOGW(TAG, "already initialised at %u Hz; ignoring request for %u Hz",
                     (unsigned)s_spi_hz, (unsigned)spi_hz);
        }
        return true;
    }

    // Neutralize SD and XPT2046 CS lines before SPI init to prevent
    // those chips from responding during ILI9341 init sequence.
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_SD_CS,    GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_TOUCH_CS, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_SD_CS,    1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_TOUCH_CS, 1));

    // Backlight hard off before the reset below. The manual reset runs before
    // LovyanGFX's Light_PWM::init(0), so without this the BL pin would float
    // through the reset pulse — exactly the flash we are removing.
    // cfg.invert == false in lgfx_ili9341_config.h, so LOW = off.
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_LCD_BL, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_LCD_BL, 0));

    // Hardware reset with the datasheet's timing (ILI9341 V1.11, p.230 s15.4):
    //   tRW >= 10 us  -> 10 ms here, margin for a slow-rising rail
    //   note 7: after releasing RESX wait 5 ms before any command, AND do not
    //           send Sleep Out for 120 ms.
    // LovyanGFX's own reset (Panel_Device::init) waits only 64 ms before the
    // SLPOUT in its init list, so we do the reset ourselves with the right
    // timing and then call init_without_reset(), which keeps everything else
    // it does (init_cs, Light_PWM::init(0), bus init, rst high).
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_LCD_RST, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(120));

    s_lcd = new LGFX_ILI9341_Red(spi_hz);
    // NOTE: with cfg.readable == false LovyanGFX never reads the panel back,
    // so init_impl always returns true — this check cannot detect a missing or
    // dead panel. Kept because it costs nothing and starts working the day we
    // enable reads.
    if (!s_lcd->init_without_reset(false)) {
        ESP_LOGE(TAG, "lcd init failed @ %u Hz", (unsigned)spi_hz);
        delete s_lcd;
        s_lcd = nullptr;
        return false;
    }

    s_lcd->setRotation(1);       // landscape 320x240

    // fillScreen BEFORE setBrightness: after power-on the panel GRAM holds
    // random data (datasheet p.114, RAMWR "Default" table: "Contents of
    // memory is set randomly"). Lighting the backlight first shows that
    // garbage for one frame.
    s_lcd->fillScreen(0x0000);   // black
    s_lcd->setBrightness(200);   // 0..255

    s_spi_hz = spi_hz;
    ESP_LOGI(TAG, "lcd ok %dx%d @ %u Hz",
             s_lcd->width(), s_lcd->height(), (unsigned)spi_hz);
    return true;
}

int display_width(void)  { return s_lcd ? s_lcd->width()  : 320; }
int display_height(void) { return s_lcd ? s_lcd->height() : 240; }

// 48 * 320 * 2 = 30720 bytes — abaixo do teto de ~32 KB de uma transacao de
// hardware do SPI. 240 / 48 = 5 fatias exatas.
static constexpr int kBlitSliceRows = 48;

void display_blit_full(const rgb565be_t* buf) {
    if (!s_lcd || !buf) return;
    const int w = s_lcd->width();
    const int h = s_lcd->height();

    // The address window is set per frame. It costs ~5 us against a 30,7 ms
    // frame at 40 MHz (0,016%) and buys back a global invariant that used to
    // span display.cpp and render.cpp: any LovyanGFX call emitting CASET or
    // RASET would silently corrupt the next blit.
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, w, h);
    // swap=false: the buffer is already big-endian, so pixelcopy takes the
    // no_convert path and writeBytes DMAs straight from PSRAM — no CPU pass
    // over 76800 pixels. writePixelsDMA (not writePixels) is what actually
    // passes use_dma=true down to Bus_SPI.
    for (int y = 0; y < h; y += kBlitSliceRows) {
        const int rows = (y + kBlitSliceRows <= h) ? kBlitSliceRows : (h - y);
        s_lcd->writePixelsDMA(&buf[(size_t)y * w], rows * w, false);
    }
    s_lcd->endWrite();
}

void display_blit_rect(const rgb565be_t* src, int src_stride,
                       int x, int y, int w, int h) {
    if (!s_lcd || !src || w <= 0 || h <= 0) return;

    // startWrite/endWrite are reference-counted by LovyanGFX, so this nests
    // safely inside a block the caller already opened.
    s_lcd->startWrite();
    s_lcd->setAddrWindow(x, y, w, h);
    for (int row = 0; row < h; ++row) {
        s_lcd->writePixels(&src[(size_t)row * src_stride], w, false);
    }
    s_lcd->endWrite();
    // Callers draw text/rects right after this returns, so finish the transfer
    // rather than leaving it in flight.
    s_lcd->waitDMA();
}

void display_wait_dma(void) {
    if (!s_lcd) return;
    s_lcd->waitDMA();
}

uint16_t* display_alloc_framebuffer_psram(void) {
    const size_t bytes = 320u * 240u * sizeof(uint16_t);
    // MALLOC_CAP_DMA is deliberately NOT requested. On the ESP32-S3 the PSRAM
    // heap is registered as SPIRAM|DEFAULT|8BIT|32BIT and never carries the
    // DMA cap (esp_psram.c, esp_psram_extram_add_to_heap_allocator), so
    // SPIRAM|DMA can only ever return nullptr — the old primary path was dead
    // code and its warning fired on every boot, twice. GDMA reaches PSRAM
    // through the cache on the S3 regardless of the flag.
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) {
        ESP_LOGE(TAG, "psram framebuffer alloc failed (%u bytes)", (unsigned)bytes);
        return nullptr;
    }
    assert(((uintptr_t)p % 4) == 0);   // GDMA requires 4-byte alignment
    return static_cast<uint16_t*>(p);
}

void display_free_framebuffer(uint16_t* fb) {
    if (fb) heap_caps_free(fb);
}

void* display_get_lgfx_ptr(void) {
    return static_cast<void*>(s_lcd);
}
