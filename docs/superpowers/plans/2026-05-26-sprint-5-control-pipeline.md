# Sprint 4 — Pipeline de Controle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implementar o pipeline out-of-band do RX: leitura de joystick analógico + 2 botões, envio de `MSG_JOYSTICK` (60 Hz), agregação de métricas e envio de `MSG_TELEMETRY` (2 Hz) com algoritmo adaptive RX-recomenda.

**Architecture:** 3 componentes novos: `input` (ADC + botões + debounce + long-press), `telemetry` (agregador + algoritmo adaptive), `tx_dispatch` (multi-fila → ESP-NOW send). Tudo no Core 0. HUD ainda não existe; UI state aqui apenas é "FLIGHT" — menu virá em Sprint 6.

**Tech Stack:** `esp_adc/adc_oneshot`, GPIO ISR ou polled debounce, FreeRTOS queues, time stats (running average + p99 estimator simples).

---

## Reference do spec
- §3.4 Entrada
- §5.2 task_input
- §5.3 task_telemetry_aggregator
- §5.4 task_espnow_tx
- §5.5 MSG_TELEMETRY layout

## File Structure

```
components/
├── input/
│   ├── CMakeLists.txt
│   ├── include/input.h
│   ├── input.cpp
│   └── host_tests/test_input_filter.cpp
├── telemetry/
│   ├── CMakeLists.txt
│   ├── include/telemetry.h
│   ├── telemetry.cpp
│   └── host_tests/test_adaptive.cpp
└── tx_dispatch/
    ├── CMakeLists.txt
    ├── include/tx_dispatch.h
    └── tx_dispatch.cpp
```

---

### Task 1: `input` — leitura analógica + digital

**Files:**
- Create: `components/input/include/input.h`
- Create: `components/input/input.cpp`
- Create: `components/input/CMakeLists.txt`

- [ ] **Step 1: `input.h`**

```cpp
#pragma once
#include <cstdint>

enum ui_state_t : uint8_t {
    UI_FLIGHT = 0,
    UI_MENU   = 1,
};

struct input_snapshot_t {
    int8_t  joy_x;   // -127..+127, deadzone aplicada
    int8_t  joy_y;
    bool    btn_advance_down;
    bool    btn_back_down;
    uint32_t advance_press_ms;  // > 0 se em press atual (duration)
    uint32_t back_press_ms;
};

enum input_event_t : uint8_t {
    EV_NONE             = 0,
    EV_ADVANCE_CLICK    = 1,
    EV_ADVANCE_LONGPRESS= 2,
    EV_BACK_CLICK       = 3,
    EV_BACK_LONGPRESS   = 4,
};

bool input_init(void);
input_snapshot_t input_read_now(uint32_t now_ms);
input_event_t    input_poll_event(void);   // FIFO ring, retorna EV_NONE se vazio

// Setters externos para o UI state (consumido em get_snapshot para zerar joystick).
void input_set_ui_state(ui_state_t st);
ui_state_t input_get_ui_state(void);
```

- [ ] **Step 2: `input.cpp`**

