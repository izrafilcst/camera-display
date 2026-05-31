# Sprint 4 Implementation Spec — BLE Pairing (CENTRAL)

**Data**: 2026-05-31
**Estado**: Pronto para Wave 2 (Coder)
**Cobre**: Tasks T1–T5 do plan `2026-05-31-sprint-4-ble-pairing.md` + spec §11 de `2026-05-26-receptor-design.md`
**Hardware alvo**: ESP32-S3-N16R8, MAC 3c:dc:75:62:1f:3c, USB-JTAG /dev/ttyACM0

---

## 1. Decisoes de Arquitetura

### 1.1 Stack BLE: NimBLE (confirmado)

ESP-IDF 6.x suporta Bluedroid e NimBLE. NimBLE e a escolha certa para este use-case porque:
- Footprint em flash ~120-150 KB vs ~300 KB do Bluedroid
- API de CENTRAL/scanning e mais limpa (sem wrappers Java-legado)
- `nimble_port_deinit()` libera efetivamente o heap; Bluedroid tem vazamentos conhecidos no deinit
- Suporte nativo a `ble_gap_disc` + `ble_gattc_*` sem callbacks de "profile" obrigatorios

Kconfig para ativar: `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_BT_CONTROLLER_ENABLED=y`. O
`sdkconfig.defaults` (ou `sdkconfig`) ja deve ter essas chaves; o coder deve confirmar.

### 1.2 NimBLE Host Task — Modelo de Threading

NimBLE no ESP-IDF usa uma task dedicada chamada **host task** (nome FreeRTOS: `"nimble_host"`),
afixada em **Core 0**, com prioridade `ESP_TASK_BT_CONTROLLER_PRIO - 1` (tipicamente 5).
Os callbacks de GAP (`ble_gap_event_fn`) e GATT (`ble_gattc_event_fn`) sao disparados
**desta mesma task**, portanto:

- Nunca bloquear na host task (sem `xEventGroupWaitBits`, sem mutexes pesados).
- Operacoes de longa duracao (ex: log extenso, NVS write) devem ser delegadas via fila ou feitas apos `ble_pair_run` retornar.
- `ble_pair_run` roda na **main task** (Core 0 no boot, Core 1 se criada como task dedicada). Ela bloqueia em `xEventGroupWaitBits(s_done_eg, BIT_DONE|BIT_ERROR, ..., portMAX_DELAY)`.

**Valores recomendados para host task** (defaults do IDF, nao alterar no V0):
- Stack: `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` — default 4096 bytes. Suficiente para scan + connect + discover + read/write.
- Core: 0 (fixo pelo port FreeRTOS do NimBLE).
- Prioridade: valor do `CONFIG_BT_NIMBLE_HOST_TASK_PRIO` — default 1 acima de idle. Adequado; a main task pode ter prioridade maior sem problema pois ela vai bloquear no EventGroup.

### 1.3 Sincronizacao: EventGroup + State Machine Pump

```
Main Task                          NimBLE Host Task
─────────────────────────          ─────────────────────────
ble_pair_sm_init(&s_sm)
ble_gap_disc(...)                  [scan running]
xEventGroupWaitBits(               on_disc_cb:
  s_done_eg,                         ble_pair_sm_feed(EV_ADV_MATCH)
  BIT_DONE|BIT_ERROR,                ble_gap_disc_cancel()
  portMAX_DELAY)                     ble_gap_connect(...)
         |                         on_connect_cb:
         |                           ble_pair_sm_feed(EV_CONNECTED)
         |                           ble_gattc_disc_svc_by_uuid(...)
         |                         on_disc_svc_cb:
         |                           ble_pair_sm_feed(EV_DISCOVERED)
         |                           ble_gattc_write_flat(...PIN...)
         |                         on_write_cb (0x1235):
         |                           ble_pair_sm_feed(EV_PIN_WRITE_OK)
         |                           ble_gattc_read(...)
         |                         on_read_cb (0x1237):
         |                           ble_pair_sm_feed(EV_TX_MAC_READ_OK, data, 6)
         |                           ble_gattc_write_flat(...RX_MAC...)
         |                         on_write_cb (0x1236):
         |                           ble_pair_sm_feed(EV_RX_MAC_WRITE_OK)
         |                           xEventGroupSetBits(s_done_eg, BIT_DONE)
         ▼
       returns true
```

