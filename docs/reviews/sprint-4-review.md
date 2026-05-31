# Sprint 4 Code Review — BLE Pairing (CENTRAL)

**Data**: 2026-05-31
**Revisor**: Code Review Agent (Ruflo Swarm)
**Branch**: main
**Commits revisados**:
  - `660b401` — docs: sprint 4 implementation spec refined (architect)
  - `93d0fe4` — feat(s4): pair_nvs + ble_pair_state logic (GREEN: 13+25 tests)
  - `d9b5fef` — feat(s4): NimBLE pair_run + app_main boot branches + Kconfig

**Escopo**: `components/pair_nvs/`, `components/ble_pair/`, `main/app_main.cpp`, `main/Kconfig.projbuild`, `main/CMakeLists.txt`, `sdkconfig.defaults`

**Veredicto**: APPROVE-WITH-NITS

Nenhum bug crítico funcional encontrado. Os três findings de warning são corrigíveis antes do próximo hardware trial sem comprometer a lógica de pareamento. Os findings info são tech debt limpo.

---

## Findings

| # | Severidade | Arquivo:linha | Descrição | Fix Sugerido |
|---|-----------|---------------|-----------|--------------|
| F-01 | warning | `components/pair_nvs/pair_nvs.cpp:72–82` | `pair_nvs_init()` chama `nvs_flash_init()` mas não trata `ESP_ERR_NVS_ALREADY_INITIALIZED` (valor `0x1109`). Essa condição ocorre se qualquer outro componente (ex: Wi-Fi, BT controller) chamar `nvs_flash_init()` antes de `pair_nvs_init()`. O guard `if (err != ESP_OK)` retornaria `false` mesmo com NVS perfeitamente funcional. A nota do spec §T1 explicita que esse retorno deve ser tratado como sucesso. No flow atual de `app_main`, `pair_nvs_init()` é chamado antes dos inits de Wi-Fi/BT, portanto o bug não se manifesta hoje — mas o IDF pode chamar `nvs_flash_init` internamente durante `nimble_port_init`, tornando o cenário plausível em versões futuras do IDF. | Adicionar `\|\| err == ESP_ERR_NVS_ALREADY_INITIALIZED` à condição de sucesso: `if (err != ESP_OK && err != ESP_ERR_NVS_ALREADY_INITIALIZED) { ESP_LOGE...; return false; }` |
| F-02 | warning | `components/ble_pair/ble_pair.cpp:170–182` | `on_gap_event(BLE_GAP_EVENT_DISC)` não tem guarda de estado antes de chamar `ble_gap_connect()`. Se `filter_duplicates=1` falhar em suprimir um segundo report do mesmo peer (ex: por bug no controller BLE ou por dois peers com o mesmo nome "CAM-TX"), o handler é invocado novamente com `s_sm.state == BLE_PAIR_CONNECTING`. `feed(EV_ADV_MATCH)` é ignorado pelo SM (correto), mas `ble_gap_connect()` é chamado uma segunda vez. NimBLE retornará `BLE_HS_EALREADY` ou erro similar; o handler chama `feed(EV_CONNECT_FAILED)` → `BLE_PAIR_ERROR` → `terminate(false)` → reboot em 5 s. O `filter_duplicates=1` mitiga em hardware normal, mas é defensivamente insuficiente para V0 com possíveis dois TX com mesmo nome no ambiente de teste. | Adicionar guarda: `if (s_sm.state != BLE_PAIR_SCANNING) break;` como primeira linha do `case BLE_GAP_EVENT_DISC`. Alinha com a guarda já presente em `BLE_GAP_EVENT_DISC_COMPLETE` (linha 187). |
| F-03 | warning | `components/ble_pair/ble_pair.cpp:255–260` vs `287` | Inconsistência de estilo entre callbacks. `on_pin_written` chama `start_tx_mac_read()` incondicionalmente após `feed(EV_PIN_WRITE_OK)`. `on_tx_mac_read` usa guarda `if (s_sm.state == BLE_PAIR_WRITING_RX_MAC) start_rx_mac_write()`. O comportamento atual de `on_pin_written` é funcionalmente correto (EV_PIN_WRITE_OK em WRITING_PIN sempre produz READING_TX_MAC), mas a ausência da guarda é uma armadilha para quem estender o SM no futuro: qualquer erro introduzido na transição WRITING_PIN→READING_TX_MAC causaria `start_tx_mac_read()` em estado inesperado sem aviso. | Uniformizar adicionando guarda antes de `start_tx_mac_read()`: `if (s_sm.state == BLE_PAIR_READING_TX_MAC) start_tx_mac_read();` |
| F-04 | info | `components/pair_nvs/pair_nvs.cpp:73–76` | `pair_nvs_init()` chama `nvs_flash_erase()` como side-effect de `ESP_ERR_NVS_NO_FREE_PAGES`. Isso apaga toda a partição NVS, incluindo potencialmente dados de outros componentes (Wi-Fi provisioning, certificados TLS, etc.) que possam residir na mesma partição. Para este projeto V0 a partição NVS é de uso exclusivo do firmware, portanto sem impacto imediato. Mas a decisão de apagar NVS silenciosamente é destrutiva e deve ser documentada como intencional. | Adicionar comentário: `// Full NVS erase is acceptable: this project uses NVS exclusively for pairing state. No other components share the partition in V0.` Também logar após o erase: `ESP_LOGW(TAG, "NVS fully erased — pairing state lost");` |
| F-05 | info | `components/ble_pair/ble_pair.cpp:387–388` | A sequência de deinit em `ble_pair_run` é `nimble_port_stop()` → `nimble_port_deinit()`, sem `nimble_port_freertos_deinit()` chamado explicitamente por `ble_pair_run`. A task FreeRTOS (`host_task`) chama `nimble_port_freertos_deinit()` em si mesma (linha 346), o que é o padrão documentado em exemplos do ESP-IDF NimBLE. Porém o spec §1.9 gotcha #4 documenta a sequência como: `stop → freertos_deinit → deinit` (todos chamados pelo chamador externo). Se `nimble_port_deinit()` não aguardar a destruição da task pelo `vTaskDelete(NULL)` interno ao `nimble_port_freertos_deinit`, há janela de use-after-free nos recursos NimBLE. Na prática, o ESP-IDF NimBLE port usa uma semáforo interno para aguardar `nimble_port_run` retornar antes de `nimble_port_deinit` prosseguir, tornando o padrão seguro em prática. A inconsistência com o spec é um info rather than um bug real. | Documentar no comentário inline que o padrão host_task-auto-deinit é intencional e seguro nesta versão do ESP-IDF, evitando refatoração desnecessária por futuros leitores. |
| F-06 | info | `main/app_main.cpp` (ausente) | A spec §1.5 exige `esp_get_free_heap_size()` logado em 3 pontos: (1) boot, (2) após `nimble_port_deinit`, (3) após `espnow_link_init`. O código implementa apenas o ponto 1 (linha 146). Os pontos 2 e 3 estão ausentes. Esses logs são diagnósticos de desenvolvimento, não funcionais, mas eram critério de aceitação do Wave 2 (spec §4) e são úteis para verificar que NimBLE libera ~150 KB antes do Wi-Fi. | Adicionar `ESP_LOGI(TAG, "post-BLE-deinit heap=%u", (unsigned)esp_get_free_heap_size());` logo após `ble_pair_run()` retornar com sucesso, e `ESP_LOGI(TAG, "post-espnow heap=%u", ...)` após `espnow_link_init()`. |
| F-07 | info | `docs/superpowers/specs/sprint-4-impl-spec.md:§1.7` vs `main/app_main.cpp:204,212` | O pseudocódigo em spec §1.7 mostra NVS load antes do PEER_MAC override check (`pair_nvs_load_tx_mac` → `have_paired`; `!have_paired && PEER_MAC non-placeholder` → override). O critério de aceitação em spec §4 mostra a ordem inversa: `PEER_MAC override > NVS paired`. A implementação segue o §4 (PEER_MAC vence sobre NVS), que é a interpretação mais útil para desenvolvimento (permite bypass de NVS com Kconfig sem reflash). A inconsistência interna do spec deve ser resolvida atualizando o §1.7 para alinhar com §4. | Atualizar o pseudocódigo em spec §1.7 para mostrar: `pair_nvs_init()` → FORCE_PAIR_AGAIN? → PEER_MAC override? → NVS load? → BLE pair. Nenhuma mudança de código necessária. |
| F-08 | info | `components/ble_pair/ble_pair.cpp:306–320` | `on_sync()` mapeia falhas de `ble_hs_util_ensure_addr` e `ble_hs_id_infer_auto` para `feed(EV_SCAN_TIMEOUT)` com `BLE_PAIR_ERR_SCAN_TIMEOUT`. O erro code não reflete a causa real (falha de inicialização de endereço BLE, não timeout de scan). Diagnóstico em campo confuso: o log `err="SCAN_TIMEOUT"` após reboot aparecerá mesmo sem timeout algum. `BLE_PAIR_ERR_STACK_INIT` seria o erro correto. | Substituir `feed(EV_SCAN_TIMEOUT)` nas duas linhas de erro em `on_sync()` por uma chamada direta `to_error(&s_sm, BLE_PAIR_ERR_STACK_INIT); terminate(false);` (necessita tornar `to_error` acessível em `ble_pair.cpp`, ou adicionar um evento `EV_STACK_INIT_FAILED` específico). |
| F-09 | info | `components/ble_pair/CMakeLists.txt:5` | `REQUIRES` não inclui `esp_hw_support` ou `esp_system`, que é onde `esp_read_mac` reside no ESP-IDF 6.x. A dependência é satisfeita transitivamente pelo componente `bt`. Zero build warnings confirmam que funciona, mas dependências implícitas criam fragilidade: se `bt` remover essa dependência transitiva em uma futura versão, o build quebra silenciosamente. | Adicionar `esp_hw_support` (ou `esp_system`, dependendo da versão IDF) ao `REQUIRES` de `ble_pair`. |
| F-10 | info | (ausente em todo o codebase) | A spec §1.8 especificou persistir `ble_pair_err_t` em `RTC_FAST_ATTR` e logar no boot seguinte se `esp_reset_reason() == ESP_RST_SW`. Esta feature não foi implementada. Nos reboots de falha de pairing (que por design ocorrem em loop se o TX não aparecer), o desenvolvedor não tem acesso ao erro da tentativa anterior via log — cada reboot começa do zero. | Aceitar como tech debt documentado (V0 tem `ESP_LOGE` imediato antes do reboot que é visível no console USB-JTAG). Implementar na Sprint 5 se houver feedback de dificuldade de diagnóstico em campo. |