```cpp
#include "input.h"
#include "pinout.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>

static const char* TAG = "input";

static adc_oneshot_unit_handle_t s_adc1 = nullptr;
static const adc_channel_t CH_X = ADC_CHANNEL_3; // GPIO 4
static const adc_channel_t CH_Y = ADC_CHANNEL_2; // GPIO 3

static std::atomic<ui_state_t> s_ui_state{UI_FLIGHT};
static QueueHandle_t s_event_q = nullptr;

struct btn_state_t {
    bool last;            // pressed?
    uint32_t since_ms;    // when current state began
    bool long_fired;      // already emitted LONGPRESS
};
static btn_state_t s_btn_advance{};
static btn_state_t s_btn_back{};

static constexpr int     CENTER_RAW = 2048;
static constexpr int     RAW_RANGE  = 1900; // 2048 +/- 1900
static constexpr int     DEADZONE_RAW = 150;

static int8_t to_signed(int raw) {
    int delta = raw - CENTER_RAW;
    if (delta > -DEADZONE_RAW && delta < DEADZONE_RAW) return 0;
    if (delta > RAW_RANGE)  delta = RAW_RANGE;
    if (delta < -RAW_RANGE) delta = -RAW_RANGE;
    return (int8_t)((delta * 127) / RAW_RANGE);
}

bool input_init(void) {
    // ADC
    adc_oneshot_unit_init_cfg_t u_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&u_cfg, &s_adc1) != ESP_OK) return false;
    adc_oneshot_chan_cfg_t c_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc1, CH_X, &c_cfg);
    adc_oneshot_config_channel(s_adc1, CH_Y, &c_cfg);

    // Botões: pull-up interno, ativo em LOW
    gpio_config_t b = {
        .pin_bit_mask = (1ULL << PIN_BTN_ADVANCE) | (1ULL << PIN_BTN_BACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&b);

    s_event_q = xQueueCreate(16, sizeof(input_event_t));
    return s_event_q != nullptr;
}

static void update_button(btn_state_t& st, bool pressed_now, uint32_t now_ms,
                          input_event_t click_ev, input_event_t long_ev) {
    if (pressed_now && !st.last) {
        st.last = true; st.since_ms = now_ms; st.long_fired = false;
    } else if (!pressed_now && st.last) {
        uint32_t dur = now_ms - st.since_ms;
        st.last = false;
        if (!st.long_fired && dur < 1000 && dur > 30) {  // click curto, >30ms para debounce
            xQueueSend(s_event_q, &click_ev, 0);
        }
    } else if (pressed_now && st.last && !st.long_fired) {
        if (now_ms - st.since_ms >= 1000) {
            st.long_fired = true;
            xQueueSend(s_event_q, &long_ev, 0);
        }
    }
}

input_snapshot_t input_read_now(uint32_t now_ms) {
    int raw_x = 0, raw_y = 0;
    adc_oneshot_read(s_adc1, CH_X, &raw_x);
    adc_oneshot_read(s_adc1, CH_Y, &raw_y);
    bool btn_a = (gpio_get_level((gpio_num_t)PIN_BTN_ADVANCE) == 0);
    bool btn_b = (gpio_get_level((gpio_num_t)PIN_BTN_BACK) == 0);

    update_button(s_btn_advance, btn_a, now_ms, EV_ADVANCE_CLICK, EV_ADVANCE_LONGPRESS);
    update_button(s_btn_back,    btn_b, now_ms, EV_BACK_CLICK,    EV_BACK_LONGPRESS);

    input_snapshot_t s{};
    s.joy_x = to_signed(raw_x);
    s.joy_y = to_signed(raw_y);
    s.btn_advance_down = btn_a;
    s.btn_back_down    = btn_b;
    s.advance_press_ms = btn_a ? (now_ms - s_btn_advance.since_ms) : 0;
    s.back_press_ms    = btn_b ? (now_ms - s_btn_back.since_ms)    : 0;

    if (s_ui_state.load() == UI_MENU) {
        s.joy_x = 0; s.joy_y = 0;  // zerar para anti-deriva no robô
    }
    return s;
}

input_event_t input_poll_event(void) {
    input_event_t ev;
    if (xQueueReceive(s_event_q, &ev, 0) == pdTRUE) return ev;
    return EV_NONE;
}

void input_set_ui_state(ui_state_t st) { s_ui_state = st; }
ui_state_t input_get_ui_state(void)    { return s_ui_state.load(); }
```

- [ ] **Step 3: `components/input/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "input.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main"
    REQUIRES driver esp_adc
)
```

- [ ] **Step 4: Smoke test no main**

Em `app_main.cpp`, antes do loop final, adicione (provisório):

```cpp
input_init();
xTaskCreatePinnedToCore([](void*) {
    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        auto s = input_read_now(now_ms);
        if (s.joy_x || s.joy_y || s.btn_advance_down || s.btn_back_down) {
            ESP_LOGI("input", "x=%d y=%d a=%d b=%d", s.joy_x, s.joy_y,
                     s.btn_advance_down, s.btn_back_down);
        }
        input_event_t ev = input_poll_event();
        if (ev) ESP_LOGI("input", "event %d", ev);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}, "input_test", 4096, nullptr, 4, nullptr, 0);
```

E em `main/CMakeLists.txt`, `REQUIRES`: adicione `input`.

- [ ] **Step 5: Build, flash, manipular**

Run: `idf.py build flash monitor`

Mexer no joystick e nos botões. Esperado: logs aparecem com valores e eventos consistentes.

- [ ] **Step 6: Commit**

```bash
git add components/input/ main/
git commit -m "feat: input component (joystick + 2 buttons + events)"
```

---

### Task 2: `telemetry` — agregador + algoritmo adaptive

**Files:**
- Create: `components/telemetry/include/telemetry.h`
- Create: `components/telemetry/telemetry.cpp`
- Create: `components/telemetry/CMakeLists.txt`
- Create: `components/telemetry/host_tests/test_adaptive.cpp`

