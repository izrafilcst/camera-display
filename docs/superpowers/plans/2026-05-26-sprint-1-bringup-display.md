# Sprint 1 — Bring-up Display Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inicializar o projeto ESP-IDF e fazer o display ILI9341 SPI mostrar padrões estáticos com DMA, sem rede e sem decodificação. Confirmar throughput SPI e identificar tearing baseline.

**Architecture:** Projeto ESP-IDF com layout de components/. Sprint 1 cria o esqueleto, integra LovyanGFX (managed component), define a pinagem em um header central e implementa rotinas de "render de teste" para validar barramento, clock e DMA. Sem rede, sem PSRAM de framebuffer ainda (PSRAM ativada mas FB estático na DRAM interna por simplicidade desta sprint).

**Tech Stack:** ESP-IDF v5.2+, LovyanGFX (via espressif registry / managed_components), CMake, Unity (test framework on host + target).

---

## Reference do spec
- §3 Hardware (MCU, display, pinagem)
- §4.6 Orçamento de blit
- §12 Risco "SPI 80 MHz instável"

## File Structure

```
camera-display/
├── CMakeLists.txt                       (Sprint 1: criar)
├── sdkconfig.defaults                   (Sprint 1: criar)
├── partitions.csv                       (Sprint 1: criar)
├── idf_component.yml                    (Sprint 1: criar — registra LovyanGFX)
├── main/
│   ├── CMakeLists.txt                   (Sprint 1: criar)
│   ├── app_main.cpp                     (Sprint 1: criar)
│   └── pinout.h                         (Sprint 1: criar)
└── components/
    └── display/
        ├── CMakeLists.txt               (Sprint 1: criar)
        ├── include/
        │   ├── display.h                (Sprint 1: criar — API pública)
        │   └── lgfx_ili9341_config.h    (Sprint 1: criar — config LovyanGFX)
        ├── display.cpp                  (Sprint 1: criar)
        └── test_patterns.cpp            (Sprint 1: criar — padrões debug)
```

**Responsabilidades:**
- `main/app_main.cpp`: entrypoint; chama `display_init()` e roda loop de demo
- `main/pinout.h`: header único com todos os GPIOs do projeto (display + futuras entradas)
- `components/display/display.{cpp,h}`: API pública (`display_init`, `display_blit_buffer`, `display_present`)
- `components/display/lgfx_ili9341_config.h`: configuração do template LovyanGFX para o ILI9341 da placa vermelha
- `components/display/test_patterns.cpp`: rotinas como `draw_color_bars()`, `draw_gradient()`, `draw_tearing_pattern()`

---

### Task 1: Inicialização do projeto ESP-IDF

**Files:**
- Create: `CMakeLists.txt`
- Create: `sdkconfig.defaults`
- Create: `partitions.csv`
- Create: `idf_component.yml`
- Create: `main/CMakeLists.txt`
- Create: `main/app_main.cpp`

- [ ] **Step 1: Verificar ESP-IDF instalado**

Run: `idf.py --version`
Expected: `ESP-IDF v5.2` ou maior. Se ausente, instalar via https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/get-started/index.html

- [ ] **Step 2: Criar `CMakeLists.txt` raiz**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camera_display)
```

- [ ] **Step 3: Criar `sdkconfig.defaults`**

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_FREERTOS_HZ=1000
```

- [ ] **Step 4: Criar `partitions.csv`**

```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 6M,
storage,  data, spiffs,  ,        2M,
```

- [ ] **Step 5: Criar `idf_component.yml` para LovyanGFX**

```yaml
dependencies:
  lovyan03/LovyanGFX: "^1.1.16"
  idf:
    version: ">=5.2.0"
```

- [ ] **Step 6: Criar `main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES display
)
```

- [ ] **Step 7: Criar `main/app_main.cpp` placeholder**

```cpp
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "camera-display Sprint 1 boot");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 8: Build vazio**

Run: `idf.py set-target esp32s3 && idf.py build`
Expected: build completa sem erro. Tamanho do bin > 0.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt sdkconfig.defaults partitions.csv idf_component.yml main/
git commit -m "chore: bootstrap esp-idf project skeleton"
```

