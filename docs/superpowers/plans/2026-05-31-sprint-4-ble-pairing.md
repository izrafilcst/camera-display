# Sprint 4 — BLE Pairing (CENTRAL) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implementar a Fase 1 do protocolo do TX (handshake BLE) descrita pelo usuário. RX escaneia `CAM-TX`, conecta, escreve PIN em 0x1235, lê MAC do TX em 0x1237, escreve próprio MAC em 0x1236. Persiste TX MAC em NVS para pular o pareamento nos boots seguintes.

**Architecture:**
- Novo componente `ble_pair` (NimBLE host stack) com state machine isolada.
- Novo componente `pair_nvs` (wrapper sobre `nvs_flash`) com tabelinha `pairing/{tx_mac, paired}`.
- `app_main` ganha branch de boot: smoke test → BLE pair (se não pareado) → ESP-NOW + render.
- Após pareamento, NimBLE é deinit'd antes de Wi-Fi/ESP-NOW iniciar — libera ~150 KB de heap.

**Tech Stack:** ESP-IDF NimBLE (`esp_nimble_hci`, `nimble`), NVS, FreeRTOS event group para sincronização do state machine com callbacks NimBLE.

**Reference do spec:** `docs/superpowers/specs/2026-05-26-receptor-design.md` §11 (a ser criada nesta sprint).

---

## User-approved decisions (impl-spec gates these)

| Item | Decisão |
|---|---|
| Stack BLE | NimBLE |
| Lifecycle | init no boot → handshake → deinit antes de Wi-Fi |
| NVS | namespace `pairing`, keys `tx_mac` (blob 6B) + `paired` (u8) |
| Re-pair gate | Kconfig `CONFIG_RECEIVER_FORCE_PAIR_AGAIN` — botão físico é Sprint 7 |
| Falha do handshake | log erro + `vTaskDelay(5s)` + `esp_restart()` |
| Scan timeout | 30 s, Kconfig `CONFIG_BLE_SCAN_TIMEOUT_S` (default 30) |
| Encrypted NVS | Não no V0 (vira Sprint 8 com flash encryption) |
| BLE bonding / LE Secure Conn | Não no V0 (TX não suporta ainda) |
| Smoke test | Mantém intacto; checado ANTES do branch BLE |
| `CONFIG_RECEIVER_PEER_MAC` | Sobrescreve NVS quando setado a um valor não-placeholder — útil pra testar ESP-NOW sem TX |

---

## File Structure

```
components/
├── ble_pair/
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── include/
│   │   └── ble_pair.h
│   ├── ble_pair.cpp                # NimBLE wiring + state machine
│   ├── ble_pair_state.cpp          # pure state-machine (host-testable)
│   ├── include/internal/
│   │   └── ble_pair_state.h        # internal state-machine API
│   └── host_tests/
│       ├── Makefile
│       └── test_ble_pair_state.cpp # state transitions, error paths
├── pair_nvs/
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── pair_nvs.h
│   ├── pair_nvs.cpp
│   └── host_tests/
│       ├── Makefile
│       └── test_pair_nvs_logic.cpp # MAC-blob serialization, placeholder rejection
main/
├── app_main.cpp                    # branched boot logic
├── CMakeLists.txt                  # add ble_pair, pair_nvs to REQUIRES
└── Kconfig.projbuild               # add PAIRING_PIN, FORCE_PAIR_AGAIN
```

---

## Task 1 — `pair_nvs` component (NVS persistence helper)

**Files:**
- Create: `components/pair_nvs/include/pair_nvs.h`
- Create: `components/pair_nvs/pair_nvs.cpp`
- Create: `components/pair_nvs/CMakeLists.txt`
- Create: `components/pair_nvs/host_tests/{Makefile, test_pair_nvs_logic.cpp}`

- [ ] **Step 1: Header `pair_nvs.h`**