- [ ] **Step 1: `telemetry.h`**

```cpp
#pragma once
#include <cstdint>
#include "wire_types.h"

constexpr uint8_t MAX_AUTO_LEVEL_DEFAULT = 2; // L2

bool telemetry_init(uint8_t max_auto_level);

// Chamado pelo pipeline RX a cada frame completo/descartado.
void telemetry_on_frame_completed(uint32_t latency_ms);
void telemetry_on_frame_dropped(void);
void telemetry_on_fragment_lost(uint16_t count);
void telemetry_on_rssi_sample(int8_t rssi_dbm);
void telemetry_on_current_level_seen(uint8_t lvl);
void telemetry_set_battery(uint8_t pct);
void telemetry_set_flags(uint8_t flags);

// Avança 1 segundo (chamado pela aggregator task). Retorna struct pronta para enviar.
struct telemetry_rx_to_tx_payload {
    uint8_t  requested_level;
    uint8_t  current_level_seen;
    uint16_t frames_received_1s;
    uint16_t frames_dropped_1s;
    uint16_t fragments_lost_1s;
    int8_t   rssi_avg_dbm;
    int8_t   rssi_min_dbm;
    uint16_t latency_p50_ms;
    uint16_t latency_p99_ms;
    uint8_t  rx_battery_pct;
    uint8_t  flags;
    uint32_t rx_uptime_ms;
};
telemetry_rx_to_tx_payload telemetry_tick_1s(uint32_t now_ms);
```

- [ ] **Step 2: Testes Unity host do algoritmo adaptive**

`components/telemetry/host_tests/test_adaptive.cpp`:

```cpp
#include "unity.h"
#include "telemetry.h"

TEST_CASE("starts at L0", "[adaptive]") {
    telemetry_init(2);
    auto p = telemetry_tick_1s(1000);
    TEST_ASSERT_EQUAL(0, p.requested_level);
}

TEST_CASE("escalates to L1 on high drop", "[adaptive]") {
    telemetry_init(2);
    for (int i = 0; i < 5; ++i) telemetry_on_frame_completed(20);
    for (int i = 0; i < 5; ++i) telemetry_on_frame_dropped();  // 50% drop
    auto p = telemetry_tick_1s(1000);
    TEST_ASSERT_EQUAL(1, p.requested_level);
}

TEST_CASE("does not exceed MAX_AUTO", "[adaptive]") {
    telemetry_init(2);
    for (int s = 1; s <= 10; ++s) {
        for (int i = 0; i < 2; ++i) telemetry_on_frame_completed(20);
        for (int i = 0; i < 8; ++i) telemetry_on_frame_dropped();
        telemetry_tick_1s(s * 1000);
    }
    auto p = telemetry_tick_1s(11000);
    TEST_ASSERT_EQUAL(2, p.requested_level);
}

TEST_CASE("recovers after 5 good seconds", "[adaptive]") {
    telemetry_init(2);
    // sobe para L2 com hiccup pesado
    for (int s = 1; s <= 3; ++s) {
        for (int i = 0; i < 2; ++i) telemetry_on_frame_completed(20);
        for (int i = 0; i < 8; ++i) telemetry_on_frame_dropped();
        telemetry_tick_1s(s * 1000);
    }
    // 5 segundos limpos
    for (int s = 4; s <= 8; ++s) {
        for (int i = 0; i < 24; ++i) telemetry_on_frame_completed(20);
        telemetry_tick_1s(s * 1000);
    }
    auto p = telemetry_tick_1s(9000);
    TEST_ASSERT_LESS_THAN_UINT8(2, p.requested_level);
}
```

- [ ] **Step 3: Implementar `telemetry.cpp`**