Cada `ble_pair_sm_feed` atualiza o estado interno. Quando o estado terminal (DONE ou ERROR)
e atingido, o callback chama `xEventGroupSetBits`. Isso e atomico no FreeRTOS e seguro de
chamar da host task.

### 1.4 `esp_read_mac(ESP_MAC_WIFI_STA)` pre-Wi-Fi — Validado

No ESP32-S3, `esp_read_mac` le da eFuse (BLOCK1) independente do estado do driver Wi-Fi.
A funcao interna `esp_efuse_mac_get_default` e chamada diretamente, sem alocar recursos
de rede. Portanto, `ble_pair_get_rx_mac(out)` pode chamar `esp_read_mac(out, ESP_MAC_WIFI_STA)`
no boot, antes de `esp_wifi_init`, com seguranca. Referencia: IDF 6.x `esp_mac.c` linha
`ESP_MAC_WIFI_STA: case -> efuse_get_mac`.

### 1.5 Memoria: NimBLE Init/Deinit

Heap interno apos boot (estimativa para ESP32-S3-N16R8 com esta configuracao):
- Total heap interno disponivel no boot: ~300 KB
- NimBLE ativo (host task + controller): consome ~150 KB heap interno + ~32 KB DRAM estatica
- Apos `nimble_port_stop() + nimble_port_deinit()`: os ~150 KB dinamicos sao liberados
- Wi-Fi + ESP-NOW subsequente: consome ~80 KB heap interno

Portanto a janela pos-deinit pre-Wi-Fi deve ter ~220 KB livres — suficiente para `esp_wifi_init`.
O coder deve logar `esp_get_free_heap_size()` em tres pontos:
1. No boot (antes de qualquer init)
2. Apos `nimble_port_deinit()` (ou em caso de falha, antes do reboot)
3. Apos `espnow_link_init()`

Sequencia obrigatoria para evitar conflito de GDMA (herdado do Sprint 2):
```
1. nvs_flash_init()
2. [se nao pareado] nimble_port_init() → ble_pair_run() → nimble_port_deinit()
3. esp_wifi_init() + esp_wifi_start()   ← GDMA Wi-Fi primeiro
4. display_init()                        ← GDMA SPI2 segundo
5. render_init / decoder_init / reassembly_init / link_state_init
```

### 1.6 NVS Error Handling — Casos Completos

| Codigo `nvs_get_blob` | Significado | Tratamento |
|---|---|---|
| `ESP_OK` | Dado existe e valido | Checar `pair_nvs_is_valid_mac_for_persist`; se invalido, tratar como nao pareado |
| `ESP_ERR_NVS_NOT_FOUND` | Primeiro boot ou NVS limpo | Normal — retorna false, segue para BLE pair |
| `ESP_ERR_NVS_INVALID_LENGTH` | Blob salvo com tamanho errado | Limpa e retorna false (NVS corrompida parcialmente) |
| `ESP_ERR_NVS_NOT_INITIALIZED` | `pair_nvs_init()` nao foi chamado | Bug — `ESP_ERROR_CHECK`; nao deve ocorrer |
| `ESP_ERR_NVS_NO_FREE_PAGES` | NVS cheia | Log warning + retorna false; BLE pair na proxima oportunidade |
| Outros | Corrupcao grave | Log error + `nvs_erase_all` + `nvs_commit` + retorna false |

### 1.7 Kconfig Precedencia — Ordem de Boot