---

## Análise aprofundada dos tópicos solicitados

### 1. Concorrência callback NimBLE vs main task — `s_sm` é seguro?

**Conclusão: seguro com ressalva documentada.**

`s_sm` é acessado por dois agentes: (a) `ble_pair_run()` na main task (inicialização via `ble_pair_sm_init`), e (b) callbacks na NimBLE host task (feeds via `feed()`). Após `nimble_port_freertos_init(host_task)` retornar, a main task **bloqueia** imediatamente em `xEventGroupWaitBits(portMAX_DELAY)` e não acessa `s_sm` novamente até receber `BIT_DONE | BIT_ERROR`. A leitura de `s_sm.tx_mac` em `ble_pair_run()` (linha 394) ocorre somente após o wait retornar e `nimble_port_stop()`+`nimble_port_deinit()` completarem — garantindo que a host task está encerrada. Portanto, não há acesso concorrente real a `s_sm`.

O spec §1.9 nota #7 documenta explicitamente este invariante: "a main task bloqueia no EventGroup até o término, não há acesso concorrente real". O código honra esse contrato. Recomenda-se adicionar um comentário acima de `s_sm` em `ble_pair.cpp` reiterando o invariante, conforme sugerido pelo spec, para evitar que futura refatoração o quebre silenciosamente.

