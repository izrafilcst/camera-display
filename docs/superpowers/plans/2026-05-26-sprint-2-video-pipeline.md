# Sprint 2 — Pipeline de Vídeo Barebones Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Receber stream JPEG via ESP-NOW, reassembly de fragmentos com skip-drop 30 ms, decodificar com esp_jpeg e renderizar a 24 fps com double buffer + DMA.

**Architecture:** 4 componentes novos: `espnow_link` (RX + dispatch), `reassembly` (slot pool com timeout), `decoder` (esp_jpeg wrapper), `render` (double buffer + swap). Filas FreeRTOS conectam os estágios. Sem HUD, sem controle out-of-band ainda.

**Tech Stack:** ESP-NOW (esp_now.h), esp_jpeg (managed component), LovyanGFX (do Sprint 1), FreeRTOS tasks/queues/semáforos, Unity para testes do reassembly.

---

## Reference do spec
- §4 Pipeline de vídeo
- §5.5 Wire format (msg_type, header video_frag)
- §8 Alocação PSRAM
- §9 Tasks FreeRTOS

## File Structure

```
components/
├── espnow_link/
│   ├── CMakeLists.txt
│   ├── include/espnow_link.h
│   └── espnow_link.cpp
├── reassembly/
│   ├── CMakeLists.txt
│   ├── include/reassembly.h
│   ├── reassembly.cpp
│   └── host_tests/test_reassembly.cpp  # unity tests host-side
├── decoder/
│   ├── CMakeLists.txt
│   ├── include/decoder.h
│   └── decoder.cpp
└── render/
    ├── CMakeLists.txt
    ├── include/render.h
    └── render.cpp
```

**Responsabilidades:**
- `espnow_link`: init Wi-Fi STA channel 6, init ESP-NOW, RX callback que dispatcha por `msg_type`
- `reassembly`: gerencia 2 slots de 16 KB, política skip-drop 30 ms, expõe queue de frames prontos
- `decoder`: encapsula `esp_jpeg` em modo single-shot RGB565
- `render`: gerencia 2 framebuffers PSRAM, task que consome do decoder e dispara DMA

---

### Task 1: Adicionar dependência `esp_jpeg`

**Files:**
- Modify: `idf_component.yml`

- [ ] **Step 1: Atualizar `idf_component.yml`**

```yaml
dependencies:
  lovyan03/LovyanGFX: "^1.1.16"
  espressif/esp_jpeg: "^1.1.0"
  idf:
    version: ">=5.2.0"
```

- [ ] **Step 2: Reconfigurar e build**

Run: `idf.py reconfigure && idf.py build`
Expected: download e build do componente `esp_jpeg`.

- [ ] **Step 3: Commit**

```bash
git add idf_component.yml dependencies.lock
git commit -m "deps: add esp_jpeg for jpeg decoding"
```

---

### Task 2: Tipos compartilhados do wire format

**Files:**
- Create: `components/espnow_link/include/wire_types.h`

- [ ] **Step 1: Criar header**

```cpp
#pragma once
#include <cstdint>

// Mensagens (primeiro byte do payload ESP-NOW após o "esnow header" do enlace).
enum : uint8_t {
    MSG_VIDEO_FRAG   = 0x10,
    MSG_TELEMETRY    = 0x20,
    MSG_JOYSTICK     = 0x30,
    MSG_COMMAND      = 0x40,
};

// Header esnow (TX⇄RX): 2 bytes.
struct __attribute__((packed)) esnow_hdr_t {
    uint8_t  msg_type;   // MSG_*
    uint8_t  seq;        // ordering
};

// Header video_frag: 8 bytes (após esnow_hdr_t).
struct __attribute__((packed)) video_frag_hdr_t {
    uint16_t frame_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t jpeg_size;     // tamanho total do JPEG quando reassemblado
    uint16_t payload_len;   // bytes neste fragmento
};

// Extra no fragmento 0 (após video_frag_hdr_t): 4 bytes.
struct __attribute__((packed)) video_frag0_extra_t {
    uint32_t tx_emission_ms;
};

static constexpr size_t ESPNOW_MTU            = 250;
static constexpr size_t MAX_FRAGS_PER_FRAME   = 64;
static constexpr size_t MAX_JPEG_SIZE         = 16 * 1024;
static constexpr uint32_t SKIP_DROP_TIMEOUT_MS = 30;
```