```
CONFIG_RECEIVER_SMOKE_TEST=y ?
  YES → demo loop, return (nenhum outro branch executa)
  NO  →
    pair_nvs_init()
    CONFIG_RECEIVER_FORCE_PAIR_AGAIN=y ?
      YES → pair_nvs_clear()   (forca re-pair)
    pair_nvs_load_tx_mac(tx_mac) → have_paired
    !have_paired && CONFIG_RECEIVER_PEER_MAC nao-placeholder ?
      YES → override: memcpy(tx_mac, override, 6); have_paired=true
            log WARN "using CONFIG_RECEIVER_PEER_MAC override"
    !have_paired ?
      YES → ble_pair_run(CONFIG_RECEIVER_PAIRING_PIN, tx_mac, &err)
            falhou ? → log + vTaskDelay(5s) + esp_restart()
            ok      ? → pair_nvs_save_tx_mac(tx_mac)
    [have_paired] → espnow_link_init, render, etc.
```

Implementar com `#if CONFIG_RECEIVER_SMOKE_TEST` no topo. As demais verificacoes sao
em runtime (o Kconfig `FORCE_PAIR_AGAIN` nao e `#if` — e verificado em runtime para
que o binary sem reflash possa ser testado com `sdkconfig` diferente, mas como e uma
flag de build, e equivalente).

### 1.8 Salvar Erro de Falha em RTC Fast Memory

Para evitar perda de razao de falha entre reboots, o erro sera salvo em RTC fast memory:

```c
// Em ble_pair.h (ou pair_nvs.h interno):
RTC_FAST_ATTR static ble_pair_err_t s_last_pair_err = BLE_PAIR_ERR_NONE;
```

No boot, `app_main` loga `s_last_pair_err` se diferente de `NONE` antes de limpa-lo.
Isso requer que a variavel seja declarada com `RTC_FAST_ATTR` (macro do IDF para `__attribute__((section(".rtc.data")))`)
e inicializada apenas na primeira vez (usando um campo sentinela ou o reset de deep sleep vs warm reboot).

Verificar se `esp_reset_reason() == ESP_RST_SW` para saber que o reboot foi intencional
(falha de pairing) vs power-on. Se for `ESP_RST_SW`, logar `s_last_pair_err`.

### 1.9 Gotchas do NimBLE no ESP-IDF 6.x

1. **`ble_gattc_disc_svc_by_uuid` retorna apenas o servico, nao as characteristics em um callback.** E necessario chamar `ble_gattc_disc_all_chrs` ou `ble_gattc_disc_chrs_by_uuid` separadamente por chr UUID (0x1235, 0x1236, 0x1237). O `ble_gattc_disc_all_chrs` e mais simples e recomendado para este caso (poucos chrs).

2. **`ble_gap_connect` com filtro de endereco.** O endereco obtido no callback de `ble_gap_disc` e do tipo `BLE_ADDR_TYPE_PUBLIC` ou `RANDOM`. Deve ser passado exatamente como recebido para `ble_gap_connect`, preservando o `type` da struct `ble_addr_t`.

3. **Scan com duplicates filter.** Por padrao `ble_gap_disc` filtra duplicados. Se o TX interromper o advertising entre scans, aumentar `itvl` e `window` ou usar `BLE_GAP_DISC_MODE_PASSIVE` para capturar mais rapidamente.

4. **`nimble_port_freertos_deinit` vs `nimble_port_deinit`.** A sequencia correta para liberar tudo e:
   ```c
   nimble_port_stop();           // sinaliza host task para parar
   nimble_port_freertos_deinit(); // deleta a host task FreeRTOS
   nimble_port_deinit();         // libera recursos NimBLE + controller
   ```
   Omitir `nimble_port_freertos_deinit` deixa a task FreeRTOS no scheduler consumindo stack.

5. **MTU e writes > 20 bytes.** O MAC (6B) e o PIN (4B) cabem no MTU default de 23B (ATT_MTU_DEFAULT). Sem necessidade de MTU exchange para este protocolo.

6. **`ble_gattc_write_flat` vs `ble_gattc_write_no_rsp_flat`.** Usar `write_flat` (com response) para 0x1235 e 0x1236 — o protocolo precisa do ACK para saber se o TX aceitou o PIN/MAC. `write_no_rsp` nao dispara callback de confirmacao.

