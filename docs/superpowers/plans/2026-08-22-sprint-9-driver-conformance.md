# Sprint 9 — Conformidade do driver ILI9341 + desbloqueio de boot — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Alinhar o driver de display com o datasheet ILI9341 V1.11, trocar o blit por DMA assíncrono de verdade, e destravar o boot da placa.

**Architecture:** O `components/display` é um wrapper sobre o LovyanGFX 1.2.21. As correções ficam em cinco waves: três offline (build + host tests) e duas de bancada. A mudança estrutural é o framebuffer passar a viver em **big-endian**, o que elimina 76 800 conversões de CPU por frame e faz `writePixelsDMA` render DMA real — e, junto com isso, todo push de pixel passa a ficar atrás do `display.h`.

**Tech Stack:** ESP-IDF ≥5.2 (PlatformIO), LovyanGFX 1.2.21, esp_jpeg, ESP32-S3-N16R8, host tests em g++/make.

**Spec:** [`../specs/2026-08-22-ili9341-driver-conformance-design.md`](../specs/2026-08-22-ili9341-driver-conformance-design.md)

## Global Constraints

- Branch: `sprint-9-driver-conformance`. Commits convencionais. Tag final `s9-done`.
- `LCD_SPI_HZ` = **40000000** exato. Não reintroduzir 80 MHz nem um `_SAFE`.
- Framebuffer: 320×240 RGB565 **big-endian**, 153 600 bytes, alinhado a 4 bytes.
- Fatia de blit: **48 linhas** = 30 720 bytes. Não aumentar sem medir.
- Build: `pio run` — zero warnings (o projeto usa `-Werror` nos host tests e `CONFIG_COMPILER_WARN_WRITE_STRINGS=y`).
- Host tests: `cd components/display/host_tests && make test` — todos passando.
- **Nada das waves 1–3 é observável em hardware até a wave 4 destravar o boot.** Não relatar "display funcionando" com base em host tests verdes.
- Não regredir Sprint 4 (pair_nvs, ble_pair) nem o wire format de `ef75431` — ver `docs/HANDOFF-rx-multicore-freeze.md` §8.

---

# Wave 1 — Correções isoladas

Cada task aqui é independente e mergeável sozinha. Gate: `pio run` limpo.

---

### Task 1: Clock SPI para 40 MHz (§A)

**Files:**
- Modify: `main/pinout.h:24-26`
- Modify: `main/app_main.cpp:153`, `main/app_main.cpp:246`
- Modify: `components/display/include/lgfx_ili9341_config.h:30`

**Interfaces:**
- Consumes: nada
- Produces: macro `LCD_SPI_HZ` (substitui `LCD_SPI_HZ_TARGET` e `LCD_SPI_HZ_SAFE`, ambos removidos)

Sem teste automatizado: é constante de configuração de hardware. Gate é build + inspeção.

- [ ] **Step 1: Substituir as duas constantes por uma em `main/pinout.h`**

Trocar o bloco atual:

```c
// SPI clock targets
#define LCD_SPI_HZ_TARGET   80000000   // 80 MHz — testar estabilidade no hw real
#define LCD_SPI_HZ_SAFE     40000000   // 40 MHz — baseline garantido
```

por:

```c
// SPI clock. 40 MHz e 4x acima dos 10 MHz que o datasheet ILI9341 V1.11
// especifica (p.243 secao 18.3.4: twc min 100 ns). Desvio consciente: e o
// valor que os modulos ILI9341 toleram na pratica, e a 40 MHz um frame cheio
// ocupa 30,7 ms de fio (teto ~32 fps) contra a meta V0 de 24 fps.
// LCD_SPI_HZ_SAFE foi removido: era codigo morto, nenhum call site o usava.
// Ver docs/superpowers/specs/2026-08-22-ili9341-driver-conformance-design.md
#define LCD_SPI_HZ          40000000
```

- [ ] **Step 2: Atualizar os dois call sites em `main/app_main.cpp`**

Nas linhas 153 e 246, trocar `display_init(LCD_SPI_HZ_TARGET)` por `display_init(LCD_SPI_HZ)`.

- [ ] **Step 3: Baixar `freq_read` em `components/display/include/lgfx_ili9341_config.h`**

Trocar:

```cpp
            cfg.freq_read   = 16000000;
```

por:

```cpp
            // 6 MHz: o datasheet da trc min 150 ns para leitura (p.243 secao
            // 18.3.4), ou seja 6,67 MHz de teto. Hoje readable == false e
            // nada le, mas deixar 16 MHz aqui e uma armadilha armada para
            // quem ligar leitura amanha.
            cfg.freq_read   = 6000000;
```

- [ ] **Step 4: Confirmar que nenhuma referência órfã sobrou**

Run: `grep -rn "LCD_SPI_HZ_TARGET\|LCD_SPI_HZ_SAFE" main components`
Expected: nenhuma saída.

- [ ] **Step 5: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 6: Commit**

```bash
git add main/pinout.h main/app_main.cpp components/display/include/lgfx_ili9341_config.h
git commit -m "fix(display): SPI 80->40 MHz e freq_read 16->6 MHz (datasheet p.243)"
```

---

### Task 2: Alocação de framebuffer sem `MALLOC_CAP_DMA` (§B)

**Files:**
- Modify: `components/display/display.cpp:1-9` (includes), `:66-78` (`display_alloc_framebuffer_psram`)
- Modify: `components/display/include/display.h:48-59` (docstring)

**Interfaces:**
- Consumes: nada
- Produces: `display_alloc_framebuffer_psram()` mantém a assinatura `uint16_t*(void)`; passa a retornar `nullptr` com `ESP_LOGE` em vez de tentar um segundo cap