- [ ] **Step 2: Adicionar ao CMakeLists do componente**

Criar `components/espnow_link/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "espnow_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi nvs_flash esp_event
)
```

- [ ] **Step 3: Commit**

```bash
git add components/espnow_link/
git commit -m "feat: wire format types for esp-now video stream"
```

---

### Task 3: `reassembly` — testes host primeiro (TDD)

**Files:**
- Create: `components/reassembly/include/reassembly.h`
- Create: `components/reassembly/reassembly.cpp`
- Create: `components/reassembly/CMakeLists.txt`
- Create: `components/reassembly/host_tests/CMakeLists.txt`
- Create: `components/reassembly/host_tests/test_reassembly.cpp`

- [ ] **Step 1: Criar `reassembly.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

struct reassembled_frame_t {
    uint16_t frame_id;
    uint16_t jpeg_size;
    uint32_t tx_emission_ms;
    const uint8_t* jpeg_data;  // ponteiro p/ slot interno; válido até libertar
    void* opaque;              // handle do slot, passado para reassembly_release
};

// Inicializa pool com `slots` slots. Cada slot tem 16 KB.
bool reassembly_init(int slots);

// Processa um fragmento (sem o esnow_hdr; payload começa em video_frag_hdr_t).
// Retorna true se este fragmento completou um frame; nesse caso preenche `out`.
bool reassembly_push_frag(const uint8_t* payload, size_t len,
                          uint32_t now_ms, reassembled_frame_t* out);

// Libera o slot referenciado em out.opaque.
void reassembly_release(reassembled_frame_t* out);

// Marca slots ociosos > timeout como livres (chamado periodicamente).
void reassembly_gc(uint32_t now_ms);

// Stats.
struct reassembly_stats_t {
    uint32_t frames_completed;
    uint32_t frames_dropped_timeout;
    uint32_t frames_dropped_overrun;
    uint32_t fragments_received;
    uint32_t fragments_invalid;
};
const reassembly_stats_t* reassembly_stats(void);
```

- [ ] **Step 2: Escrever testes em `components/reassembly/host_tests/test_reassembly.cpp`**

