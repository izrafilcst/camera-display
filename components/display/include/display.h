#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Display lifecycle
// ---------------------------------------------------------------------------

/**
 * Inicializa o barramento SPI, reseta o ILI9341 e liga o backlight.
 *
 * O reset e feito manualmente com a temporizacao do datasheet (p.230 s15.4:
 * tRW >= 10 us, e Sleep Out so 120 ms apos soltar RESX) e o LovyanGFX e
 * inicializado via init_without_reset() para nao pulsar o RST de novo com a
 * temporizacao dele (64 ms, insuficiente).
 *
 * @param spi_hz  Frequencia SPI em Hz. Use LCD_SPI_HZ (40 MHz).
 * @return true se OK; false em falha de alocacao.
 * @note  NAO detecta painel ausente: com cfg.readable == false o LovyanGFX
 *        nunca le o painel, entao o init sempre reporta sucesso.
 * @note  Nao e thread-safe. Chamar uma unica vez antes de criar tasks.
 * @note  Reentrante: a segunda chamada e no-op e retorna true, avisando por
 *        log se a frequencia pedida for diferente da que ja esta em uso.
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
 * Aloca um framebuffer 320*240*2 = 153600 bytes na PSRAM.
 *
 * NAO pede MALLOC_CAP_DMA: no ESP32-S3 o heap de PSRAM e registrado sem esse
 * cap, entao SPIRAM|DMA retorna nullptr sempre. O GDMA alcanca a PSRAM pela
 * cache de qualquer forma.
 * Requer CONFIG_SPIRAM=y e CONFIG_SPIRAM_MODE_OCT=y (ESP32-S3-N16R8).
 *
 * @return Ponteiro alinhado a 4 bytes (requisito GDMA), ou nullptr se a PSRAM
 *         estiver indisponivel ou sem memoria. O alinhamento e conferido por
 *         assert() na implementacao.
 */
uint16_t* display_alloc_framebuffer_psram(void);

/**
 * Libera framebuffer alocado por display_alloc_framebuffer_psram.
 * @param fb  nullptr e no-op; comportamento seguro.
 * @pre   display_wait_dma() deve ter sido chamado se fb estava em DMA ativo.
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

// rgb565() and pattern_* are intentionally NOT exposed here. They are
// implementation helpers used by test_patterns.cpp (and the host test, which
// forward-declares them locally). Including them in the public C interface
// would create extern "C" linkage mismatches and leak internals.
