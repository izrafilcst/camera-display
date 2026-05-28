# Sprint 2 Implementation Spec — Video Pipeline

**Data**: 2026-05-28
**Estado**: Aprovado para implementacao
**Cobre**: Tasks T1–T7 do plan `2026-05-26-sprint-2-video-pipeline.md` + REQ-1..5 do gate `sprint-2-parser-hardening-requirements.md`
**Hardware alvo**: ESP32-S3-N16R8 + ILI9341 SPI 320x240 "modulo vermelho"

---

## 1. Pinagem — Sem Conflito Com Sprint 1

A pinagem SPI do display (GPIO 7–12) e a pinagem de entrada (GPIO 1–4, 14 pos-correcao do Sprint 1) permanecem inalteradas. O Sprint 2 nao adiciona pinos novos ao hardware receptor — todo o pipeline e software.

O Wi-Fi (para ESP-NOW) reutiliza a antena interna do ESP32-S3; sem GPIO dedicado.

**Ordem de inicializacao recomendada** (mitiga G3 do Sprint 1 — esgotamento de canais GDMA):

```
1. nvs_flash_init()
2. esp_wifi_init() + esp_wifi_start()   ← aloca canais GDMA Wi-Fi primeiro
3. display_init(LCD_SPI_HZ_TARGET)      ← SPI2 + DMA pega o canal restante
4. render_init() / decoder_init() / reassembly_init()
```

Se a ordem for invertida e `SPI_DMA_CH_AUTO` nao encontrar canal livre, `display_init` retornara false e o log mostrara `E lgfx: dma channel alloc failed`. Nesse caso, reverter para a ordem acima.

---

## 2. Estrutura de Componentes

```
components/
├── espnow_link/
│   ├── CMakeLists.txt
│   ├── Kconfig                        ← novo (REQ-5)
│   ├── include/
│   │   ├── espnow_link.h
│   │   └── wire_types.h               ← fonte unica de verdade para todos os tipos
│   ├── espnow_link.cpp
│   └── host_tests/
│       ├── CMakeLists.txt
│       ├── Makefile
│       └── test_peer_mac.cpp          ← novo (REQ-5)
├── reassembly/
│   ├── CMakeLists.txt
│   ├── include/reassembly.h
│   ├── reassembly.cpp
│   └── host_tests/
│       ├── CMakeLists.txt
│       ├── Makefile
│       └── test_reassembly.cpp
├── decoder/
│   ├── CMakeLists.txt
│   ├── include/decoder.h
│   └── decoder.cpp
└── render/
    ├── CMakeLists.txt
    ├── include/render.h
    └── render.cpp
```

---

## 3. `wire_types.h` — Fonte Unica e Assertivas Compile-Time

Localizado em `components/espnow_link/include/wire_types.h`. Incluido por todos os componentes que precisam de tipos de protocolo.

### 3.1 REQ-1: static_assert para MAX_FRAGS_PER_FRAME

```cpp
static constexpr size_t MAX_FRAGS_PER_FRAME   = 64;
static_assert(MAX_FRAGS_PER_FRAME > 0 && MAX_FRAGS_PER_FRAME <= 64,
              "MAX_FRAGS_PER_FRAME must fit in frags_bitmap (uint64_t)");
```

Justificativa: `slot_t::frags_bitmap` e `uint64_t`; 64 bits e o limite absoluto. Qualquer tentativa de aumentar esse valor para >64 falha a compilacao com mensagem clara, antes de qualquer dano em runtime.

**Arquivo**: `components/espnow_link/include/wire_types.h`
**Linha**: imediatamente apos a definicao da constante.

### 3.2 REQ-3: MAX_JPEG_SIZE — Fonte Unica

```cpp
static constexpr size_t MAX_JPEG_SIZE         = 16 * 1024;  // 16 KiB
```

Este e o unico lugar onde `MAX_JPEG_SIZE` e definido. `reassembly.cpp` e `decoder.cpp` incluem `wire_types.h` e usam essa constante diretamente — nenhuma redefinicao local.

Log de aviso para banda [12 KiB, 16 KiB) em `reassembly_push_frag`, implementado ANTES do memcpy:

