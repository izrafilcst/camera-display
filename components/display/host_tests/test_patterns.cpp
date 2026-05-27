// RED tests for components/display/test_patterns.cpp
// Companion implementation (../test_patterns.cpp) does NOT exist yet.
// Build will fail with undefined-reference errors — that is the intended RED state.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Forward-declarations of the functions under test (defined in ../test_patterns.cpp)
// rgb565 is a non-static helper in the implementation TU so the linker can find it.
// pattern_* are plain C++ (no extern "C") since test_patterns.cpp is a .cpp file.
// ---------------------------------------------------------------------------
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

void pattern_color_bars(uint16_t* buf);
void pattern_gradient(uint16_t* buf);
void pattern_checker(uint16_t* buf, int cell_px);

// ---------------------------------------------------------------------------
// Assertion macros
// ---------------------------------------------------------------------------
#define ASSERT_EQ_U16(got, expected, msg)                                     \
    do {                                                                       \
        uint16_t _g = (uint16_t)(got);                                        \
        uint16_t _e = (uint16_t)(expected);                                   \
        if (_g != _e) {                                                        \
            fprintf(stderr,                                                    \
                    "%s:%d FAIL [%s]: got=0x%04X expected=0x%04X\n",          \
                    __FILE__, __LINE__, (msg), _g, _e);                        \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define ASSERT_NEQ_U16(a, b, msg)                                              \
    do {                                                                       \
        uint16_t _a = (uint16_t)(a);                                           \
        uint16_t _b = (uint16_t)(b);                                           \
        if (_a == _b) {                                                        \
            fprintf(stderr,                                                    \
                    "%s:%d FAIL [%s]: values equal 0x%04X (expected different)\n", \
                    __FILE__, __LINE__, (msg), _a);                            \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define ASSERT_EQ_INT(got, expected, msg)                                      \
    do {                                                                       \
        int _g = (int)(got);                                                   \
        int _e = (int)(expected);                                              \
        if (_g != _e) {                                                        \
            fprintf(stderr,                                                    \
                    "%s:%d FAIL [%s]: got=%d expected=%d\n",                   \
                    __FILE__, __LINE__, (msg), _g, _e);                        \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

// Tolerance check: |got_channel - expected_channel| <= tol
#define ASSERT_CHANNEL_NEAR(got_raw, shift, mask, expected_val, tol, msg)     \
    do {                                                                       \
        int _got = (int)(((got_raw) >> (shift)) & (mask));                    \
        int _exp = (int)(expected_val);                                        \
        int _diff = _got - _exp; if (_diff < 0) _diff = -_diff;               \
        if (_diff > (tol)) {                                                   \
            fprintf(stderr,                                                    \
                    "%s:%d FAIL [%s]: channel got=%d expected=%d tol=%d\n",   \
                    __FILE__, __LINE__, (msg), _got, _exp, (tol));             \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Display constants
// ---------------------------------------------------------------------------
static const int W = 320;
static const int H = 240;

static inline uint16_t pixel(const uint16_t* buf, int x, int y) {
    return buf[y * W + x];
}

// ---------------------------------------------------------------------------
// Shared buffer (static, avoids VLA / stack overflow risk for 153.6 KB)
// ---------------------------------------------------------------------------
static uint16_t g_buf[320 * 240];

// ---------------------------------------------------------------------------
// Test counter
// ---------------------------------------------------------------------------
static int g_passed = 0;
#define TEST_PASS(name) do { fprintf(stdout, "  PASS: %s\n", (name)); g_passed++; } while(0)

// ===========================================================================
// rgb565 — helper conversion tests
// ===========================================================================

static void test_rgb565_red_pure() {
    // should encode red pure (255,0,0) as 0xF800
    uint16_t got = rgb565(255, 0, 0);
    ASSERT_EQ_U16(got, 0xF800, "rgb565 red pure");
    TEST_PASS("should encode red pure (255,0,0) as 0xF800");
}

static void test_rgb565_green_pure() {
    // should encode green pure (0,255,0) as 0x07E0
    uint16_t got = rgb565(0, 255, 0);
    ASSERT_EQ_U16(got, 0x07E0, "rgb565 green pure");
    TEST_PASS("should encode green pure (0,255,0) as 0x07E0");
}

static void test_rgb565_blue_pure() {
    // should encode blue pure (0,0,255) as 0x001F
    uint16_t got = rgb565(0, 0, 255);
    ASSERT_EQ_U16(got, 0x001F, "rgb565 blue pure");
    TEST_PASS("should encode blue pure (0,0,255) as 0x001F");
}

static void test_rgb565_white() {
    // should encode white (255,255,255) as 0xFFFF
    uint16_t got = rgb565(255, 255, 255);
    ASSERT_EQ_U16(got, 0xFFFF, "rgb565 white");
    TEST_PASS("should encode white (255,255,255) as 0xFFFF");
}

static void test_rgb565_black() {
    // should encode black (0,0,0) as 0x0000
    uint16_t got = rgb565(0, 0, 0);
    ASSERT_EQ_U16(got, 0x0000, "rgb565 black");
    TEST_PASS("should encode black (0,0,0) as 0x0000");
}

static void test_rgb565_mid_gray() {
    // should place R channel in bits 15..11 for gray (128,128,128)
    uint16_t got = rgb565(128, 128, 128);
    // R5 = 128>>3 = 16, G6 = 128>>2 = 32, B5 = 128>>3 = 16
    // expected = (16<<11)|(32<<5)|16 = 0x8410
    uint16_t expected = (uint16_t)(((128 & 0xF8) << 8) | ((128 & 0xFC) << 3) | (128 >> 3));
    ASSERT_EQ_U16(got, expected, "rgb565 mid gray exact");
    TEST_PASS("should encode gray (128,128,128) with R in bits 15..11, G in 10..5, B in 4..0");
}

// ===========================================================================
// pattern_color_bars
// ===========================================================================

static void test_color_bars_pixel_0_0_white() {
    // should set pixel(0,0) to white (bar 0)
    pattern_color_bars(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 0, 0), 0xFFFF, "color_bars pixel(0,0) white");
    TEST_PASS("should set pixel(0,0) to white when pattern_color_bars fills bar 0");
}

static void test_color_bars_pixel_40_0_yellow() {
    // should set pixel(40,0) to yellow 0xFFE0 (bar 1)
    pattern_color_bars(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 40, 0), 0xFFE0, "color_bars pixel(40,0) yellow");
    TEST_PASS("should set pixel(40,0) to yellow (0xFFE0) when pattern_color_bars fills bar 1");
}

static void test_color_bars_pixel_280_100_black() {
    // should set pixel(280,100) to black (bar 7)
    pattern_color_bars(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 280, 100), 0x0000, "color_bars pixel(280,100) black");
    TEST_PASS("should set pixel(280,100) to black (0x0000) when pattern_color_bars fills bar 7");
}

static void test_color_bars_same_y_invariant() {
    // should produce same value for pixel(39,50) and pixel(39,200) (same bar, different y)
    pattern_color_bars(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 39, 50), pixel(g_buf, 39, 200),
                  "color_bars vertical invariant same bar");
    TEST_PASS("should set pixel(39,50) equal to pixel(39,200) when same bar regardless of y");
}

static void test_color_bars_adjacent_bars_differ() {
    // should produce different values for pixel(0,0) and pixel(40,0) (different bars)
    pattern_color_bars(g_buf);
    ASSERT_NEQ_U16(pixel(g_buf, 0, 0), pixel(g_buf, 40, 0),
                   "color_bars adjacent bars differ");
    TEST_PASS("should set pixel(0,0) != pixel(40,0) when bars are different");
}

// ===========================================================================
// pattern_gradient
// ===========================================================================

static void test_gradient_pixel_0_0_blue() {
    // should set pixel(0,0) to blue 0x001F: R=0,G=0,B=255
    pattern_gradient(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 0, 0), 0x001F, "gradient pixel(0,0) blue");
    TEST_PASS("should set pixel(0,0) to 0x001F (R=0,G=0,B=255) when gradient starts at origin");
}

static void test_gradient_pixel_319_239_yellow() {
    // should set pixel(319,239) to 0xFFE0: R=255,G=255,B=0
    pattern_gradient(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 319, 239), 0xFFE0, "gradient pixel(319,239) yellow");
    TEST_PASS("should set pixel(319,239) to 0xFFE0 (R=255,G=255,B=0) when gradient ends at far corner");
}

static void test_gradient_mid_x_r_channel_near_127() {
    // should set R channel of pixel(160,0) near 127 (tolerance ±1 in 5-bit R)
    // R = (160*255)/(319) ≈ 127.8 → 127; R5 = 127>>3 = 15
    // tolerance: ±1 in the 5-bit field
    pattern_gradient(g_buf);
    uint16_t p = pixel(g_buf, 160, 0);
    ASSERT_CHANNEL_NEAR(p, 11, 0x1F, 15, 1, "gradient mid x R5 near 15");
    TEST_PASS("should set R5 field of pixel(160,0) near 15 with tolerance ±1 when x=160");
}

static void test_gradient_mid_x_g_channel_zero_at_y0() {
    // should set G channel of pixel(160,0) to 0 when y=0
    pattern_gradient(g_buf);
    uint16_t p = pixel(g_buf, 160, 0);
    ASSERT_CHANNEL_NEAR(p, 5, 0x3F, 0, 0, "gradient y=0 G6 is 0");
    TEST_PASS("should set G6 field of pixel(160,0) to 0 when y=0");
}

static void test_gradient_r_channel_max_at_x319() {
    // should set R5 field to 31 (R=255) at x=319
    pattern_gradient(g_buf);
    uint16_t p = pixel(g_buf, 319, 0);
    ASSERT_CHANNEL_NEAR(p, 11, 0x1F, 31, 0, "gradient R5=31 at x=319");
    TEST_PASS("should set R5 to 31 (no overflow) at x=319");
}

// ===========================================================================
// pattern_checker
// ===========================================================================

static void test_checker_20_0_0_white() {
    // should set pixel(0,0) to white when cell_px=20 (cell (0,0) is white)
    pattern_checker(g_buf, 20);
    ASSERT_EQ_U16(pixel(g_buf, 0, 0), 0xFFFF, "checker(20) pixel(0,0) white");
    TEST_PASS("should set pixel(0,0) to white when pattern_checker(20) fills first cell");
}

static void test_checker_20_pixel_20_0_black() {
    // should set pixel(20,0) to black when cell_px=20 (cell (1,0) is black)
    pattern_checker(g_buf, 20);
    ASSERT_EQ_U16(pixel(g_buf, 20, 0), 0x0000, "checker(20) pixel(20,0) black");
    TEST_PASS("should set pixel(20,0) to black when pattern_checker(20) fills second cell");
}

static void test_checker_20_pixel_40_0_white() {
    // should set pixel(40,0) to white when cell_px=20 (cell (2,0) is white)
    pattern_checker(g_buf, 20);
    ASSERT_EQ_U16(pixel(g_buf, 40, 0), 0xFFFF, "checker(20) pixel(40,0) white");
    TEST_PASS("should set pixel(40,0) to white when pattern_checker(20) third cell wraps back to white");
}

static void test_checker_20_pixel_0_20_black() {
    // should set pixel(0,20) to black when cell_px=20 (cell (0,1) is black)
    pattern_checker(g_buf, 20);
    ASSERT_EQ_U16(pixel(g_buf, 0, 20), 0x0000, "checker(20) pixel(0,20) black");
    TEST_PASS("should set pixel(0,20) to black when pattern_checker(20) second row first cell is black");
}

static void test_checker_1_adjacent_pixels_differ() {
    // should alternate pixel to pixel when cell_px=1
    pattern_checker(g_buf, 1);
    ASSERT_NEQ_U16(pixel(g_buf, 0, 0), pixel(g_buf, 1, 0),
                   "checker(1) adjacent pixels differ");
    TEST_PASS("should alternate pixel(0,0) and pixel(1,0) when pattern_checker(1) uses 1px cells");
}

static void test_checker_0_does_not_crash() {
    // should not crash (clamp to 1) when cell_px=0
    // If it crashes, exit(1) via signal — test runner will detect.
    // We also verify it produces valid alternating output (clamp to 1 behaviour).
    pattern_checker(g_buf, 0);
    // Clamp to 1: pixel(0,0) and pixel(1,0) must differ
    ASSERT_NEQ_U16(pixel(g_buf, 0, 0), pixel(g_buf, 1, 0),
                   "checker(0) clamps to 1, adjacent pixels differ");
    TEST_PASS("should not crash and clamp to 1-px cells when cell_px=0");
}

// ===========================================================================
// main
// ===========================================================================

int main() {
    fprintf(stdout, "\n=== rgb565 ===\n");
    test_rgb565_red_pure();
    test_rgb565_green_pure();
    test_rgb565_blue_pure();
    test_rgb565_white();
    test_rgb565_black();
    test_rgb565_mid_gray();

    fprintf(stdout, "\n=== pattern_color_bars ===\n");
    test_color_bars_pixel_0_0_white();
    test_color_bars_pixel_40_0_yellow();
    test_color_bars_pixel_280_100_black();
    test_color_bars_same_y_invariant();
    test_color_bars_adjacent_bars_differ();

    fprintf(stdout, "\n=== pattern_gradient ===\n");
    test_gradient_pixel_0_0_blue();
    test_gradient_pixel_319_239_yellow();
    test_gradient_mid_x_r_channel_near_127();
    test_gradient_mid_x_g_channel_zero_at_y0();
    test_gradient_r_channel_max_at_x319();

    fprintf(stdout, "\n=== pattern_checker ===\n");
    test_checker_20_0_0_white();
    test_checker_20_pixel_20_0_black();
    test_checker_20_pixel_40_0_white();
    test_checker_20_pixel_0_20_black();
    test_checker_1_adjacent_pixels_differ();
    test_checker_0_does_not_crash();

    fprintf(stdout, "\nAll %d tests passed.\n", g_passed);
    return 0;
}