7. **Concorrencia de `s_sm` com a host task.** O `ble_pair_sm_t s_sm` e compartilhado entre a main task (que inicializa) e a host task (que faz feeds). Como a main task bloqueia no EventGroup ate o termino, nao ha acesso concorrente real. Mas para clareza, o coder pode adicionar um comentario explicitando que `s_sm` e single-writer apos o `ble_gap_disc` inicial.

8. **Scan timeout via `ble_gap_disc` com duration.** Usar `duration_ms = CONFIG_BLE_SCAN_TIMEOUT_S * 1000`. O callback e chamado com `event->type == BLE_GAP_EVENT_DISC_COMPLETE` quando o tempo expira. Dentro do callback, checar se `s_sm.state == BLE_PAIR_SCANNING` antes de fazer feed de `EV_SCAN_TIMEOUT` — o `ADV_MATCH` pode ter chegado e disparado `ble_gap_disc_cancel()` antes do timeout natural.

---

## 2. Mapeamento T1–T5 para Codigo

### T1: `pair_nvs` component

**Arquivo**: `components/pair_nvs/include/pair_nvs.h` — conforme plano §T1 Step 1.

**Arquivo**: `components/pair_nvs/pair_nvs.cpp`

```cpp
// Dependencias ESP-IDF: nvs_flash.h, nvs.h
// Dependencia de componente: espnow_link (para peer_mac_is_placeholder)

bool pair_nvs_init(void) {
    // nvs_flash_init() pode ter sido chamado por app_main antes; e idempotente.
    // Abrir handle com NVS_READONLY para load, NVS_READWRITE para save/clear.
    // Retornar false se nvs_open falhar com erro fatal.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err == ESP_OK;
}

bool pair_nvs_load_tx_mac(uint8_t out[6]) {
    nvs_handle_t h;
    if (nvs_open(PAIR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 6;
    esp_err_t err = nvs_get_blob(h, PAIR_NVS_KEY_TX_MAC, out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != 6) return false;
    // Double-check: se o MAC salvo e invalido (placeholder), tratar como nao pareado
    return pair_nvs_is_valid_mac_for_persist(out);
}

bool pair_nvs_save_tx_mac(const uint8_t mac[6]) {
    if (!pair_nvs_is_valid_mac_for_persist(mac)) return false;
    nvs_handle_t h;
    if (nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, PAIR_NVS_KEY_TX_MAC, mac, 6);
    if (err == ESP_OK) {
        uint8_t sentinel = 1;
        err = nvs_set_u8(h, PAIR_NVS_KEY_PAIRED, sentinel);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void pair_nvs_clear(void) {
    nvs_handle_t h;
    if (nvs_open(PAIR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, PAIR_NVS_KEY_TX_MAC);
    nvs_erase_key(h, PAIR_NVS_KEY_PAIRED);
    nvs_commit(h);
    nvs_close(h);
}

bool pair_nvs_is_valid_mac_for_persist(const uint8_t mac[6]) {
    // Reusa logica de peer_mac_is_placeholder (espnow_link component).
    // No host build, a funcao e reimplementada inline nos testes.
    return !peer_mac_is_placeholder(mac);
}
```

**Arquivo**: `components/pair_nvs/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "pair_nvs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash espnow_link
)
```

**Nota para coder**: `pair_nvs_init()` NAO deve chamar `nvs_flash_init()` se `app_main` ja o faz. Tornar idempotente retornando `ESP_OK` se ja inicializado. Verificar com `nvs_flash_init()` retornando `ESP_ERR_NVS_ALREADY_INITIALIZED` — tratar como sucesso.

### T2: `ble_pair` state machine pura

**Arquivo**: `components/ble_pair/include/ble_pair.h` — conforme plano §T2 Step 1.

**Arquivo**: `components/ble_pair/include/internal/ble_pair_state.h` — conforme plano §T2 Step 2.

**Arquivo**: `components/ble_pair/ble_pair_state.cpp`

Implementar como switch/case plano (nao table-driven, para maxima legibilidade e auditabilidade):

