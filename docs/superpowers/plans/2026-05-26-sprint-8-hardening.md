# Sprint 7 — Hardening + Critérios de Sucesso Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validar empiricamente todos os critérios de sucesso do §10 do spec, decidir se anti-tearing fase 2 é necessário, fazer soak térmico de 30 min e ajustar pontos finais antes de tagear V0.

**Architecture:** Sprint sem código novo de produto. Adiciona componente `bench` (instrumentação + relatório), scripts de teste, e ajustes pontuais conforme medições. Decisões finais: SPI clock definitivo, fase 2 anti-tearing, watermark de stack/heap.

**Tech Stack:** ESP-IDF `esp_pm`, `esp_timer`, `heap_caps_get_largest_free_block`, `temperature_sensor_install` (interno do S3), `vTaskGetRunTimeStats`, scripts host (Python opcional).

---

## Reference do spec
- §10 Critérios de sucesso
- §12 Riscos e mitigações
- §4.5 Anti-tearing (fase 2)

## File Structure

```
components/
└── bench/
    ├── CMakeLists.txt
    ├── include/bench.h
    └── bench.cpp
docs/superpowers/
├── sprint-7-soak-results.md   (NOVO, relatório final)
└── notes/...
```

---

### Task 1: Componente `bench` — instrumentação central

**Files:**
- Create: `components/bench/include/bench.h`
- Create: `components/bench/bench.cpp`
- Create: `components/bench/CMakeLists.txt`

- [ ] **Step 1: API**

```cpp
#pragma once
#include <cstdint>

void bench_init(void);

// Tempo de decode (us) — chamado pelo decoder após cada frame.
void bench_log_decode(uint32_t us);

// Tempo de blit (us) — chamado pelo render após DMA terminar.
void bench_log_blit(uint32_t us);

// Latência total end-to-end (ms) — pacote chegando → swap pronto.
void bench_log_latency(uint32_t ms);

// Dump periódico (chamado a cada 10 s).
void bench_dump(void);

// Reset.
void bench_reset(void);
```

- [ ] **Step 2: Implementar histograma simples**

```cpp
#include "bench.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>
#include <vector>

static const char* TAG = "bench";
static std::vector<uint32_t> s_dec_us, s_blit_us, s_lat_ms;
static int64_t s_t0 = 0;

static uint32_t pctl(std::vector<uint32_t>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = std::min(v.size() - 1, (size_t)((v.size() - 1) * p));
    return v[i];
}

void bench_init(void)               { s_t0 = esp_timer_get_time(); }
void bench_log_decode(uint32_t us)  { s_dec_us.push_back(us); }
void bench_log_blit(uint32_t us)    { s_blit_us.push_back(us); }
void bench_log_latency(uint32_t ms) { s_lat_ms.push_back(ms); }
void bench_reset(void)              { s_dec_us.clear(); s_blit_us.clear(); s_lat_ms.clear(); }

void bench_dump(void) {
    auto dec_p50  = pctl(s_dec_us, 0.5);
    auto dec_p99  = pctl(s_dec_us, 0.99);
    auto blit_p50 = pctl(s_blit_us, 0.5);
    auto blit_p99 = pctl(s_blit_us, 0.99);
    auto lat_p50  = pctl(s_lat_ms, 0.5);
    auto lat_p99  = pctl(s_lat_ms, 0.99);
    auto lat_max  = pctl(s_lat_ms, 1.0);

    size_t heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_spi = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
        "decode p50=%uus p99=%uus | blit p50=%uus p99=%uus | "
        "lat p50=%ums p99=%ums max=%ums | heap int=%u spi=%u | n=%u",
        (unsigned)dec_p50,  (unsigned)dec_p99,
        (unsigned)blit_p50, (unsigned)blit_p99,
        (unsigned)lat_p50,  (unsigned)lat_p99, (unsigned)lat_max,
        (unsigned)heap_int, (unsigned)heap_spi,
        (unsigned)s_dec_us.size());
}
```