---

### Task 2: Definir pinagem central

**Files:**
- Create: `main/pinout.h`

- [ ] **Step 1: Criar `main/pinout.h`**

```cpp
#pragma once

// Display SPI (módulo vermelho ILI9341 + SD + touch)
#define LCD_SPI_HOST       SPI2_HOST
#define PIN_LCD_MOSI       11
#define PIN_LCD_SCK        12
#define PIN_LCD_MISO       13   // usado apenas se queremos ler do touch; deixa conectado mas idle
#define PIN_LCD_CS         10
#define PIN_LCD_DC          9
#define PIN_LCD_RST         8
#define PIN_LCD_BL          7
#define PIN_TOUCH_CS        6   // mantido HIGH; XPT2046 não usado
#define PIN_SD_CS           5   // mantido HIGH; SD não usado

// Entrada (sprint 4) — placeholders aqui para coerência futura
#define PIN_JOY_X           4   // ADC1_CH3
#define PIN_JOY_Y           3   // ADC1_CH2
#define PIN_BTN_ADVANCE     2
#define PIN_BTN_BACK        1

// SPI clock
#define LCD_SPI_HZ_TARGET   80000000
#define LCD_SPI_HZ_SAFE     40000000
```

- [ ] **Step 2: Commit**

```bash
git add main/pinout.h
git commit -m "feat: central pinout header"
```

---

### Task 3: Componente `display` esqueleto

**Files:**
- Create: `components/display/CMakeLists.txt`
- Create: `components/display/include/display.h`
- Create: `components/display/display.cpp`

- [ ] **Step 1: Criar `components/display/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "display.cpp" "test_patterns.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main"
    REQUIRES lovyan03__LovyanGFX
)
```

- [ ] **Step 2: Criar `components/display/include/display.h`**

```cpp
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa display. Retorna true se OK.
bool display_init(uint32_t spi_hz);

// Largura/altura efetivas após rotação.
int display_width(void);
int display_height(void);

// Blita um buffer RGB565 inteiro (320x240 pixels) usando DMA. Bloqueia até DMA iniciar.
void display_blit_full(const uint16_t* rgb565_buf);

// Espera DMA do blit anterior terminar (busy-wait curto).
void display_wait_dma(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Criar `components/display/display.cpp` skeleton (sem LovyanGFX ainda)**

```cpp
#include "display.h"
#include "esp_log.h"

static const char* TAG = "display";

bool display_init(uint32_t spi_hz) {
    ESP_LOGW(TAG, "display_init stub spi_hz=%u", (unsigned)spi_hz);
    return false;
}
int display_width(void)  { return 320; }
int display_height(void) { return 240; }
void display_blit_full(const uint16_t*) {}
void display_wait_dma(void) {}
```

- [ ] **Step 4: Atualizar `main/app_main.cpp` para chamar `display_init`**

Substitua `main/app_main.cpp` por:

```cpp
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "pinout.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "camera-display Sprint 1 boot");
    bool ok = display_init(LCD_SPI_HZ_SAFE);
    ESP_LOGI(TAG, "display_init returned %d", ok);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 5: Criar `components/display/test_patterns.cpp` placeholder**

```cpp
#include "display.h"
// Padrões de teste implementados em Task 5.
```

- [ ] **Step 6: Build**

Run: `idf.py build`
Expected: build OK.

- [ ] **Step 7: Commit**

```bash
git add components/display/ main/app_main.cpp
git commit -m "feat: display component skeleton"
```

---

### Task 4: Configuração LovyanGFX para ILI9341

**Files:**
- Create: `components/display/include/lgfx_ili9341_config.h`
- Modify: `components/display/display.cpp`

- [ ] **Step 1: Criar `components/display/include/lgfx_ili9341_config.h`**

```cpp
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pinout.h"

class LGFX_ILI9341_Red : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX_ILI9341_Red(uint32_t spi_hz) {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = LCD_SPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = spi_hz;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = PIN_LCD_MISO;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs        = PIN_LCD_CS;
            cfg.pin_rst       = PIN_LCD_RST;
            cfg.pin_busy      = -1;
            cfg.panel_width   = 240;
            cfg.panel_height  = 320;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable      = false;
            cfg.invert        = false;
            cfg.rgb_order     = false;
            cfg.dlen_16bit    = false;
            cfg.bus_shared    = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};
```