Sem teste automatizado: precisa de PSRAM real. Gate é build + inspeção. A prova de que o cap nunca casa está na spec, Apêndice A (`esp_psram.c:316` em v5.2 / `:425` em v5.4).

- [ ] **Step 1: Adicionar o include de `assert`**

Em `components/display/display.cpp`, acrescentar junto aos includes existentes:

```cpp
#include <cassert>
#include <cstdint>
#include <cstddef>   // size_t — usado tambem pelo blit fatiado da Task 8
```

- [ ] **Step 2: Reescrever `display_alloc_framebuffer_psram`**

Trocar a função inteira por:

```cpp
uint16_t* display_alloc_framebuffer_psram(void) {
    const size_t bytes = 320u * 240u * sizeof(uint16_t);
    // MALLOC_CAP_DMA is deliberately NOT requested. On the ESP32-S3 the PSRAM
    // heap is registered as SPIRAM|DEFAULT|8BIT|32BIT and never carries the
    // DMA cap (esp_psram.c, esp_psram_extram_add_to_heap_allocator), so
    // SPIRAM|DMA can only ever return nullptr — the old primary path was dead
    // code and its warning fired on every boot, twice. GDMA reaches PSRAM
    // through the cache on the S3 regardless of the flag.
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) {
        ESP_LOGE(TAG, "psram framebuffer alloc failed (%u bytes)", (unsigned)bytes);
        return nullptr;
    }
    assert(((uintptr_t)p % 4) == 0);   // GDMA requires 4-byte alignment
    return static_cast<uint16_t*>(p);
}
```

- [ ] **Step 3: Corrigir o docstring em `components/display/include/display.h`**

Trocar o bloco de comentário de `display_alloc_framebuffer_psram` por:

```c
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
```

- [ ] **Step 4: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 5: Commit**

```bash
git add components/display/display.cpp components/display/include/display.h
git commit -m "fix(display): remove MALLOC_CAP_DMA que nunca casa em PSRAM no S3"
```

---

### Task 3: Ordem de init e comentário do BGR (§E, §F)

**Files:**
- Modify: `components/display/display.cpp:32-34`
- Modify: `components/display/include/lgfx_ili9341_config.h:56-62`

**Interfaces:**
- Consumes: nada
- Produces: nada (mudança de ordem e de comentário)

- [ ] **Step 1: Inverter `fillScreen` e `setBrightness` em `display.cpp`**

Trocar:

```cpp
    s_lcd->setRotation(1);       // landscape 320x240
    s_lcd->setBrightness(200);   // 0..255
    s_lcd->fillScreen(0x0000);   // black
```

por:

```cpp
    s_lcd->setRotation(1);       // landscape 320x240

    // fillScreen BEFORE setBrightness: after power-on the panel GRAM holds
    // random data (datasheet p.114, RAMWR "Default" table: "Contents of
    // memory is set randomly"). Lighting the backlight first shows that
    // garbage for one frame.
    s_lcd->fillScreen(0x0000);   // black
    s_lcd->setBrightness(200);   // 0..255
```

- [ ] **Step 2: Corrigir o comentário do `rgb_order` em `lgfx_ili9341_config.h`**

Trocar:

```cpp
            cfg.rgb_order        = false;  // BGR — nativo do ILI9341
```

por:

```cpp
            // rgb_order=false faz o LovyanGFX escrever MAD_BGR (0x08) no
            // MADCTL. ATENCAO: BGR nao e o default do chip — o datasheet
            // p.127-128 mostra MADCTL = 8'h00 apos Power On E apos HW Reset,
            // ou seja bit BGR = 0 = painel com filtro RGB. Setamos BGR porque
            // e o que os modulos vermelhos 2.8" pedem, nao porque o chip
            // prefira. Se as cores sairem trocadas (vermelho vira azul),
            // inverter para true.
            cfg.rgb_order        = false;
```

- [ ] **Step 3: Corrigir também o cabeçalho do arquivo**

No bloco de comentário do topo (`lgfx_ili9341_config.h:6-17`), trocar a linha:

```
 *  - rgb_order = false: ILI9341 aceita BGR (ordem nativa do chip).
 *    Se cores aparecerem complementares (vermelho vira azul), trocar para true.
```

por:

```
 *  - rgb_order = false: escreve MAD_BGR no MADCTL. Nao e o default do chip
 *    (que e RGB, 8'h00) — e o que o modulo vermelho pede. Ver comentario
 *    detalhado no cfg.rgb_order abaixo.
```

- [ ] **Step 4: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 5: Commit**

```bash
git add components/display/display.cpp components/display/include/lgfx_ili9341_config.h
git commit -m "fix(display): limpa GRAM antes de acender backlight; corrige doc do BGR"
```

---

### Task 4: Janela de endereçamento por frame (§G)

**Files:**
- Modify: `components/display/display.cpp:36-41` (remover o pin), `:51-59` (`display_blit_full`)
- Modify: `components/render/render.cpp:95-104` (remover helper), `:123`, `:134`, `:159` (remover chamadas)

**Interfaces:**
- Consumes: nada
- Produces: `display_blit_full` passa a ser autossuficiente — não depende mais de nenhum estado de janela deixado por terceiros. `restore_full_blit_window` deixa de existir.

> **Nota para o executor:** a Task 8 reescreve `display_blit_full` de novo (para DMA fatiado). Isso é intencional — esta task entrega a correção do §G isolada e mergeável; a Task 8 entrega a do §C. Não pule esta task achando que a 8 a absorve.

- [ ] **Step 1: Remover o `setAddrWindow` pinado de `display_init`**

Em `components/display/display.cpp`, apagar este bloco inteiro:

