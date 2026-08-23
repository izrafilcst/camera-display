#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RGB565 armazenado em big-endian — a ordem de bytes que o ILI9341 le do fio.
 *
 * E o mesmo `uint16_t` de sempre: o nome carrega o contrato, nao um tipo
 * distinto, entao o compilador NAO pega uma troca acidental. O enforcement de
 * verdade e a fronteira desta API — nenhum modulo fora de display.cpp empurra
 * pixel para o painel; quem precisa desenhar chama display_blit_full ou
 * display_blit_rect.
 *
 * Valor logico: bits[15..11]=R5, [10..5]=G6, [4..0]=B5, byte-swapped.
 * Ex.: vermelho puro = 0x00F8 (e nao 0xF800).
 */
typedef uint16_t rgb565be_t;

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
 * Envia um framebuffer big-endian RGB565 completo (320x240) via DMA SPI.
 *
 * Fatiado em blocos de 48 linhas (30720 bytes) porque acima de ~32 KB o
 * LovyanGFX quebra a transferencia sozinho e insere vTaskDelay(1) entre os
 * pedacos — ~4 ms de tick desperdicados por frame com FREERTOS_HZ=1000.
 *
 * @param fb  Buffer de 320*240*2 bytes em big-endian, alinhado a 4 bytes.
 * @note  Retorno NAO significa blit completo: a ultima fatia fica em voo.
 *        Chamar display_wait_dma() antes de reutilizar o buffer.
 * @note  fb == nullptr: no-op.
 */
void display_blit_full(const rgb565be_t* fb);

/**
 * Envia um retangulo de pixels big-endian para uma regiao da tela.
 *
 * Usado pelos overlays (badge FREEZE, thumbnail). Existe para que nenhum
 * modulo fora de display.cpp precise falar com o LovyanGFX sobre pixels —
 * o argumento `swap` do writePixels/pushImage vive so aqui.
 *
 * Aninha com seguranca dentro de um startWrite()/endWrite() que o chamador
 * ja tenha aberto (LovyanGFX conta as chamadas por referencia).
 *
 * @param src         Origem em big-endian RGB565.
 * @param src_stride  Largura de uma linha de `src` em pixels.
 * @param x,y         Canto superior esquerdo no painel.
 * @param w,h         Dimensoes do retangulo em pixels.
 * @note  BLOQUEIA ate o DMA terminar, ao contrario de display_blit_full —
 *        os chamadores emendam outros desenhos logo depois.
 * @note  src == nullptr, w <= 0 ou h <= 0: no-op.
 */
void display_blit_rect(const rgb565be_t* src, int src_stride,
                       int x, int y, int w, int h);

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
