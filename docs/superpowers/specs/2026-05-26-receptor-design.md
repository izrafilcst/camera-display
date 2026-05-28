# Design — Receptor Portátil ("Ground Control") V0

**Data**: 2026-05-26
**Estado**: Design aprovado, aguardando implementation plan
**Hardware alvo**: ESP32-S3-N16R8 + display ILI9341 SPI 320×240 (módulo "vermelho" com SD/touch não usados) + joystick PSP-1000 + 2 botões

---

## 1. Escopo

Projetar o **receptor portátil** que opera como ground control para um robô móvel:

- **Recebe** stream JPEG @ 24 fps via ESP-NOW e renderiza no display TFT 320×240.
- **Envia** comandos de joystick (30–60 Hz), telemetria de qualidade (2 Hz) e comandos discretos ao robô.
- **Mostra** HUD configurável: status de link, bateria do robô, FPS, latência, RSSI, nível adaptive, crosshair, heading, GPS, bearing/distância home.

Este spec cobre o **primeiro pipeline funcional (V0)**. Pairing BLE com encriptação ESP-NOW, GPS/IMU/compass reais e modos avançados de degradação ficam para planos subsequentes.

---

## 2. Decisões consolidadas do brainstorm

| # | Tópico | Decisão |
|---|---|---|
| Q1 | Display | ILI9341 SPI 4-wire, 320×240 paisagem (MADCTL rot=1), sem TE pin, biblioteca LovyanGFX |
| Q2 | Decoder JPEG | esp_jpeg (libjpeg-turbo otimizada), saída RGB565 direta |
| Q3a | Framebuffers | 2 (double buffer), PSRAM, swap-on-DMA-complete |
| Q3b | Jitter buffer | 2 slots de 16 KB (reassembly + pronto-para-decode) |
| Q4 | Render | Full-frame video + overlay redesenhado a cada frame |
| Q5 | Política de descarte | Skip-drop 30 ms timeout (herdado da spec TX) |
| Q6 | HUD/menu | 3 camadas (sempre visível, configurável, placeholders); navegação via joystick + 2 botões; settings em NVS |
| Q7 | Adaptive quality | RX recomenda, TX aceita; algoritmo de escada com histerese; MAX_AUTO = L2 |
| Q8 | Perda de link | FREEZE = último frame + ícone; DISCONNECTED = tela status + thumb; reconexão passiva |
| Q9 | Anti-tearing | Tearing-tolerante fase 1; escalar empiricamente se necessário |
| Q10 | Escopo | Spec único cobrindo vídeo + controle (RX é ground control completo) |

---

## 3. Hardware

### 3.1 MCU
- ESP32-S3-N16R8
- 16 MB Flash QIO
- 8 MB PSRAM Octal
- Xtensa LX7 dual-core @ 240 MHz
- Wi-Fi 802.11 b/g/n
- BLE 5.0 (não usado em V0)

### 3.2 Display
- **Controlador**: ILI9341 (módulo "vermelho" comum, com slot SD e touch XPT2046 que **não são usados**)
- **Interface**: SPI 4-wire (MOSI, SCK, CS, DC, RST, BL), modo 0
- **Clock SPI**: 40 MHz como baseline estável; testar 80 MHz, recuar se erros
- **Resolução**: 240×320 nativo, usado como **320×240 paisagem** via MADCTL rot=1
- **Profundidade**: RGB565 (16 bpp)
- **TE pin**: **não roteado** no PCB do módulo vermelho — anti-tearing precisa ser por software
- **CS dos periféricos não usados**: XPT2046 e SD-card mantidos em HIGH permanente

### 3.3 Pinagem proposta (SPI dedicada)

| Função | GPIO sugerido | Notas |
|---|---|---|
| MOSI | GPIO 11 | SPI2_HOST |
| SCK | GPIO 12 | SPI2_HOST |
| LCD_CS | GPIO 10 | SPI2_HOST |
| LCD_DC | GPIO 9 | data/command |
| LCD_RST | GPIO 8 | reset hard |
| LCD_BL | GPIO 7 | backlight (PWM opcional) |
| TOUCH_CS | GPIO 6 | mantido HIGH (não usado) |
| SD_CS | GPIO 5 | mantido HIGH (não usado) |

Confirmar contra a placa real antes da implementação.

### 3.4 Entrada