```cpp
    // The blit window never changes for the full-frame pipeline, so set it
    // once here and let display_blit_full skip the per-frame CASET/RASET
    // (~5 us/frame at 80 MHz SPI).
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, s_lcd->width(), s_lcd->height());
    s_lcd->endWrite();
```

- [ ] **Step 2: Devolver o `setAddrWindow` para dentro de `display_blit_full`**

Trocar a função por:

```cpp
void display_blit_full(const uint16_t* buf) {
    if (!s_lcd || !buf) return;
    // The address window is set per frame. It costs ~5 us against a 30,7 ms
    // frame at 40 MHz (0,016%) and buys back a global invariant that used to
    // span display.cpp and render.cpp: any LovyanGFX call emitting CASET or
    // RASET would silently corrupt the next blit.
    // swap=true converts little-endian RGB565 to the big-endian byte order
    // the ILI9341 expects. (Task 8 removes this by moving the framebuffer to
    // big-endian end to end.)
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, s_lcd->width(), s_lcd->height());
    s_lcd->writePixels(buf, s_lcd->width() * s_lcd->height(), true);
    s_lcd->endWrite();
}
```

- [ ] **Step 3: Apagar `restore_full_blit_window` de `components/render/render.cpp`**

Apagar o comentário e a função inteira (linhas 95–104):

```cpp
// After any overlay draws via fillRect/fillScreen/pushImage the ILI9341
// controller holds whatever sub-window the LovyanGFX call ended with
// ... (todo o bloco) ...
static inline void restore_full_blit_window(lgfx::LGFX_Device* lcd) {
    lcd->setAddrWindow(0, 0, 320, 240);
}
```

- [ ] **Step 4: Apagar as três chamadas**

Remover a linha `restore_full_blit_window(lcd);` de dentro de `render_show_freeze` (duas ocorrências, nos ramos `blink` e `else if`) e de `render_show_disconnected` (uma ocorrência).

- [ ] **Step 5: Confirmar que não sobrou referência**

Run: `grep -rn "restore_full_blit_window" components main`
Expected: nenhuma saída.

- [ ] **Step 6: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 7: Commit**

```bash
git add components/display/display.cpp components/render/render.cpp
git commit -m "refactor(display): setAddrWindow por frame; mata invariante global de janela"
```

---

# Wave 2 — Reset conforme datasheet

---

### Task 5: Reset manual + `init_without_reset` (§D)

**Files:**
- Modify: `components/display/display.cpp:1-46` (includes, estado estático, `display_init` inteiro)
- Modify: `components/display/include/display.h:12-18` (docstring de `display_init`)

**Interfaces:**
- Consumes: `LCD_SPI_HZ` (Task 1), `PIN_LCD_BL`, `PIN_LCD_RST` (`main/pinout.h`)
- Produces: `display_init(uint32_t spi_hz)` mantém assinatura; ganha guarda de reentrância com `s_spi_hz`

- [ ] **Step 1: Acrescentar os includes de FreeRTOS**

Em `components/display/display.cpp`:

```cpp
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
```

- [ ] **Step 2: Acrescentar o estado da frequência**

Junto de `static LGFX_ILI9341_Red* s_lcd = nullptr;`:

```cpp
static uint32_t s_spi_hz = 0;
```

- [ ] **Step 3: Reescrever `display_init`**

```cpp
bool display_init(uint32_t spi_hz) {
    if (s_lcd != nullptr) {
        if (spi_hz != s_spi_hz) {
            ESP_LOGW(TAG, "already initialised at %u Hz; ignoring request for %u Hz",
                     (unsigned)s_spi_hz, (unsigned)spi_hz);
        }
        return true;
    }

    // Neutralize SD and XPT2046 CS lines before SPI init to prevent
    // those chips from responding during ILI9341 init sequence.
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_SD_CS,    GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_TOUCH_CS, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_SD_CS,    1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_TOUCH_CS, 1));

    // Backlight hard off before the reset below. The manual reset runs before
    // LovyanGFX's Light_PWM::init(0), so without this the BL pin would float
    // through the reset pulse — exactly the flash we are removing.
    // cfg.invert == false in lgfx_ili9341_config.h, so LOW = off.
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_LCD_BL, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_LCD_BL, 0));

    // Hardware reset with the datasheet's timing (ILI9341 V1.11, p.230 s15.4):
    //   tRW >= 10 us  -> 10 ms here, margin for a slow-rising rail
    //   note 7: after releasing RESX wait 5 ms before any command, AND do not
    //           send Sleep Out for 120 ms.
    // LovyanGFX's own reset (Panel_Device::init) waits only 64 ms before the
    // SLPOUT in its init list, so we do the reset ourselves with the right
    // timing and then call init_without_reset(), which keeps everything else
    // it does (init_cs, Light_PWM::init(0), bus init, rst high).
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_direction((gpio_num_t)PIN_LCD_RST, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level((gpio_num_t)PIN_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(120));

    s_lcd = new LGFX_ILI9341_Red(spi_hz);
    // NOTE: with cfg.readable == false LovyanGFX never reads the panel back,
    // so init_impl always returns true — this check cannot detect a missing or
    // dead panel. Kept because it costs nothing and starts working the day we
    // enable reads.
    if (!s_lcd->init_without_reset(false)) {
        ESP_LOGE(TAG, "lcd init failed @ %u Hz", (unsigned)spi_hz);
        delete s_lcd;
        s_lcd = nullptr;
        return false;
    }

    s_lcd->setRotation(1);       // landscape 320x240

    // fillScreen BEFORE setBrightness: after power-on the panel GRAM holds
    // random data (datasheet p.114, RAMWR "Default" table).
    s_lcd->fillScreen(0x0000);   // black
    s_lcd->setBrightness(200);   // 0..255

    s_spi_hz = spi_hz;
    ESP_LOGI(TAG, "lcd ok %dx%d @ %u Hz",
             s_lcd->width(), s_lcd->height(), (unsigned)spi_hz);
    return true;
}
```

