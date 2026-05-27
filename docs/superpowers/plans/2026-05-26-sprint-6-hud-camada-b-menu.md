# Sprint 6 — HUD Camada B + Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expandir o HUD com itens toggleáveis (FPS, latência, RSSI, nível adaptive, drop rate, crosshair) e implementar o menu de configuração navegável via joystick + 2 botões com persistência em NVS.

**Architecture:** 2 componentes novos: `nvs_settings` (load/save) e `menu` (state machine + renderer). HUD ganha funções para desenhar cada item da camada B condicionado em `settings.enabled_bits`. UI state machine transita FLIGHT ↔ MENU; durante MENU, `input.set_ui_state(UI_MENU)` zera joystick out.

**Tech Stack:** ESP-IDF NVS (esp_partition + nvs_flash), LovyanGFX para menu, estado em memória + flush no toggle.

---

## Reference do spec
- §6.1 Camada B (itens)
- §6.2 Persistência NVS
- §6.3 Navegação
- §6.4 Layout (rodapé + crosshair)

## File Structure

```
components/
├── nvs_settings/
│   ├── CMakeLists.txt
│   ├── include/nvs_settings.h
│   └── nvs_settings.cpp
├── menu/
│   ├── CMakeLists.txt
│   ├── include/menu.h
│   └── menu.cpp
└── hud/                       (expandido)
    └── hud.cpp                (adiciona draw_layer_b)
```

---

### Task 1: `nvs_settings` — persistência

**Files:**
- Create: `components/nvs_settings/include/nvs_settings.h`
- Create: `components/nvs_settings/nvs_settings.cpp`
- Create: `components/nvs_settings/CMakeLists.txt`

- [ ] **Step 1: API**

`nvs_settings.h`:

```cpp
#pragma once
#include <cstdint>

// Bits da camada B do HUD.
enum hud_bit_t : uint32_t {
    HUD_FPS         = 1u << 0,
    HUD_LATENCY     = 1u << 1,
    HUD_RSSI        = 1u << 2,
    HUD_ADAPTIVE    = 1u << 3,
    HUD_DROP        = 1u << 4,
    HUD_CROSSHAIR   = 1u << 5,
    HUD_HEADING     = 1u << 6,  // placeholder
    HUD_GPS         = 1u << 7,  // placeholder
    HUD_BEARING     = 1u << 8,  // placeholder
    HUD_HORIZON     = 1u << 9,  // placeholder
};

struct hud_settings_t {
    uint32_t enabled_bits;
    uint8_t  max_auto_level;   // 0..4, padrão 2
    uint8_t  version;          // schema
};

constexpr uint8_t  SETTINGS_VERSION = 1;
constexpr uint32_t SETTINGS_DEFAULT_BITS = HUD_FPS | HUD_LATENCY | HUD_RSSI;

bool nvs_settings_init(void);
hud_settings_t nvs_settings_load(void);
bool nvs_settings_save(const hud_settings_t& s);
```

- [ ] **Step 2: Implementar**

`nvs_settings.cpp`:

```cpp
#include "nvs_settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "settings";
static const char* NS = "hud";

bool nvs_settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

hud_settings_t nvs_settings_load(void) {
    nvs_handle_t h;
    hud_settings_t s{ SETTINGS_DEFAULT_BITS, 2, SETTINGS_VERSION };
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return s;
    size_t sz = sizeof(s);
    if (nvs_get_blob(h, "all", &s, &sz) != ESP_OK || sz != sizeof(s) || s.version != SETTINGS_VERSION) {
        s = { SETTINGS_DEFAULT_BITS, 2, SETTINGS_VERSION };
    }
    nvs_close(h);
    return s;
}

bool nvs_settings_save(const hud_settings_t& s) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_set_blob(h, "all", &s, sizeof(s)) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}
```

- [ ] **Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "nvs_settings.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
```

- [ ] **Step 4: Smoke**

Em `app_main.cpp` antes de demais inits:

```cpp
nvs_settings_init();
hud_settings_t cfg = nvs_settings_load();
ESP_LOGI(TAG, "settings: bits=0x%08x max_auto=%u v=%u",
         (unsigned)cfg.enabled_bits, cfg.max_auto_level, cfg.version);