- [ ] **Step 2: Implementar `display_init` em `components/display/display.cpp`**

Substitua o conteúdo de `display.cpp` por:

```cpp
#include "display.h"
#include "lgfx_ili9341_config.h"
#include "esp_log.h"

static const char* TAG = "display";
static LGFX_ILI9341_Red* s_lcd = nullptr;

bool display_init(uint32_t spi_hz) {
    s_lcd = new LGFX_ILI9341_Red(spi_hz);
    if (!s_lcd->init()) {
        ESP_LOGE(TAG, "lcd init failed @ %u Hz", (unsigned)spi_hz);
        return false;
    }
    s_lcd->setRotation(1);                  // landscape 320x240
    s_lcd->setBrightness(200);              // 0..255
    s_lcd->fillScreen(0x0000);
    ESP_LOGI(TAG, "lcd ok %dx%d @ %u Hz",
             s_lcd->width(), s_lcd->height(), (unsigned)spi_hz);
    return true;
}

int display_width(void)  { return s_lcd ? s_lcd->width()  : 320; }
int display_height(void) { return s_lcd ? s_lcd->height() : 240; }

void display_blit_full(const uint16_t* buf) {
    if (!s_lcd || !buf) return;
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, s_lcd->width(), s_lcd->height());
    s_lcd->writePixels(buf, s_lcd->width() * s_lcd->height(), true /* swap */);
    s_lcd->endWrite();
}

void display_wait_dma(void) {
    if (!s_lcd) return;
    s_lcd->waitDMA();
}
```

- [ ] **Step 3: Build e flash**

Run: `idf.py build flash monitor`
Expected: log "lcd ok 320x240 @ 40000000 Hz" e tela preta (não congelada do bootloader).

- [ ] **Step 4: Verificação manual**

Olhe o display físico:
- Backlight aceso
- Tela completamente preta (não com lixo de boot)

Se backlight não acender, conferir polaridade do `invert` em `Light_PWM` ou pinagem do BL.

- [ ] **Step 5: Commit**

```bash
git add components/display/
git commit -m "feat: ili9341 init via lovyangfx, 320x240 landscape"
```

---

### Task 5: Padrões de teste estáticos

**Files:**
- Modify: `components/display/test_patterns.cpp`
- Create: declarações em `components/display/include/display.h`
- Modify: `main/app_main.cpp`

- [ ] **Step 1: Adicionar declarações em `display.h`**

Adicione antes do `#ifdef __cplusplus` final:

```cpp
// Padrões de teste — preenchem um buffer 320x240 RGB565.
void pattern_color_bars(uint16_t* buf);
void pattern_gradient(uint16_t* buf);
void pattern_checker(uint16_t* buf, int cell_px);
void pattern_tearing_stripes(uint16_t* buf, uint32_t frame_counter);
```

- [ ] **Step 2: Implementar em `components/display/test_patterns.cpp`**

```cpp
#include "display.h"
#include <cstdint>

static const int W = 320;
static const int H = 240;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void pattern_color_bars(uint16_t* buf) {
    const uint16_t colors[8] = {
        rgb565(255,255,255), rgb565(255,255,  0),
        rgb565(  0,255,255), rgb565(  0,255,  0),
        rgb565(255,  0,255), rgb565(255,  0,  0),
        rgb565(  0,  0,255), rgb565(  0,  0,  0),
    };
    int bar_w = W / 8;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int idx = x / bar_w;
            if (idx > 7) idx = 7;
            buf[y * W + x] = colors[idx];
        }
    }
}

void pattern_gradient(uint16_t* buf) {
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t r = (uint8_t)((x * 255) / (W - 1));
            uint8_t g = (uint8_t)((y * 255) / (H - 1));
            uint8_t b = (uint8_t)(255 - r);
            buf[y * W + x] = rgb565(r, g, b);
        }
    }
}

void pattern_checker(uint16_t* buf, int cell_px) {
    if (cell_px < 1) cell_px = 1;
    uint16_t a = rgb565(255,255,255);
    uint16_t b = rgb565(  0,  0,  0);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            bool on = ((x / cell_px) + (y / cell_px)) & 1;
            buf[y * W + x] = on ? a : b;
        }
    }
}

void pattern_tearing_stripes(uint16_t* buf, uint32_t frame_counter) {
    // Faixas horizontais alternando branco/preto deslocando 8 px/frame.
    int shift = (frame_counter * 8) % H;
    for (int y = 0; y < H; ++y) {
        bool on = (((y + shift) / 16) & 1);
        uint16_t c = on ? rgb565(255,255,255) : rgb565(0,0,0);
        for (int x = 0; x < W; ++x) buf[y * W + x] = c;
    }
}
```