- [ ] **Step 4: Atualizar o docstring de `display_init` em `display.h`**

```c
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
```

- [ ] **Step 5: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 6: Commit**

```bash
git add components/display/display.cpp components/display/include/display.h
git commit -m "fix(display): reset manual com timing do datasheet (p.230) + init_without_reset"
```

---

# Wave 3 — Big-endian end-to-end

> **Atomicidade:** as Tasks 7–10 são commits separados para revisão, mas **só fazem sentido juntas**. Entre a Task 7 e a Task 10 o pipeline fica temporariamente inconsistente (patterns em BE, blit ainda pedindo swap). Como o boot está travado, nada disso é flashável no meio do caminho de qualquer forma. Não mergear a wave pela metade.

---

### Task 6: RED — host tests esperando big-endian

**Files:**
- Modify: `components/display/host_tests/test_patterns.cpp`
- Test: o próprio arquivo (é o harness)

**Interfaces:**
- Consumes: `rgb565`, `pattern_color_bars`, `pattern_gradient`, `pattern_checker` (declarados por forward-decl no topo do harness)
- Produces: suíte que só passa quando `rgb565` devolver BE

> Os forward-decls no topo do harness continuam usando `uint16_t` mesmo depois de a implementação passar a usar `rgb565be_t`. Isso é correto e intencional: `rgb565be_t` é `typedef uint16_t`, tipo idêntico e mesmo name mangling. Não trocar por `#include "display.h"` — o harness é deliberadamente standalone.

- [ ] **Step 1: Adicionar o helper `le_of` e o comentário de contrato**

Inserir logo depois de `static inline uint16_t pixel(...)` (por volta da linha 82):

```cpp
// The framebuffer contract is big-endian RGB565 (see rgb565be_t in
// display.h): the uint16_t sits in memory as {R5G3, G3B5} — the order the
// ILI9341 reads off the wire. Expectations below are therefore byte-swapped
// relative to the familiar 0xF800-style constants. le_of() undoes the swap so
// the bit-field assertions stay readable.
static inline uint16_t le_of(uint16_t be) {
    return (uint16_t)((be >> 8) | (be << 8));
}
```

- [ ] **Step 2: Virar as expectativas dos testes de `rgb565`**

```cpp
static void test_rgb565_red_pure() {
    // should encode red pure (255,0,0) as 0x00F8 (0xF800 byte-swapped)
    uint16_t got = rgb565(255, 0, 0);
    ASSERT_EQ_U16(got, 0x00F8, "rgb565 red pure BE");
    TEST_PASS("should encode red pure (255,0,0) as 0x00F8 big-endian");
}

static void test_rgb565_green_pure() {
    // should encode green pure (0,255,0) as 0xE007 (0x07E0 byte-swapped)
    uint16_t got = rgb565(0, 255, 0);
    ASSERT_EQ_U16(got, 0xE007, "rgb565 green pure BE");
    TEST_PASS("should encode green pure (0,255,0) as 0xE007 big-endian");
}

static void test_rgb565_blue_pure() {
    // should encode blue pure (0,0,255) as 0x1F00 (0x001F byte-swapped)
    uint16_t got = rgb565(0, 0, 255);
    ASSERT_EQ_U16(got, 0x1F00, "rgb565 blue pure BE");
    TEST_PASS("should encode blue pure (0,0,255) as 0x1F00 big-endian");
}
```

`test_rgb565_white` (0xFFFF) e `test_rgb565_black` (0x0000) ficam **inalterados** — são palíndromos de byte.

- [ ] **Step 3: Virar a expectativa do mid gray**

```cpp
static void test_rgb565_mid_gray() {
    // should place R in bits 15..11 of the logical value for gray (128,128,128)
    uint16_t got = rgb565(128, 128, 128);
    // logical = (16<<11)|(32<<5)|16 = 0x8410 ; stored big-endian = 0x1084
    uint16_t logical  = (uint16_t)(((128 & 0xF8) << 8) | ((128 & 0xFC) << 3) | (128 >> 3));
    uint16_t expected = (uint16_t)((logical >> 8) | (logical << 8));
    ASSERT_EQ_U16(got, expected, "rgb565 mid gray exact BE");
    TEST_PASS("should encode gray (128,128,128) with R in bits 15..11 of the logical value");
}
```

- [ ] **Step 4: Adicionar o teste que fixa a ordem de bytes na memória**

Este é o teste que realmente prende o contrato — os outros só olham o valor do `uint16_t`.

```cpp
static void test_rgb565_is_big_endian_in_memory() {
    // should lay red out as {0xF8, 0x00} in memory, R-high byte first.
    // Assumes a little-endian host (x86-64 / arm64 Linux — every supported
    // dev machine). On a big-endian host this assertion would be inverted.
    uint16_t v = rgb565(255, 0, 0);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    ASSERT_EQ_INT(b[0], 0xF8, "first memory byte is R-high");
    ASSERT_EQ_INT(b[1], 0x00, "second memory byte is G-low|B");
    TEST_PASS("should store red as bytes {0xF8,0x00} so the ILI9341 sees big-endian");
}
```

- [ ] **Step 5: Virar as expectativas dos patterns**

