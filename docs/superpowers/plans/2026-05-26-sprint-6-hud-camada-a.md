# Sprint 5 — HUD Camada A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implementar a camada A do HUD (sempre visível): status do link e bateria do robô. Overlay desenhado sobre `FB_back` a cada frame, integrado ao pipeline sem regressão de FPS.

**Architecture:** Novo componente `hud` com renderer agnóstico que escreve em RGB565 (não no LCD diretamente — deixa o blit no `render`). Estado fonte: `link_state` (já existe) e `telemetry_in` (novo, vem do TX). Sprint 5 implementa só camada A; Sprint 6 expande para camada B + menu.

**Tech Stack:** Fonte bitmap embarcada (LovyanGFX já tem; usaremos draw_string sobre canvas LGFX_Sprite mapeado para FB_back); RGB565 nativo.

---

## Reference do spec
- §6.1 Camada A
- §6.4 Layout

## File Structure

```
components/
├── telemetry_in/                  (NOVO — recebe telemetria do TX)
│   ├── CMakeLists.txt
│   ├── include/telemetry_in.h
│   └── telemetry_in.cpp
└── hud/                            (NOVO)
    ├── CMakeLists.txt
    ├── include/hud.h
    ├── hud.cpp
    └── glyphs.cpp                  (utilitários de desenho)
```

---

### Task 1: `telemetry_in` — receber telemetria do TX

Suposição: o TX envia uma estrutura `tx_to_rx_telemetry` periodicamente (1–2 Hz) com pelo menos bateria, e futuramente GPS/IMU. Em V0 só o campo `battery_pct` é populado de verdade.

**Files:**
- Create: `components/telemetry_in/include/telemetry_in.h`
- Create: `components/telemetry_in/telemetry_in.cpp`
- Create: `components/telemetry_in/CMakeLists.txt`

- [ ] **Step 1: Definir struct e API**

`telemetry_in.h`:

```cpp
#pragma once
#include <cstdint>

// Wire format do TX→RX em MSG_TELEMETRY (msg_type pode ser 0x21 — telem-in, distinto do 0x20 que sai do RX).
// Confirmar com o TX team. Em V0 dimensionamos com folga para futuros campos.
struct __attribute__((packed)) tx_to_rx_telemetry_t {
    uint8_t  msg_type;       // 0x21 (sugestão; alinhar com TX)
    uint8_t  seq;
    uint8_t  battery_pct;    // 0..100, 0xFF=N/A
    uint8_t  current_level;  // L0..L4 (eco)
    int16_t  rssi_at_tx;     // RSSI visto pelo TX (do RX)
    uint8_t  flags;
    // Reservados — preenchidos quando sensores existirem:
    int32_t  gps_lat_e7;     // INT32_MIN = N/A
    int32_t  gps_lon_e7;
    int16_t  heading_deg10;  // 0..3599 (décimos), -1 = N/A
    int16_t  pitch_deg10;
    int16_t  roll_deg10;
    int16_t  altitude_m;
};

void telemetry_in_init(void);

// Chamado pelo dispatch ao receber MSG_TELEMETRY (tipo 0x21).
void telemetry_in_on_msg(const uint8_t* payload, int len);

// Snapshot atômico para o HUD ler.
struct hud_data_snapshot_t {
    uint8_t  battery_pct;
    uint8_t  current_level;
    int16_t  rssi_at_tx;
    int32_t  gps_lat_e7;
    int32_t  gps_lon_e7;
    int16_t  heading_deg10;
    bool     gps_valid;
    bool     heading_valid;
    uint32_t last_update_ms;
};
hud_data_snapshot_t telemetry_in_snapshot(uint32_t now_ms);
```

- [ ] **Step 2: Implementar**

`telemetry_in.cpp`:

```cpp
#include "telemetry_in.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <atomic>

static const char* TAG = "tel_in";
static std::atomic<uint32_t> s_last_update{0};
static tx_to_rx_telemetry_t s_last{};

void telemetry_in_init(void) {
    memset(&s_last, 0, sizeof(s_last));
    s_last.battery_pct = 0xFF;
    s_last.current_level = 0;
    s_last.gps_lat_e7 = INT32_MIN;
    s_last.gps_lon_e7 = INT32_MIN;
    s_last.heading_deg10 = -1;
}

void telemetry_in_on_msg(const uint8_t* payload, int len) {
    if (len < (int)sizeof(tx_to_rx_telemetry_t)) return;
    memcpy(&s_last, payload, sizeof(s_last));
    s_last_update = (uint32_t)(esp_timer_get_time() / 1000);
}

hud_data_snapshot_t telemetry_in_snapshot(uint32_t now_ms) {
    hud_data_snapshot_t s{};
    s.battery_pct    = s_last.battery_pct;
    s.current_level  = s_last.current_level;
    s.rssi_at_tx     = s_last.rssi_at_tx;
    s.gps_lat_e7     = s_last.gps_lat_e7;
    s.gps_lon_e7     = s_last.gps_lon_e7;
    s.heading_deg10  = s_last.heading_deg10;
    s.gps_valid      = (s_last.gps_lat_e7 != INT32_MIN);
    s.heading_valid  = (s_last.heading_deg10 >= 0);
    s.last_update_ms = s_last_update.load();
    return s;
}
```

