#include "display.h"
#include <cstdint>

static const int W = 320;
static const int H = 240;

/**
 * Pack 8-bit R/G/B into RGB565 and return it byte-swapped, so the uint16_t
 * sits in memory as big-endian — the order the ILI9341 reads off the wire.
 * Logical field layout before the swap: bits[15..11]=R5, [10..5]=G6, [4..0]=B5.
 * Non-static so the host test TU can link against it directly.
 */
rgb565be_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t logical = static_cast<uint16_t>(
        ((r & 0xF8u) << 8) |
        ((g & 0xFCu) << 3) |
        (b >> 3)
    );
    return static_cast<rgb565be_t>((logical >> 8) | (logical << 8));
}

/**
 * Fill buf with 8 vertical color bars (SMPTE-style):
 * White | Yellow | Cyan | Green | Magenta | Red | Blue | Black
 * Each bar is W/8 = 40 pixels wide.
 */
void pattern_color_bars(rgb565be_t* buf) {
    const rgb565be_t colors[8] = {
        rgb565(255, 255, 255),  // 0: white
        rgb565(255, 255,   0),  // 1: yellow
        rgb565(  0, 255, 255),  // 2: cyan
        rgb565(  0, 255,   0),  // 3: green
        rgb565(255,   0, 255),  // 4: magenta
        rgb565(255,   0,   0),  // 5: red
        rgb565(  0,   0, 255),  // 6: blue
        rgb565(  0,   0,   0),  // 7: black
    };
    const int bar_w = W / 8;  // 40 px per bar
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int idx = x / bar_w;
            if (idx > 7) idx = 7;
            buf[y * W + x] = colors[idx];
        }
    }
}

/**
 * Fill buf with an RGB gradient:
 *   R increases left-to-right  (0 at x=0, 255 at x=W-1)
 *   G increases top-to-bottom  (0 at y=0, 255 at y=H-1)
 *   B = 255 - R                (decreases left-to-right)
 * Corner colours: TL=blue, TR=yellow, BL=magenta, BR=green.
 */
void pattern_gradient(rgb565be_t* buf) {
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t r = static_cast<uint8_t>((x * 255) / (W - 1));
            uint8_t g = static_cast<uint8_t>((y * 255) / (H - 1));
            uint8_t b = static_cast<uint8_t>(255 - r);
            buf[y * W + x] = rgb565(r, g, b);
        }
    }
}

/**
 * Fill buf with a black-and-white checkerboard.
 * Cell (cx, cy) is white when (cx+cy) is even, black when odd.
 * cell_px is clamped to >= 1 to prevent divide-by-zero.
 */
void pattern_checker(rgb565be_t* buf, int cell_px) {
    if (cell_px < 1) cell_px = 1;
    const rgb565be_t white = rgb565(255, 255, 255);
    const rgb565be_t black = rgb565(  0,   0,   0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool on = (((x / cell_px) + (y / cell_px)) & 1) != 0;
            buf[y * W + x] = on ? black : white;
        }
    }
}

/**
 * Fill buf with horizontally-scrolling tearing-test stripes.
 * Alternating 16-pixel white/black horizontal bands shift 8 px per frame.
 * At 24 fps this produces 0.33 s per full cycle — visible tearing if any.
 */
void pattern_tearing_stripes(rgb565be_t* buf, uint32_t frame_counter) {
    const int shift = static_cast<int>((frame_counter * 8u) % static_cast<uint32_t>(H));
    const rgb565be_t white = rgb565(255, 255, 255);
    const rgb565be_t black = rgb565(  0,   0,   0);
    for (int y = 0; y < H; ++y) {
        const bool on = (((y + shift) / 16) & 1) != 0;
        const rgb565be_t c = on ? white : black;
        for (int x = 0; x < W; ++x) {
            buf[y * W + x] = c;
        }
    }
}