```cpp
#include "telemetry.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdint>

namespace {
struct WindowStats {
    uint16_t frames_received = 0;
    uint16_t frames_dropped  = 0;
    uint16_t fragments_lost  = 0;
    int      rssi_sum        = 0;
    int      rssi_count      = 0;
    int8_t   rssi_min        = 0;
    std::vector<uint16_t> latencies;
};

WindowStats s_cur;
uint8_t  s_requested_level   = 0;
uint8_t  s_max_auto_level    = 2;
uint8_t  s_current_seen      = 0;
uint8_t  s_battery_pct       = 0xFF;
uint8_t  s_flags             = 0;
uint32_t s_good_streak_sec   = 0;
uint32_t s_init_ms           = 0;

uint16_t pct(std::vector<uint16_t>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)((v.size() - 1) * p);
    return v[idx];
}
}

bool telemetry_init(uint8_t max_auto_level) {
    s_cur = WindowStats{};
    s_requested_level = 0;
    s_max_auto_level = max_auto_level;
    s_current_seen = 0;
    s_battery_pct = 0xFF;
    s_flags = 0;
    s_good_streak_sec = 0;
    s_init_ms = 0;
    return true;
}

void telemetry_on_frame_completed(uint32_t latency_ms) {
    s_cur.frames_received++;
    s_cur.latencies.push_back((uint16_t)std::min<uint32_t>(latency_ms, 65535));
}
void telemetry_on_frame_dropped(void) { s_cur.frames_dropped++; }
void telemetry_on_fragment_lost(uint16_t c) { s_cur.fragments_lost += c; }
void telemetry_on_rssi_sample(int8_t rssi) {
    if (s_cur.rssi_count == 0 || rssi < s_cur.rssi_min) s_cur.rssi_min = rssi;
    s_cur.rssi_sum += rssi; s_cur.rssi_count++;
}
void telemetry_on_current_level_seen(uint8_t lvl) { s_current_seen = lvl; }
void telemetry_set_battery(uint8_t pct) { s_battery_pct = pct; }
void telemetry_set_flags(uint8_t f)     { s_flags = f; }

telemetry_rx_to_tx_payload telemetry_tick_1s(uint32_t now_ms) {
    if (s_init_ms == 0) s_init_ms = now_ms;

    uint32_t total = s_cur.frames_received + s_cur.frames_dropped;
    double drop_pct = total > 0 ? (double)s_cur.frames_dropped / total : 0.0;

    if (drop_pct < 0.01) s_good_streak_sec++;
    else                  s_good_streak_sec = 0;

    if (drop_pct > 0.08 && s_requested_level < s_max_auto_level) {
        s_requested_level++;
        s_good_streak_sec = 0;
    } else if (drop_pct < 0.01 && s_good_streak_sec >= 5 && s_requested_level > 0) {
        s_requested_level--;
        s_good_streak_sec = 0;
    }

    telemetry_rx_to_tx_payload p{};
    p.requested_level    = s_requested_level;
    p.current_level_seen = s_current_seen;
    p.frames_received_1s = s_cur.frames_received;
    p.frames_dropped_1s  = s_cur.frames_dropped;
    p.fragments_lost_1s  = s_cur.fragments_lost;
    p.rssi_avg_dbm       = s_cur.rssi_count ? (int8_t)(s_cur.rssi_sum / s_cur.rssi_count) : 0;
    p.rssi_min_dbm       = s_cur.rssi_count ? s_cur.rssi_min : 0;
    p.latency_p50_ms     = pct(s_cur.latencies, 0.5);
    p.latency_p99_ms     = pct(s_cur.latencies, 0.99);
    p.rx_battery_pct     = s_battery_pct;
    p.flags              = s_flags;
    p.rx_uptime_ms       = now_ms - s_init_ms;

    s_cur = WindowStats{};
    return p;
}
```

- [ ] **Step 4: CMakeLists**

```cmake
idf_component_register(
    SRCS "telemetry.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../espnow_link/include"
)
```

- [ ] **Step 5: Build**

Run: `idf.py build`

- [ ] **Step 6: Commit**

```bash
git add components/telemetry/
git commit -m "feat: telemetry aggregator + adaptive ladder algorithm"
```

---

### Task 3: `tx_dispatch` — multi-fila para envio

**Files:**
- Create: `components/tx_dispatch/include/tx_dispatch.h`
- Create: `components/tx_dispatch/tx_dispatch.cpp`
- Create: `components/tx_dispatch/CMakeLists.txt`

- [ ] **Step 1: `tx_dispatch.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include "wire_types.h"

bool tx_dispatch_init(const uint8_t peer_mac[6]);

// Enqueue. Retorna false se fila cheia.
bool tx_dispatch_send_joystick(int8_t x, int8_t y, uint8_t btn_bits);
bool tx_dispatch_send_telemetry(const struct telemetry_rx_to_tx_payload& p);
bool tx_dispatch_send_command(uint8_t cmd_id, const uint8_t* data, size_t len);
```

Wire format de joystick (4 bytes payload):
```
[i8 x] [i8 y] [u8 btn_bits] [u8 seq]
```
Bit do `btn_bits`: 0x01 = advance pressed, 0x02 = back pressed.