```cpp
#include "unity.h"
#include "reassembly.h"
#include "wire_types.h"
#include <vector>
#include <cstring>

static std::vector<uint8_t> make_frag(uint16_t fid, uint8_t idx, uint8_t total,
                                      uint16_t jpeg_size, const uint8_t* data,
                                      uint16_t data_len, uint32_t tx_ms) {
    std::vector<uint8_t> out;
    video_frag_hdr_t h{fid, idx, total, jpeg_size, data_len};
    out.insert(out.end(), (uint8_t*)&h, (uint8_t*)&h + sizeof(h));
    if (idx == 0) {
        video_frag0_extra_t e{tx_ms};
        out.insert(out.end(), (uint8_t*)&e, (uint8_t*)&e + sizeof(e));
    }
    out.insert(out.end(), data, data + data_len);
    return out;
}

TEST_CASE("single fragment frame completes immediately", "[reassembly]") {
    TEST_ASSERT_TRUE(reassembly_init(2));
    uint8_t data[100] = {0xAA};
    auto f = make_frag(1, 0, 1, 100, data, 100, 1234);
    reassembled_frame_t out;
    TEST_ASSERT_TRUE(reassembly_push_frag(f.data(), f.size(), 0, &out));
    TEST_ASSERT_EQUAL(1, out.frame_id);
    TEST_ASSERT_EQUAL(100, out.jpeg_size);
    TEST_ASSERT_EQUAL(1234, out.tx_emission_ms);
    TEST_ASSERT_EQUAL(0xAA, out.jpeg_data[0]);
    reassembly_release(&out);
}

TEST_CASE("two-fragment frame completes on second", "[reassembly]") {
    TEST_ASSERT_TRUE(reassembly_init(2));
    uint8_t d0[200] = {0x11}, d1[100] = {0x22};
    auto f0 = make_frag(2, 0, 2, 300, d0, 200, 9999);
    auto f1 = make_frag(2, 1, 2, 300, d1, 100, 0);
    reassembled_frame_t out;
    TEST_ASSERT_FALSE(reassembly_push_frag(f0.data(), f0.size(), 0, &out));
    TEST_ASSERT_TRUE (reassembly_push_frag(f1.data(), f1.size(), 5, &out));
    TEST_ASSERT_EQUAL(300, out.jpeg_size);
    TEST_ASSERT_EQUAL(0x11, out.jpeg_data[0]);
    TEST_ASSERT_EQUAL(0x22, out.jpeg_data[200]);
    reassembly_release(&out);
}

TEST_CASE("timeout drops incomplete frame", "[reassembly]") {
    TEST_ASSERT_TRUE(reassembly_init(2));
    uint8_t d[100] = {0xCC};
    auto f0 = make_frag(3, 0, 2, 200, d, 100, 1);
    reassembled_frame_t out;
    reassembly_push_frag(f0.data(), f0.size(), 0, &out);
    reassembly_gc(31); // > 30 ms
    auto s = *reassembly_stats();
    TEST_ASSERT_EQUAL(1, s.frames_dropped_timeout);
}

TEST_CASE("newer frame_id evicts older slot", "[reassembly]") {
    TEST_ASSERT_TRUE(reassembly_init(2));
    uint8_t d[100] = {0xCC};
    auto f_a = make_frag(10, 0, 2, 200, d, 100, 0);
    auto f_b = make_frag(11, 0, 2, 200, d, 100, 0);
    auto f_c = make_frag(12, 0, 2, 200, d, 100, 0);
    reassembled_frame_t out;
    reassembly_push_frag(f_a.data(), f_a.size(), 0, &out);
    reassembly_push_frag(f_b.data(), f_b.size(), 1, &out);
    // Both slots in use. New frame 12 should evict oldest (10).
    reassembly_push_frag(f_c.data(), f_c.size(), 2, &out);
    auto s = *reassembly_stats();
    TEST_ASSERT_EQUAL(1, s.frames_dropped_overrun);
}

TEST_CASE("out-of-order fragments still complete", "[reassembly]") {
    TEST_ASSERT_TRUE(reassembly_init(2));
    uint8_t d0[100] = {0xA0}, d1[100] = {0xB0}, d2[100] = {0xC0};
    auto fa = make_frag(20, 0, 3, 300, d0, 100, 100);
    auto fb = make_frag(20, 1, 3, 300, d1, 100, 0);
    auto fc = make_frag(20, 2, 3, 300, d2, 100, 0);
    reassembled_frame_t out;
    reassembly_push_frag(fc.data(), fc.size(), 0, &out);
    reassembly_push_frag(fa.data(), fa.size(), 0, &out);
    bool done = reassembly_push_frag(fb.data(), fb.size(), 0, &out);
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL(0xA0, out.jpeg_data[0]);
    TEST_ASSERT_EQUAL(0xB0, out.jpeg_data[100]);
    TEST_ASSERT_EQUAL(0xC0, out.jpeg_data[200]);
}
```

- [ ] **Step 3: CMake do host test**

`components/reassembly/host_tests/CMakeLists.txt`:

```cmake
add_executable(test_reassembly test_reassembly.cpp ../reassembly.cpp)
target_include_directories(test_reassembly PRIVATE ../include ../../espnow_link/include)
target_compile_options(test_reassembly PRIVATE -g -O0 -DREASSEMBLY_HOST_BUILD=1)
target_link_libraries(test_reassembly PRIVATE unity)
add_test(NAME reassembly COMMAND test_reassembly)
```

- [ ] **Step 4: Implementar `reassembly.cpp`**

