#include "display.h"
#include "lgfx_ili9341_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

static const char* TAG = "display";
static LGFX_ILI9341_Red* s_lcd = nullptr;

bool display_init(uint32_t spi_hz) {
    if (s_lcd != nullptr) {
        // Already initialized — safe to return true without re-init.
        return true;
    }

    // Neutralize SD and XPT2046 CS lines before SPI init to prevent
    // those chips from responding during ILI9341 init sequence.
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_SD_CS,    GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_TOUCH_CS, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_SD_CS,    1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_TOUCH_CS, 1));

    s_lcd = new LGFX_ILI9341_Red(spi_hz);
    if (!s_lcd->init()) {
        ESP_LOGE(TAG, "lcd init failed @ %u Hz", (unsigned)spi_hz);
        delete s_lcd;
        s_lcd = nullptr;
        return false;
    }

    s_lcd->setRotation(1);       // landscape 320x240
    s_lcd->setBrightness(200);   // 0..255
    s_lcd->fillScreen(0x0000);   // black

    ESP_LOGI(TAG, "lcd ok %dx%d @ %u Hz",
             s_lcd->width(), s_lcd->height(), (unsigned)spi_hz);
    return true;
}

int display_width(void)  { return s_lcd ? s_lcd->width()  : 320; }
int display_height(void) { return s_lcd ? s_lcd->height() : 240; }

void display_blit_full(const uint16_t* buf) {
    if (!s_lcd || !buf) return;
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, s_lcd->width(), s_lcd->height());
    // swap=true: convert little-endian RGB565 to big-endian for ILI9341.
    s_lcd->writePixels(buf, s_lcd->width() * s_lcd->height(), true);
    s_lcd->endWrite();
}

void display_wait_dma(void) {
    if (!s_lcd) return;
    s_lcd->waitDMA();
}

uint16_t* display_alloc_framebuffer_psram(void) {
    const size_t bytes = 320u * 240u * sizeof(uint16_t);
    // Try DMA-capable PSRAM first (preferred path on ESP32-S3).
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!p) {
        // Fallback: PSRAM without explicit DMA flag.
        // LovyanGFX on S3+IDF5.2 can DMA from PSRAM regardless of this flag,
        // but log a warning for visibility.
        ESP_LOGW(TAG, "psram DMA alloc failed, retrying without DMA cap");
        p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    return static_cast<uint16_t*>(p);
}

void display_free_framebuffer(uint16_t* fb) {
    if (fb) heap_caps_free(fb);
}

void* display_get_lgfx_ptr(void) {
    return static_cast<void*>(s_lcd);
}
