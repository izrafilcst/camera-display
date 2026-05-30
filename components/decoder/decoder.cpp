// Sprint 2 — decoder component (esp_jpeg ^1.1.0 wrapper)
//
// The Sprint 2 coder targeted an older `jpeg_dec_open/process` API that
// lives in esp-adf, not in the Espressif Component Registry's esp_jpeg.
// The real esp_jpeg 1.1.x API is one-shot (`esp_jpeg_decode`) — no
// persistent handle, no parse_header step. Rewritten here against the
// actual header `jpeg_decoder.h`.
//
// Validates SOI marker before invoking the decoder to guard against
// corrupted frames from RF interference (spec gotcha 6.3).

#include "decoder.h"
#include "jpeg_decoder.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "decoder";

bool decoder_init(void) {
    // esp_jpeg 1.1.x is stateless; nothing to allocate up front.
    return true;
}

void decoder_deinit(void) {
    // No persistent state to release.
}

int64_t decoder_decode_to_rgb565(const uint8_t* jpeg, size_t jpeg_len,
                                  uint16_t* out_buf,
                                  int expected_w, int expected_h) {
    // Validate SOI marker — cheap pre-filter against RF garbage.
    if (jpeg_len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "invalid JPEG SOI marker (0x%02X 0x%02X)",
                 jpeg_len > 0 ? jpeg[0] : 0,
                 jpeg_len > 1 ? jpeg[1] : 0);
        return -1;
    }

    int64_t t0 = esp_timer_get_time();

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata       = const_cast<uint8_t*>(jpeg);
    cfg.indata_size  = static_cast<uint32_t>(jpeg_len);
    cfg.outbuf       = reinterpret_cast<uint8_t*>(out_buf);
    cfg.outbuf_size  = static_cast<uint32_t>(expected_w * expected_h * 2);  // RGB565
    cfg.out_format   = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale    = JPEG_IMAGE_SCALE_0;
    // swap_color_bytes=0: leave RGB565 in native (little-endian) order in
    // PSRAM. display_blit_full / writePixels(swap=true) then swaps each
    // 16-bit pixel on its way to the ILI9341, which expects big-endian.
    // (Sprint 2 review F-03 keeps an eye on this — verify empirically.)
    cfg.flags.swap_color_bytes = 0;

    esp_jpeg_image_output_t img = {};
    esp_err_t r = esp_jpeg_decode(&cfg, &img);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "esp_jpeg_decode failed: %s", esp_err_to_name(r));
        return -1;
    }

    if (static_cast<int>(img.width)  != expected_w ||
        static_cast<int>(img.height) != expected_h) {
        ESP_LOGW(TAG, "JPEG size mismatch: got %ux%u expected %dx%d",
                 (unsigned)img.width, (unsigned)img.height,
                 expected_w, expected_h);
        return -1;
    }

    return esp_timer_get_time() - t0;
}