```c
#pragma once
#include <cstdint>
#include <cstdbool>

#ifdef __cplusplus
extern "C" {
#endif

// Namespace constant — exposed for tests; production callers use the API
#define PAIR_NVS_NAMESPACE "pairing"
#define PAIR_NVS_KEY_TX_MAC "tx_mac"
#define PAIR_NVS_KEY_PAIRED "paired"

bool pair_nvs_init(void);
bool pair_nvs_load_tx_mac(uint8_t out[6]);    // returns false if not paired
bool pair_nvs_save_tx_mac(const uint8_t mac[6]);
void pair_nvs_clear(void);                     // wipes `paired` and `tx_mac`

// Logic-only entry point — testable on host without ESP-IDF NVS.
// Validates that mac is non-placeholder before allowing persist.
bool pair_nvs_is_valid_mac_for_persist(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implementation** — wraps `nvs_open`, `nvs_get_blob`, `nvs_set_blob`, `nvs_commit`. Validation in `pair_nvs_is_valid_mac_for_persist` rejects `peer_mac_is_placeholder(mac)` (reuses the espnow_link helper). Init must be idempotent.

- [ ] **Step 3: Host tests** for the validation logic (placeholder rejection, blob round-trip via in-memory fake). Mirrors `components/espnow_link/host_tests/test_peer_mac.cpp` style.

- [ ] **Step 4: Build target** `idf.py build` (via `pio run` since user is on PIO).

- [ ] **Step 5: Commit** `feat(pair_nvs): TX MAC persistence helper with placeholder gate`

---

## Task 2 — `ble_pair` state machine (pure logic, host-testable)

The state machine sits behind the NimBLE callbacks. Keeping it pure (no BLE calls) lets us cover all transitions on the host before bringing up the BLE stack.

**Files:**
- Create: `components/ble_pair/include/ble_pair.h`
- Create: `components/ble_pair/include/internal/ble_pair_state.h`
- Create: `components/ble_pair/ble_pair_state.cpp`
- Create: `components/ble_pair/host_tests/{Makefile, test_ble_pair_state.cpp}`

- [ ] **Step 1: Public header `ble_pair.h`**

```c
#pragma once
#include <cstdint>
#include <cstdbool>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_PAIR_IDLE            = 0,
    BLE_PAIR_SCANNING        = 1,
    BLE_PAIR_CONNECTING      = 2,
    BLE_PAIR_DISCOVERING     = 3,
    BLE_PAIR_WRITING_PIN     = 4,
    BLE_PAIR_READING_TX_MAC  = 5,
    BLE_PAIR_WRITING_RX_MAC  = 6,
    BLE_PAIR_DONE            = 7,
    BLE_PAIR_ERROR           = 8,
} ble_pair_state_t;

typedef enum {
    BLE_PAIR_ERR_NONE                   = 0,
    BLE_PAIR_ERR_SCAN_TIMEOUT           = 1,
    BLE_PAIR_ERR_CONNECT_FAILED         = 2,
    BLE_PAIR_ERR_DISCOVER_FAILED        = 3,
    BLE_PAIR_ERR_PIN_REJECTED           = 4,    // ATT_ERR_WRITE_NOT_PERMITTED
    BLE_PAIR_ERR_RX_MAC_AUTH            = 5,    // ATT_ERR_INSUFFICIENT_AUTHEN
    BLE_PAIR_ERR_BAD_TX_MAC_LEN         = 6,
    BLE_PAIR_ERR_STACK_INIT             = 7,
    BLE_PAIR_ERR_DISCONNECTED_EARLY     = 8,
} ble_pair_err_t;

bool ble_pair_run(uint32_t pin, uint8_t tx_mac_out[6], ble_pair_err_t* err_out);
void ble_pair_get_rx_mac(uint8_t out[6]);  // wraps esp_read_mac(ESP_MAC_WIFI_STA)
const char* ble_pair_state_str(ble_pair_state_t s);
const char* ble_pair_err_str(ble_pair_err_t e);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Internal state-machine header `internal/ble_pair_state.h`**

```c
#pragma once
#include "ble_pair.h"

typedef enum {
    EV_START,                       // ble_pair_run called
    EV_ADV_MATCH,                   // adv with name "CAM-TX" seen
    EV_SCAN_TIMEOUT,                // 30s elapsed
    EV_CONNECTED,
    EV_CONNECT_FAILED,
    EV_DISCOVERED,                  // svc 0x1234 + 3 chrs found
    EV_DISCOVER_FAILED,
    EV_PIN_WRITE_OK,
    EV_PIN_WRITE_REJECTED,
    EV_TX_MAC_READ_OK,              // payload includes the 6 bytes
    EV_TX_MAC_READ_BAD_LEN,
    EV_RX_MAC_WRITE_OK,
    EV_RX_MAC_WRITE_AUTH_FAIL,
    EV_DISCONNECTED,
} ble_pair_event_t;

typedef struct {
    ble_pair_state_t state;
    uint8_t          tx_mac[6];
    ble_pair_err_t   err;
} ble_pair_sm_t;

void ble_pair_sm_init(ble_pair_sm_t* sm);
void ble_pair_sm_feed(ble_pair_sm_t* sm,
                      ble_pair_event_t ev,
                      const uint8_t* payload, size_t plen);
```