```cpp
static void test_color_bars_pixel_0_0_white() {
    pattern_color_bars(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 0, 0), 0xFFFF, "color_bars pixel(0,0) white");
    TEST_PASS("should set pixel(0,0) to white when pattern_color_bars fills bar 0");
}

static void test_color_bars_pixel_40_0_yellow() {
    // yellow 0xFFE0 stored big-endian is 0xE0FF
    pattern_color_bars(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 40, 0), 0xE0FF, "color_bars pixel(40,0) yellow BE");
    TEST_PASS("should set pixel(40,0) to yellow (0xE0FF big-endian) in bar 1");
}

static void test_gradient_pixel_0_0_blue() {
    // blue 0x001F stored big-endian is 0x1F00
    pattern_gradient(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 0, 0), 0x1F00, "gradient pixel(0,0) blue BE");
    TEST_PASS("should set pixel(0,0) to 0x1F00 (R=0,G=0,B=255) big-endian");
}

static void test_gradient_pixel_319_239_yellow() {
    // yellow 0xFFE0 stored big-endian is 0xE0FF
    pattern_gradient(g_buf);
    ASSERT_EQ_U16(pixel(g_buf, 319, 239), 0xE0FF, "gradient pixel(319,239) yellow BE");
    TEST_PASS("should set pixel(319,239) to 0xE0FF (R=255,G=255,B=0) big-endian");
}
```

`test_color_bars_pixel_280_100_black` (0x0000), `test_color_bars_same_y_invariant`, `test_color_bars_adjacent_bars_differ`, `test_checker_20_0_0_white` (0xFFFF), `test_checker_20_pixel_20_0_black` (0x0000), `test_checker_20_pixel_40_0_white` (0xFFFF), `test_checker_20_pixel_0_20_black` (0x0000), `test_checker_1_adjacent_pixels_differ` e `test_checker_0_does_not_crash` ficam **inalterados** — usam só preto, branco ou comparações relativas.

- [ ] **Step 6: Passar os testes de bit-field por `le_of`**

Nos três testes de canal, envolver o valor lido:

```cpp
static void test_gradient_mid_x_r_channel_near_127() {
    pattern_gradient(g_buf);
    uint16_t p = pixel(g_buf, 160, 0);
    ASSERT_CHANNEL_NEAR(le_of(p), 11, 0x1F, 15, 1, "gradient mid x R5 near 15");
    TEST_PASS("should set R5 field of pixel(160,0) near 15 with tolerance +/-1 when x=160");
}

static void test_gradient_mid_x_g_channel_zero_at_y0() {
    pattern_gradient(g_buf);
    uint16_t p = pixel(g_buf, 160, 0);
    ASSERT_CHANNEL_NEAR(le_of(p), 5, 0x3F, 0, 0, "gradient y=0 G6 is 0");
    TEST_PASS("should set G6 field of pixel(160,0) to 0 when y=0");
}

static void test_gradient_r_channel_max_at_x319() {
    pattern_gradient(g_buf);
    uint16_t p = pixel(g_buf, 319, 0);
    ASSERT_CHANNEL_NEAR(le_of(p), 11, 0x1F, 31, 0, "gradient R5=31 at x=319");
    TEST_PASS("should set R5 to 31 (no overflow) at x=319");
}
```

- [ ] **Step 7: Registrar o teste novo em `main()`**

Na seção `=== rgb565 ===`, depois de `test_rgb565_mid_gray();`:

```cpp
    test_rgb565_is_big_endian_in_memory();
```

- [ ] **Step 8: Rodar e confirmar o RED**

Run: `cd components/display/host_tests && make test`
Expected: FAIL na primeira asserção de `rgb565` — algo como
`test_patterns.cpp:NNN FAIL [rgb565 red pure BE]: got=0xF800 expected=0x00F8`

- [ ] **Step 9: Commit do RED**

```bash
git add components/display/host_tests/test_patterns.cpp
git commit -m "test(display): red — host tests esperam framebuffer big-endian"
```

---

### Task 7: GREEN — `rgb565` emite big-endian + typedef `rgb565be_t`

**Files:**
- Modify: `components/display/include/display.h` (typedef novo, perto do topo)
- Modify: `components/display/test_patterns.cpp` (helper + assinaturas)
- Modify: `main/app_main.cpp:29-32` (forward-decls)

**Interfaces:**
- Consumes: nada
- Produces:
  - `typedef uint16_t rgb565be_t;` em `display.h`
  - `rgb565be_t rgb565(uint8_t r, uint8_t g, uint8_t b)`
  - `void pattern_color_bars(rgb565be_t* buf)`
  - `void pattern_gradient(rgb565be_t* buf)`
  - `void pattern_checker(rgb565be_t* buf, int cell_px)`
  - `void pattern_tearing_stripes(rgb565be_t* buf, uint32_t frame_counter)`

- [ ] **Step 1: Adicionar o typedef em `components/display/include/display.h`**

Logo depois do `extern "C" {` de abertura:

```c
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
```

- [ ] **Step 2: Reescrever o helper em `components/display/test_patterns.cpp`**

```cpp
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
```

O docstring antigo afirmava "big-endian word, ILI9341 native order" enquanto a fórmula produzia o valor little-endian. Agora a afirmação e o código concordam.

- [ ] **Step 3: Trocar `uint16_t*` por `rgb565be_t*` nas quatro assinaturas de pattern**

Em `components/display/test_patterns.cpp`, nas definições de `pattern_color_bars`, `pattern_gradient`, `pattern_checker` e `pattern_tearing_stripes`. Também nas declarações locais de cor dentro delas (`const uint16_t colors[8]` → `const rgb565be_t colors[8]`; `const uint16_t white`/`black` → `const rgb565be_t`).

- [ ] **Step 4: Atualizar os forward-decls em `main/app_main.cpp:29-32`**

```cpp
extern void pattern_color_bars(rgb565be_t* buf);
extern void pattern_gradient(rgb565be_t* buf);
extern void pattern_checker(rgb565be_t* buf, int cell_px);
extern void pattern_tearing_stripes(rgb565be_t* buf, uint32_t frame_counter);
```

