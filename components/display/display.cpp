#include "display.h"
#include "esp_log.h"

static const char* TAG = "display";

bool display_init(uint32_t spi_hz) {
    ESP_LOGW(TAG, "display_init stub spi_hz=%u", (unsigned)spi_hz);
    return false;
}

int display_width(void)  { return 320; }
int display_height(void) { return 240; }

void display_blit_full(const uint16_t* /*buf*/) {}
void display_wait_dma(void) {}

uint16_t* display_alloc_framebuffer_psram(void) { return nullptr; }
void display_free_framebuffer(uint16_t* /*fb*/) {}
void* display_get_lgfx_ptr(void) { return nullptr; }