### 2. Ordem `vEventGroupDelete` + `nimble_port_deinit` — risco de vazamento?

**Conclusão: sem vazamento; ordem correta.**

A sequência em `ble_pair_run()`:

```
383: xEventGroupWaitBits(...)     // bloqueia até BIT_DONE ou BIT_ERROR
387: nimble_port_stop()           // sinaliza host task para encerrar
388: nimble_port_deinit()         // aguarda encerramento e libera NimBLE
390: vEventGroupDelete(s_done_eg) // safe: host task já encerrada
391: s_done_eg = nullptr
```

`nimble_port_deinit()` aguarda a host task encerrar (`nimble_port_run()` retorna → `host_task` chama `nimble_port_freertos_deinit()` → `vTaskDelete(NULL)` → `nimble_port_deinit()` unblocks). Após `nimble_port_deinit()` retornar, nenhuma callback NimBLE pode mais chamar `terminate()`. O `vEventGroupDelete` em linha 390 é portanto seguro e nunca há use-after-free do EventGroup.

**Caso de falha entre `xEventGroupCreate` e `nimble_port_init`:** se `nimble_port_init` falha (linha 372–378), o código deleta o EventGroup imediatamente e retorna `false`. Correto — sem vazamento.

**Caso de falha entre `nimble_port_init` e `nimble_port_freertos_init`:** se algo falha silenciosamente aqui (edge case), `xEventGroupWaitBits` bloqueia indefinidamente com `portMAX_DELAY`. O IDF task watchdog não ajuda pois a task está explicitamente em wait. Isso é um reboot hang potencial, mas irrealista na prática: `ble_hs_cfg.sync_cb` e `ble_svc_gap_init()` não podem falhar de forma silenciosa.