```cpp
void ble_pair_sm_init(ble_pair_sm_t* sm) {
    sm->state = BLE_PAIR_IDLE;
    sm->err   = BLE_PAIR_ERR_NONE;
    memset(sm->tx_mac, 0, 6);
}

void ble_pair_sm_feed(ble_pair_sm_t* sm,
                      ble_pair_event_t ev,
                      const uint8_t* payload, size_t plen) {
    // Terminal states: ignore all events
    if (sm->state == BLE_PAIR_DONE || sm->state == BLE_PAIR_ERROR) return;

    switch (sm->state) {
    case BLE_PAIR_IDLE:
        if (ev == EV_START) sm->state = BLE_PAIR_SCANNING;
        break;
    case BLE_PAIR_SCANNING:
        if (ev == EV_ADV_MATCH)   sm->state = BLE_PAIR_CONNECTING;
        else if (ev == EV_SCAN_TIMEOUT) { sm->state = BLE_PAIR_ERROR; sm->err = BLE_PAIR_ERR_SCAN_TIMEOUT; }
        break;
    case BLE_PAIR_CONNECTING:
        if (ev == EV_CONNECTED)     sm->state = BLE_PAIR_DISCOVERING;
        else if (ev == EV_CONNECT_FAILED) { sm->state = BLE_PAIR_ERROR; sm->err = BLE_PAIR_ERR_CONNECT_FAILED; }
        break;
    case BLE_PAIR_DISCOVERING:
        if (ev == EV_DISCOVERED)    sm->state = BLE_PAIR_WRITING_PIN;
        else if (ev == EV_DISCOVER_FAILED) { sm->state = BLE_PAIR_ERROR; sm->err = BLE_PAIR_ERR_DISCOVER_FAILED; }
        break;
    case BLE_PAIR_WRITING_PIN:
        if (ev == EV_PIN_WRITE_OK)  sm->state = BLE_PAIR_READING_TX_MAC;
        else if (ev == EV_PIN_WRITE_REJECTED) { sm->state = BLE_PAIR_ERROR; sm->err = BLE_PAIR_ERR_PIN_REJECTED; }
        break;
    case BLE_PAIR_READING_TX_MAC:
        if (ev == EV_TX_MAC_READ_OK) {
            if (plen == 6) {
                memcpy(sm->tx_mac, payload, 6);
                sm->state = BLE_PAIR_WRITING_RX_MAC;
            } else {
                sm->state = BLE_PAIR_ERROR;
                sm->err   = BLE_PAIR_ERR_BAD_TX_MAC_LEN;
            }
        } else if (ev == EV_TX_MAC_READ_BAD_LEN) {
            sm->state = BLE_PAIR_ERROR;
            sm->err   = BLE_PAIR_ERR_BAD_TX_MAC_LEN;
        }
        break;
    case BLE_PAIR_WRITING_RX_MAC:
        if (ev == EV_RX_MAC_WRITE_OK)        sm->state = BLE_PAIR_DONE;
        else if (ev == EV_RX_MAC_WRITE_AUTH_FAIL) { sm->state = BLE_PAIR_ERROR; sm->err = BLE_PAIR_ERR_RX_MAC_AUTH; }
        break;
    default:
        break;
    }

    // Global: EV_DISCONNECTED before DONE is always an error
    // (handled after switch so DONE set above is not overridden)
    if (ev == EV_DISCONNECTED && sm->state != BLE_PAIR_DONE && sm->state != BLE_PAIR_ERROR) {
        sm->state = BLE_PAIR_ERROR;
        sm->err   = BLE_PAIR_ERR_DISCONNECTED_EARLY;
    }
}
```

**Atencao do coder**: O `EV_DISCONNECTED` global deve ser verificado APOS o switch, para nao interferir com a transicao `WRITING_RX_MAC → DONE` (o TX pode desconectar imediatamente apos o ACK, e o callback de disconnect pode chegar antes do callback de write-complete — ordem de eventos BLE e nao deterministica). O coder deve medir isso empiricamente e potencialmente adicionar um flag `s_expecting_disconnect` para ignorar o `EV_DISCONNECTED` apos `DONE`.