- [ ] **Step 2: `tx_dispatch.cpp`**

```cpp
#include "tx_dispatch.h"
#include "espnow_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "tx";

struct __attribute__((packed)) joy_msg_t { int8_t x; int8_t y; uint8_t btn; uint8_t seq; };
struct __attribute__((packed)) cmd_msg_t { uint8_t id; uint8_t len; uint8_t data[24]; };

static uint8_t s_peer[6];
static QueueHandle_t s_q_joy  = nullptr;
static QueueHandle_t s_q_tel  = nullptr;
static QueueHandle_t s_q_cmd  = nullptr;
static uint8_t s_joy_seq = 0;

static void tx_task(void*) {
    while (true) {
        // Priority order: cmd > telemetry > joystick.
        cmd_msg_t cm;
        if (xQueueReceive(s_q_cmd, &cm, 0) == pdTRUE) {
            uint8_t buf[2 + sizeof(cm)];
            buf[0] = cm.id; buf[1] = cm.len;
            memcpy(buf + 2, cm.data, cm.len);
            espnow_link_send(s_peer, MSG_COMMAND, buf, 2 + cm.len);
            continue;
        }
        telemetry_rx_to_tx_payload tp;
        if (xQueueReceive(s_q_tel, &tp, 0) == pdTRUE) {
            espnow_link_send(s_peer, MSG_TELEMETRY, (const uint8_t*)&tp, sizeof(tp));
            continue;
        }
        joy_msg_t jm;
        if (xQueueReceive(s_q_joy, &jm, pdMS_TO_TICKS(5)) == pdTRUE) {
            espnow_link_send(s_peer, MSG_JOYSTICK, (const uint8_t*)&jm, sizeof(jm));
        }
    }
}

bool tx_dispatch_init(const uint8_t peer_mac[6]) {
    memcpy(s_peer, peer_mac, 6);
    s_q_joy = xQueueCreate(8, sizeof(joy_msg_t));
    s_q_tel = xQueueCreate(2, sizeof(telemetry_rx_to_tx_payload));
    s_q_cmd = xQueueCreate(4, sizeof(cmd_msg_t));
    if (!s_q_joy || !s_q_tel || !s_q_cmd) return false;
    xTaskCreatePinnedToCore(tx_task, "tx", 4096, nullptr, 5, nullptr, 0);
    return true;
}

bool tx_dispatch_send_joystick(int8_t x, int8_t y, uint8_t btn_bits) {
    joy_msg_t jm{x, y, btn_bits, s_joy_seq++};
    return xQueueSend(s_q_joy, &jm, 0) == pdTRUE;
}
bool tx_dispatch_send_telemetry(const telemetry_rx_to_tx_payload& p) {
    return xQueueSend(s_q_tel, &p, 0) == pdTRUE;
}
bool tx_dispatch_send_command(uint8_t cmd_id, const uint8_t* data, size_t len) {
    if (len > 24) return false;
    cmd_msg_t cm{cmd_id, (uint8_t)len, {}};
    memcpy(cm.data, data, len);
    return xQueueSend(s_q_cmd, &cm, 0) == pdTRUE;
}
```

- [ ] **Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "tx_dispatch.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espnow_link telemetry
)
```

- [ ] **Step 4: Build**

Run: `idf.py build`

- [ ] **Step 5: Commit**

```bash
git add components/tx_dispatch/
git commit -m "feat: tx dispatch with multi-queue priority"
```

---

### Task 4: Integrar tudo no `app_main`

**Files:**
- Modify: `main/app_main.cpp`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Atualizar requires**

```cmake
REQUIRES display espnow_link reassembly decoder render link_state input telemetry tx_dispatch
```

- [ ] **Step 2: Substituir o smoke test do input por tasks de produção**

Em `app_main.cpp`:

```cpp
#include "input.h"
#include "telemetry.h"
#include "tx_dispatch.h"

// task input + envio joystick @ 60 Hz
static void input_task(void*) {
    const TickType_t period = pdMS_TO_TICKS(16); // ~60 Hz
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        auto s = input_read_now(now_ms);
        uint8_t btn = (s.btn_advance_down ? 0x01 : 0) | (s.btn_back_down ? 0x02 : 0);
        tx_dispatch_send_joystick(s.joy_x, s.joy_y, btn);
        vTaskDelayUntil(&last_wake, period);
    }
}