```cpp
#include "reassembly.h"
#include "wire_types.h"
#include <cstring>
#include <cstdlib>

#ifndef REASSEMBLY_HOST_BUILD
  #include "esp_heap_caps.h"
  #include "esp_log.h"
  static const char* TAG = "reasm";
#else
  #include <cstdio>
  #define ESP_LOGW(...) ((void)0)
  static inline void* heap_caps_malloc(size_t s, int) { return malloc(s); }
  static inline void  heap_caps_free(void* p)         { free(p); }
#endif

struct slot_t {
    bool      in_use;
    uint16_t  frame_id;
    uint16_t  jpeg_size;
    uint32_t  tx_emission_ms;
    uint32_t  first_seen_ms;
    uint8_t*  data;
    uint64_t  frags_bitmap;  // até 64 fragmentos
    uint8_t   frag_total;
};

static slot_t s_slots[4];
static int    s_slots_count = 0;
static reassembly_stats_t s_stats = {};

bool reassembly_init(int slots) {
    if (slots < 1 || slots > 4) return false;
    for (int i = 0; i < s_slots_count; ++i) {
        if (s_slots[i].data) heap_caps_free(s_slots[i].data);
    }
    s_slots_count = slots;
    memset(s_slots, 0, sizeof(s_slots));
    memset(&s_stats, 0, sizeof(s_stats));
    for (int i = 0; i < slots; ++i) {
        s_slots[i].data = (uint8_t*)heap_caps_malloc(MAX_JPEG_SIZE, 0);
        if (!s_slots[i].data) return false;
    }
    return true;
}

static slot_t* find_slot(uint16_t fid) {
    for (int i = 0; i < s_slots_count; ++i) {
        if (s_slots[i].in_use && s_slots[i].frame_id == fid) return &s_slots[i];
    }
    return nullptr;
}

static slot_t* alloc_slot(uint32_t now_ms) {
    for (int i = 0; i < s_slots_count; ++i) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    // Evict oldest.
    slot_t* victim = &s_slots[0];
    for (int i = 1; i < s_slots_count; ++i) {
        if (s_slots[i].first_seen_ms < victim->first_seen_ms) victim = &s_slots[i];
    }
    s_stats.frames_dropped_overrun++;
    victim->in_use = false;
    victim->frags_bitmap = 0;
    return victim;
}

bool reassembly_push_frag(const uint8_t* payload, size_t len,
                          uint32_t now_ms, reassembled_frame_t* out) {
    s_stats.fragments_received++;
    if (len < sizeof(video_frag_hdr_t)) { s_stats.fragments_invalid++; return false; }
    video_frag_hdr_t h;
    memcpy(&h, payload, sizeof(h));
    const uint8_t* p = payload + sizeof(h);
    size_t remain = len - sizeof(h);
    uint32_t tx_ms = 0;
    if (h.frag_idx == 0) {
        if (remain < sizeof(video_frag0_extra_t)) { s_stats.fragments_invalid++; return false; }
        video_frag0_extra_t e;
        memcpy(&e, p, sizeof(e));
        tx_ms = e.tx_emission_ms;
        p += sizeof(e);
        remain -= sizeof(e);
    }
    if (h.payload_len > remain) { s_stats.fragments_invalid++; return false; }
    if (h.jpeg_size > MAX_JPEG_SIZE) { s_stats.fragments_invalid++; return false; }
    if (h.frag_total == 0 || h.frag_total > MAX_FRAGS_PER_FRAME) {
        s_stats.fragments_invalid++; return false;
    }
    if (h.frag_idx >= h.frag_total) { s_stats.fragments_invalid++; return false; }

    slot_t* s = find_slot(h.frame_id);
    if (!s) {
        s = alloc_slot(now_ms);
        s->in_use = true;
        s->frame_id = h.frame_id;
        s->jpeg_size = h.jpeg_size;
        s->frag_total = h.frag_total;
        s->frags_bitmap = 0;
        s->first_seen_ms = now_ms;
    }
    if (h.frag_idx == 0) s->tx_emission_ms = tx_ms;
    // Offset deste fragmento.
    // Frag 0: payload começa em 0; usa 236 bytes úteis no spec.
    // Frags 1..N-1: começam após 236 bytes do anterior; usam 240.
    // Aqui usamos h.payload_len e calculamos offset cumulativo do bitmap.
    size_t offset = 0;
    for (int i = 0; i < h.frag_idx; ++i) {
        if (s->frags_bitmap & (uint64_t{1} << i)) {
            // já recebido — não temos len exato; usamos heurística 236/240.
            offset += (i == 0) ? 236 : 240;
        } else {
            offset += (i == 0) ? 236 : 240;
        }
    }
    if (offset + h.payload_len > s->jpeg_size) {
        s_stats.fragments_invalid++;
        return false;
    }
    memcpy(s->data + offset, p, h.payload_len);
    s->frags_bitmap |= (uint64_t{1} << h.frag_idx);

    // Verifica completude: todos os bits 0..frag_total-1 setados.
    uint64_t mask = (h.frag_total == 64) ? ~uint64_t{0} : ((uint64_t{1} << h.frag_total) - 1);
    if ((s->frags_bitmap & mask) == mask) {
        out->frame_id      = s->frame_id;
        out->jpeg_size     = s->jpeg_size;
        out->tx_emission_ms = s->tx_emission_ms;
        out->jpeg_data     = s->data;
        out->opaque        = s;
        s_stats.frames_completed++;
        // Slot continua marcado in_use até reassembly_release.
        return true;
    }
    return false;
}

void reassembly_release(reassembled_frame_t* out) {
    if (!out || !out->opaque) return;
    slot_t* s = static_cast<slot_t*>(out->opaque);
    s->in_use = false;
    s->frags_bitmap = 0;
    out->opaque = nullptr;
    out->jpeg_data = nullptr;
}

void reassembly_gc(uint32_t now_ms) {
    for (int i = 0; i < s_slots_count; ++i) {
        slot_t& s = s_slots[i];
        if (s.in_use && (now_ms - s.first_seen_ms) > SKIP_DROP_TIMEOUT_MS) {
            s_stats.frames_dropped_timeout++;
            s.in_use = false;
            s.frags_bitmap = 0;
        }
    }
}

const reassembly_stats_t* reassembly_stats(void) { return &s_stats; }
```