A `terminate()` tem guarda `if (!s_done_eg) return;` (linha 78) como defesa extra para callbacks tardios pós-deinit. Correto.

### 3. Race no scan complete — `BLE_GAP_EVENT_DISC` vs `DISC_COMPLETE`

**Conclusão: o padrão DISC_COMPLETE está correto; o DISC handler tem risco residual menor (ver F-02).**

`BLE_GAP_EVENT_DISC` e `BLE_GAP_EVENT_DISC_COMPLETE` são ambos despachados da mesma NimBLE host task, sequencialmente. Não há concorrência entre os dois. Quando `DISC` chega com `adv_has_name == true`:

1. `ble_gap_disc_cancel()` cancela o scan (enfileira comando HCI)
2. `feed(EV_ADV_MATCH)` → SM: SCANNING → CONNECTING
3. `ble_gap_connect()`

O `DISC_COMPLETE` subsequente (em resposta ao cancel) chegará somente após o return do handler atual. O estado já será CONNECTING, portanto o guard `if (s_sm.state == BLE_PAIR_SCANNING)` na linha 187 o rejeitará. **Sem race com DISC_COMPLETE.**

O risco documentado como F-02 é diferente: duplicação de `DISC` event para o mesmo peer antes do cancel efetivar-se. `filter_duplicates=1` mitiga em 99% dos casos, mas a ausência do guard de estado no handler é uma omissão defensiva.

### 4. Boot precedence em `app_main.cpp` — bate com o spec?

**Conclusão: bate com o critério de aceitação (spec §4), diverge do pseudocódigo (spec §1.7). Inconsistência é do spec, não do código.**

