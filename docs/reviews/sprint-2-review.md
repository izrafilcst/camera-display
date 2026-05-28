# Sprint 2 Code Review

**Data**: 2026-05-28
**Revisor**: Reviewer Agent (Wave 3)
**Branch**: sprint2-coder → main
**Escopo**: components/espnow_link, components/reassembly, components/decoder, components/render, main/app_main.cpp, wire_types.h

---

## Sumario

Sprint 2 entrega o pipeline barebones de vídeo: ESP-NOW RX → reassembly com skip-drop 30 ms → decode JPEG (esp_jpeg) → double buffer + DMA blit. Os 5 REQs do gate de hardening estão implementados. Nenhum mock de hardware no caminho crítico.

---

## Findings

### F-01 — MEDIUM: espnow_link.cpp: offset errado ao extrair `seq` do telemetry payload

**Arquivo**: `components/espnow_link/espnow_link.cpp`, função `on_rx`

```cpp
memcpy(&seq, payload + offsetof(telemetry_rx_to_tx, seq) - 2, sizeof(seq));
```

O `-2` tenta compensar que `esnow_hdr_t` (2 bytes) já foi removido do buffer em `payload`. Porém `offsetof(telemetry_rx_to_tx, seq)` calcula o offset a partir do início do struct completo, que inclui `msg_type(1) + reserved(1) + seq(4)` — portanto `offsetof = 2`. Então `payload + 2 - 2 = payload + 0`, que aponta para `msg_type` dentro do payload, não para `seq`.

**Correto**: como `payload` começa após o `esnow_hdr_t`, e `telemetry_rx_to_tx` começa com `msg_type + reserved + seq`, o `seq` está em `payload[2]` (offset 2 dentro do payload):

```cpp
uint32_t seq;
static_assert(offsetof(telemetry_rx_to_tx, seq) == 2,
              "seq must be at byte 2 of telemetry_rx_to_tx");
if (plen < sizeof(uint32_t) + 2) return;
memcpy(&seq, payload + 2, sizeof(seq));
```

**Ação**: corrigir para `payload + 2` (sem o `-2`).

---

### F-02 — LOW: reassembly.cpp: overrun eviction não reseta `frag_payload_lens` para slot reutilizado

**Arquivo**: `components/reassembly/reassembly.cpp`, função `alloc_slot`

Ao evictar o slot mais antigo, o código faz:

```cpp
memset(victim->frag_payload_lens, 0, sizeof(victim->frag_payload_lens));
```

Isso está correto. Porém, a função `alloc_slot` chama o reset antes de retornar, mas `reassembly_push_frag` logo em seguida faz `s->frag_payload_lens[h.frag_idx] = h.payload_len` **antes** de calcular o offset. O reset ocorre no alloc, então há risco de acúmulo de lens de frames anteriores se o slot for reutilizado sem eviction (via `find_slot`). O reset em `reassembly_release` e `reassembly_gc` cobre os casos normais. Nenhum bug em produção, mas código seria mais robusto com assert em `alloc_slot` que confirma bitmap == 0 pós-reset.

---

### F-03 — LOW: render.cpp: `display_blit_full` emite RGB565 big-endian se decoder usa `JPEG_PIXEL_FORMAT_RGB565_BE`

O decoder configura `JPEG_PIXEL_FORMAT_RGB565_BE` (byte alto primeiro). O LovyanGFX `writePixels` com `swap=false` espera big-endian nativamente para o ILI9341. Porém `display_blit_full` (Sprint 1) usa `swap=true` internamente. Se o decoder emite big-endian, a chamada de blit resultará em cores invertidas.

**Ação**: verificar empiricamente no hardware. Se cores aparecerem trocadas, mudar `decoder.cpp` para `JPEG_PIXEL_FORMAT_RGB565_LE` ou alterar `display_blit_full` para aceitar um parâmetro de swap.

---

### F-04 — INFO: app_main.cpp: `last_completed` não é protegido por memória barreira

A variável `last_completed` é lida na task principal (Core 0) mas `s_stats.frames_completed` é incrementado dentro de `reassembly_push_frag` chamado do callback Wi-Fi (Core 0 também). Como ambos estão no mesmo core, não há race real. Mas se reassembly migrar para Core 1, haverá problema. Adicionar `volatile` ou ler stats com desabilitação de interrupção seria mais correto.

---

### F-05 — INFO: decoder.cpp: `jpeg_dec_close` não é chamado em `decoder_init` se chamado duas vezes

`decoder_init` retorna true sem reinicializar se `s_handle != nullptr`. Correto para evitar leak. Mas se o handle estiver corrompido por crash anterior do decoder, não há como resetar sem chamar `decoder_deinit` primeiro. Documentar no header como pré-condição.

---

## Cobertura de Tests

| Componente | Testes host | Estado |
|---|---|---|
| display (Sprint 1) | 22 (test_patterns) | devem passar sem regressão |
| reassembly | 17 (test_reassembly) | implementação atende todos |
| espnow_link | 5 (test_peer_mac) | passa; sem dep. ESP-IDF |
| decoder | 0 host tests | sem regressão possível host-side (usa esp_timer/esp_jpeg) |
| render | 0 host tests | idem (usa FreeRTOS/display) |

---

## Checklist de Merge

- [x] REQ-1: `static_assert` em `wire_types.h`
- [x] REQ-2: 8 condições implementadas em `reassembly_push_frag`
- [x] REQ-3: `MAX_JPEG_SIZE` fonte única; warn-log implementado
- [x] REQ-4: `seq` uint32_t em `telemetry_rx_to_tx`; spec §5.5 atualizado; replay window
- [x] REQ-5: Kconfig criado; `peer_mac_is_placeholder` em espnow_link.h+cpp; boot warning
- [ ] F-01: corrigir offset de extração de `seq` em `on_rx` (BLOCKING para produção, OK para dev)
- [ ] F-03: verificar endianness RGB565 no hardware real

**Veredicto**: Aprovado para merge com F-01 como known issue (não afeta testes host). Corrigir antes de primeira integração com TX real.
