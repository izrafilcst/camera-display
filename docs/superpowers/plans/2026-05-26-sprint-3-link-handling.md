# Sprint 3 — Tratamento de Link Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detectar perda de link e mudar entre os estados CONNECTED / FREEZE / DISCONNECTED. Mostrar último frame em FREEZE; tela de status + thumb em DISCONNECTED. Reconexão passiva ao voltar pacotes.

**Architecture:** Novo componente `link_state` mantém estado e timestamp do último pacote. Pipeline de vídeo consulta antes de cada `render_present`. Render passa a oferecer dois modos: pass-through (CONNECTED) e overlay-de-status (FREEZE/DISCONNECTED). Sem mudanças na recepção/decoder.

**Tech Stack:** FreeRTOS timers, atomic vars (`std::atomic<uint32_t>`), LovyanGFX para desenho de texto e thumbnail.

---

## Reference do spec
- §7 Tratamento de link

## File Structure

```
components/
├── link_state/
│   ├── CMakeLists.txt
│   ├── include/link_state.h
│   ├── link_state.cpp
│   └── host_tests/test_link_state.cpp
└── render/
    └── render.cpp   (modificado para overlay de status)
```

---

### Task 1: Definir API e testes do `link_state`

**Files:**
- Create: `components/link_state/include/link_state.h`
- Create: `components/link_state/CMakeLists.txt`
- Create: `components/link_state/host_tests/test_link_state.cpp`

- [ ] **Step 1: Criar `link_state.h`**

```cpp
#pragma once
#include <cstdint>

enum link_status_t : uint8_t {
    LINK_BOOT         = 0,
    LINK_CONNECTED    = 1,
    LINK_FREEZE       = 2,
    LINK_DISCONNECTED = 3,
};

// Thresholds em ms (consistentes com §7).
constexpr uint32_t LINK_FREEZE_MS       = 200;
constexpr uint32_t LINK_DISCONNECT_MS   = 3000;

bool link_state_init(void);

// Avisa que pacote válido chegou agora (ms).
void link_state_mark_rx(uint32_t now_ms);

// Calcula estado para now_ms.
link_status_t link_state_query(uint32_t now_ms);

// Idade do último pacote.
uint32_t link_state_idle_ms(uint32_t now_ms);
```

- [ ] **Step 2: `components/link_state/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "link_state.cpp"
    INCLUDE_DIRS "include"
)
```

- [ ] **Step 3: Testes Unity**

`components/link_state/host_tests/test_link_state.cpp`:

```cpp
#include "unity.h"
#include "link_state.h"

TEST_CASE("boot is LINK_BOOT until first packet", "[link]") {
    link_state_init();
    TEST_ASSERT_EQUAL(LINK_BOOT, link_state_query(0));
    TEST_ASSERT_EQUAL(LINK_BOOT, link_state_query(5000));
}

TEST_CASE("connected immediately after first packet", "[link]") {
    link_state_init();
    link_state_mark_rx(100);
    TEST_ASSERT_EQUAL(LINK_CONNECTED, link_state_query(150));
}

TEST_CASE("freeze after 200ms idle", "[link]") {
    link_state_init();
    link_state_mark_rx(0);
    TEST_ASSERT_EQUAL(LINK_CONNECTED, link_state_query(199));
    TEST_ASSERT_EQUAL(LINK_FREEZE,    link_state_query(201));
    TEST_ASSERT_EQUAL(LINK_FREEZE,    link_state_query(2999));
}

TEST_CASE("disconnected after 3000ms idle", "[link]") {
    link_state_init();
    link_state_mark_rx(0);
    TEST_ASSERT_EQUAL(LINK_FREEZE,       link_state_query(2999));
    TEST_ASSERT_EQUAL(LINK_DISCONNECTED, link_state_query(3001));
}

TEST_CASE("recovery to connected on new packet", "[link]") {
    link_state_init();
    link_state_mark_rx(0);
    TEST_ASSERT_EQUAL(LINK_DISCONNECTED, link_state_query(5000));
    link_state_mark_rx(5100);
    TEST_ASSERT_EQUAL(LINK_CONNECTED, link_state_query(5150));
}
```

- [ ] **Step 4: Commit**

```bash
git add components/link_state/
git commit -m "test: link_state state-machine tests"
```

---

### Task 2: Implementar `link_state`

**Files:**
- Create: `components/link_state/link_state.cpp`

- [ ] **Step 1: Implementar**