O critério de aceitação em spec §4 explicita: `SMOKE > FORCE_PAIR > PEER_MAC override > NVS paired > BLE pair`. O código implementa exatamente essa precedência. O pseudocódigo em spec §1.7 mostra `NVS load` antes do `PEER_MAC check`, o que contradiz §4. O coder resolveu em favor do §4, que é a interpretação mais útil em desenvolvimento (PEER_MAC override bypassa NVS sem reflash). Ver F-07 para a correção documental.

### 5. NVS error handling — `nvs_flash_erase` em `NO_FREE_PAGES`

**Conclusão: aceitável para V0 com documentação, mas há risco residual (F-01) que precisa ser corrigido antes.**

A decisão de apagar NVS em `NO_FREE_PAGES` é documentada no spec §1.6 como "Log warning + retorna false; BLE pair na próxima oportunidade". A implementação atual faz mais: apaga e reinicializa. Para este projeto onde NVS é de uso exclusivo do firmware, a consequência é apenas perder o pairing state e re-parear. Aceitável para V0.

O risco real é F-01 (`ALREADY_INITIALIZED` não tratado), que pode fazer `pair_nvs_init` retornar `false` incorretamente e travar o boot.

### 6. `esp_restart()` em loop infinito — detectável por watchdog?

**Conclusão: loop de reboot É detectável pelo watchdog de reboot, mas não pelo task watchdog.**

O padrão atual em `app_main.cpp` (linhas 224–227):
```c
vTaskDelay(pdMS_TO_TICKS(5000));
esp_restart();
```

Cada ciclo dura ~5 s (delay) + tempo de boot (~2–3 s) ≈ 8 s. O task watchdog (TWDT) com default timeout de 5 s não dispara porque a task está em `vTaskDelay` (yield legítimo). O brownout detection ativo (sdkconfig.defaults linha 25) não ajuda. O desenvolvedor verá o reboot loop nos logs via USB-JTAG mas não há mecanismo automático de break-out.

O spec §3 classifica isso como "BAIXO/ACEITÁVEL V0" e aponta que `CONFIG_RECEIVER_PEER_MAC` não-placeholder pode ser usado para sair do loop sem TX presente. **Comportamento documentado e aceitável para V0.**

Para V1: implementar contador de tentativas em RTC Fast Memory (relacionado ao F-10 da spec §1.8) — após N reboots consecutivos por SCAN_TIMEOUT, entrar em modo de espera longa (deep sleep 60 s) em vez de reboot imediato.

### 7. `on_pin_written` e a sequência `feed` + `start_tx_mac_read`

**Conclusão: funcionalmente correto, mas defensivamente fraco. Ver F-03.**

Em `on_pin_written` (linhas 258–260):
```cpp
feed(EV_PIN_WRITE_OK);        // WRITING_PIN → READING_TX_MAC (sempre)
start_tx_mac_read();          // chamado incondicionalmente
```

`EV_PIN_WRITE_OK` no estado `WRITING_PIN` transiciona **sempre** para `READING_TX_MAC`. Nunca produz ERROR. Logo, `start_tx_mac_read()` chamado logo após é sempre adequado. Correto.

Compare com `on_tx_mac_read` (linha 287): `if (s_sm.state == BLE_PAIR_WRITING_RX_MAC) start_rx_mac_write()`. Esta guarda é necessária porque `EV_TX_MAC_READ_OK` pode resultar em ERROR (payload nulo, plen != 6 detectado internamente pela SM). O pattern é correto mas inconsistente. F-03 aborda a uniformização.

### 8. Coverage de `ble_pair.cpp` — 0% host-side

**Conclusão: aceitável para V0 dado o design da separação SM/glue.**

O design intencional da Sprint 4 é que **toda a lógica de protocolo** reside em `ble_pair_state.cpp` (25 testes, 100% das transições cobertas) e que `ble_pair.cpp` é "pure glue" — mapeamento 1:1 de eventos NimBLE para `feed()`. Cada callback tem no máximo 2 decisões (status == 0 ou não, length == 6 ou não), e as consequências são testadas indiretamente via SM tests.