- [ ] **Step 3: Atualizar `main/app_main.cpp` com loop de demo**

```cpp
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "pinout.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "camera-display Sprint 1 boot, free heap=%u",
             (unsigned)esp_get_free_heap_size());

    if (!display_init(LCD_SPI_HZ_SAFE)) {
        ESP_LOGE(TAG, "display init failed");
        while(true) vTaskDelay(1000);
    }

    // Buffer único em DRAM (153.6 KB cabe nos ~320 KB da SRAM interna).
    uint16_t* fb = (uint16_t*) heap_caps_malloc(
        320 * 240 * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        ESP_LOGE(TAG, "fb alloc failed");
        while(true) vTaskDelay(1000);
    }

    uint32_t frame = 0;
    while (true) {
        int64_t t0 = esp_timer_get_time();
        switch ((frame / 60) % 4) {
            case 0: pattern_color_bars(fb); break;
            case 1: pattern_gradient(fb); break;
            case 2: pattern_checker(fb, 20); break;
            case 3: pattern_tearing_stripes(fb, frame); break;
        }
        int64_t t1 = esp_timer_get_time();
        display_blit_full(fb);
        display_wait_dma();
        int64_t t2 = esp_timer_get_time();
        if ((frame % 30) == 0) {
            ESP_LOGI(TAG, "frame %u: fill=%lld us blit=%lld us",
                     (unsigned)frame, (long long)(t1 - t0), (long long)(t2 - t1));
        }
        ++frame;
        vTaskDelay(pdMS_TO_TICKS(33)); // ~30 fps demo
    }
}
```

- [ ] **Step 4: Build, flash, observar**

Run: `idf.py build flash monitor`

Verificação manual no display:
- 4 padrões alternando a cada 2 s
- Cores nítidas (color bars: branco, amarelo, ciano, verde, magenta, vermelho, azul, preto)
- Gradiente suave
- Checker sem bandas escuras
- Tearing pattern: faixas deslocando — observar se há linha visível "rasgando" no meio

Log esperado:
```
frame N: fill=12345 us blit=29000 us
```

Anotar o tempo de blit. @ 40 MHz esperado ~30 ms; @ 80 MHz ~15 ms.

- [ ] **Step 5: Commit**

```bash
git add components/display/test_patterns.cpp main/app_main.cpp components/display/include/display.h
git commit -m "feat: static test patterns + 30fps demo loop"
```

---

### Task 6: Testar SPI a 80 MHz

**Files:**
- Modify: `main/app_main.cpp` (1 linha)

- [ ] **Step 1: Trocar `LCD_SPI_HZ_SAFE` por `LCD_SPI_HZ_TARGET`**

No `app_main.cpp`, mudar a chamada:

```cpp
if (!display_init(LCD_SPI_HZ_TARGET)) {
    // ...
}
```

- [ ] **Step 2: Build e flash**

Run: `idf.py build flash monitor`

- [ ] **Step 3: Verificação**

Observar:
- Log "lcd ok 320x240 @ 80000000 Hz"
- Tempo de blit no log: esperado ~15 ms
- Display ainda renderiza padrões corretamente (sem lixo, sem snowmix)

Se aparecer lixo na tela ou cores erradas: clone do ILI9341 não aguenta 80 MHz. Voltar para `LCD_SPI_HZ_SAFE` e documentar abaixo.