- [ ] **Step 3: State machine implementation** in `ble_pair_state.cpp`. Pure logic; no BLE calls. Each `_sm_feed` updates `state` and may populate `tx_mac` or `err`.

Transitions (informally):
- `IDLE × EV_START → SCANNING`
- `SCANNING × EV_ADV_MATCH → CONNECTING`
- `SCANNING × EV_SCAN_TIMEOUT → ERROR(SCAN_TIMEOUT)`
- `CONNECTING × EV_CONNECTED → DISCOVERING`
- `CONNECTING × EV_CONNECT_FAILED → ERROR(CONNECT_FAILED)`
- `DISCOVERING × EV_DISCOVERED → WRITING_PIN`
- `DISCOVERING × EV_DISCOVER_FAILED → ERROR(DISCOVER_FAILED)`
- `WRITING_PIN × EV_PIN_WRITE_OK → READING_TX_MAC`
- `WRITING_PIN × EV_PIN_WRITE_REJECTED → ERROR(PIN_REJECTED)`
- `READING_TX_MAC × EV_TX_MAC_READ_OK (plen==6) → WRITING_RX_MAC` (store tx_mac)
- `READING_TX_MAC × EV_TX_MAC_READ_BAD_LEN → ERROR(BAD_TX_MAC_LEN)`
- `WRITING_RX_MAC × EV_RX_MAC_WRITE_OK → DONE`
- `WRITING_RX_MAC × EV_RX_MAC_WRITE_AUTH_FAIL → ERROR(RX_MAC_AUTH)`
- `* × EV_DISCONNECTED (state != DONE) → ERROR(DISCONNECTED_EARLY)`
- `DONE × * → DONE` (terminal, ignore further events)

- [ ] **Step 4: Host tests (RED first)** — `test_ble_pair_state.cpp` covers every transition above, including the disallowed events (e.g. PIN_OK from SCANNING is dropped).

- [ ] **Step 5: Build host tests** — `make test`. Should fail until the impl exists, then go GREEN.

- [ ] **Step 6: Commit** `feat(ble_pair): pure state machine for pairing handshake`

---

## Task 3 — NimBLE wiring

**Files:**
- Create: `components/ble_pair/ble_pair.cpp`
- Create: `components/ble_pair/CMakeLists.txt`
- Create: `components/ble_pair/Kconfig`

- [ ] **Step 1: Kconfig** — adds `CONFIG_BLE_SCAN_TIMEOUT_S` (default 30), `CONFIG_RECEIVER_FORCE_PAIR_AGAIN` (default n).

- [ ] **Step 2: `CMakeLists.txt`** — `REQUIRES bt nimble nvs_flash esp_event esp_timer espnow_link`. PSRAM is not needed here.

- [ ] **Step 3: Implementation `ble_pair.cpp`** — wires NimBLE callbacks to `ble_pair_sm_feed`:

```c
// Pseudo-structure
static EventGroupHandle_t s_done_eg;   // bits: DONE, ERROR
static ble_pair_sm_t s_sm;

static void on_disc(struct ble_gap_event* ev, ...) {
    if (ev == adv && name_matches("CAM-TX")) ble_pair_sm_feed(&s_sm, EV_ADV_MATCH, ...);
    if (ev == scan_complete) ble_pair_sm_feed(&s_sm, EV_SCAN_TIMEOUT, ...);
}
static void on_connect(...) { ble_pair_sm_feed(&s_sm, EV_CONNECTED or EV_CONNECT_FAILED, ...); }
// ... discover, write, read callbacks follow the same pattern

bool ble_pair_run(uint32_t pin, uint8_t tx_mac_out[6], ble_pair_err_t* err_out) {
    nimble_port_init();
    nimble_port_freertos_init(host_task);
    ble_pair_sm_init(&s_sm);
    s_done_eg = xEventGroupCreate();

    ble_pair_sm_feed(&s_sm, EV_START, nullptr, 0);
    ble_gap_disc(...);   // start scan; timeout = CONFIG_BLE_SCAN_TIMEOUT_S * 1000

    // After EV_ADV_MATCH the scan callback stops scan and starts connect.
    // After EV_CONNECTED the connect cb does discover, etc.
    // Each state transition pumps the next BLE op.

    EventBits_t bits = xEventGroupWaitBits(s_done_eg, BIT_DONE | BIT_ERROR,
                                            pdTRUE, pdFALSE, portMAX_DELAY);
    nimble_port_stop();
    nimble_port_deinit();
    // (nimble_port_freertos_deinit also called; releases ~150 KB)

    if (bits & BIT_DONE) {
        memcpy(tx_mac_out, s_sm.tx_mac, 6);
        *err_out = BLE_PAIR_ERR_NONE;
        return true;
    }
    *err_out = s_sm.err;
    return false;
}
```