`app_main.cpp` já inclui `display.h` (linha 13), então o typedef está disponível.

- [ ] **Step 5: Rodar os host tests e confirmar o GREEN**

Run: `cd components/display/host_tests && make test`
Expected: PASS em todos — `All 23 tests passed.` (eram 22; a Task 6 adicionou o de ordem de bytes)

- [ ] **Step 6: Build do firmware**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 7: Commit**

```bash
git add components/display/include/display.h components/display/test_patterns.cpp main/app_main.cpp
git commit -m "feat(display): rgb565be_t + patterns emitindo big-endian [23/23 host tests]"
```

---

### Task 8: Blit por DMA fatiado + `display_blit_rect`

**Files:**
- Modify: `components/display/include/display.h` (assinaturas de blit)
- Modify: `components/display/display.cpp` (`display_blit_full`, nova `display_blit_rect`)

**Interfaces:**
- Consumes: `rgb565be_t` (Task 7)
- Produces:
  - `void display_blit_full(const rgb565be_t* fb)` — retorna com a última fatia em voo
  - `void display_blit_rect(const rgb565be_t* src, int src_stride, int x, int y, int w, int h)` — **bloqueia** até o DMA terminar

- [ ] **Step 1: Declarar a API nova em `display.h`**

Trocar a declaração de `display_blit_full` e acrescentar a nova:

```c
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
```

- [ ] **Step 2: Reescrever `display_blit_full` em `display.cpp`**

```cpp
// 48 * 320 * 2 = 30720 bytes — abaixo do teto de ~32 KB de uma transacao de
// hardware do SPI. 240 / 48 = 5 fatias exatas.
static constexpr int kBlitSliceRows = 48;

void display_blit_full(const rgb565be_t* buf) {
    if (!s_lcd || !buf) return;
    const int w = s_lcd->width();
    const int h = s_lcd->height();

    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, w, h);
    // swap=false: the buffer is already big-endian, so pixelcopy takes the
    // no_convert path and writeBytes DMAs straight from PSRAM — no CPU pass
    // over 76800 pixels. writePixelsDMA (not writePixels) is what actually
    // passes use_dma=true down to Bus_SPI.
    for (int y = 0; y < h; y += kBlitSliceRows) {
        const int rows = (y + kBlitSliceRows <= h) ? kBlitSliceRows : (h - y);
        s_lcd->writePixelsDMA(&buf[(size_t)y * w], rows * w, false);
    }
    s_lcd->endWrite();
}
```

- [ ] **Step 3: Implementar `display_blit_rect` logo abaixo**

```cpp
void display_blit_rect(const rgb565be_t* src, int src_stride,
                       int x, int y, int w, int h) {
    if (!s_lcd || !src || w <= 0 || h <= 0) return;

    // startWrite/endWrite are reference-counted by LovyanGFX, so this nests
    // safely inside a block the caller already opened.
    s_lcd->startWrite();
    s_lcd->setAddrWindow(x, y, w, h);
    for (int row = 0; row < h; ++row) {
        s_lcd->writePixels(&src[(size_t)row * src_stride], w, false);
    }
    s_lcd->endWrite();
    // Callers draw text/rects right after this returns, so finish the transfer
    // rather than leaving it in flight.
    s_lcd->waitDMA();
}
```

- [ ] **Step 4: Confirmar que `swap=true` sumiu do projeto**

Run: `grep -rn "writePixels\|pushImage" components main --include=*.cpp --include=*.h`
Expected: só ocorrências dentro de `components/display/display.cpp`, todas com `false`.

- [ ] **Step 5: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 6: Commit**

```bash
git add components/display/include/display.h components/display/display.cpp
git commit -m "perf(display): blit por DMA fatiado (48 linhas) + display_blit_rect"
```

---

### Task 9: `render` usa a API de blit e os tipos BE

**Files:**
- Modify: `components/render/include/render.h:14-19`
- Modify: `components/render/render.cpp:23-32` (estado), `:62-64`, `:75-93`, `:106-162`

**Interfaces:**
- Consumes: `rgb565be_t`, `display_blit_full`, `display_blit_rect`, `display_wait_dma` (Tasks 7 e 8)
- Produces: `rgb565be_t* render_back_buffer(void)`

`components/render/CMakeLists.txt` já declara `REQUIRES display`, então nada muda ali.

- [ ] **Step 1: Atualizar `render.h`**

```c
#pragma once
#include <cstdint>
#include "display.h"   // rgb565be_t
```

e a declaração:

```c
/**
 * Return a pointer to the "back" framebuffer for the decoder to write into.
 * @return 320*240 big-endian RGB565 buffer in PSRAM; 4-byte aligned.
 * @note   Not thread-safe; must be called from the single decode task.
 */
rgb565be_t* render_back_buffer(void);
```

- [ ] **Step 2: Trocar os tipos do estado em `render.cpp`**

```cpp
static rgb565be_t*       s_fb[2]    = {nullptr, nullptr};
```

e

```cpp
static rgb565be_t s_thumb[THUMB_W * THUMB_H];
```

- [ ] **Step 3: Atualizar `render_back_buffer` e `render_capture_thumb`**

```cpp
rgb565be_t* render_back_buffer(void) {
    return s_fb[s_back_idx];
}
```

Em `render_capture_thumb`, só o tipo do ponteiro local:

```cpp
    const rgb565be_t* src = s_fb[s_back_idx ^ 1];
```

- [ ] **Step 4: Substituir o caminho de pixels do badge FREEZE**

Trocar o ramo `else if (s_has_presented)` inteiro de `render_show_freeze` por:

