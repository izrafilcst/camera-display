// Sprint 2 — decoder component (esp_jpeg wrapper)
// Validates SOI marker before invoking the decoder to guard against
// corrupted frames from RF interference (see spec gotcha 6.3).

#include "decoder.h"
#include "esp_jpeg_dec.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "decoder";
static jpeg_dec_handle_t s_handle = nullptr;

bool decoder_init(void) {
    if (s_handle) return true;  // already initialised
    jpeg_dec_config_t cfg = {};
    cfg.output_type  = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.rotate       = JPEG_ROTATE_0D;
    cfg.block_enable = false;
    // scale and clipper left at {0,0} = no scaling/clipping

    if (jpeg_dec_open(&cfg, &s_handle) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed");
        s_handle = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "decoder ready");
    return true;
}

void decoder_deinit(void) {
    if (s_handle) {
        jpeg_dec_close(s_handle);
        s_handle = nullptr;
    }
}

int64_t decoder_decode_to_rgb565(const uint8_t* jpeg, size_t jpeg_len,
                                  uint16_t* out_buf,
                                  int expected_w, int expected_h) {
    if (!s_handle) return -1;

    // Validate SOI marker — guards against garbage frames from RF noise
    if (jpeg_len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "invalid JPEG SOI marker (0x%02X 0x%02X)",
                 jpeg_len > 0 ? jpeg[0] : 0,
                 jpeg_len > 1 ? jpeg[1] : 0);
        return -1;
    }

    int64_t t0 = esp_timer_get_time();

    jpeg_dec_io_t io = {};
    io.inbuf      = const_cast<uint8_t*>(jpeg);
    io.inbuf_len  = static_cast<int>(jpeg_len);
    io.outbuf     = reinterpret_cast<uint8_t*>(out_buf);
    io.outbuf_len = 0;  // filled by parse_header

    jpeg_dec_header_info_t hdr = {};
    if (jpeg_dec_parse_header(s_handle, &io, &hdr) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_parse_header failed");
        return -1;
    }

    if (static_cast<int>(hdr.width)  != expected_w ||
        static_cast<int>(hdr.height) != expected_h) {
        ESP_LOGW(TAG, "JPEG size mismatch: got %dx%d expected %dx%d",
                 hdr.width, hdr.height, expected_w, expected_h);
        return -1;
    }

    if (jpeg_dec_process(s_handle, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_process failed");
        return -1;
    }

    int64_t t1 = esp_timer_get_time();
    return t1 - t0;
}