| Componente | Interface | GPIO sugerido |
|---|---|---|
| Joystick PSP-1000 eixo X | ADC1 channel | GPIO 4 (ADC1_CH3) |
| Joystick PSP-1000 eixo Y | ADC1 channel | GPIO 3 (ADC1_CH2) |
| Botão "avançar" | digital input pull-up | GPIO 2 |
| Botão "voltar" | digital input pull-up | GPIO 1 |

ADC: usar `esp_adc/adc_continuous.h` em modo single-shot a 100 Hz; suficiente para resposta < 10 ms.

### 3.5 Alimentação
- USB-C para desenvolvimento
- Bateria Li-ion 1S (3.7 V) opcional com divisor resistivo em GPIO ADC para `rx_battery_pct`
  - Hardware específico ainda não definido; campo no telemetry tem placeholder `0xFF` se ausente

---

## 4. Pipeline de Vídeo

### 4.1 Visão geral

```
┌─────────────────────────────────────────────────────────────────────┐
│  Core 0 (PRO_CPU) — Rede                                            │
│                                                                     │
│  ESP-NOW RX cb  ──► dispatch_by_msg_type ──► reassembly             │
│                                                  │                  │
│                                                  ▼                  │
│                                          jitter_buf [2 × 16 KB]     │
│                            (timestamp tx_emission_ms preservado)    │
└──────────────────────────────────────────────────│──────────────────┘
                                                   │ queue handle
                                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Core 1 (APP_CPU) — Vídeo                                           │
│                                                                     │
│  task_decode ──► esp_jpeg(slot) ──► FB_back [RGB565, 153.6 KB]      │
│       │                                  │                          │
│       └──► atualiza latency stats ───────┤                          │
│                                          ▼                          │
│  task_render ──► overlay(FB_back) ──► DMA blit ──► ILI9341          │
│                                          │                          │
│                            on_complete ──┴── swap front/back        │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.2 Recepção e reassembly (Core 0)

- ESP-NOW callback identifica `msg_type` no primeiro byte.
- Para `MSG_VIDEO_FRAG` (0x10):
  - Lê header de 8 B: `frame_id u16, frag_idx u8, frag_total u8, jpeg_size u16, payload_len u16`
  - Se `frag_idx == 0`, lê 4 B extra de `tx_emission_ms u32` e abre novo slot de reassembly.
  - Mantém slot ativo enquanto `frag_idx` chegam para o mesmo `frame_id`.
  - **Skip-drop**: se um fragmento ausente passa de 30 ms desde o primeiro fragmento do frame, o slot inteiro é descartado e contador `frames_dropped` incrementa.
  - Quando `frag_count == frag_total`, slot é marcado pronto e enfileirado para `task_decode`.
- Frame fora de ordem com `frame_id` maior chegando: descarta slot anterior (skip-drop por ordem).

### 4.3 Decoder (Core 1)

- esp_jpeg em modo single-shot, output RGB565.
- Input: ponteiro para JPEG no slot, tamanho `jpeg_size`.
- Output: FB_back direto (sem cópia intermediária).
- Marca `t_decode_end` para cálculo de latência.

### 4.4 Render + blit

- `task_render` espera FB_back pronto (semáforo do `task_decode`).
- Desenha overlay (ver §6) sobre FB_back.
- Dispara DMA SPI (LovyanGFX `startWrite()` + `writePixels()` async).
- Em `dmaFinish` callback: swap atomico de ponteiros `FB_front ↔ FB_back`; libera `task_decode` para próximo frame.

### 4.5 Anti-tearing (fase 1, tolerante)

- Não tenta sincronizar com refresh interno do ILI9341.
- Aceita tearing residual em movimento rápido.
- Fase 2 (se necessário após medição empírica):
  - **(B)** two-half-blit com pausa entre metades, ou
  - **(C)** solda de fio no pad TE (alguns clones expõem; pesquisar PCB)

### 4.6 Orçamento de tempo por frame (24 fps = 41,6 ms)

| Estágio | Estimativa | Notas |
|---|---:|---|
| Reassembly (último frag → slot pronto) | 10–15 ms | depende do canal |
| Decode esp_jpeg (q=16, RGB565) | 8–12 ms | medir em hw real |
| Overlay redesenho | < 2 ms | conjunto típico |
| DMA blit (320×240 @ 80 MHz SPI) | ~15 ms | recuar para ~30 ms @ 40 MHz se necessário |
| **Total típico** | **35–60 ms** | aperta ou estoura o orçamento |

> Critério original do CLAUDE.md de "latência < 30 ms" é **inalcançável** com este pipeline; renegociado para **p99 < 100 ms** (ver §10).

---

## 5. Pipeline de Controle

### 5.1 Visão geral

```
ADC joystick + GPIOs botões ──► task_input (100 Hz)
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
              ▼                    ▼                    ▼
      máquina_estado_UI   joystick_q (60 Hz)   button_events
              │                    │                    │
              │ menu_open         │                    │
              ▼                    │                    │
      task_ui (30 Hz, render menu)│                    │
                                   │                    │