- [ ] **Step 4: Documentar resultado**

Edite `docs/superpowers/specs/2026-05-26-receptor-design.md` §3.2 substituindo a frase do clock SPI por uma das duas:

```
- **Clock SPI**: 80 MHz validado no hw real (Sprint 1)
```
ou
```
- **Clock SPI**: 40 MHz; 80 MHz testado e instável neste clone (Sprint 1)
```

- [ ] **Step 5: Commit**

```bash
git add main/app_main.cpp docs/superpowers/specs/2026-05-26-receptor-design.md
git commit -m "test: validate spi clock at target frequency"
```

---

### Task 7: Allocator helper para framebuffers em PSRAM

**Files:**
- Modify: `components/display/include/display.h`
- Modify: `components/display/display.cpp`

Justificativa: Sprint 2 vai precisar de framebuffers em PSRAM. Vamos preparar o helper agora.

- [ ] **Step 1: Adicionar em `display.h`**

```cpp
// Aloca um framebuffer 320*240*2 = 153600 bytes em PSRAM (DMA-capable).
// Retorna nullptr se falhar.
uint16_t* display_alloc_framebuffer_psram(void);
void display_free_framebuffer(uint16_t* fb);
```

- [ ] **Step 2: Implementar em `display.cpp`**

```cpp
#include "esp_heap_caps.h"

uint16_t* display_alloc_framebuffer_psram(void) {
    const size_t bytes = 320 * 240 * sizeof(uint16_t);
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!p) {
        // Tenta sem DMA flag (algumas variantes não suportam DMA em PSRAM).
        p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    return (uint16_t*)p;
}

void display_free_framebuffer(uint16_t* fb) {
    if (fb) heap_caps_free(fb);
}
```

- [ ] **Step 3: Smoke test no main**

Adicionar antes do loop em `app_main.cpp`:

```cpp
uint16_t* psram_fb = display_alloc_framebuffer_psram();
if (psram_fb) {
    ESP_LOGI(TAG, "psram fb ok at %p (free spiram=%u)",
             psram_fb, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    display_free_framebuffer(psram_fb);
} else {
    ESP_LOGE(TAG, "psram fb alloc failed");
}
```

- [ ] **Step 4: Build, flash, validar log**

Run: `idf.py build flash monitor`
Expected log:
```
psram fb ok at 0x3Cxxxxxx (free spiram=8000000+)
```

Endereço começando em `0x3C` confirma PSRAM. Memória livre > 7 MB.

- [ ] **Step 5: Commit**

```bash
git add components/display/
git commit -m "feat: psram framebuffer allocator helper"
```

---

## Critérios de aceitação do Sprint 1

- [ ] `idf.py build` completa sem warnings críticos
- [ ] Boot mostra log "lcd ok 320x240 @ <freq> Hz"
- [ ] 4 padrões de teste rotacionam no display visualmente corretos
- [ ] Tempo de blit medido no log < 33 ms @ clock escolhido
- [ ] Allocator PSRAM retorna ponteiro em `0x3Cxxxxxx`
- [ ] Tearing baseline anotado (observação visual do `pattern_tearing_stripes`) — guarda como referência para sprint 7

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| Clone do ILI9341 com pinagem MOSI/MISO trocada | Conferir multímetro contra silkscreen; ajustar `pinout.h` |
| Backlight invertido | Trocar `invert` em `Light_PWM::config` |
| 80 MHz falha → cair para 40 MHz | Esperado — Task 6 documenta o resultado real |
| PSRAM não enumerada | Verificar `CONFIG_SPIRAM_MODE_OCT=y` em `sdkconfig.defaults` (N16R8 é octal) |

## Self-Review (autor)

- Spec §3.3 pinout coberto pela Task 2
- Spec §3.2 clock 40/80 MHz coberto pela Task 6
- Spec §4.4 swap front/back: ainda não implementado neste sprint (Sprint 2)
- Spec §8 alocação PSRAM: helper na Task 7, uso real em Sprint 2

Nenhum placeholder. Tipos consistentes. Sem dependências circulares de componentes.