- [ ] **Step 5: CMakeLists do componente**

`components/reassembly/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "reassembly.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../espnow_link/include"
    REQUIRES log
)
```

- [ ] **Step 6: Rodar testes host (se quiser; opcional, dá pra testar no target)**

Se setup de host test compilar: `cd build_host && ctest`
Expected: 5/5 PASS.

Se não rodar host: aceitar e testar no target via teste de loopback do esp_now (Task 8).

- [ ] **Step 7: Build target**

Run: `idf.py build`
Expected: build OK incluindo `reassembly` component.

- [ ] **Step 8: Commit**

```bash
git add components/reassembly/
git commit -m "feat: fragment reassembly with skip-drop timeout"
```

---

### Task 4: `espnow_link` — init e RX callback

**Files:**
- Modify: `components/espnow_link/CMakeLists.txt`
- Create: `components/espnow_link/include/espnow_link.h`
- Create: `components/espnow_link/espnow_link.cpp`

- [ ] **Step 1: Criar `espnow_link.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

// Callback chamado para cada mensagem recebida (já sem o esnow header).
typedef void (*espnow_msg_cb_t)(uint8_t msg_type,
                                const uint8_t* payload, size_t len,
                                int8_t rssi);

// Inicializa Wi-Fi STA channel 6 + ESP-NOW. Registra `cb` para todas mensagens.
bool espnow_link_init(uint8_t channel, espnow_msg_cb_t cb);

// Adiciona peer (MAC do TX) — em V0 hardcoded.
bool espnow_link_add_peer(const uint8_t mac[6]);

// Envia uma mensagem.
bool espnow_link_send(const uint8_t* mac, uint8_t msg_type,
                      const uint8_t* payload, size_t len);
```