```cpp
if (h.jpeg_size > MAX_JPEG_SIZE) {
    s_stats.fragments_invalid++;
    return false;  // rejeitado antes de qualquer uso
}
if (h.jpeg_size >= 12 * 1024) {
    ESP_LOGW(TAG, "jpeg_size=%u em banda alta [12K,16K] - frame %u",
             h.jpeg_size, h.frame_id);
}
```

---

## 4. Reassembly — Decisoes de Buffer e Tratamento de Erros

### 4.1 Pool de Slots

- **Numero de slots**: 2 (spec §4.2 "2 slots de 16 KB").
- **Alocacao**: `heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM)` no host-build (`malloc` via macro); PSRAM no target.
- **Bitmap**: `uint64_t frags_bitmap` — bit N setado quando fragmento N foi recebido.

### 4.2 Calculo de Offset por Fragmento

O payload de cada fragmento e copiado para `slot->data + offset`, onde `offset` e calculado como:

```
offset(frag_idx) = soma dos payload_len dos fragmentos anteriores
                    (de 0 a frag_idx-1) ja recebidos
```

Problema: quando um fragmento chega fora de ordem (ex: frag 2 antes do frag 1), o offset dos fragmentos anteriores ainda e desconhecido. Solucao adotada: **armazenar o payload_len de cada fragmento recebido no slot** (array `uint16_t frag_payload_lens[MAX_FRAGS_PER_FRAME]`). Para frags nao recebidos ainda, usar 0.

Para calcular offset ao receber frag_idx N:

```cpp
size_t offset = 0;
for (int i = 0; i < h.frag_idx; ++i) {
    offset += s->frag_payload_lens[i];
    // frag nao recebido ainda: assume len maximo do MTU? Nao — armazenamos ao receber.
}
```

**Gotcha**: o frag_idx 0 inclui o `video_frag0_extra_t` (4 bytes de tx_emission_ms) antes do payload; esse overhead nao conta no `payload_len`. O `payload_len` no header sempre se refere ao JPEG payload puro.

**Validacao de sobreposicao (REQ-2, linha "offset + h.payload_len > h.jpeg_size")**:

```cpp
size_t offset = calcular_offset(s, h.frag_idx);
if (offset + h.payload_len > h.jpeg_size) {
    s_stats.fragments_invalid++;
    return false;
}
```

Esta verificacao cobre o caso de `payload_len` gigante que, mesmo individualmente <= restante do buffer RX, resultaria em escrita fora do slot alocado (que tem exatamente `MAX_JPEG_SIZE` bytes, e `h.jpeg_size <= MAX_JPEG_SIZE` ja foi validado acima).

### 4.3 REQ-2: Tabela Completa de Rejeicoes

Todas as condicoes abaixo resultam em `s_stats.fragments_invalid++; return false` sem mutacao do slot:

| # | Condicao | Implementacao |
|---|---|---|
| 1 | `len < sizeof(video_frag_hdr_t)` | Primeiros bytes insuficientes — checked antes de qualquer leitura do header |
| 2 | `h.frag_total == 0` | Divisao por zero latente + slot nunca completaria |
| 3 | `h.frag_idx >= h.frag_total` | Indice fora dos limites do bitmap |
| 4 | `h.jpeg_size > MAX_JPEG_SIZE` | Heap overflow no slot de 16 KiB |
| 5 | `h.payload_len > remaining bytes after headers` | OOB read do buffer RX |
| 6 | `h.payload_len == 0` | Fragmento sem dados — suspeito; rejeitado |
| 7 | `h.frag_idx == 0 && remaining < sizeof(video_frag0_extra_t)` | tx_emission_ms truncado |
| 8 | `offset + h.payload_len > h.jpeg_size` | OOB write no slot |

A verificacao de `h.frag_total > MAX_FRAGS_PER_FRAME` tambem e feita (nao entraria na tabela mas e necessaria para o bitmap ser valido).

### 4.4 Politica de Eviction (Timeout e Overrun)

- **Timeout (skip-drop)**: `reassembly_gc(now_ms)` chamado periodicamente (a cada fragmento recebido, no callback ESP-NOW). Slots com `now_ms - first_seen_ms > SKIP_DROP_TIMEOUT_MS` sao liberados.
- **Overrun**: quando ambos slots estao ocupados e chega fragmento de `frame_id` novo, o slot com menor `first_seen_ms` (mais antigo) e evictado. `s_stats.frames_dropped_overrun++`.

---