métricas_link  ─► task_telemetry_aggregator (2 Hz)     │
                          │                             │
                          ▼                             │
                  telemetry_q                           │
                          │                             │
                          ▼                             ▼
                  ╔═══════════════════════════════════════╗
                  ║  task_espnow_tx (consumer multi-fila) ║
                  ╚═══════════════════════════════════════╝
                          │
                          ▼
                      ESP-NOW
```

### 5.2 task_input (Core 0, 100 Hz)

- Lê ADC1 canais X/Y do joystick.
- Aplica deadzone (~5% do range central).
- Lê GPIO dos 2 botões com debounce 20 ms.
- Detecta eventos:
  - `BTN_AVANCAR_PRESS`, `BTN_AVANCAR_LONGPRESS_1S`, `BTN_AVANCAR_RELEASE`
  - `BTN_VOLTAR_PRESS`, `BTN_VOLTAR_LONGPRESS_1S`, `BTN_VOLTAR_RELEASE`
- Roteamento por estado da UI:
  - **UI estado = FLIGHT**: joystick XY → `joystick_q` a 60 Hz; botões enviam comandos de voo (mapeamento futuro)
  - **UI estado = MENU**: joystick Y → cursor menu; joystick X ignorado; XY zerados em `joystick_q` (anti-deriva)

### 5.3 task_telemetry_aggregator (Core 0, 2 Hz)

Coleta métricas da janela móvel de 1 s e aplica regra adaptive:

```c
struct telemetry_state {
    uint16_t frames_received_1s;
    uint16_t frames_dropped_1s;
    uint16_t fragments_lost_1s;
    int8_t   rssi_avg_dbm;
    int8_t   rssi_min_dbm;
    uint16_t latency_p50_ms;
    uint16_t latency_p99_ms;
    uint8_t  rx_battery_pct;    // 0xFF se sem hw
    uint8_t  current_level_seen;
    uint8_t  requested_level;
};
```

**Algoritmo adaptive (escada com histerese):**

```
drop_pct = frames_dropped_1s / (frames_received_1s + frames_dropped_1s)

a cada janela de 1 s:
    if drop_pct < 0.01:
        good_streak_seconds += 1
    else:
        good_streak_seconds = 0  # reset agressivo a qualquer hiccup

    if drop_pct > 0.08 and requested_level < MAX_AUTO:
        requested_level += 1
        cooldown = 1s
    elif drop_pct < 0.01 and good_streak_seconds >= 5 and requested_level > 0:
        requested_level -= 1
        cooldown = 1s
        good_streak_seconds = 0  # impede oscilação imediata

MAX_AUTO = 2  (L2)
clamp: 0 ≤ requested_level ≤ MAX_AUTO
histerese: 1 mudança por janela 1 s
```

L3 e L4 só atingíveis via `MSG_COMMAND` manual do usuário (item de menu "Force quality").

### 5.4 task_espnow_tx (Core 0, multi-fila)

Consome (prioridade decrescente):
1. `command_q` (raro, alta prioridade quando presente)
2. `joystick_q` (60 Hz nominal)
3. `telemetry_q` (2 Hz nominal)

Cada item é embrulhado com header ESP-NOW (`type + seq`) e enviado ao peer hardcoded (V0). Sem encriptação em V0; pairing BLE + PMK/LMK ficam para Plano 2.

### 5.5 Protocolo MSG_TELEMETRY (RX→TX, 25 B)

> **REQ-4 (Sprint 2 hardening)**: `seq` widened from `uint8_t` (1 B, wraps at 256) to
> `uint32_t` (4 B, wraps at ~136 years @ 2 Hz). `esnow_hdr_t` simplified to
> `msg_type + reserved`. Replay window = 32 packets enforced in `espnow_link.cpp`.

```c
struct __attribute__((packed)) esnow_hdr_t {
    uint8_t  msg_type;   // MSG_*
    uint8_t  reserved;   // 0x00 (was: seq — moved into per-message body)
};