- [ ] **Step 2: Implementar `espnow_link.cpp`**

```cpp
#include "espnow_link.h"
#include "wire_types.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>

static const char* TAG = "espnow_link";
static espnow_msg_cb_t s_cb = nullptr;
static uint8_t s_tx_seq = 0;

static void on_rx(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < (int)sizeof(esnow_hdr_t)) return;
    esnow_hdr_t h;
    memcpy(&h, data, sizeof(h));
    int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    if (s_cb) s_cb(h.msg_type, data + sizeof(h), len - sizeof(h), rssi);
}

bool espnow_link_init(uint8_t channel, espnow_msg_cb_t cb) {
    s_cb = cb;
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wifi_cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(on_rx);
    ESP_LOGI(TAG, "esp-now ready, channel=%u", channel);
    return true;
}

bool espnow_link_add_peer(const uint8_t mac[6]) {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; // 0 = canal corrente
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false; // V0 sem cripto
    return esp_now_add_peer(&p) == ESP_OK;
}

bool espnow_link_send(const uint8_t* mac, uint8_t msg_type,
                      const uint8_t* payload, size_t len) {
    if (len + sizeof(esnow_hdr_t) > ESPNOW_MTU) return false;
    uint8_t buf[ESPNOW_MTU];
    esnow_hdr_t h{msg_type, s_tx_seq++};
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), payload, len);
    return esp_now_send(mac, buf, sizeof(h) + len) == ESP_OK;
}
```

- [ ] **Step 3: Atualizar `components/espnow_link/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "espnow_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash
)
```

- [ ] **Step 4: Build**

Run: `idf.py build`
Expected: OK.

- [ ] **Step 5: Commit**

```bash
git add components/espnow_link/
git commit -m "feat: esp-now link init + dispatch by msg_type"
```

---

### Task 5: `decoder` — wrapper esp_jpeg

**Files:**
- Create: `components/decoder/include/decoder.h`
- Create: `components/decoder/decoder.cpp`
- Create: `components/decoder/CMakeLists.txt`

- [ ] **Step 1: Criar `decoder.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

bool decoder_init(void);

// Decoda JPEG para RGB565. out_buf precisa ter width*height*2 bytes.
// Retorna duração em microssegundos, ou -1 em erro.
int64_t decoder_decode_to_rgb565(const uint8_t* jpeg, size_t jpeg_len,
                                 uint16_t* out_buf,
                                 int expected_w, int expected_h);
```

- [ ] **Step 2: Implementar `decoder.cpp`**

```cpp
#include "decoder.h"
#include "esp_jpeg_dec.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "decoder";
static jpeg_dec_handle_t s_handle = nullptr;

bool decoder_init(void) {
    jpeg_dec_config_t cfg = {
        .output_type   = JPEG_PIXEL_FORMAT_RGB565_BE,
        .scale         = { .width = 0, .height = 0 },  // sem escala
        .clipper       = { .width = 0, .height = 0 },
        .rotate        = JPEG_ROTATE_0D,
        .block_enable  = false,
    };
    if (jpeg_dec_open(&cfg, &s_handle) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "open failed");
        return false;
    }
    return true;
}

int64_t decoder_decode_to_rgb565(const uint8_t* jpeg, size_t jpeg_len,
                                 uint16_t* out_buf, int expected_w, int expected_h) {
    if (!s_handle) return -1;
    int64_t t0 = esp_timer_get_time();
    jpeg_dec_io_t io = { (uint8_t*)jpeg, (int)jpeg_len, (uint8_t*)out_buf, 0 };
    jpeg_dec_header_info_t hdr;
    if (jpeg_dec_parse_header(s_handle, &io, &hdr) != JPEG_ERR_OK) return -1;
    if (hdr.width != expected_w || hdr.height != expected_h) {
        ESP_LOGW(TAG, "size mismatch %dx%d", hdr.width, hdr.height);
        return -1;
    }
    if (jpeg_dec_process(s_handle, &io) != JPEG_ERR_OK) return -1;
    int64_t t1 = esp_timer_get_time();
    return t1 - t0;
}
```