```

Adicionar `nvs_settings` aos REQUIRES.

Run: `idf.py build flash monitor`. Esperado: log "settings: bits=0x00000007 max_auto=2 v=1" no primeiro boot.

- [ ] **Step 5: Commit**

```bash
git add components/nvs_settings/ main/
git commit -m "feat: nvs settings persistence"
```

---

### Task 2: Stats agregadas para o HUD

O HUD precisa de FPS, latência, RSSI, drop, level — esses dados vivem em `telemetry` e `reassembly`. Vamos expor um snapshot consolidado.

**Files:**
- Modify: `components/telemetry/include/telemetry.h`
- Modify: `components/telemetry/telemetry.cpp`

- [ ] **Step 1: Adicionar snapshot leve em `telemetry.h`**

```cpp
struct telemetry_live_snapshot_t {
    uint16_t fps_1s;            // frames_received_1s da última janela fechada
    uint16_t latency_p50_ms;
    uint16_t latency_p99_ms;
    int8_t   rssi_avg_dbm;
    uint16_t drops_1s;
    uint8_t  current_level;
    uint8_t  requested_level;
};
telemetry_live_snapshot_t telemetry_live(void);
```

- [ ] **Step 2: Implementar**

Em `telemetry.cpp`, manter cópia do payload mais recente após cada `telemetry_tick_1s`:

```cpp
static telemetry_rx_to_tx_payload s_last_payload{};

// no fim de telemetry_tick_1s, antes do return:
s_last_payload = p;
return p;

// adicione:
telemetry_live_snapshot_t telemetry_live(void) {
    telemetry_live_snapshot_t s{};
    s.fps_1s          = s_last_payload.frames_received_1s;
    s.latency_p50_ms  = s_last_payload.latency_p50_ms;
    s.latency_p99_ms  = s_last_payload.latency_p99_ms;
    s.rssi_avg_dbm    = s_last_payload.rssi_avg_dbm;
    s.drops_1s        = s_last_payload.frames_dropped_1s;
    s.current_level   = s_last_payload.current_level_seen;
    s.requested_level = s_last_payload.requested_level;
    return s;
}
```

- [ ] **Step 3: Build**

Run: `idf.py build`

- [ ] **Step 4: Commit**

```bash
git add components/telemetry/
git commit -m "feat: telemetry_live snapshot for hud consumers"
```

---

### Task 3: HUD camada B — desenho condicional

**Files:**
- Modify: `components/hud/include/hud.h`
- Modify: `components/hud/hud.cpp`

- [ ] **Step 1: Adicionar em `hud.h`**

```cpp
#include "nvs_settings.h"
#include "telemetry.h"

void hud_draw_layer_b(uint16_t* buf,
                      const hud_settings_t& cfg,
                      const telemetry_live_snapshot_t& live,
                      const hud_data_snapshot_t& tx_data);