O risco real de 0% coverage em `ble_pair.cpp` é que bugs de wiring (ex: chamar `feed(EV_PIN_WRITE_OK)` quando deveria ser `feed(EV_PIN_WRITE_REJECTED)`) não são capturados por testes automatizados. Este risco é aceito como tech debt documentado: requer BLE stack mock ou hardware real para teste.

### 9. Kconfig precedence implementada vs documentada

**Conclusão: implementação correta conforme critério de aceitação; spec interna inconsistente. Ver F-07.**

A precedência implementada (`SMOKE > FORCE_PAIR > PEER_MAC override > NVS > BLE pair`) está correta conforme spec §4. O `FORCE_PAIR_AGAIN` usa `#if` (build-time), enquanto as demais verificações são runtime. Isso é adequado: `FORCE_PAIR_AGAIN=n` no `sdkconfig.defaults` significa que um firmware com NVS pareado não re-pareia por acidente; apenas um reflash com `FORCE_PAIR_AGAIN=y` força o re-pair.

---

## Spec Adherence Checklist (Sprint 4 Acceptance Criteria)

| Critério | Status | Notas |
|---|---|---|
| `make test` pair_nvs passa (>= 6 casos) | PASS | 13/13 confirmado |
| `make test` ble_pair_state passa (>= 20 casos) | PASS | 25/25 confirmado |
| `pio run` compila sem warnings novos | PASS | 0 warnings, 0 errors confirmado pelo autor |
| Todos os 53 testes pré-existentes passam | PASS | 91/91 total reportado (53 + 38 novos) |
| Boot logic: SMOKE > FORCE_PAIR > PEER_MAC override > NVS > BLE pair | PASS | Implementação segue critério §4 |
| `nimble_port_deinit` chamado antes de `esp_wifi_init` | PASS | `ble_pair_run` retorna antes de `espnow_link_init` |
| `esp_get_free_heap_size()` logado em 3 pontos | PARTIAL | Apenas 1 ponto (boot); pontos 2 e 3 ausentes (F-06) |
| `EV_DISCONNECTED` após DONE não transiciona para ERROR | PASS | SM tratamento early: check terminal states before switch |
| `ble_pair_run` não chamável duas vezes sem reinit | N/A | Não testável em host; documentado como não-idempotente em V0 |
| RTC Fast Memory para último erro de pair | NOT IMPLEMENTED | F-10 — spec §1.8 feature omitida |
| `esp_mac` dependência explícita em CMakeLists | PARTIAL | Satisfeita transitivamente (F-09) |

---

## Wins

1. **Separação SM/glue bem executada**: `ble_pair_state.cpp` é completamente puro (sem includes NimBLE, sem FreeRTOS), compilável e testável com `g++ -std=c++17`. O padrão estabelecido em Sprint 2 (reassembly) e Sprint 3 (link_state) é seguido consistentemente. 25 testes cobrem 100% das transições e todos os error codes.

2. **`EV_DISCONNECTED` global antes do switch**: A implementação coloca o handler de `EV_DISCONNECTED` **antes** do switch de estados (linha 34–37), ao contrário do spec §T2 que sugeria colocá-lo após. Isso é **mais correto**: garante que o disconnect em qualquer estado não-terminal → ERROR, sem depender da ordem do switch. O único risco (disconnect chegar "ao mesmo tempo" que DONE) é neutralizado pelo check `sm->state != BLE_PAIR_DONE` no início da função — estados terminais absorvem todos eventos.

3. **Dual-build `pair_nvs` hermeticamente correto**: A inline de `pn_is_placeholder` em `pair_nvs.cpp` (em vez de depender de `espnow_link`) é uma decisão de design melhor que o spec sugeria. Elimina uma dependência circular potencial (pair_nvs dependeria de espnow_link que poderia depender de pair_nvs), e mantém o host build completamente hermético.