```cpp
    } else if (s_has_presented) {
        // Erase the badge by streaming the underlying front-buffer slice.
        display_wait_dma();
        const rgb565be_t* fb = s_fb[s_back_idx ^ 1];
        display_blit_rect(&fb[4 * 320 + 248], 320, 248, 4, 64, 16);
    }
```

- [ ] **Step 5: Substituir o `pushImage` do thumbnail**

Em `render_show_disconnected`, trocar:

```cpp
    if (s_thumb_valid) {
        lcd->pushImage(120, 140, THUMB_W, THUMB_H, s_thumb);
        lcd->setCursor(120, 205);
        lcd->print("ultimo frame");
    }
```

por:

```cpp
    if (s_thumb_valid) {
        display_blit_rect(s_thumb, THUMB_W, 120, 140, THUMB_W, THUMB_H);
        lcd->setCursor(120, 205);
        lcd->print("ultimo frame");
    }
```

- [ ] **Step 6: Atualizar o comentário do modelo de concorrência no topo do arquivo**

A linha `//   - render_show_freeze / render_show_disconnected (link_ui_task, Core 1) draw on the LCD directly via LovyanGFX.` passa a:

```cpp
//   - render_show_freeze / render_show_disconnected (link_ui_task, Core 1)
//     draw badges via LovyanGFX (fillRect/print — cor convertida internamente,
//     imune a ordem de byte) e pixels via display_blit_rect.
```

- [ ] **Step 7: Confirmar que `render.cpp` não fala mais de pixels com o LGFX**

Run: `grep -n "writePixels\|pushImage\|setAddrWindow" components/render/render.cpp`
Expected: nenhuma saída.

- [ ] **Step 8: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 9: Commit**

```bash
git add components/render/include/render.h components/render/render.cpp
git commit -m "refactor(render): overlays via display_blit_rect; framebuffer big-endian"
```

---

### Task 10: Decoder emite big-endian

**Files:**
- Modify: `components/decoder/decoder.cpp:47-52`
- Modify: `components/decoder/include/decoder.h:14-26` (docstring)

**Interfaces:**
- Consumes: nada
- Produces: `decoder_decode_to_rgb565` passa a escrever big-endian em `out_buf`

> `components/decoder/CMakeLists.txt` declara `REQUIRES espressif__esp_jpeg esp_timer log` — **não** depende de `display`. O parâmetro continua `uint16_t*` de propósito: importar `rgb565be_t` acoplaria o decodificador JPEG ao driver de LCD em troca de zero segurança de tipo (o typedef é transparente). A ordem de bytes fica documentada. Ver spec §4.3, "Exceção deliberada".

- [ ] **Step 1: Virar o flag em `decoder.cpp`**

Trocar:

```cpp
    // swap_color_bytes=0: leave RGB565 in native (little-endian) order in
    // PSRAM. display_blit_full / writePixels(swap=true) then swaps each
    // 16-bit pixel on its way to the ILI9341, which expects big-endian.
    // (Sprint 2 review F-03 keeps an eye on this — verify empirically.)
    cfg.flags.swap_color_bytes = 0;
```

por:

```cpp
    // swap_color_bytes=1: esp_jpeg writes RGB565 already byte-swapped, i.e.
    // big-endian, which is what the ILI9341 reads off the wire. The swap
    // happens inside the decode loop at no measurable cost, and it lets
    // display_blit_full take LovyanGFX's no_convert path — a straight DMA out
    // of PSRAM instead of a CPU pass over 76800 pixels per frame.
    cfg.flags.swap_color_bytes = 1;
```

- [ ] **Step 2: Atualizar o docstring em `decoder.h`**

```c
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
```

- [ ] **Step 3: Build**

Run: `pio run`
Expected: SUCCESS, zero warnings.

- [ ] **Step 4: Host tests de regressão (nada deve ter quebrado)**

Run: `cd components/display/host_tests && make test`
Expected: `All 23 tests passed.`

- [ ] **Step 5: Commit**

```bash
git add components/decoder/decoder.cpp components/decoder/include/decoder.h
git commit -m "feat(decoder): esp_jpeg emite RGB565 big-endian (swap_color_bytes=1)"
```

- [ ] **Step 6: Merge da wave 3 para `main`**

```bash
git checkout main
git merge --no-ff sprint-9-driver-conformance -m "merge: sprint 9 waves 1-3 (conformidade ILI9341 offline)"
git checkout sprint-9-driver-conformance
```

---

# Wave 4 — Desbloqueio do boot (BANCADA)

> **Bloqueada por hardware.** Requer a placa ligada em `/dev/ttyACM0` e o usuário rodando `pio device monitor` no terminal dele — o agente pode flashar, mas não consegue ler o monitor interativo. Ver `docs/HANDOFF-rx-multicore-freeze.md`.

---

### Task 11: Bissecção do freeze em `cpu_start: Multicore app`

**Files:**
- Modify: `sdkconfig.defaults` (uma hipótese por vez)
- Modify: `platformio.ini` (só na hipótese 7)

**Interfaces:**
- Consumes: firmware das waves 1–3 já mergeado
- Produces: identificação da causa raiz; boot chegando em `app_main`

**Critério de sucesso único:** aparecer `app_init: Application information:` no log serial. Hoje o log para em `cpu_start: Multicore app` e o chip reseta sozinho após ~35 s.

> **Ressalva contra confusão:** a Task 1 baixou o SPI do LCD para 40 MHz. Isso **não pode** consertar este freeze — ele acontece antes do `app_main`, e o barramento do LCD só é inicializado depois. Os dois tocam o mesmo rail de alimentação, mas em momentos diferentes do boot. Não marcar a Task 11 como resolvida pela Task 1.

- [ ] **Step 1: Hipótese 1 — NVS corrompida**

```bash
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/ttyACM0 --chip esp32s3 erase_flash
pio run -t upload --upload-port /dev/ttyACM0
```