```

- [ ] **Step 2: Implementar em `hud.cpp`**

```cpp
void hud_draw_layer_b(uint16_t* buf, const hud_settings_t& cfg,
                      const telemetry_live_snapshot_t& live,
                      const hud_data_snapshot_t& tx) {
    s_canvas.setBuffer(buf, 320, 240, 16);

    // Rodapé: fundo escurecido para legibilidade
    s_canvas.fillRect(0, 222, 320, 18, 0x10A2);  // azul muito escuro semi-translúcido

    int x = 4;
    char str[24];
    s_canvas.setTextColor(0xFFFF, 0x10A2);
    s_canvas.setTextSize(1);

    auto draw_kv = [&](const char* label, const char* val) {
        snprintf(str, sizeof(str), "%s:%s", label, val);
        s_canvas.setCursor(x, 227);
        s_canvas.print(str);
        x += (int)strlen(str) * 6 + 6;
    };

    if (cfg.enabled_bits & HUD_FPS) {
        snprintf(str, sizeof(str), "%u", live.fps_1s);
        draw_kv("FPS", str);
    }
    if (cfg.enabled_bits & HUD_LATENCY) {
        snprintf(str, sizeof(str), "%u", live.latency_p50_ms);
        draw_kv("LAT", str);
    }
    if (cfg.enabled_bits & HUD_RSSI) {
        snprintf(str, sizeof(str), "%d", live.rssi_avg_dbm);
        draw_kv("RSSI", str);
    }
    if (cfg.enabled_bits & HUD_ADAPTIVE) {
        snprintf(str, sizeof(str), "L%u", live.current_level);
        draw_kv("LV", str);
    }
    if (cfg.enabled_bits & HUD_DROP) {
        snprintf(str, sizeof(str), "%u", live.drops_1s);
        draw_kv("DRP", str);
    }

    // Crosshair (centro)
    if (cfg.enabled_bits & HUD_CROSSHAIR) {
        const int cx = 160, cy = 120;
        s_canvas.drawFastHLine(cx - 8, cy, 16, 0xFFFF);
        s_canvas.drawFastVLine(cx, cy - 8, 16, 0xFFFF);
        s_canvas.drawCircle(cx, cy, 4, 0xFFFF);
    }

    // Placeholders direcionais — dimmed se não houver dado
    if (cfg.enabled_bits & HUD_HEADING) {
        if (tx.heading_valid) {
            snprintf(str, sizeof(str), "%03u", (unsigned)(tx.heading_deg10 / 10));
            s_canvas.setTextColor(0xFFFF, 0x0000);
        } else {
            snprintf(str, sizeof(str), "---");
            s_canvas.setTextColor(0x8410, 0x0000);
        }
        s_canvas.setCursor(150, 22);
        s_canvas.print(str);
    }
}
```

- [ ] **Step 3: Integrar no decode_task**

Em `app_main.cpp` após `hud_draw_layer_a`:

```cpp
auto live = telemetry_live();
hud_draw_layer_b(back, g_settings, live, tx_snap);
```

Onde `g_settings` é uma variável global atualizada:

```cpp
static hud_settings_t g_settings;
// em app_main, após nvs_settings_init:
g_settings = nvs_settings_load();
```

- [ ] **Step 4: Build, flash, observar**

Run: `idf.py build flash monitor`
Validação visual:
- Rodapé com `FPS:24 LAT:42 RSSI:-65`
- (Sem crosshair ainda — bits default não tem HUD_CROSSHAIR)

- [ ] **Step 5: Commit**

```bash
git add components/hud/ main/
git commit -m "feat: hud layer B with toggleable items"
```

---

### Task 4: `menu` — state machine

**Files:**
- Create: `components/menu/include/menu.h`
- Create: `components/menu/menu.cpp`
- Create: `components/menu/CMakeLists.txt`

- [ ] **Step 1: API**

`menu.h`:

```cpp
#pragma once
#include <cstdint>
#include "input.h"
#include "nvs_settings.h"

bool menu_init(hud_settings_t* shared_cfg);

// Chamado a 30 Hz. Recebe eventos do input e snapshot.
// Retorna true se menu está aberto agora (precisa desenhar overlay).
bool menu_tick(input_event_t ev, const input_snapshot_t& snap, uint32_t now_ms);

// Desenha o menu sobre o buffer. Só chama se menu_tick retornou true.
void menu_draw(uint16_t* buf);
```

- [ ] **Step 2: Estado e itens**

`menu.cpp`:

```cpp
#include "menu.h"
#include "esp_log.h"
#include <cstring>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static const char* TAG = "menu";

struct menu_item_t {
    const char* label;
    uint32_t bit;     // HUD_* ou 0 para non-toggle
};

static menu_item_t s_items[] = {
    {"FPS",            HUD_FPS},
    {"Latencia",       HUD_LATENCY},
    {"RSSI",           HUD_RSSI},
    {"Nivel L",        HUD_ADAPTIVE},
    {"Drop rate",      HUD_DROP},
    {"Crosshair",      HUD_CROSSHAIR},
    {"Heading*",       HUD_HEADING},
    {"GPS*",           HUD_GPS},
    {"Bearing*",       HUD_BEARING},
    {"Horizonte*",     HUD_HORIZON},
    // *itens dimmed enquanto dados não chegam
};
static const int N_ITEMS = sizeof(s_items)/sizeof(s_items[0]);

static bool          s_open = false;
static int           s_cursor = 0;
static int           s_cursor_throttle_ms = 0;
static hud_settings_t* s_cfg = nullptr;
static LGFX_Sprite   s_canvas;

bool menu_init(hud_settings_t* shared_cfg) {
    s_cfg = shared_cfg;
    s_open = false;
    s_cursor = 0;
    return true;
}