struct __attribute__((packed)) telemetry_rx_to_tx {
    uint8_t  msg_type;             // 0x20
    uint8_t  reserved;             // 0x00
    uint32_t seq;                  // REQ-4: uint32_t, anti-replay (window=32)
    uint8_t  requested_level;      // 0..4 (L0..L4)
    uint8_t  current_level_seen;   // último L visto chegando
    uint16_t frames_received_1s;
    uint16_t frames_dropped_1s;
    uint16_t fragments_lost_1s;
    int8_t   rssi_avg_dbm;
    int8_t   rssi_min_dbm;
    uint16_t latency_p50_ms;
    uint16_t latency_p99_ms;
    uint8_t  rx_battery_pct;       // 0..100, 0xFF=N/A
    uint8_t  flags;                // bit0=menu_open, bit1=link_freeze
    uint32_t rx_uptime_ms;
};
```

---

## 6. HUD / Menu

### 6.1 Camadas

**A) Sempre visível (não configurável)** — top-bar do display:
- Status link (CONNECTED / FREEZE / RECONNECT) — ícone colorido
- Bateria robô (%) — número

**B) Configurável via menu (NVS):**
- FPS, Latência (ms), RSSI (dBm), Nível adaptive (L0–L4), Drop rate (%)
- Crosshair central
- Heading (graduação tipo FPV)
- GPS coords (lat/lon)
- Bearing + distância para home
- Horizonte artificial

**C) Placeholders (dimmed até telemetria correspondente chegar):**
- GPS, compass, IMU

### 6.2 Persistência

NVS namespace `"hud"`:
- `enabled_bits` (uint32): bitmask dos itens da camada B ativos
- `max_auto_level` (uint8): override de MAX_AUTO (default 2)
- `version` (uint8): schema version para migrações futuras

### 6.3 Navegação

| Ação | Efeito |
|---|---|
| Long-press **Avançar** (≥ 1 s) | Estado FLIGHT → abre menu raiz. Estado MENU → ignorado (use click Voltar para subir um nível). |
| Joystick Y | Cursor up/down em listas |
| Click **Avançar** | Confirma / entra em submenu / toggle item |
| Click **Voltar** | Back / sai do submenu atual (sobe um nível). No menu raiz, sem efeito. |
| Long-press **Voltar** (≥ 1 s) | Fecha menu (volta a FLIGHT) de qualquer profundidade |

Durante menu aberto: `joystick_q` recebe zeros (anti-deriva). Bandeira `flags.bit0=menu_open` reportada no telemetry.

### 6.4 Layout (esboço)

```
┌─────────────────────────────────────────────┐
│ [LINK ✓] BAT: 78%              FPS: 24      │  ← topo
│                                             │
│                                             │
│                  ╋ (crosshair)              │  ← centro
│                                             │
│                                             │
│ LAT:42  RSSI:-65  L1  DROP:0.3%             │  ← rodapé
└─────────────────────────────────────────────┘
```

**Posições fixas, visibilidade configurável:**

| Posição | Conteúdo | Camada / Visibilidade |
|---|---|---|
| Topo-esquerda | `[LINK ✓]` status link | A — sempre visível |
| Topo-esquerda | `BAT: NN%` bateria do robô | A — sempre visível |
| Topo-direita | `FPS: NN` | B — toggle no menu |
| Centro | crosshair | B — toggle no menu |
| Rodapé (esq→dir) | `LAT:NN`, `RSSI:NN`, `LN`, `DROP:N.N%` | B — toggles independentes |
| (futuro) | Heading tape, GPS, bearing/distância | B — placeholders C até hw existir |

Tipografia mínima (~8×16 px). Cores RGB565 contrastantes (texto branco com sombra preta de 1 px).

---

## 7. Tratamento de Link

| Estado | Critério | Comportamento da tela | Comportamento de rede |
|---|---|---|---|
| **CONNECTED** | pacote recebido nos últimos 200 ms | Vídeo normal | Operação normal |
| **FREEZE** | sem pacote por 200 ms – 3 s | FB_front congelado + ícone FREEZE piscando no HUD | Continua ouvindo |
| **DISCONNECTED** | sem pacote > 3 s | Tela escurecida + texto "AGUARDANDO LINK" + uptime do disconnect + thumb do último frame | Reconexão passiva (sem reset Wi-Fi) |

Transição **DISCONNECTED → CONNECTED**: assim que primeiro fragmento válido chega, volta a vídeo normal. Sem handshake.

---

## 8. Alocação de Memória (PSRAM 8 MB)

| Buffer | Quantidade | Tamanho | Total |
|---|---:|---:|---:|
| Framebuffer RGB565 | 2 | 153.6 KB | 307.2 KB |
| Slot jitter buffer JPEG | 2 | 16 KB | 32 KB |
| Reassembly scratch ativo | 1 | 16 KB | 16 KB |
| Thumb último frame (DISCONNECTED) | 1 | 8 KB (80×60 RGB565) | 8 KB |
| Filas FreeRTOS (joystick/telem/cmd) | — | — | < 2 KB |
| **TOTAL** | | | **~365 KB** (de 8 MB) |

Margem confortável; 95% da PSRAM ociosa em V0.

---

## 9. Tasks FreeRTOS

| Task | Core | Prioridade | Stack | Hz | Função |
|---|---|---|---|---|---|
| `task_espnow_rx` | 0 | 6 (alta) | 4 KB | event-driven | Reassembly + dispatch |
| `task_espnow_tx` | 0 | 5 | 3 KB | event-driven | Envio multi-fila |
| `task_input` | 0 | 4 | 2 KB | 100 | ADC + botões + UI state |
| `task_telemetry_aggregator` | 0 | 3 | 2 KB | 2 | Métricas + adaptive |
| `task_decode` | 1 | 6 (alta) | 6 KB | ~24 | esp_jpeg |
| `task_render` | 1 | 6 (alta) | 4 KB | ~24 | overlay + DMA blit |
| `task_ui` | 1 | 4 | 3 KB | 30 | menu render quando aberto |

---

## 10. Critérios de Sucesso (Renegociados)

Critérios originais do CLAUDE.md ajustados para realismo do pipeline:

- [ ] **24 fps renderizados** sem stutter perceptível por **≥ 10 min contínuos**
- [ ] **Latência display p99 < 100 ms** (do `tx_emission_ms` até pixel) ⚠ *ajustado de < 30 ms — inalcançável com decode + blit reais*
- [ ] **Drop rate < 1%** em canal limpo (sem fontes Wi-Fi competindo no canal 6)
- [ ] **fps_min ≥ 20** em canal limpo
- [ ] **Reconexão automática < 2 s** após link voltar
- [ ] **Soak térmico 30 min**: temperatura interna < 80 °C
- [ ] **Sem tearing severo** em movimento normal (movimento horizontal violento aceita tearing leve)
- [ ] **Menu funcional**: abrir, navegar, toggle item, persistir em NVS, sobreviver reboot

---

## 11. Fora de escopo (V0)

- Pairing BLE inicial + chaves PMK/LMK ESP-NOW (vai para Plano 2)
- Hardware real de GPS, IMU, compass (placeholders só)
- Modos de degradação L3/L4 automáticos (só manuais via `MSG_COMMAND`)
- Two-half-blit ou solda do pad TE (só se medição mostrar tearing severo)
- Bateria do RX com hw real (campo `rx_battery_pct` reporta `0xFF` enquanto ausente)
- Salvamento de vídeo em SD card
- Funcionalidade do touchscreen XPT2046

---

## 12. Riscos e mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|---|---|---|---|
| SPI 80 MHz instável em clone ILI9341 | Média | Recuar para 40 MHz; blit dobra para ~30 ms | Testar nos primeiros dias |
| Tearing severo sem TE pin | Média | Imagem ruim em movimento | Fase 2: two-half-blit ou solda TE |
| Decode esp_jpeg > 12 ms | Baixa | Estoura orçamento | Reduzir resolução intermediária ou cair para L2 default |
| ADC do joystick PSP-1000 instável | Média | UX ruim no menu | Filtro IIR + deadzone alargada |
| Concorrência Wi-Fi vs SPI no Core 0 | Média | Stutter de blit | Mover SPI inteiro para Core 1 (já planejado) |
| Latência > 100 ms p99 | Média | Critério de sucesso falha | Reduzir jitter_buf para 1 slot e medir |

---

## 13. Itens explicitamente decididos

Os 8 pontos em aberto do CLAUDE.md + 2 emergentes, todos fechados:

1. Display físico → ILI9341 SPI, sem TE
2. Decoder → esp_jpeg
3. Buffering → 2 FB + 2 slots jitter
4. Render → full-frame + overlay redesenhado
5. Política de descarte → skip-drop 30 ms
6. UI/overlay → 3 camadas, configurável via menu, NVS, joystick + 2 botões
7. Feedback ao TX → RX recomenda, MAX_AUTO=L2
8. Freeze frame → último frame + ícone (FREEZE); tela status + thumb (DISCONNECTED)
9. Anti-tearing → tolerante fase 1, escala se necessário
10. Escopo bidirecional → spec único cobre vídeo + controle