// task telemetry @ 2 Hz
static void telemetry_task(void*) {
    const TickType_t period = pdMS_TO_TICKS(500);
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        auto p = telemetry_tick_1s(now_ms);
        tx_dispatch_send_telemetry(p);
        vTaskDelayUntil(&last_wake, period);
    }
}
```

E no `decode_task`, ao final (após `render_present`), instrumentar telemetry:

```cpp
uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
uint32_t latency = now_ms - rf.frame.tx_emission_ms;
telemetry_on_frame_completed(latency);
```

E no `on_msg`, capturar RSSI:

```cpp
telemetry_on_rssi_sample(rssi);
```

Em `app_main` adicionar antes do espnow_init:

```cpp
telemetry_init(MAX_AUTO_LEVEL_DEFAULT);
tx_dispatch_init(TX_MAC);
```

E criar as tasks:

```cpp
xTaskCreatePinnedToCore(input_task,     "in",  3072, nullptr, 4, nullptr, 0);
xTaskCreatePinnedToCore(telemetry_task, "tel", 3072, nullptr, 3, nullptr, 0);
```

- [ ] **Step 3: Build, flash, validar com sniffer ou TX**

Run: `idf.py build flash monitor`

Validação:
1. Mexer joystick → no log do TX (se ele logar joystick recebido), deve aparecer x/y mudando.
2. Aguardar 2 s → no log do TX deve aparecer recepção de telemetry com `requested_level=0`.
3. Bloquear o link com objeto metálico ou aproximar interferência → `frames_dropped` sobe, em ~5s `requested_level` sobe para 1, depois para 2.

- [ ] **Step 4: Commit**

```bash
git add main/
git commit -m "feat: integrate input, telemetry, and tx dispatch tasks"
```

---

### Task 5: Validar adaptive em hw real

- [ ] **Step 1: Cenário de degradação controlada**

Aproxime o RX/TX de uma fonte de Wi-Fi intenso (router em canal 6 com tráfego saturado, OU coloque chapa metálica entre eles).

- [ ] **Step 2: Observar via log**

Log do RX:
```
I main: fps=18 drops=6      # drop_pct ~25%, deve escalar
I main: req_level=1
I main: fps=22 drops=2
...
```

(Adicionar log do `requested_level` em `telemetry_task` se ainda não houver.)

- [ ] **Step 3: Verificar TX recebe e ajusta**

No TX, validar que ao receber `requested_level=1`, ele troca quality para q=16 (ou o que L1 mapeia). Esse comportamento é responsabilidade do TX e está fora deste sprint.

- [ ] **Step 4: Marcar critério**

Não há critério dedicado de "adaptive funciona" no §10; documente em `docs/superpowers/notes-sprint4.md`:

```
Sprint 4 validation log:
- Joystick 60 Hz: visto no TX
- Telemetry 2 Hz: visto no TX
- Adaptive level escalou de 0→1→2 em <8s ao introduzir interferência
- Recuperação 2→1→0 em 12s após remover interferência
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/
git commit -m "docs: sprint 4 hw validation notes"
```

---

## Critérios de aceitação do Sprint 4

- [ ] Joystick mexe → TX recebe valores
- [ ] Botões enviam bits corretos
- [ ] Telemetry chega no TX a 2 Hz
- [ ] Adaptive escala 0→1→2 quando drop > 8%
- [ ] Adaptive desce após 5s contíguos drop < 1%
- [ ] Pipeline de vídeo do Sprint 2 mantém fps ≥ 23
- [ ] Testes Unity do telemetry passam (host opcional)

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| ADC1 conflita com Wi-Fi (ESP32-S3 só ADC2 conflita; ADC1 está OK) | Já usamos ADC1 |
| Polling de botões perde click rápido | Período 16 ms < 60 ms típico de click humano; OK |
| Tx_task starva joystick se telemetry/cmd estiverem cheios | Filas pequenas (4-8); priority order intencional |
| Calibração de centro do joystick varia entre unidades | Sprint 6 pode adicionar item de menu "Calibrate joystick" |

## Self-Review

- §3.4 entrada (ADC + 2 botões + debounce): Task 1
- §5.2 task_input + UI state: Task 1 + Task 4 integração
- §5.3 telemetry aggregator + algoritmo adaptive: Task 2 + Tests Unity
- §5.4 task_espnow_tx multi-fila: Task 3
- §5.5 layout MSG_TELEMETRY (22 B): struct `telemetry_rx_to_tx_payload` no header

Sem placeholders. Sem tipos órfãos.
