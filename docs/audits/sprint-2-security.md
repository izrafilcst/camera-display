# Sprint 2 — Security Audit

**Data**: 2026-05-28
**Auditor**: Security Auditor Agent (Wave 3)
**Commit base**: sprint2-coder branch
**Foco**: validar REQ-1..5; analisar superfície de ataque introduzida pelo parser de fragmentos ESP-NOW

---

## Executive Summary

Sprint 2 introduz a primeira entrada controlada pelo atacante: pacotes ESP-NOW carregando fragmentos de vídeo. O gate de hardening definido em `sprint-2-parser-hardening-requirements.md` foi implementado. Os 5 REQs obrigatórios estão presentes. Nenhum finding HIGH automático por REQ não implementado.

Dois findings MEDIUM foram identificados (um já corrigido pelo reviewer), e três findings LOW/INFO.

---

## Validacao dos REQs

### REQ-1: static_assert(MAX_FRAGS_PER_FRAME <= 64)
**Status: IMPLEMENTADO**

`components/espnow_link/include/wire_types.h`:
```cpp
static_assert(MAX_FRAGS_PER_FRAME > 0 && MAX_FRAGS_PER_FRAME <= 64,
              "MAX_FRAGS_PER_FRAME must fit in frags_bitmap (uint64_t)");
```
Linha imediatamente após a definição da constante. Build falha se `MAX_FRAGS_PER_FRAME` > 64. Cobertura de teste: qualquer build com valor >64 quebra imediatamente.

### REQ-2: Rejeicao de envelope malformado (8 condicoes)
**Status: IMPLEMENTADO**

`components/reassembly/reassembly.cpp` — função `reassembly_push_frag`:
- Row 1 (`len < sizeof(hdr)`): linha 49 — checked before any memcpy
- Row 2 (`frag_total == 0`): linha 57
- Row 3 (`frag_idx >= frag_total`): linha 64
- Row 4 (`jpeg_size > MAX_JPEG_SIZE`): linha 71
- Row 5 (`payload_len > remaining`): linha 105
- Row 6 (`payload_len == 0`): linha 83
- Row 7 (`frag_idx==0 && missing extra`): linha 90
- Row 8 (`offset + payload_len > jpeg_size`): linha 132

Todas as 8 condições incrementam `s_stats.fragments_invalid` e retornam false antes de qualquer mutação de slot.

Testes host: `components/reassembly/host_tests/test_reassembly.cpp` — 8 casos `test_req2_*` cobrem cada linha.

### REQ-3: MAX_JPEG_SIZE fonte unica + warn em [12K,16K)
**Status: IMPLEMENTADO**

- Definição única: `components/espnow_link/include/wire_types.h:MAX_JPEG_SIZE = 16*1024`
- Nenhuma redefinição local em reassembly.cpp ou decoder.cpp
- Warn-log implementado em `reassembly_push_frag` **antes** do memcpy:
  ```cpp
  if (h.jpeg_size >= 12 * 1024) {
      ESP_LOGW(TAG, "jpeg_size=%u in high band [12K,16K] frame_id=%u", ...);
  }
  ```
- Cap `h.jpeg_size > MAX_JPEG_SIZE` verificado na Row 4, antes do warn-log.

### REQ-4: seq uint32_t + replay window
**Status: IMPLEMENTADO**

- `telemetry_rx_to_tx::seq` widened para `uint32_t` em `wire_types.h` (struct size ≥ 25 B)
- Spec `docs/superpowers/specs/2026-05-26-receptor-design.md` §5.5 atualizado com nota REQ-4
- `check_and_update_seq()` em `espnow_link.cpp` com `REPLAY_WINDOW = 32`
- Bug F-01 (offset errado no `memcpy` de seq) corrigido antes da auditoria ser finalizada

**Limitação**: replay protection só cobre `MSG_TELEMETRY`. `MSG_VIDEO_FRAG` não tem seq individual (o `frame_id` serve como identificador mas não como anti-replay). Para V0 (single peer, canal 6) este é risco aceitável.

### REQ-5: Kconfig peer MAC + placeholder check
**Status: IMPLEMENTADO**

- `components/espnow_link/Kconfig`: `RECEIVER_PEER_MAC` com default `"AA:BB:CC:DD:EE:FF"`
- `peer_mac_is_placeholder()` declarada em `espnow_link.h`, implementada em `espnow_link.cpp`
- Boot warning em `app_main.cpp` antes de `espnow_link_add_peer`
- Warning também disparado dentro de `espnow_link_add_peer` como segunda camada de defesa
- Teste host: `components/espnow_link/host_tests/test_peer_mac.cpp` — 5 casos (default, broadcast, zero, dois MACs reais)