- [ ] **Step 4: `ble_pair_get_rx_mac`** wraps `esp_read_mac(out, ESP_MAC_WIFI_STA)`. Needs Wi-Fi to have been initialized once (we call `esp_wifi_init` lazily in `espnow_link_init` already — but for BLE-only boot we need an early `esp_read_mac` that doesn't require Wi-Fi running). On ESP32-S3, `esp_read_mac` reads from eFuse so it works pre-Wi-Fi. Verify with a log on first boot.

- [ ] **Step 5: Build** — `pio run`. Expect BLE stack ~120-180 KB flash, watch the size delta.

- [ ] **Step 6: Commit** `feat(ble_pair): nimble scan/connect/discover/write/read dispatch`

---

## Task 4 — `app_main` boot branches

**Files:**
- Modify: `main/app_main.cpp`
- Modify: `main/Kconfig.projbuild` (add `CONFIG_RECEIVER_PAIRING_PIN` default 1234)
- Modify: `main/CMakeLists.txt` — add `ble_pair pair_nvs` to REQUIRES

- [ ] **Step 1: Add Kconfig**

```kconfig
config RECEIVER_PAIRING_PIN
    int "BLE pairing PIN (uint32 LE)"
    default 1234
    help
        PIN that the RX writes to chr 0x1235 during the BLE handshake.
        Must match what the TX expects (1234 is the documented test PIN).
```

- [ ] **Step 2: Rewrite app_main flow**

```c
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Sprint 4 boot — free heap=%u", ...);

#if CONFIG_RECEIVER_SMOKE_TEST
    // existing smoke loop, unchanged
    return;
#else
    pair_nvs_init();

#if CONFIG_RECEIVER_FORCE_PAIR_AGAIN
    ESP_LOGW(TAG, "FORCE_PAIR_AGAIN — wiping NVS pairing state");
    pair_nvs_clear();
#endif

    uint8_t tx_mac[6];
    bool have_paired = pair_nvs_load_tx_mac(tx_mac);

    // Optional override via Kconfig (testing without TX)
    uint8_t override_mac[6];
    if (!have_paired
        && parse_peer_mac(CONFIG_RECEIVER_PEER_MAC, override_mac)
        && !peer_mac_is_placeholder(override_mac)) {
        ESP_LOGW(TAG, "using CONFIG_RECEIVER_PEER_MAC override");
        memcpy(tx_mac, override_mac, 6);
        have_paired = true;
    }

    if (!have_paired) {
        ESP_LOGI(TAG, "no paired TX in NVS — starting BLE pairing");
        ble_pair_err_t err;
        if (!ble_pair_run(CONFIG_RECEIVER_PAIRING_PIN, tx_mac, &err)) {
            ESP_LOGE(TAG, "pairing failed: %s — reboot in 5s",
                     ble_pair_err_str(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_restart();
        }
        pair_nvs_save_tx_mac(tx_mac);
        ESP_LOGI(TAG, "paired: %02X:%02X:%02X:%02X:%02X:%02X — saved to NVS",
                 tx_mac[0], tx_mac[1], tx_mac[2], tx_mac[3], tx_mac[4], tx_mac[5]);
    } else {
        ESP_LOGI(TAG, "using saved peer: %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_mac[0], tx_mac[1], tx_mac[2], tx_mac[3], tx_mac[4], tx_mac[5]);
    }

    link_state_init();
    if (!espnow_link_init(6, on_msg)) { ... }
    espnow_link_add_peer(tx_mac);
    // ... rest unchanged
#endif
}
```

- [ ] **Step 3: Build, flash, monitor** — without TX present the device should log "no paired TX" then "starting BLE pairing" then eventually `SCAN_TIMEOUT` and reboot. That's a positive negative test (the failure mode looks right).

- [ ] **Step 4: Commit** `feat(main): branch boot on paired-state, run BLE pair when unpaired`

---

## Task 5 — Hardware validation with real TX

- [ ] **Step 1: Power TX, power RX. RX boots, advertises log "no paired TX". TX is advertising as `CAM-TX`.**
- [ ] **Step 2: Observe log sequence**:
  - `BLE_PAIR_SCANNING → ADV_MATCH`
  - `BLE_PAIR_CONNECTING → CONNECTED`
  - `BLE_PAIR_DISCOVERING → DISCOVERED`
  - `BLE_PAIR_WRITING_PIN → PIN_WRITE_OK`
  - `BLE_PAIR_READING_TX_MAC → TX_MAC_READ_OK`
  - `BLE_PAIR_WRITING_RX_MAC → RX_MAC_WRITE_OK`
  - `paired: XX:XX:XX:XX:XX:XX — saved to NVS`
  - `esp-now ready, channel=6`
  - frames começam a chegar: `fps=N drops_timeout=...`
- [ ] **Step 3: Power-cycle RX**. Boot deve pular BLE direto pra ESP-NOW (log "using saved peer").
- [ ] **Step 4: Build com `CONFIG_RECEIVER_FORCE_PAIR_AGAIN=y`** e flashar. RX deve apagar NVS no boot e re-parear.

- [ ] **Step 5: Critério de sucesso**: handshake completo em < 5s desde POR, primeiro frame JPEG decodificado em < 1s após pareamento.

---

## Critérios de aceitação Sprint 4

- [ ] `pair_nvs` host tests passam (placeholder rejection + blob round-trip)
- [ ] `ble_pair_state` host tests passam (todas as transições e error paths)
- [ ] `idf.py/pio run` compila com `bt`+`nimble` adicionados ao build sem warnings novos
- [ ] Boot sem NVS pareado → entra em `BLE_PAIR_SCANNING`
- [ ] Boot com NVS pareado → pula direto pra ESP-NOW
- [ ] Wrong PIN → estado `ERROR(PIN_REJECTED)` + reboot em 5s
- [ ] (HW) Pareamento real com TX completa em < 5s
- [ ] (HW) RX recebe ≥ 1 frame JPEG após pareamento via ESP-NOW

---

## Riscos

| Risco | Mitigação |
|---|---|
| Memória NimBLE + Wi-Fi/ESP-NOW conflitam em RAM interna | Garantir `nimble_port_deinit` antes de `esp_wifi_init`; logar heap antes/depois |
| `esp_read_mac(ESP_MAC_WIFI_STA)` pode requerer Wi-Fi inicializado em algumas builds | No S3 lê direto da eFuse pré-Wi-Fi; validar empiricamente no Task 4 |
| NimBLE callbacks são chamados na host task — não chamar APIs blocking lá | Toda transição = `ble_pair_sm_feed` + `xEventGroupSetBits` no terminal; nada pesado |
| TX desconecta logo após `WRITING_RX_MAC` (por design) | State machine trata `EV_DISCONNECTED` após DONE como esperado; antes disso → ERROR |
| Re-pareamento involuntário (NVS corrupted) | `pair_nvs_load_tx_mac` valida via `pair_nvs_is_valid_mac_for_persist`; placeholder/zero force re-pair |

---

## Out of scope (não fazer nesta sprint)

- Telemetria reversa para TX → Sprint 5
- Adaptive ladder L0–L4 → Sprint 5
- HUD overlays e menu → Sprints 6-7
- Encrypted NVS / Secure Boot / Flash Encryption → Sprint 8
- BLE bonding / LE Secure Connections → Sprint 8 (depende do TX adicionar suporte)
- Botão físico de "force re-pair" → Sprint 7 (com joystick/menu)

---

## Swarm execution (idem Sprints 2, 3)

- **Wave 1 (paralelo)**: Architect produz `docs/superpowers/specs/sprint-4-impl-spec.md`; Tester escreve testes RED em ambos `pair_nvs/host_tests` e `ble_pair/host_tests`.
- **Wave 2 (sequencial)**: Coder (com `/c-pro`, `/cpp-pro`, `/tdd-workflow`) implementa `pair_nvs` → `ble_pair_state` → `ble_pair.cpp` (NimBLE wiring) → `app_main` branches.
- **Wave 3 (paralelo)**: Reviewer escreve `docs/reviews/sprint-4-review.md`; Security auditor escreve `docs/audits/sprint-4-security.md`. Foco do auditor: NVS exposure, BLE state machine (pode atacante completar o handshake?), memória pós-deinit, PIN bruteforce (TX deveria gatear isso, mas RX deve logar tentativas falhas).