4. **`on_svc_disc` trata o caso `svc_start == 0 || svc_end == 0`**: A verificação em `on_svc_disc` antes de prosseguir para `start_chr_disc()` evita chamar `ble_gattc_disc_all_chrs` com handles zerados. Robustez defensiva bem aplicada.

5. **`terminate()` com null-guard em `s_done_eg`**: O guard `if (!s_done_eg) return;` em `terminate()` (linha 78) é a defesa correta contra callbacks NimBLE tardios que possam chegar após `vEventGroupDelete`. Sem esse guard, xEventGroupSetBits em handle deletado seria UB.

6. **Boot log atualizado**: `"Sprint 4 boot"` (linha 145) — correção da nit F-08 de Sprint 3 aplicada corretamente.

---

## Tech Debt

| Item | Aceito? | Revisitar em |
|---|---|---|
| `ESP_ERR_NVS_ALREADY_INITIALIZED` não tratado em `pair_nvs_init` (F-01) | Corrigir antes do próximo HW trial | Sprint 4 fix commit |
| Guard de estado ausente em `BLE_GAP_EVENT_DISC` handler (F-02) | Corrigir antes de hardware trial com múltiplos BLE peers no ambiente | Sprint 4 fix commit |
| Inconsistência de estilo: sem state guard em `on_pin_written` (F-03) | Corrigir junto com F-02 (mesmo arquivo) | Sprint 4 fix commit |
| NVS erase-all em `NO_FREE_PAGES` sem comentário explícito (F-04) | Documental | Próximo commit de documentação |
| Dependência `esp_hw_support` transitiva em `ble_pair/CMakeLists.txt` (F-09) | Funcional, risco baixo | Sprint 5 cleanup |
| Heap logs em apenas 1 de 3 pontos especificados (F-06) | Diagnóstico faltante, não funcional | Sprint 4 fix commit (trivial) |
| Spec §1.7 pseudocódigo inconsistente com §4 (F-07) | Documental | Próxima revisão do spec |
| RTC Fast Memory para último erro de pair (F-10) | Feature omitida; diagnosticável via console V0 | Sprint 5 |
| `ble_pair.cpp` sem cobertura host-side (tópico 8) | Aceito como design consciente | Sprint 8 (BLE mock) |
| `ble_pair_run` não-idempotente (não rechamável sem reinit NimBLE) | Documentado como V0 | Sprint 5 se necessário |
| Mapeamento incorreto de erro em `on_sync` (F-08) — usa `EV_SCAN_TIMEOUT` para falha de init | Diagnóstico confuso em campo | Sprint 4 fix commit |

---

## Resumo

Sprint 4 entrega a fundação de BLE pairing com qualidade arquitetural alta. A separação state machine / NimBLE glue é o ponto mais forte: 25 testes host validam todas as 8 transições happy-path, 7 error codes e comportamento terminal de DONE/ERROR, sem exigir stack BLE. A lógica de `pair_nvs` é igualmente sólida com 13 testes e dual-build hermético.

A análise de concorrência dos tópicos solicitados não revelou bug crítico: `s_sm` é acessado em single-writer após init (main task bloqueia); o EventGroup é destruído somente após `nimble_port_deinit` que aguarda encerramento da host task; o `EV_DISCONNECTED` pós-DONE é absorvido pelo estado terminal.

Os três findings de warning são corrigíveis em um único commit antes do primeiro hardware trial: (F-01) tratar `ESP_ERR_NVS_ALREADY_INITIALIZED` em `pair_nvs_init`; (F-02) adicionar guard de estado no `BLE_GAP_EVENT_DISC` handler; (F-03) uniformizar o padrão de state-guard em `on_pin_written`. Nenhum dos três impacta a lógica de pareamento em cenário nominal (TX único, NVS virgem, `nvs_flash_init` chamado uma única vez), portanto o sprint pode avançar para validação de hardware com esses três itens como pré-condição de merge para produção.