---

## Findings

### S2-01 — MEDIUM (CORRIGIDO): espnow_link.cpp offset errado na extração de seq

Já identificado como F-01 pelo reviewer e corrigido antes desta auditoria. O fix adicionou `static_assert(offsetof(telemetry_rx_to_tx, seq) == 2)` como proteção compile-time contra futuras mudanças no struct.

**Status**: FECHADO.

### S2-02 — MEDIUM: Ausência de validação do tamanho mínimo total do payload MSG_VIDEO_FRAG

`on_rx` em `espnow_link.cpp` repassa todo payload ao callback sem verificar tamanho mínimo antes de chamar `reassembly_push_frag`. A proteção existe dentro de `reassembly_push_frag` (Row 1). Porém, um pacote de 0 bytes após o `esnow_hdr_t` chegaria como `len=2` total, `payload` de 0 bytes, e seria corretamente rejeitado. Sem exploitability, mas defesa-em-profundidade recomenda checar `len > sizeof(esnow_hdr_t)` em `on_rx` antes de despachar.

**Severidade**: MEDIUM (profundidade de defesa, não exploitável com proteção atual)
**Recomendação**: adicionar `if (len <= sizeof(esnow_hdr_t)) return;` em `on_rx`.

### S2-03 — LOW: frag_payload_lens não validado contra MAX_JPEG_SIZE acumulado

Cada `frag_payload_lens[i]` é armazenado antes de verificar se o offset resultante excede `jpeg_size`. A Row 8 verifica `offset + payload_len > jpeg_size` logo depois, o que previne o overflow. Mas se um frag anterior foi aceito com payload_len grande e agora chega frag com idx+1, o `calc_offset` pode acumular offsets que juntos excedem `jpeg_size`, e o novo frag é rejeitado — correto. O slot, porém, fica em estado parcialmente corrompido (dados escritos para frags anteriores ok, mas o frame nunca completará pois faltam frags). Isso é comportamento correto (skip-drop via timeout) mas poderia ser explicitado no log.

**Severidade**: LOW
**Recomendação**: ao Row 8 rejeitar, logar `frame_id` e considerar invalidar o slot inteiro.

### S2-04 — LOW: `reassembly_init` com `slots > 4` retorna false silenciosamente

Se `slots > 4`, `reassembly_init` retorna false sem log. O caller (`app_main`) loga e aborta, mas a causa não é clara no log.

**Severidade**: LOW
**Recomendação**: adicionar `ESP_LOGE(TAG, "slots=%d out of range [1,4]", slots)` antes do return false.

### S2-05 — INFO: ESP-NOW sem criptografia (esperado em V0)

`espnow_link_add_peer` define `p.encrypt = false`. Qualquer dispositivo no canal 6 pode enviar pacotes que o receptor processará (após filtragem por `peer_addr` que o ESP-NOW faz automaticamente se o peer foi registrado). Em V0 com peer único hardcoded, o risco é baixo. Para V1+, considerar PMK/LMK.

**Severidade**: INFO (documentado no plano de sprint)

---

## Superficie de Ataque Adicionada

| Vetor | Mitigacao presente |
|---|---|
| Pacote ESP-NOW com `frame_id` variável (slot exhaustion) | 2 slots; evict oldest; OPT-A (rate limit) não implementado |
| `jpeg_size` gigante (heap overflow) | REQ-3 + REQ-2 row 4 |
| `payload_len` malformado (OOB read/write) | REQ-2 rows 5 e 8 |
| `frag_total=0` (div by zero) | REQ-2 row 2 |
| `frag_idx >= frag_total` (bitmap overflow) | REQ-2 row 3 |
| Replay de MSG_TELEMETRY | REQ-4 (window=32) |
| Peer MAC hardcoded no binário | REQ-5 (Kconfig + warn) |
| JPEG malformado crashando decoder | SOI validation em decoder.cpp |

---

## Conclusao

Todos os 5 REQs bloqueantes estão implementados. O gate de hardening está satisfeito. O sprint pode mergear para `main` com os findings S2-02..S2-04 como backlog de baixa prioridade.

**Veredicto**: APROVADO para merge.