### T3: `ble_pair.cpp` — NimBLE wiring

**Arquivo**: `components/ble_pair/ble_pair.cpp`

Handlers a implementar (todos em `static` file scope):

| Callback NimBLE | Mapeamento |
|---|---|
| `ble_gap_event_fn` com `BLE_GAP_EVENT_DISC` | checar nome "CAM-TX" → `EV_ADV_MATCH` + stop scan + connect |
| `ble_gap_event_fn` com `BLE_GAP_EVENT_DISC_COMPLETE` | `EV_SCAN_TIMEOUT` se estado ainda SCANNING |
| `ble_gap_event_fn` com `BLE_GAP_EVENT_CONNECT` (status==0) | `EV_CONNECTED` + start discover |
| `ble_gap_event_fn` com `BLE_GAP_EVENT_CONNECT` (status!=0) | `EV_CONNECT_FAILED` + set BIT_ERROR |
| `ble_gap_event_fn` com `BLE_GAP_EVENT_DISCONNECT` | `EV_DISCONNECTED` |
| `ble_gattc_event_fn` com `BLE_GATTC_EVT_DISC_SVC_DONE` | checar UUID 0x1234 → discover chrs |
| `ble_gattc_event_fn` com `BLE_GATTC_EVT_DISC_CHR_DONE` | guardar handles de 0x1235/1236/1237 → `EV_DISCOVERED` ou `EV_DISCOVER_FAILED` |
| `ble_gattc_event_fn` com `BLE_GATTC_EVT_WRITE` (chr 0x1235) | `EV_PIN_WRITE_OK` ou `EV_PIN_WRITE_REJECTED` |
| `ble_gattc_event_fn` com `BLE_GATTC_EVT_READ` (chr 0x1237) | `EV_TX_MAC_READ_OK`/`EV_TX_MAC_READ_BAD_LEN` |
| `ble_gattc_event_fn` com `BLE_GATTC_EVT_WRITE` (chr 0x1236) | `EV_RX_MAC_WRITE_OK` ou `EV_RX_MAC_WRITE_AUTH_FAIL` |

**Handles de CHR** — armazenar em `static uint16_t s_chr_pin_hdl`, `s_chr_rx_mac_hdl`, `s_chr_tx_mac_hdl`. Checar `BLE_HS_ATT_ERR_ATTR_NOT_FOUND` se svc nao tem os 3 chrs → `EV_DISCOVER_FAILED`.

**Arquivo**: `components/ble_pair/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "ble_pair.cpp" "ble_pair_state.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "include/internal"
    REQUIRES bt espnow_link
    PRIV_REQUIRES nvs_flash esp_event esp_timer
)
```

**Nota**: `nimble` nao e um componente separado no IDF 6.x — e parte de `bt`. `REQUIRES bt` e suficiente.

**Arquivo**: `components/ble_pair/Kconfig`

```kconfig
menu "BLE Pairing"

config BLE_SCAN_TIMEOUT_S
    int "BLE scan timeout in seconds"
    default 30
    range 5 120
    help
        How long the RX scans for CAM-TX before giving up and rebooting.

config RECEIVER_FORCE_PAIR_AGAIN
    bool "Force re-pairing on next boot (wipes NVS pairing state)"
    default n
    help
        When enabled, the receiver wipes the saved TX MAC from NVS and
        re-runs the BLE pairing handshake even if a valid pair record exists.
        Disable after successfully testing re-pair to avoid re-pairing on
        every boot.

endmenu
```

### T4: `app_main` boot branches

**Arquivo**: `main/Kconfig.projbuild` — adicionar:

```kconfig
config RECEIVER_PAIRING_PIN
    int "BLE pairing PIN sent to TX characteristic 0x1235"
    default 1234
    range 0 999999
    help
        uint32 LE encoding: 1234 = 0xD2 0x04 0x00 0x00.
        Must match what the TX expects. Change only if TX firmware changes.
```