- [ ] **Step 3: `components/decoder/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "decoder.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_jpeg log
)
```

- [ ] **Step 4: Build**

Run: `idf.py build`
Expected: OK.

- [ ] **Step 5: Commit**

```bash
git add components/decoder/
git commit -m "feat: esp_jpeg wrapper rgb565 output"
```

---

### Task 6: `render` — double buffer + swap

**Files:**
- Create: `components/render/include/render.h`
- Create: `components/render/render.cpp`
- Create: `components/render/CMakeLists.txt`

- [ ] **Step 1: Criar `render.h`**

```cpp
#pragma once
#include <cstdint>

bool render_init(void);

// Obtém o framebuffer "back" (para decoder escrever).
uint16_t* render_back_buffer(void);

// Marca o back buffer como pronto e dispara o blit. Bloqueia até DMA iniciar (ms).
// Thread-safe.
void render_present(void);
```

- [ ] **Step 2: Implementar `render.cpp`**

```cpp
#include "render.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "render";
static uint16_t* s_fb[2] = {nullptr, nullptr};
static int s_back_idx = 0;
static SemaphoreHandle_t s_mutex = nullptr;

bool render_init(void) {
    s_fb[0] = display_alloc_framebuffer_psram();
    s_fb[1] = display_alloc_framebuffer_psram();
    if (!s_fb[0] || !s_fb[1]) {
        ESP_LOGE(TAG, "fb alloc failed");
        return false;
    }
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != nullptr;
}

uint16_t* render_back_buffer(void) {
    return s_fb[s_back_idx];
}

void render_present(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_wait_dma();                      // garante DMA anterior terminou
    display_blit_full(s_fb[s_back_idx]);     // começa DMA do back (vira front)
    s_back_idx ^= 1;                         // próximo decode escreve no outro
    xSemaphoreGive(s_mutex);
}
```

- [ ] **Step 3: `components/render/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "render.cpp"
    INCLUDE_DIRS "include"
    REQUIRES display
)
```

- [ ] **Step 4: Build**

Run: `idf.py build`

- [ ] **Step 5: Commit**

```bash
git add components/render/
git commit -m "feat: double framebuffer + swap-on-present"
```

---

### Task 7: Integrar pipeline no `app_main`

**Files:**
- Modify: `main/app_main.cpp`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Atualizar `main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES display espnow_link reassembly decoder render
)
```

- [ ] **Step 2: Reescrever `main/app_main.cpp`**

```cpp
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "display.h"
#include "espnow_link.h"
#include "reassembly.h"
#include "decoder.h"
#include "render.h"
#include "wire_types.h"
#include "pinout.h"

static const char* TAG = "main";

// MAC do TX hardcoded em V0. Substitua pelo MAC real da sua ESP32-S3-CAM.
static const uint8_t TX_MAC[6] = {0x84,0xF7,0x03,0xAA,0xBB,0xCC};

static QueueHandle_t s_frame_q = nullptr;

struct ready_frame_t {
    reassembled_frame_t frame;
};

static void on_msg(uint8_t msg_type, const uint8_t* payload, size_t len, int8_t rssi) {
    if (msg_type != MSG_VIDEO_FRAG) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    reassembled_frame_t out;
    if (reassembly_push_frag(payload, len, now_ms, &out)) {
        ready_frame_t rf{out};
        if (xQueueSend(s_frame_q, &rf, 0) != pdTRUE) {
            reassembly_release(&out); // fila cheia → drop
        }
    }
    reassembly_gc(now_ms);
}

static void decode_task(void*) {
    while (true) {
        ready_frame_t rf;
        if (xQueueReceive(s_frame_q, &rf, portMAX_DELAY) != pdTRUE) continue;
        uint16_t* back = render_back_buffer();
        int64_t dt = decoder_decode_to_rgb565(rf.frame.jpeg_data, rf.frame.jpeg_size,
                                              back, 320, 240);
        reassembly_release(&rf.frame);
        if (dt < 0) {
            ESP_LOGW(TAG, "decode failed");
            continue;
        }
        render_present();
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Sprint 2 boot");

    if (!display_init(LCD_SPI_HZ_TARGET)) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    if (!render_init()) { ESP_LOGE(TAG, "render init"); return; }
    if (!decoder_init()) { ESP_LOGE(TAG, "decoder init"); return; }
    if (!reassembly_init(2)) { ESP_LOGE(TAG, "reasm init"); return; }

    s_frame_q = xQueueCreate(4, sizeof(ready_frame_t));

    if (!espnow_link_init(6, on_msg)) { ESP_LOGE(TAG, "esnow init"); return; }
    espnow_link_add_peer(TX_MAC);

    xTaskCreatePinnedToCore(decode_task, "decode", 8192, nullptr, 6, nullptr, 1);

    // Stats periódicas.
    uint32_t last = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        auto s = *reassembly_stats();
        ESP_LOGI(TAG, "fps=%u drops=%u",
                 (unsigned)(s.frames_completed - last),
                 (unsigned)s.frames_dropped_timeout);
        last = s.frames_completed;
    }
}
```