- [ ] **Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "bench.cpp"
    INCLUDE_DIRS "include"
)
```

- [ ] **Step 4: Conectar nos pontos certos**

Em `decoder.cpp` ao final de `decoder_decode_to_rgb565`:

```cpp
extern "C" void bench_log_decode(uint32_t);
// ...
int64_t dt = t1 - t0;
bench_log_decode((uint32_t)dt);
return dt;
```

Em `render.cpp`, medir o blit dentro do mutex:

```cpp
extern "C" void bench_log_blit(uint32_t);
// em render_present:
int64_t tb0 = esp_timer_get_time();
display_blit_full(s_fb[s_back_idx]);
display_wait_dma();
int64_t tb1 = esp_timer_get_time();
bench_log_blit((uint32_t)(tb1 - tb0));
```

Em `app_main.cpp`, no decode_task após decode:

```cpp
extern "C" void bench_log_latency(uint32_t);
uint32_t latency = now_ms - rf.frame.tx_emission_ms;
bench_log_latency(latency);
telemetry_on_frame_completed(latency);
```

E uma task que chama `bench_dump()` a cada 10 s.

- [ ] **Step 5: Build, flash, observar**

Run: `idf.py build flash monitor`

Esperado log a cada 10 s:
```
decode p50=9500us p99=12000us | blit p50=15500us p99=16100us | lat p50=68ms p99=92ms max=110ms | heap int=180000 spi=7800000 | n=240
```

- [ ] **Step 6: Commit**

```bash
git add components/bench/ main/ components/decoder/ components/render/
git commit -m "feat: bench instrumentation (decode/blit/latency histograms)"
```

---

### Task 2: Soak 10 min de FPS estável

- [ ] **Step 1: Procedimento**

1. RX e TX numa bancada típica (3 m de distância, sem Wi-Fi intenso competindo)
2. Boot RX, espera 10 s para estabilizar
3. Inicia cronômetro de 10 min
4. Monitorar logs `bench_dump` a cada 10 s

- [ ] **Step 2: Capturar log**

Salvar saída do monitor em `docs/superpowers/sprint-7-soak-results.md`. Calcular:
- fps_avg = média dos `frames_received_1s` no período
- fps_min = mínimo
- drop_rate = sum(drops) / sum(frames+drops)
- latency_p50 / p99 / max

- [ ] **Step 3: Comparar com critérios §10**

| Critério | Valor medido | Pass |
|---|---|---|
| fps_avg ≥ 23.5 | ? | |
| fps_min ≥ 20 | ? | |
| latência p99 < 100 ms | ? | |
| drop < 1% | ? | |

Se algum falha, abrir issue/nota em `docs/superpowers/notes/sprint-7-issues.md` com causa hipotética e ajuste proposto.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/sprint-7-soak-results.md
git commit -m "test: sprint 7 soak 10min results"
```

---

### Task 3: Soak térmico 30 min

- [ ] **Step 1: Ativar sensor de temperatura interno do ESP32-S3**

Em `bench.cpp` adicionar:

```cpp
#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t s_temp = nullptr;

void bench_init_temp(void) {
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    temperature_sensor_install(&cfg, &s_temp);
    temperature_sensor_enable(s_temp);
}

float bench_read_temp_c(void) {
    if (!s_temp) return -1.0f;
    float v = 0;
    temperature_sensor_get_celsius(s_temp, &v);
    return v;
}
```

Em `bench_dump`, anexar:

```cpp
float tc = bench_read_temp_c();
ESP_LOGI(TAG, "temp=%.1fC", tc);
```

E chamar `bench_init_temp()` em `bench_init`.

- [ ] **Step 2: Soak 30 min**

Boot e deixar 30 min em uso normal (vídeo + HUD + adaptive ativo). Monitorar temperatura.

- [ ] **Step 3: Critério**

Temp média < 80 °C; pico < 85 °C aceitável momentâneo.

Salvar gráfico/tabela em `docs/superpowers/sprint-7-soak-results.md` na seção `## Térmico`.

- [ ] **Step 4: Commit**

```bash
git add components/bench/ docs/superpowers/sprint-7-soak-results.md
git commit -m "test: thermal soak 30min"
```

---

### Task 4: Validação visual de tearing

- [ ] **Step 1: Cenário**

Apontar a câmera do robô para um padrão de listras verticais alternadas branco/preto (ex: um teclado). Movimentar a câmera horizontalmente em diferentes velocidades.

- [ ] **Step 2: Anotar**

Em `docs/superpowers/sprint-7-soak-results.md` seção `## Tearing`:

```
Movimento lento (~10°/s): sem tearing visível
Movimento médio (~30°/s): tearing leve, 1 linha visível ocasional
Movimento rápido (~90°/s): tearing severo, "rasgo" claro em 1-2 linhas
```

- [ ] **Step 3: Decisão fase 2**

Decidir se vale implementar fase 2 (two-half-blit ou solda TE):

| Severidade observada | Decisão V0 |
|---|---|
| Leve em movimento rápido apenas | Aceitar; documentar |
| Moderado em movimento médio | Tentar two-half-blit (Task 5) |
| Severo até em movimento lento | Investigar solda TE (custo alto, considerar V1) |