bool menu_tick(input_event_t ev, const input_snapshot_t& snap, uint32_t now_ms) {
    // Abrir/fechar
    if (!s_open) {
        if (ev == EV_ADVANCE_LONGPRESS) {
            s_open = true;
            s_cursor = 0;
            input_set_ui_state(UI_MENU);
        }
        return false;
    }
    if (ev == EV_BACK_LONGPRESS) {
        s_open = false;
        input_set_ui_state(UI_FLIGHT);
        // salvar settings
        if (s_cfg) nvs_settings_save(*s_cfg);
        return false;
    }
    // Navegação Y do joystick (com throttle)
    if (now_ms - s_cursor_throttle_ms > 150) {
        if (snap.joy_y < -40 && s_cursor > 0)               { s_cursor--; s_cursor_throttle_ms = now_ms; }
        else if (snap.joy_y > 40 && s_cursor < N_ITEMS-1)   { s_cursor++; s_cursor_throttle_ms = now_ms; }
    }
    // Click avançar = toggle
    if (ev == EV_ADVANCE_CLICK) {
        if (s_cfg) s_cfg->enabled_bits ^= s_items[s_cursor].bit;
    }
    // Click voltar = sair (vai direto, sem submenu por enquanto)
    if (ev == EV_BACK_CLICK) {
        s_open = false;
        input_set_ui_state(UI_FLIGHT);
        if (s_cfg) nvs_settings_save(*s_cfg);
    }
    return s_open;
}

void menu_draw(uint16_t* buf) {
    s_canvas.setBuffer(buf, 320, 240, 16);
    // Fundo semi-translúcido
    s_canvas.fillRect(40, 20, 240, 200, 0x0000);
    s_canvas.drawRect(40, 20, 240, 200, 0xFFFF);
    s_canvas.setTextColor(0xFFFF, 0x0000);
    s_canvas.setTextSize(1);
    s_canvas.setCursor(50, 28);
    s_canvas.print("HUD - configurar");
    for (int i = 0; i < N_ITEMS; ++i) {
        int y = 48 + i * 16;
        bool sel = (i == s_cursor);
        bool on  = s_cfg && (s_cfg->enabled_bits & s_items[i].bit);
        s_canvas.setTextColor(sel ? 0x0000 : 0xFFFF, sel ? 0xFFFF : 0x0000);
        s_canvas.fillRect(48, y - 2, 224, 14, sel ? 0xFFFF : 0x0000);
        s_canvas.setCursor(54, y);
        s_canvas.printf("[%c] %s", on ? 'X' : ' ', s_items[i].label);
    }
    s_canvas.setTextColor(0x8410, 0x0000);
    s_canvas.setCursor(50, 200);
    s_canvas.print("AVA=toggle  VLT=sair  LONG VLT=fechar");
}
```

- [ ] **Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "menu.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_settings input lovyan03__LovyanGFX
)
```

- [ ] **Step 4: Integrar no pipeline**

Em `app_main.cpp` adicionar:

```cpp
#include "menu.h"
// global:
// (já tem g_settings)

// em app_main, após nvs_settings_load:
menu_init(&g_settings);
```

E no `decode_task`, após `hud_draw_layer_b`:

```cpp
input_event_t ev = input_poll_event();
bool menu_open = menu_tick(ev, input_read_now(now_ms), now_ms);
if (menu_open) {
    menu_draw(back);
}
```

Adicione `menu` aos REQUIRES do main.

- [ ] **Step 5: Build, flash, testar UX**

Run: `idf.py build flash monitor`

Validação manual:
1. Em flight: HUD visível, vídeo rolando.
2. Pressione **Avançar** ≥ 1s → menu abre com 10 itens, cursor no FPS.
3. Joystick Y move cursor.
4. Click avançar toggla item (X aparece/desaparece).
5. Click voltar fecha menu, mudanças aplicam imediatamente.
6. Reboot da placa → mudanças sobrevivem.

- [ ] **Step 6: Commit**

```bash
git add components/menu/ main/
git commit -m "feat: hud config menu with joystick nav and nvs persistence"
```

---

### Task 5: Marcar itens placeholder (dimmed) quando dado ausente

**Files:**
- Modify: `components/menu/menu.cpp`

- [ ] **Step 1: Refinar o draw para dimmed**

Em `menu_draw`, dentro do loop, antes de imprimir o label:

```cpp
bool placeholder = (s_items[i].bit & (HUD_HEADING|HUD_GPS|HUD_BEARING|HUD_HORIZON));
bool data_present = false;
if (placeholder) {
    // consulta tx data — passamos snapshot global (criar accessor) ou via include
    extern hud_data_snapshot_t menu_get_tx_snapshot(void);
    auto t = menu_get_tx_snapshot();
    if (s_items[i].bit == HUD_HEADING) data_present = t.heading_valid;
    if (s_items[i].bit == HUD_GPS)     data_present = t.gps_valid;
    // bearing depende de gps + home definido (não temos em V0 → false)
    // horizonte depende de IMU pitch/roll (não temos → false)
}
uint16_t fg = (placeholder && !data_present) ? 0x8410 :
              (sel ? 0x0000 : 0xFFFF);
uint16_t bg = sel ? 0xFFFF : 0x0000;
s_canvas.setTextColor(fg, bg);
```