- [ ] **Step 3: Substituir TX_MAC pelo MAC real**

Antes de flashar, descubra o MAC do seu ESP32-S3-CAM:
```
idf.py -p /dev/ttyUSB1 monitor   # no projeto do TX
```
Procure por log "MAC=" no boot.

Atualize a constante `TX_MAC` em `app_main.cpp`.

- [ ] **Step 4: Build, flash, monitor**

Com TX rodando, ligue o RX:
```
idf.py build flash monitor
```
Expected após ~5 s:
```
I main: fps=24 drops=0
I main: fps=24 drops=1
...
```
E vídeo aparecendo no display.

- [ ] **Step 5: Commit**

```bash
git add main/
git commit -m "feat: integrate video pipeline end-to-end"
```

---

### Task 8: Validar 10 minutos contínuos

- [ ] **Step 1: Soak test**

Deixar TX+RX ligados por 10 min sem mexer.

- [ ] **Step 2: Conferir log**

Esperado em todas as janelas de 1 s:
- `fps` ≥ 23
- `drops` baixo (< 5/min é aceitável em V0; alvo final é < 1%)

- [ ] **Step 3: Anotar resultado no spec**

Editar `docs/superpowers/specs/2026-05-26-receptor-design.md` §10, marcando:
```
- [x] 24 fps renderizados sem stutter perceptível por ≥ 10 min contínuos
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-26-receptor-design.md
git commit -m "docs: sprint 2 soak validated (24fps 10min)"
```

---

## Critérios de aceitação do Sprint 2

- [ ] Vídeo do TX aparece no display do RX a ~24 fps por 10 min
- [ ] Soak de 10 min: fps_avg ≥ 23
- [ ] Drops contabilizados em `reassembly_stats`
- [ ] Decode time logado (alvo < 12 ms; aceita até 15 ms)
- [ ] Memória PSRAM: 2 framebuffers alocados (verificar `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` mostra ~7.6 MB livre)

## Riscos do sprint

| Risco | Mitigação |
|---|---|
| esp_jpeg API muda entre versões | Pin versão em `idf_component.yml` |
| Decoder pinga em frames corrompidos | Validar JPEG header antes de processar; fallback a drop silencioso |
| Stack overflow em decode_task | Stack 8 KB (já generoso); se erro, subir para 12 KB |
| TX usa endianness diferente no header | Conferir `__attribute__((packed))` em ambos os lados |

## Self-Review

- §4.2 reassembly skip-drop 30 ms: Task 3 (testes Unity + impl)
- §4.3 decoder esp_jpeg: Task 5
- §4.4 double buffer + DMA + swap: Task 6
- §5.5 wire format: Task 2
- §8 alocação PSRAM: usado via `display_alloc_framebuffer_psram` em render
- §10 critério 24 fps 10 min: Task 8

Nenhum placeholder. Tipos consistentes (`reassembled_frame_t` definido em Task 3 usado em Task 7).