- [ ] **Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "telemetry_in.cpp"
    INCLUDE_DIRS "include"
)
```

- [ ] **Step 4: Dispatch em `on_msg`**

Em `app_main.cpp`, na função `on_msg`, adicionar bloco antes do `if (msg_type != MSG_VIDEO_FRAG)`:

```cpp
if (msg_type == 0x21 /* TX telem to RX */) {
    telemetry_in_on_msg(payload, (int)len);
    return;
}
```

E em `app_main` antes dos demais inits:

```cpp
telemetry_in_init();
```

Adicionar `telemetry_in` aos REQUIRES do main.

- [ ] **Step 5: Build**

Run: `idf.py build`

- [ ] **Step 6: Commit**

```bash
git add components/telemetry_in/ main/
git commit -m "feat: telemetry_in receives tx state (battery/level/gps)"
```

---

### Task 2: `hud` — overlay camada A

**Files:**
- Create: `components/hud/include/hud.h`
- Create: `components/hud/hud.cpp`
- Create: `components/hud/CMakeLists.txt`

- [ ] **Step 1: API**

`hud.h`:

```cpp
#pragma once
#include <cstdint>
#include "link_state.h"
#include "telemetry_in.h"

bool hud_init(void);

// Desenha a camada A sobre um buffer 320x240 RGB565. Idempotente.
void hud_draw_layer_a(uint16_t* buf,
                      link_status_t link,
                      const hud_data_snapshot_t& tx_data,
                      uint32_t now_ms);
```

- [ ] **Step 2: Implementar `hud.cpp` usando LGFX_Sprite mapeado**

```cpp
#include "hud.h"
#include "display.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <cstdio>

static LGFX_Sprite s_canvas; // espelha o buffer; criado em init com 0 alloc.

bool hud_init(void) {
    // Sem alloc interna; usaremos createFromBuffer no draw.
    return true;
}

static uint16_t color_for_link(link_status_t st) {
    switch (st) {
        case LINK_CONNECTED:    return 0x07E0; // verde
        case LINK_FREEZE:       return 0xFD20; // âmbar
        case LINK_DISCONNECTED: return 0xF800; // vermelho
        default:                return 0x8410; // cinza
    }
}

static const char* label_for_link(link_status_t st) {
    switch (st) {
        case LINK_CONNECTED:    return "LINK";
        case LINK_FREEZE:       return "FRZ ";
        case LINK_DISCONNECTED: return "OFF ";
        default:                return "----";
    }
}

void hud_draw_layer_a(uint16_t* buf, link_status_t link,
                      const hud_data_snapshot_t& tx, uint32_t now_ms) {
    if (!buf) return;
    s_canvas.setBuffer(buf, 320, 240, 16);   // RGB565 zero-copy

    // Bloco topo-esquerda (link)
    uint16_t lc = color_for_link(link);
    s_canvas.fillRect(2, 2, 56, 16, 0x0000);
    s_canvas.drawRect(2, 2, 56, 16, lc);
    s_canvas.setTextColor(lc, 0x0000);
    s_canvas.setTextSize(1);
    s_canvas.setCursor(8, 6);
    s_canvas.print(label_for_link(link));

    // Bateria
    char bat_str[12];
    if (tx.battery_pct == 0xFF) {
        snprintf(bat_str, sizeof(bat_str), "BAT --");
    } else {
        snprintf(bat_str, sizeof(bat_str), "BAT %u%%", (unsigned)tx.battery_pct);
    }
    uint16_t bc = 0xFFFF;
    if (tx.battery_pct < 20) bc = 0xF800;        // vermelho < 20%
    else if (tx.battery_pct < 40) bc = 0xFD20;   // âmbar < 40%
    s_canvas.fillRect(62, 2, 64, 16, 0x0000);
    s_canvas.setTextColor(bc, 0x0000);
    s_canvas.setCursor(66, 6);
    s_canvas.print(bat_str);
}
```

- [ ] **Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "hud.cpp"
    INCLUDE_DIRS "include"
    REQUIRES display link_state telemetry_in lovyan03__LovyanGFX
)
```

- [ ] **Step 4: Integrar no `decode_task`**

Em `app_main.cpp`, dentro de `decode_task` após o decode mas antes de `render_present`:

```cpp
#include "hud.h"
// ...
uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
hud_data_snapshot_t tx_snap = telemetry_in_snapshot(now_ms);
hud_draw_layer_a(back, link_state_query(now_ms), tx_snap, now_ms);
render_present();
```