**Arquivo**: `main/CMakeLists.txt` — adicionar `ble_pair pair_nvs` ao REQUIRES existente.

**Arquivo**: `main/app_main.cpp` — seguir pseudocodigo do plano §T4 Step 2 exatamente.
Logar heap em 3 pontos (ver §1.5). Logar `s_last_pair_err` se `esp_reset_reason() == ESP_RST_SW`.

### T5: Hardware validation

Fora do escopo do coder — requer TX real. Ver plano §T5.

---

## 3. Riscos Residuais e Mitigacoes

| Risco | Severidade | Mitigacao |
|---|---|---|
| TX desconecta antes do callback de write 0x1236 | MEDIO | Flag `s_expecting_disconnect` pos-DONE; ver §T2 nota |
| `ble_gattc_disc_all_chrs` nao encontra chr 0x1237 (tipo READ) em alguns stacks TX | MEDIO | Verificar atributo de propriedade; logar handle e props no discover |
| NimBLE host task stack overflow (4096 B padrao) | BAIXO | Monitorar `uxTaskGetStackHighWaterMark`; aumentar se < 512 B restantes |
| Reboot loop se TX nao estiver presente | BAIXO/ACEITAVEL V0 | Documentado; usuario pode setar `CONFIG_RECEIVER_PEER_MAC` nao-placeholder para sair do loop |
| Race `EV_DISCONNECTED` x `EV_RX_MAC_WRITE_OK` | MEDIO | Ver §T2 nota; tratar com flag ou com tolerancia na state machine |

---

## 4. Criterios de Aceitacao (Wave 2 gate)

- [ ] `make test` em `components/pair_nvs/host_tests` passa (>= 6 casos)
- [ ] `make test` em `components/ble_pair/host_tests` passa (>= 20 casos)
- [ ] `pio run` compila sem warnings novos (flash delta esperado: +120–180 KB com NimBLE)
- [ ] Todos os 53 testes pre-existentes continuam passando (display 22 + reassembly 19 + espnow_link 5 + link_state 7)
- [ ] Boot logic respeita precedencia: SMOKE > FORCE_PAIR > PEER_MAC override > NVS paired > BLE pair
- [ ] `nimble_port_deinit` chamado antes de `esp_wifi_init` (verificar por log)
- [ ] `esp_get_free_heap_size()` logado em 3 pontos

---

## 5. Notas para o Reviewer (Wave 3)

- Verificar que `EV_DISCONNECTED` apos `DONE` nao transiciona para `ERROR` (DONE e terminal)
- Verificar que `ble_pair_run` nao e chamavel duas vezes sem reinicializar NimBLE (nao e idempotente no V0 — documentar)
- Race entre host task e main task: o EventGroup e o unico ponto de sincronizacao; verificar que nenhum dado compartilhado e acessado simultaneamente alem de `s_sm` (single-writer pos-init)
- NVS: `pair_nvs_init` idempotente — chamar duas vezes nao corrompte dados
- Kconfig precedencia deve ser testada com `CONFIG_RECEIVER_FORCE_PAIR_AGAIN=y` + NVS pareado: deve re-parear

---

## 6. Notas para o Security Auditor (Wave 3)

- Attack surface: RX e CENTRAL (inicia conexao) — nao e peripheral. Nao aceita conexoes inbound BLE. Superficies de ataque BLE reduzidas.
- Spoof CAM-TX: qualquer dispositivo que anuncie com nome "CAM-TX" e aceite PIN 1234 vai passar. Documentar como threat S4-01; backlog Sprint 8.
- PIN 1234 em Kconfig (plaintext no binario): aceitavel V0. Sprint 8 migra para Secure Boot + encrypted config.
- NVS `tx_mac` sem encrypted NVS: legivel por outro app se sem Secure Boot. Documentar como S4-02.
- `tx_mac` em RTC fast memory (last error): nao expoe MAC — apenas `ble_pair_err_t` (inteiro). OK.
- Memory disclosure via core dump: `s_sm.tx_mac` visivel. Documentar como S4-03; mitigacao em Sprint 8.