## 5. ESP-NOW Link — Gotchas e Seguranca

### 5.1 Registro de Recv Callback

`esp_now_register_recv_cb` so pode ser chamado APOS `esp_now_init()`. A sequencia correta:

```cpp
esp_now_init();
esp_now_register_recv_cb(on_rx);
```

O callback `on_rx` executa no contexto de uma task de rede interna do ESP-IDF (alta prioridade). Operacoes no callback devem ser O(1) e nao devem chamar `malloc`, `xQueueReceive`, ou qualquer funcao que bloqueie. `xQueueSendFromISR`/`xQueueSend` com timeout=0 e seguro.

### 5.2 REQ-4: Replay Window para MSG_TELEMETRY

O `esnow_hdr_t` atual usa `uint8_t seq` que wrapa em 256 pacotes (~128 s a 2 Hz). Para `MSG_TELEMETRY`, isso e inseguro (replay trivial).

**Acao concreta**:

Mover `seq` para dentro do payload de cada tipo de mensagem. O `esnow_hdr_t` passa a ser apenas 2 bytes de routing (`msg_type + reserved`):

```cpp
struct __attribute__((packed)) esnow_hdr_t {
    uint8_t  msg_type;
    uint8_t  reserved;   // 0x00, uso futuro
};
```

Para `MSG_TELEMETRY`, o struct `telemetry_rx_to_tx` ja contem `uint8_t seq`. Ampliar para `uint32_t seq` nesse struct (nao no header compartilhado — mantem o header pequeno):

```cpp
struct __attribute__((packed)) telemetry_rx_to_tx {
    uint8_t  msg_type;
    uint8_t  reserved;
    uint32_t seq;              // widened de uint8_t para uint32_t (REQ-4)
    // ... resto dos campos
};
```

Replay protection em `espnow_link.cpp`:

```cpp
#define REPLAY_WINDOW 32
static uint32_t s_last_seq[256] = {};  // indexado por msg_type

static bool check_replay(uint8_t msg_type, uint32_t seq) {
    uint32_t last = s_last_seq[msg_type];
    // Aceita: seq > last, ou reordem dentro da janela
    if (seq > last) { s_last_seq[msg_type] = seq; return true; }
    if ((last - seq) <= REPLAY_WINDOW) return true;  // reordem tolerada
    return false;  // replay ou muito antigo
}
```

**Arquivo modificado**: `docs/superpowers/specs/2026-05-26-receptor-design.md` §5.5 (seq widened para uint32_t).

### 5.3 REQ-5: Kconfig para Peer MAC

Arquivo: `components/espnow_link/Kconfig`:

```kconfig
menu "ESP-NOW link"
    config RECEIVER_PEER_MAC
        string "Hardcoded peer MAC (V0)"
        default "AA:BB:CC:DD:EE:FF"
        help
            Seis bytes MAC do transmissor, separados por dois-pontos.
            DEVE ser alterado antes de qualquer build de producao.
            Placeholder default eh AA:BB:CC:DD:EE:FF.
endmenu
```

Funcao de verificacao em `app_main.cpp`:

```cpp
static bool peer_mac_is_placeholder(const uint8_t mac[6]) {
    static const uint8_t placeholder[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    static const uint8_t broadcast[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t zero[6]        = {};
    return memcmp(mac, placeholder, 6) == 0 ||
           memcmp(mac, broadcast,   6) == 0 ||
           memcmp(mac, zero,        6) == 0;
}
```

Chamada no boot, antes de `espnow_link_add_peer`:

```cpp
if (peer_mac_is_placeholder(TX_MAC)) {
    ESP_LOGW(TAG, "*** PEER MAC EH PLACEHOLDER — configure CONFIG_RECEIVER_PEER_MAC ***");
}
```

Teste host-side: `components/espnow_link/host_tests/test_peer_mac.cpp` — verifica que o valor parseado do Kconfig default nao passa pela funcao `peer_mac_is_placeholder` como falso negativo.

---

## 6. Decoder (`esp_jpeg`) — Gotchas

### 6.1 API da `esp_jpeg` v1.x

A API publica e `esp_jpeg_dec.h` (nao `esp_jpeg.h`). Funcoes relevantes:

```cpp
jpeg_error_t jpeg_dec_open(const jpeg_dec_config_t *cfg, jpeg_dec_handle_t *handle);
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t handle, jpeg_dec_io_t *io,
                                   jpeg_dec_header_info_t *info);
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t handle, jpeg_dec_io_t *io);
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t handle);
```

`JPEG_PIXEL_FORMAT_RGB565_BE`: saida em big-endian (byte alto primeiro), compativel com o ILI9341. O LovyanGFX envia com `swap=false` quando o buffer ja e big-endian.

**Ajuste em `render.cpp`**: `display_blit_full` deve ser chamado com `swap=false` se o decoder emite RGB565 big-endian. Verificar empiricamente: se as cores aparecerem corretas, `swap=false` funciona. Se inverter, usar `swap=true`.

### 6.2 Malloc no Decoder

O `esp_jpeg` aloca internamente buffers de trabalho via `malloc`. Com PSRAM mapeada e `CONFIG_SPIRAM_USE_MALLOC=y`, essas alocacoes vao para PSRAM. Isso e aceitavel — o decoder nao e chamado de ISR.

Se `jpeg_dec_open` retornar `JPEG_ERR_NO_MEM`, aumentar PSRAM disponivel ou verificar se outro componente esta consumindo excessivamente.

### 6.3 Frames Corrompidos

`jpeg_dec_process` pode travar em loop apertado ou crashar em JPEG invalido/truncado (historico de bugs em libjpeg). Mitigacao basica (sem OPT-B que seria WDT):

- Validar os primeiros 2 bytes do JPEG: deve comecar com `0xFF 0xD8` (SOI marker).
- Se invalido, rejeitar antes de chamar o decoder.

```cpp
if (jpeg_len < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    ESP_LOGW(TAG, "jpeg SOI invalido");
    return -1;
}
```

---

## 7. Render — Double Buffer e DMA Swap

### 7.1 Sequencia de Swap

```
Frame N:
  1. decode_task escreve em FB[back_idx]
  2. render_present() chama display_wait_dma()  ← espera DMA frame N-1 terminar
  3. display_blit_full(FB[back_idx])            ← inicia DMA frame N
  4. back_idx ^= 1                              ← proxima escrita vai para o outro buffer
  5. decode_task pode comecar frame N+1 imediatamente
```

Este esquema garante que decode e DMA nunca usam o mesmo buffer simultaneamente.

### 7.2 Mutex em render_present

`xSemaphoreTake(s_mutex, portMAX_DELAY)` bloqueia se `render_present` for chamado antes do DMA anterior terminar (o `display_wait_dma` interno ja garante isso). O mutex protege contra chamadas concorrentes de multiplas tasks (improvavel em V0, mas correto por design).

---

## 8. Alocacao de Memoria

| Buffer | Tamanho | Localizacao | Quem aloca |
|---|---|---|---|
| slot reassembly [0] | 16 KiB | PSRAM | `reassembly_init` |
| slot reassembly [1] | 16 KiB | PSRAM | `reassembly_init` |
| FB_back [0] | 153.6 KiB | PSRAM | `render_init` |
| FB_back [1] | 153.6 KiB | PSRAM | `render_init` |
| **Total PSRAM** | **~339.2 KiB** | PSRAM | — |

PSRAM disponivel no N16R8: 8 MiB. Consumo de 339.2 KiB e ~4.1% do total. Livre para Wi-Fi buffers (~200 KiB) e heap geral.

---

## 9. Tasks FreeRTOS (Sprint 2)

| Task | Core | Prioridade | Stack | Responsabilidade |
|---|---|---|---|---|
| Wi-Fi/ESP-NOW (IDF interno) | 0 | 23 (CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0) | IDF gerencia | RX callback de rede |
| decode_task | 1 | 6 | 8 KiB | Consome frame_q, chama decoder, render_present |
| app_main (loop stats) | 0 | 1 | padrao | Log periodico de stats |

O callback `on_rx` de ESP-NOW executa na task interna do Wi-Fi (Core 0). Ele chama `reassembly_push_frag` e `xQueueSend` com timeout 0. Nao ha locking entre o callback e `decode_task` alem da fila FreeRTOS.

---

## 10. Traducao dos REQs em Acao Concreta