E `hud_init()` em `app_main`. Adicione `hud` aos REQUIRES do main.

- [ ] **Step 5: Build, flash, observar**

Run: `idf.py build flash monitor`

Validação visual:
- Canto superior-esquerdo do vídeo mostra retângulo "LINK" em verde quando link OK
- "BAT --" enquanto TX não envia telemetria de bateria; "BAT NN%" assim que o TX começar a enviar
- Bate em cima do vídeo (não num modal); vídeo continua atrás

- [ ] **Step 6: Commit**

```bash
git add components/hud/ main/
git commit -m "feat: hud layer A (link status + robot battery)"
```

---

### Task 3: Verificar regressão de FPS

- [ ] **Step 1: Comparar antes/depois**

Anote o `fps` médio do log do main rodando por 60 s **com** HUD overlay:
- Esperado: fps ≥ 23.5 (mesmo do Sprint 2)
- Se cair para < 22, investigar: tempo de `hud_draw_layer_a` deve ser < 1 ms — medir com `esp_timer_get_time` ao redor da chamada.

- [ ] **Step 2: Instrumentação temporária**

Adicionar log a cada ~5 s no decode_task:

```cpp
int64_t t_hud0 = esp_timer_get_time();
hud_draw_layer_a(back, ..., now_ms);
int64_t t_hud1 = esp_timer_get_time();
if ((rf.frame.frame_id % 120) == 0) {
    ESP_LOGI(TAG, "hud=%lld us", (long long)(t_hud1 - t_hud0));
}
```

Esperado: ~200–800 µs.

- [ ] **Step 3: Remover instrumentação**

Após confirmar, remover os logs e fazer:

```bash
git checkout -- main/app_main.cpp # ou edite manualmente para tirar
```

(Ou deixe atrás de um `#if HUD_PROFILE` — escolha do dev.)

- [ ] **Step 4: Commit**

```bash
git add main/
git commit -m "perf: confirm hud layer A within budget (<1ms)"
```

---

### Task 4: Mutex de proteção do LCD (com `link_ui_task`)

Sprint 3 deixou um conflito potencial: `decode_task` (Core 1) chama `render_present` enquanto `link_ui_task` (Core 1) pode chamar `render_show_freeze` no mesmo periférico SPI. Vamos resolver agora.

**Files:**
- Modify: `components/render/include/render.h`
- Modify: `components/render/render.cpp`

- [ ] **Step 1: Adicionar mutex e expor lock/unlock**

`render.cpp`: o `s_mutex` já existe; expandir para também envolver `render_show_*`:

```cpp
void render_show_freeze(void) {
    auto* lcd = display_get_lgfx_ptr();
    if (!lcd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_wait_dma();
    lcd->startWrite();
    // ... (código existente)
    lcd->endWrite();
    xSemaphoreGive(s_mutex);
}

void render_show_disconnected(uint32_t since_ms) {
    auto* lcd = display_get_lgfx_ptr();
    if (!lcd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_wait_dma();
    lcd->startWrite();
    // ... (código existente)
    lcd->endWrite();
    xSemaphoreGive(s_mutex);
}
```

- [ ] **Step 2: Build, flash**

Run: `idf.py build flash monitor`

Cenário de teste: desligue o TX por 1 s e religue, várias vezes. Não deve haver glitch visual nem trava do display.

- [ ] **Step 3: Commit**

```bash
git add components/render/
git commit -m "fix: protect LCD access across render and link_ui tasks"
```

---

## Critérios de aceitação do Sprint 5

- [ ] HUD camada A visível: status do link + bateria
- [ ] Cor do status muda corretamente (verde/âmbar/vermelho)
- [ ] Cor da bateria muda em < 40% (âmbar) e < 20% (vermelho)
- [ ] fps mantém ≥ 23.5 com HUD ativo (sem regressão de Sprint 2)
- [ ] LCD não trava nem mostra glitch durante transição CONNECTED ↔ FREEZE

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| `LGFX_Sprite::setBuffer` em buffer PSRAM tem performance ruim | Medir; se necessário, mover overlay para depois do decode no FB que está em "compute" enquanto front faz DMA. Já é o caso. |
| Conflito de msg_type com TX (0x21 não acordado) | Confirmar com TX team; pode ser 0x20 reservado bidirecional. |
| Bateria do robô não disponível no TX em V0 | OK, ficamos com `0xFF` ("BAT --"); o pipeline está pronto. |

## Self-Review

- §6.1 Camada A (status + bateria): Task 1 + 2
- §6.4 layout posições fixas: Task 2 implementa as 2 caixas do topo-esquerda
- §10 sem regressão de FPS: Task 3
- §7 sem regressão de FREEZE/DISCONNECTED: Task 4 protege LCD

Sem placeholder. Conflito mutex resolvido inline. Tipos `hud_data_snapshot_t` definidos em telemetry_in e usados em hud.