Pedir ao usuário: `pio device monitor --port /dev/ttyACM0 --baud 115200`
Se passar de `Multicore app` → causa encontrada, pular para o Step 8.

- [ ] **Step 2: Hipótese 2 — brownout (audit S-01)**

Comentar em `sdkconfig.defaults`:

```
# CONFIG_ESP_BROWNOUT_DET=y
# CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y
```

Reflash e verificar. Se destravar, **não deixar assim** — subir o threshold para um nível menos sensível (`LVL_SEL_2`) em vez de desabilitar, e registrar no `docs/audits/`.

- [ ] **Step 3: Hipótese 3 — stack canaries (audit S-02)**

Comentar `CONFIG_COMPILER_STACK_CHECK=y` e `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y`. Reflash e verificar. Mesma ressalva do Step 2: se for a causa, é um bug a reportar, não uma proteção a abandonar em silêncio.

- [ ] **Step 4: Hipótese 4 — velocidade da PSRAM**

Trocar `CONFIG_SPIRAM_SPEED_80M=y` por `CONFIG_SPIRAM_SPEED_40M=y`. Reflash e verificar.

- [ ] **Step 5: Hipótese 5 — layout de heap do Wi-Fi em PSRAM**

Remover `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM=y`. Reflash e verificar.

- [ ] **Step 6: Hipótese 6 — otimização do compilador**

Trocar `CONFIG_COMPILER_OPTIMIZATION_PERF=y` por `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`. Reflash e verificar.

- [ ] **Step 7: Hipótese 7 — IDF 6.0.1 (pré-release)**

Em `platformio.ini`, fixar `platform = espressif32 @ 6.13.0` (usa IDF 5.4). Reinstala toolchain, ~30 min. Reflash e verificar.

- [ ] **Step 8: Documentar a causa e commitar**

Atualizar `docs/HANDOFF-rx-multicore-freeze.md` com a hipótese que destravou e o que foi mantido. Commit:

```bash
git add sdkconfig.defaults docs/HANDOFF-rx-multicore-freeze.md
git commit -m "fix(boot): destrava freeze em cpu_start Multicore app"
```

---

# Wave 5 — Validação em hardware (BANCADA)

---

### Task 12: Checklist de bancada

**Files:**
- Modify: `docs/superpowers/specs/2026-08-22-ili9341-driver-conformance-design.md` §5.2 (marcar os checkboxes)

**Interfaces:**
- Consumes: tudo das waves 1–4
- Produces: confirmação empírica ou lista de regressões

Rodar com `CONFIG_RECEIVER_SMOKE_TEST=y` para os primeiros itens (padrões de teste sem depender do TX).

- [ ] **Step 1: Polaridade do backlight**

Chamar `setBrightness(0)` após o init. Tela apaga → `Light_PWM.invert = false` está certo. Tela acende no máximo → trocar para `true` em `lgfx_ili9341_config.h:72`.

- [ ] **Step 2: Ordem de cor BGR**

Com `pattern_color_bars`: a barra 5 (x de 200 a 239) deve aparecer **vermelha**. Se aparecer azul, trocar `cfg.rgb_order` para `true`.

- [ ] **Step 3: Ausência de flash de lixo no boot (valida §E)**

Ligar a placa 5× olhando a tela nos primeiros 200 ms. Não deve haver lampejo de ruído antes do preto.

- [ ] **Step 4: Cold boot repetido (valida §D)**

Ciclar a alimentação 10×. Nenhuma tela branca nem painel morto.

- [ ] **Step 5: Consumidor BE nº 3 — badge FREEZE**

Forçar estado FREEZE (desligar o TX com o link estabelecido). O badge deve piscar e, na meia-fase apagada, a faixa por baixo deve reaparecer com **as cores corretas**. Cor errada aqui = a Task 9 Step 4 ficou com o swap trocado.

- [ ] **Step 6: Consumidor BE nº 4 — thumbnail**

Forçar DISCONNECTED. O thumbnail 80×60 sob "AGUARDANDO LINK" deve ter as cores corretas. Cor errada = a Task 9 Step 5 ficou errada.

- [ ] **Step 7: Estabilidade a 40 MHz**

Soak de 10 min com `pattern_tearing_stripes`. Sem artefatos, sem linhas deslocadas, sem reset.

- [ ] **Step 8: fps e tearing**

Com o pipeline real de vídeo, conferir no log `fps=N`. Esperado ≥ 24. Registrar o valor medido.

- [ ] **Step 9: Fechar a sprint**

Marcar os checkboxes na spec §5.2, commitar, e taguear:

```bash
git add docs/superpowers/specs/2026-08-22-ili9341-driver-conformance-design.md
git commit -m "docs(s9): checklist de bancada validado"
git checkout main && git merge --no-ff sprint-9-driver-conformance
git tag s9-done
```

---

## Cobertura da spec

| Requisito da spec | Task |
|---|---|
| §A clock 40 MHz + freq_read 6 MHz | 1 |
| §B alocação sem `MALLOC_CAP_DMA` | 2 |
| §E ordem fillScreen/setBrightness | 3 |
| §F comentário do `rgb_order` | 3 |
| §G janela por frame | 4 |
| §D reset manual + `init_without_reset` | 5 |
| §C big-endian end-to-end | 6, 7, 8, 9, 10 |
| D6 pixels atrás do `display.h` (`display_blit_rect`) | 8, 9 |
| 4.1 ciclo de vida do `display_init` | 3, 5 |
| 4.3 exceção do decoder (sem typedef) | 10 |
| 4.4 fatiamento em 48 linhas | 8 |
| 5.1 bissecção do boot | 11 |
| 5.2 checklist de bancada | 12 |