### REQ-1
- **Arquivo**: `components/espnow_link/include/wire_types.h`
- **Acao**: adicionar `static_assert(MAX_FRAGS_PER_FRAME > 0 && MAX_FRAGS_PER_FRAME <= 64, ...)` imediatamente apos a definicao da constante.
- **Teste**: build quebra se `MAX_FRAGS_PER_FRAME = 65` — verificar manualmente uma vez.

### REQ-2
- **Arquivo**: `components/reassembly/reassembly.cpp`, funcao `reassembly_push_frag`
- **Acao**: implementar os 8 checks da tabela (secao 4.3) em sequencia, cada um incrementando `s_stats.fragments_invalid` e retornando `false`.
- **Teste**: `components/reassembly/host_tests/test_reassembly.cpp` — 1 caso de teste por linha da tabela (8 casos).

### REQ-3
- **Arquivo**: `components/espnow_link/include/wire_types.h` (definicao), `components/reassembly/reassembly.cpp` (uso).
- **Acao**: `MAX_JPEG_SIZE` definido apenas em `wire_types.h`. Warn-log quando `jpeg_size >= 12 * 1024` e `jpeg_size <= MAX_JPEG_SIZE`. Cap aplicado ANTES do memcpy (check 4 da tabela REQ-2).
- **Teste**: caso de teste `reject_jpeg_size_above_max` em `test_reassembly.cpp`.

### REQ-4
- **Arquivo**: `docs/superpowers/specs/2026-05-26-receptor-design.md` §5.5 (spec) + `components/espnow_link/include/wire_types.h` (struct) + `components/espnow_link/espnow_link.cpp` (replay check).
- **Acao**: `seq` no `telemetry_rx_to_tx` widened para `uint32_t`. `check_replay` implementado com janela de 32. Spec §5.5 atualizado.
- **Teste**: caso de teste `replay_window_accepts_in_window` e `replay_window_rejects_old` em `components/espnow_link/host_tests/test_peer_mac.cpp` (ou arquivo separado `test_replay.cpp`).

### REQ-5
- **Arquivo**: `components/espnow_link/Kconfig` (novo), `main/app_main.cpp` (verificacao de boot), `components/espnow_link/host_tests/test_peer_mac.cpp` (teste CI).
- **Acao**: Kconfig com default placeholder. Boot loga `ESP_LOGW` se MAC = placeholder, broadcast, ou zero. Teste host-side confirma que o valor default do Kconfig e identificado como placeholder.
- **Teste**: `test_peer_mac.cpp` — assert falha se `peer_mac_is_placeholder` retornar false para o placeholder default.

---

## 11. Riscos e Mitigacoes

| Risco | Probabilidade | Mitigacao |
|---|---|---|
| `esp_jpeg` API muda na v1.1.x | Media | Pin versao `"^1.1.0"` em `idf_component.yml`; testar com `idf.py reconfigure` |
| DMA channel exhaustion (Sprint 1 G3) | Media | Inicializar Wi-Fi ANTES do display (secao 1) |
| Decoder trava em JPEG corrompido | Alta (RF noise) | Validar bytes SOI `0xFF 0xD8` antes de chamar decoder (secao 6.3) |
| Offset errado em frags fora de ordem | Media | Armazenar `frag_payload_lens[]` no slot; calcular offset ao completar (secao 4.2) |
| `static_assert` nao compilado no host build | Baixa | `wire_types.h` e includido tanto pelo host test quanto pelo target; static_assert funciona nos dois |
| PSRAM bandwidth sufoca decode paralelo | Baixa | Framebuffers e slots sao acessos sequenciais; cache prefetch mitiga |

---

## 12. Criterios de Pronto (Sprint 2)

- [ ] REQ-1: `static_assert` presente em `wire_types.h:MAX_FRAGS_PER_FRAME`
- [ ] REQ-2: 8 testes Unity no `test_reassembly.cpp`, todos passando
- [ ] REQ-3: `MAX_JPEG_SIZE` em um unico lugar; warn-log implementado
- [ ] REQ-4: `seq` widened em `telemetry_rx_to_tx`; replay window implementado; spec §5.5 atualizado
- [ ] REQ-5: `Kconfig` com placeholder; boot warning; `test_peer_mac.cpp` existe
- [ ] Sprint 1 host tests (22 testes) continuam passando apos merge
- [ ] `idf.py build` sem warnings novos
- [ ] `sprint-2-review.md` e `sprint-2-security.md` existem em `docs/`