Anotar a decisão no documento.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/sprint-7-soak-results.md
git commit -m "test: tearing severity assessment"
```

---

### Task 5: (CONDICIONAL) Anti-tearing fase 2 — two-half-blit

Só execute se a decisão da Task 4 indicou "Moderado" ou pior.

**Files:**
- Modify: `components/display/display.cpp`
- Modify: `components/display/include/display.h`

- [ ] **Step 1: Adicionar variante**

```cpp
// display.h
void display_blit_full_split(const uint16_t* rgb565_buf, uint32_t mid_delay_us);
```

- [ ] **Step 2: Implementar**

```cpp
// display.cpp
void display_blit_full_split(const uint16_t* buf, uint32_t mid_delay_us) {
    if (!s_lcd || !buf) return;
    int W = s_lcd->width(), H = s_lcd->height();
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, 0, W, H/2);
    s_lcd->writePixels(buf, W * (H/2), true);
    s_lcd->endWrite();
    ets_delay_us(mid_delay_us);
    s_lcd->startWrite();
    s_lcd->setAddrWindow(0, H/2, W, H/2);
    s_lcd->writePixels(buf + W * (H/2), W * (H/2), true);
    s_lcd->endWrite();
}
```

- [ ] **Step 3: Trocar em `render_present`**

```cpp
display_blit_full_split(s_fb[s_back_idx], 5000);  // 5 ms entre metades
```

- [ ] **Step 4: Re-avaliar tearing**

Repetir cenário da Task 4. Comparar.

- [ ] **Step 5: Decidir manter ou voltar**

Se two-half-blit melhora E fps_avg continua ≥ 23.5 → manter.
Senão → reverter (mais fácil: `git revert HEAD` ou refazer com `display_blit_full` no render_present).

- [ ] **Step 6: Commit**

```bash
git add components/display/ components/render/
git commit -m "feat: optional two-half-blit anti-tearing (V0 decision documented)"
```

---

### Task 6: Watermarks de stack e heap

- [ ] **Step 1: Adicionar `uxTaskGetStackHighWaterMark` em `bench_dump`**

```cpp
// bench.cpp
#include "freertos/task.h"

void bench_dump(void) {
    // ... código existente ...
    char buf[16];
    for (auto name : {"decode", "render", "in", "tel", "link_ui", "tx"}) {
        TaskHandle_t h = xTaskGetHandle(name);
        if (h) {
            UBaseType_t wm = uxTaskGetStackHighWaterMark(h);
            ESP_LOGI(TAG, "task %s stack remaining=%u", name, (unsigned)wm);
        }
    }
}
```

- [ ] **Step 2: Validar**

Para cada task, `wm` deve ser > 512 bytes. Se algum task chegou a < 256, aumentar stack na criação.

- [ ] **Step 3: Commit**

```bash
git add components/bench/
git commit -m "obs: log task stack high water marks"
```

---

### Task 7: Cleanup do CLAUDE.md

O CLAUDE.md descreve só a parte de vídeo. Atualizar para refletir o sistema completo.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Atualizar visão geral**

Adicionar parágrafos sobre:
- Receptor é também ground control (joystick + telemetria)
- Pipeline está validado em hw real (link para sprint-7-soak-results.md)
- Critérios renegociados (latência p99 < 100ms)

- [ ] **Step 2: Marcar checkboxes em "Critérios de Sucesso"**

Substituir a seção pelo resultado real (cada item ☑/☐) e link para `sprint-7-soak-results.md`.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md with V0 reality (control + measured results)"
```

---

### Task 8: Tag V0

- [ ] **Step 1: Garantir tudo committado**

Run: `git status`
Esperado: clean.

- [ ] **Step 2: Criar tag**

```bash
git tag -a v0.1.0 -m "V0 — pipeline completo: vídeo + controle + HUD + menu"
```

- [ ] **Step 3: (Opcional) Push**

```bash
git push origin main --tags
```

---

## Critérios de aceitação do Sprint 7

- [ ] Soak 10 min: fps_avg ≥ 23.5, fps_min ≥ 20
- [ ] Latência p99 < 100 ms
- [ ] Drop rate < 1% em canal limpo
- [ ] Soak térmico 30 min: temp média < 80 °C
- [ ] Reconexão < 2 s (já validado em Sprint 3; reconfirmar)
- [ ] Tearing avaliado e decisão de fase 2 tomada
- [ ] Stack high water marks > 512 B em todas as tasks
- [ ] CLAUDE.md atualizado para refletir V0
- [ ] Tag `v0.1.0` no Git

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| Latência p99 > 100ms persistente | Reduzir jitter_buf para 1 slot (menos buffer = menos latência); ou cair para 40 MHz SPI se 80 MHz estiver instável |
| Temperatura > 80 °C | Reduzir CPU freq via `esp_pm_configure` (240→160 MHz) — última instância |
| Two-half-blit não melhora | Reverter; aceitar tearing em V0 e documentar como issue para V1 |
| Soak crasha por watchdog em task que demora muito | Investigar log; provavelmente `decode_task` precisa `esp_task_wdt_reset()` ou stack maior |

## Self-Review

- §10 todos os critérios cobertos: Tasks 2 (FPS/lat/drop), 3 (térmico), 4 (tearing), 6 (stack), Sprint 3 (reconexão)
- §4.5 anti-tearing fase 2: Tasks 4 e 5
- §12 riscos enfrentados: cada um tem mitigação documentada
- Tag final + cleanup do CLAUDE.md: Tasks 7-8

Sem placeholders. Sprint conclui o V0.