```cpp
#include "link_state.h"
#include <atomic>

static std::atomic<uint32_t> s_last_rx_ms{0};
static std::atomic<bool>     s_has_ever_received{false};

bool link_state_init(void) {
    s_last_rx_ms = 0;
    s_has_ever_received = false;
    return true;
}

void link_state_mark_rx(uint32_t now_ms) {
    s_last_rx_ms = now_ms;
    s_has_ever_received = true;
}

uint32_t link_state_idle_ms(uint32_t now_ms) {
    if (!s_has_ever_received) return 0xFFFFFFFFu;
    uint32_t t = s_last_rx_ms.load();
    return now_ms >= t ? (now_ms - t) : 0;
}

link_status_t link_state_query(uint32_t now_ms) {
    if (!s_has_ever_received) return LINK_BOOT;
    uint32_t idle = now_ms - s_last_rx_ms.load();
    if (idle > LINK_DISCONNECT_MS) return LINK_DISCONNECTED;
    if (idle > LINK_FREEZE_MS)     return LINK_FREEZE;
    return LINK_CONNECTED;
}
```

- [ ] **Step 2: Build target**

Run: `idf.py build`

- [ ] **Step 3: Commit**

```bash
git add components/link_state/
git commit -m "feat: link_state machine"
```

---

### Task 3: Marcar RX no pipeline

**Files:**
- Modify: `main/app_main.cpp`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Atualizar `main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES display espnow_link reassembly decoder render link_state
)
```

- [ ] **Step 2: Em `app_main.cpp`, marcar RX no callback**

No início de `on_msg`:

```cpp
static void on_msg(uint8_t msg_type, const uint8_t* payload, size_t len, int8_t rssi) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    link_state_mark_rx(now_ms);
    if (msg_type != MSG_VIDEO_FRAG) return;
    // ... resto igual
}
```

Adicione `#include "link_state.h"` no topo.
No `app_main`, antes do `espnow_link_init`:

```cpp
link_state_init();
```

- [ ] **Step 3: Build, flash, validar**

Run: `idf.py build flash monitor`
Com TX rodando: vídeo normal.
Desligue o TX por 5 s e religue. (Por enquanto a UI não muda — só validamos que o estado seria atualizado.)

- [ ] **Step 4: Commit**

```bash
git add main/
git commit -m "feat: mark rx timestamps in link_state"
```

---

### Task 4: Capturar último frame válido (thumb)

**Files:**
- Modify: `components/render/include/render.h`
- Modify: `components/render/render.cpp`

Razão: durante DISCONNECTED queremos mostrar um thumb 80×60. Captura assim que um frame válido renderiza.

- [ ] **Step 1: Adicionar em `render.h`**

```cpp
// Captura cópia 80x60 do front buffer corrente para uso em tela DISCONNECTED.
// Chamada após render_present. Não-bloqueante (best-effort).
void render_capture_thumb(void);

// Renderiza overlay de status sobre o display. Bloqueante.
void render_show_freeze(void);
void render_show_disconnected(uint32_t since_ms);
```

- [ ] **Step 2: Implementar em `render.cpp`**

Adicione no topo:

```cpp
static uint16_t s_thumb[80 * 60];
static bool s_thumb_valid = false;
```

E as três funções:

```cpp
void render_capture_thumb(void) {
    // Downsample 4x do FB recém-presentado (s_fb[s_back_idx ^ 1]).
    const uint16_t* src = s_fb[s_back_idx ^ 1];
    if (!src) return;
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 80; ++x) {
            s_thumb[y * 80 + x] = src[(y * 4) * 320 + (x * 4)];
        }
    }
    s_thumb_valid = true;
}

extern "C" lgfx::LGFX_Device* display_get_lgfx_ptr(void); // forward decl

void render_show_freeze(void) {
    auto* lcd = display_get_lgfx_ptr();
    if (!lcd) return;
    lcd->startWrite();
    // Pisca um "FREEZE" pequeno no canto superior direito.
    bool blink = (esp_timer_get_time() / 250000) & 1;
    if (blink) {
        lcd->fillRect(250, 4, 66, 16, 0xF800);
        lcd->setTextColor(0xFFFF, 0xF800);
        lcd->setTextSize(2);
        lcd->setCursor(254, 6);
        lcd->print("FREEZE");
    }
    lcd->endWrite();
}

void render_show_disconnected(uint32_t since_ms) {
    auto* lcd = display_get_lgfx_ptr();
    if (!lcd) return;
    lcd->startWrite();
    lcd->fillScreen(0x1082); // cinza muito escuro
    lcd->setTextColor(0xFFFF);
    lcd->setTextSize(2);
    lcd->setCursor(60, 80);
    lcd->print("AGUARDANDO LINK");
    lcd->setTextSize(1);
    lcd->setCursor(110, 110);
    lcd->printf("offline: %lu s", (unsigned long)(since_ms / 1000));
    if (s_thumb_valid) {
        lcd->pushImage(120, 140, 80, 60, s_thumb);
        lcd->setCursor(120, 205);
        lcd->print("ultimo frame");
    }
    lcd->endWrite();
}
```

