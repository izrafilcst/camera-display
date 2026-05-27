#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Display lifecycle
// ---------------------------------------------------------------------------

/**
 * Inicializa o barramento SPI, configura o ILI9341 e liga o backlight.
 * @param spi_hz  Frequencia SPI em Hz (tipico: 40000000 ou 80000000).
 * @return true se OK; false em falha (log ESP_LOGE com detalhe).
 * @note  Nao e thread-safe. Chamar uma unica vez antes de criar tasks.
 */
bool display_init(uint32_t spi_hz);

/** Largura efetiva apos rotacao (320 em landscape). */
int display_width(void);

/** Altura efetiva apos rotacao (240 em landscape). */
int display_height(void);

// ---------------------------------------------------------------------------
// Blit / DMA
// ---------------------------------------------------------------------------

/**
 * Envia um framebuffer RGB565 completo (320x240) via DMA SPI.
 * @param rgb565_buf  Buffer de 320*240*2 bytes, alinhado a 4 bytes.
 * @note  Retorno NAO significa blit completo — chamar display_wait_dma().
 * @note  buf == nullptr: no-op.
 */
void display_blit_full(const uint16_t* rgb565_buf);

/**
 * Bloqueia ate o DMA do ultimo blit terminar.
 * Seguro chamar sem blit pendente (retorna imediatamente).
 */
void display_wait_dma(void);

// ---------------------------------------------------------------------------
// PSRAM allocator
// ---------------------------------------------------------------------------

/**
 * Aloca um framebuffer 320*240*2 = 153600 bytes na PSRAM (DMA-capable).
 * @return Ponteiro alinhado a 4 bytes, ou nullptr se falhar.
 */
uint16_t* display_alloc_framebuffer_psram(void);

/**
 * Libera framebuffer alocado por display_alloc_framebuffer_psram.
 * fb == nullptr e no-op.
 */
void display_free_framebuffer(uint16_t* fb);

/**
 * Retorna ponteiro ao objeto LGFX_Device interno (uso em Sprint 3+).
 * @return nullptr se display_init() nao foi chamado ou falhou.
 * @note  Retorno como void* para manter display.h puro C sem headers C++.
 */
void* display_get_lgfx_ptr(void);

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// Test patterns — preenchem um buffer 320x240 RGB565.
// Declaradas fora do bloco extern "C": implementadas em C++ puro
// (test_patterns.cpp e compilado como C++; o linker espera C++ linkage).
// ---------------------------------------------------------------------------
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
void pattern_color_bars(uint16_t* buf);
void pattern_gradient(uint16_t* buf);
void pattern_checker(uint16_t* buf, int cell_px);
void pattern_tearing_stripes(uint16_t* buf, uint32_t frame_counter);
