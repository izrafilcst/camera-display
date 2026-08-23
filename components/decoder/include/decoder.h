#pragma once
#include <cstdint>
#include <cstddef>

/**
 * Initialise the esp_jpeg decoder (single shared handle, not thread-safe).
 * Idempotent: a second call with a live handle is a no-op and returns true.
 * To recover from a corrupted handle (post-crash), call decoder_deinit()
 * first to release the broken state, then re-init.
 * @return true on success.
 */
bool decoder_init(void);

/**
 * Decode a JPEG buffer to big-endian RGB565 in out_buf.
 * Validates SOI marker (0xFF 0xD8) before invoking esp_jpeg.
 * @param jpeg        JPEG data buffer.
 * @param jpeg_len    Byte count of JPEG data.
 * @param out_buf     Output buffer; width*height*2 bytes, 4-byte aligned.
 *                    Pixels are written BIG-ENDIAN (the byte order the
 *                    ILI9341 expects) — see rgb565be_t in display.h. The
 *                    parameter stays uint16_t* on purpose: this component
 *                    does not depend on `display` and the typedef would add
 *                    coupling without adding type safety.
 * @param expected_w  Expected image width in pixels.
 * @param expected_h  Expected image height in pixels.
 * @return Decode duration in microseconds, or -1 on error.
 */
int64_t decoder_decode_to_rgb565(const uint8_t* jpeg, size_t jpeg_len,
                                  uint16_t* out_buf,
                                  int expected_w, int expected_h);

/**
 * Free resources held by the decoder handle.
 */
void decoder_deinit(void);