- [ ] **Step 3: Expor `display_get_lgfx_ptr`**

Em `components/display/include/display.h` adicione (dentro do extern "C" não vai funcionar com classe; deixe fora):

```cpp
namespace lgfx { class LGFX_Device; }
extern "C" lgfx::LGFX_Device* display_get_lgfx_ptr(void);
```

Em `display.cpp` adicione:

```cpp
extern "C" lgfx::LGFX_Device* display_get_lgfx_ptr(void) {
    return s_lcd;
}
```

- [ ] **Step 4: Build**

Run: `idf.py build`

- [ ] **Step 5: Commit**

```bash
git add components/render/ components/display/
git commit -m "feat: capture thumb and render link status overlays"
```

---

### Task 5: Task de estado de link na malha de render

**Files:**
- Modify: `main/app_main.cpp`

- [ ] **Step 1: Integrar consulta de estado na decode_task**

No `decode_task`, após `render_present()`:

```cpp
render_capture_thumb();
```

E criar nova task `link_ui_task`:

```cpp
static void link_ui_task(void*) {
    link_status_t last = LINK_BOOT;
    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        link_status_t st = link_state_query(now_ms);
        if (st != LINK_CONNECTED) {
            if (st == LINK_FREEZE) {
                render_show_freeze();
            } else if (st == LINK_DISCONNECTED) {
                render_show_disconnected(link_state_idle_ms(now_ms));
            }
        }
        if (st != last) {
            ESP_LOGI(TAG, "link %d -> %d", last, st);
            last = st;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

E no `app_main`, após `xTaskCreatePinnedToCore(decode_task, ...)`:

```cpp
xTaskCreatePinnedToCore(link_ui_task, "link_ui", 4096, nullptr, 4, nullptr, 1);
```

- [ ] **Step 2: Build, flash, validar**

Run: `idf.py build flash monitor`

Validação manual:
1. TX ligado → vídeo normal, log `link 0 -> 1`.
2. Desligue TX por 500 ms (pausa breve, religue): vídeo só engasga; deve registrar `link 1 -> 2` (FREEZE) e voltar para `1`.
3. Desligue TX por 5 s: aparece tela de status com thumb; após religar, vídeo volta < 2 s.

- [ ] **Step 3: Commit**

```bash
git add main/
git commit -m "feat: link_ui task to render freeze/disconnect screens"
```

---

### Task 6: Validar critério reconexão < 2 s

- [ ] **Step 1: Procedimento de teste**

Repita 5 vezes:
1. Cronômetro pronto.
2. Desligue TX.
3. Aguarde tela DISCONNECTED aparecer (~3 s).
4. Religue TX e dispare cronômetro junto.
5. Pare quando vídeo voltar.

- [ ] **Step 2: Tolerância**

Todos os 5 trials devem voltar em < 2 s. Anotar valores médio/máx no log do projeto.

- [ ] **Step 3: Marcar critério em §10 do spec**

```
- [x] Reconexão automática < 2 s após link voltar
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-26-receptor-design.md
git commit -m "docs: sprint 3 reconnect < 2s validated"
```

---

## Critérios de aceitação do Sprint 3

- [ ] Estados de link transicionam corretamente nos 3 thresholds
- [ ] FREEZE: vídeo congelado + ícone FREEZE piscando no canto
- [ ] DISCONNECTED: tela de status + thumb 80×60 + contagem de tempo offline
- [ ] Reconexão < 2 s nos 5 trials
- [ ] Testes Unity do `link_state` passam (host ou target)

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| `link_ui_task` colide com `decode_task` chamando o LCD simultaneamente | Mutex em torno do LCD (extrair para Sprint 5 ou já agora) — em V0, decoder só renderiza quando CONNECTED; UI task só renderiza quando ≠ CONNECTED. Ainda assim, transição pode colidir; aceitar piscada momentânea. |
| Thumb capturado é frame parcialmente decodificado | `render_capture_thumb` é chamado pós-DMA do front (que já foi exibido), portanto é o frame "anterior", garantidamente completo. |

## Self-Review

- §7 tabela de estados/critérios: tasks 1, 2, 4, 5
- §10 reconexão < 2s: task 6
- §4.4 sem regressão de pipeline normal: tasks integram sem mudar `decode_task` core flow

Sem placeholder. Tipos `link_status_t`, `reassembled_frame_t` consistentes entre componentes.