E expor o accessor em `app_main.cpp`:

```cpp
extern "C" hud_data_snapshot_t menu_get_tx_snapshot(void) {
    return telemetry_in_snapshot((uint32_t)(esp_timer_get_time()/1000));
}
```

(Linkage simples para evitar circular deps.)

- [ ] **Step 2: Build, flash**

Validação: itens com `*` aparecem dimmed enquanto TX não envia dados correspondentes.

- [ ] **Step 3: Commit**

```bash
git add components/menu/ main/
git commit -m "feat: dim placeholder menu items when telemetry absent"
```

---

### Task 6: Item de menu "Max Auto Level"

O spec §6.2 permite override de `max_auto_level` via menu. Adicionar item especial não-toggle.

**Files:**
- Modify: `components/menu/menu.cpp`
- Modify: `main/app_main.cpp`

- [ ] **Step 1: Adicionar item no menu**

```cpp
static menu_item_t s_items[] = {
    // ... os 10 anteriores
    {"Max auto level", 0}, // 11º item, sem bit (especial)
};
```

E no handling de `EV_ADVANCE_CLICK`:

```cpp
if (ev == EV_ADVANCE_CLICK) {
    if (s_items[s_cursor].bit == 0) {
        // item especial: cicla max_auto_level entre 0..4
        s_cfg->max_auto_level = (s_cfg->max_auto_level + 1) % 5;
    } else {
        s_cfg->enabled_bits ^= s_items[s_cursor].bit;
    }
}
```

E no `menu_draw` ajustar o label do item especial:

```cpp
if (s_items[i].bit == 0 && i == 10) {
    s_canvas.printf("Max auto: L%u", s_cfg->max_auto_level);
}
```

- [ ] **Step 2: Propagar mudança ao telemetry**

Em `app_main.cpp`, periodicamente sincronizar:

```cpp
// no telemetry_task antes de telemetry_tick_1s:
telemetry_set_max_auto(g_settings.max_auto_level);
```

E adicionar à API do telemetry:

```cpp
// telemetry.h
void telemetry_set_max_auto(uint8_t lvl);
// telemetry.cpp
void telemetry_set_max_auto(uint8_t lvl) { s_max_auto_level = lvl; }
```

- [ ] **Step 3: Build, flash, testar**

Mude no menu: max_auto: L0 → L1 → L2 → L3 → L4 → L0. Crie cenário de drop > 8% e observe `requested_level` parar onde configurado.

- [ ] **Step 4: Commit**

```bash
git add components/menu/ components/telemetry/ main/
git commit -m "feat: max_auto_level configurable via menu"
```

---

## Critérios de aceitação do Sprint 6

- [ ] Menu abre com long-press avançar (1 s)
- [ ] Joystick Y navega itens com throttle adequado
- [ ] Click avançar toggla item, click voltar sai (ou long-press voltar)
- [ ] Settings persistem após reboot (NVS)
- [ ] Itens placeholder (*) aparecem dimmed quando dado ausente
- [ ] Crosshair aparece centralizado quando ativado
- [ ] Camada B atualiza FPS, LAT, RSSI, LV, DRP em tempo real
- [ ] Max auto level configurável e respeitado pelo adaptive
- [ ] FPS no flight ≥ 23.5 mesmo com menu carregado em memória

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| Throttle de cursor muito rápido = scroll incontrolável | 150 ms inicial; ajustar se necessário |
| NVS write a cada toggle pode degradar flash | Save só em close-do-menu (já é o caso) |
| Menu sobre vídeo deixa frame anterior visível (não-modal) | Aceitar; é o design — vídeo continua atrás |
| Long-press de avançar sendo enviado também ao TX como botão pressionado | Aceitar; TX deve tratar ou ignorar; ou: durante long-press detection, mascarar o botão no `joystick_q` |

## Self-Review

- §6.1 Camada B: Task 3 implementa todos os itens
- §6.2 NVS persistência: Task 1
- §6.3 Navegação (long-press, joystick Y, clicks): Tasks 4, 5
- §6.4 Layout (rodapé + crosshair + heading-tape placeholder): Task 3
- §6 Max auto level via menu: Task 6
- §10 sem regressão FPS: Validação na Task 5

Sem placeholders. Tipos `hud_settings_t`, `hud_data_snapshot_t`, `telemetry_live_snapshot_t` consistentes.
