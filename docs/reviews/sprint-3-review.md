# Sprint 3 Code Review — Link Handling

**Data**: 2026-05-30
**Revisor**: Code Review Agent (Ruflo Swarm)
**Branch**: main
**Commits revisados**:
  - `64ea05c` — test: red tests for sprint 3 link state machine
  - `0cd0805` — feat(link_state): atomic state machine with wrap-safe idle math
  - `f514aec` — feat(render,main): link status overlays, thumb capture, link_ui task
**Escopo**: `components/link_state/`, `components/render/render.cpp`, `components/render/include/render.h`, `components/render/CMakeLists.txt`, `main/app_main.cpp`, `main/CMakeLists.txt`

**Veredicto**: REQUEST-CHANGES (dois bugs críticos; sem os dois o sprint não pode ir para produção)

---

## Findings

| # | Severidade | Arquivo:linha | Descrição | Fix Sugerido |
|---|-----------|---------------|-----------|--------------|
| F-01 | critical | `components/render/render.cpp:119–133` | `render_show_disconnected` chama `lcd->fillScreen()` e `lcd->pushImage()` dentro de `startWrite()/endWrite()`. Ambas as funções do LovyanGFX emitem internamente `CASET`/`RASET` (setAddrWindow) para suas regiões de destino. Ao final de `endWrite()` o ILI9341 retém o addr window da última operação (`pushImage` deixa o window em `120,140 → 80×60`). A função `display_blit_full` (chamada por `render_present` na reconexão) usa `writePixels` sem re-emitir `setAddrWindow`, dependendo do window fixado no `display_init()` (F6 Sprint 1). Quando o link volta de DISCONNECTED → CONNECTED, o primeiro frame completo de vídeo é escrito em `76.800` pixels mas o controller só aceita `4.800` pixels antes de voltar ao início da janela `80×60` → display completamente garbled até `display_init()` ser chamado de novo (o que nunca acontece). `render_show_freeze` via `fillRect` tem o mesmo problema em menor escala, corrompendo o window para `(250, 4, 66×16)` durante o estado FREEZE — o primeiro frame pós-FREEZE é igualmente corrompido. | Adicionar `lcd->setAddrWindow(0, 0, 320, 240)` ao final de `render_show_freeze`, `render_show_disconnected` e `render_capture_thumb` (antes de `endWrite`/`xSemaphoreGive`), restaurando o window full-frame. Alternativamente, refatorar `display_blit_full` para sempre re-emitir `setAddrWindow` (reverte F6, custo ~5 µs/frame, aceitável para a correção). |
| F-02 | critical | `components/link_state/link_state.cpp:26–27` + `link_state_query:39–40` | `link_state_mark_rx` escreve dois campos atômicos distintos com `memory_order_relaxed`: primeiro `s_last_rx_ms`, depois `s_has_rx`. No modelo de memória fraca do Xtensa LX7 (e pelo padrão C++), o compilador ou o hardware pode reordenar os dois stores. Um leitor no Core 1 (`link_state_query`, `link_ui_task`) pode observar `s_has_rx == true` com `s_last_rx_ms` ainda em `0`. Resultado: `idle = now_ms - 0 = now_ms`; se o sistema já está há mais de 3 s rodando quando chega o primeiro pacote válido, a expressão resulta em `idle > 3000` → DISCONNECTED é exibido durante um ciclo de 100 ms. Embora seja fugaz, a consequência prática é um flash do overlay de DISCONNECTED exatamente no momento em que o link acabou de ser estabelecido. Com system uptime > 49 dias pós-wrap é garantido. | Tornar o store de `s_has_rx` um `release` e o load de `s_has_rx` um `acquire`: `s_has_rx.store(true, std::memory_order_release)` no writer e `s_has_rx.load(std::memory_order_acquire)` nos leitores. Isso garante que todos os stores anteriores ao `release` (inclusive `s_last_rx_ms`) são visíveis para o thread que vê o `acquire`. O store de `s_last_rx_ms` pode continuar `relaxed` — a barreira no `s_has_rx` release é suficiente. |
| F-03 | warning | `components/render/render.cpp:95–112` | `render_show_freeze` pinta o badge quando `blink == true` mas não apaga quando `blink == false`. O comportamento de "piscar" dependeria de o decode task sobrescrever o canto superior direito — mas durante FREEZE o decode task não está entregando frames (por definição). Resultado: o badge FREEZE fica permanentemente aceso, não piscando, durante todo o estado FREEZE. | Na branch `blink == false`, re-pintar a região `(250, 4, 66, 16)` com os pixels correspondentes do front buffer (`s_fb[s_back_idx ^ 1]`), ou com a cor de fundo do frame frozen (um `fillRect` escuro). A região é pequena (66×16 = 1056 pixels) — trivial no budget do SPI. |
| F-04 | warning | `components/link_state/include/link_state.h:16–17,20` | `LINK_FREEZE_MS`, `LINK_DISCONNECT_MS` e `LINK_IDLE_UNKNOWN` são declarados como `static const uint32_t` dentro do bloco `extern "C"`. Em C++ `static const` em namespace escopo (global) cria cópias com internal linkage em cada translation unit que inclui o header — não é ODR-violation para tipos integrais, mas é um anti-padrão moderno e pode causar warnings de "unused variable" em TUs que incluem o header mas não usam as constantes. O spec/plano (`docs/superpowers/plans/2026-05-26-sprint-3-link-handling.md:52–53`) especificou `constexpr`, não `static const`. | Substituir por `constexpr uint32_t`. Como `constexpr` implica `const` e é usável em contextos de template/case, é mais idiomático. Fora do bloco `extern "C"` (constexpr não tem significado em C linkage context; as constantes são apenas visíveis de C++). |
| F-05 | warning | `components/render/render.cpp:30` | `static uint16_t s_thumb[80 * 60]` (9.600 B) vive em BSS (SRAM interna). O spec §8 aloca o thumb explicitamente em PSRAM (`8 KB` na tabela de memória). A SRAM interna do ESP32-S3 é compartilhada com as pilhas de Wi-Fi/BT, pilhas das tasks e dados de runtime. Consumir 9,6 KB de SRAM para um buffer que nunca está no hot path (só lido em DISCONNECTED) é desnecessário. | Alocar via `heap_caps_calloc(THUMB_W * THUMB_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM)` em `render_init`; liberar em `render_deinit`. Compatível com `lcd->pushImage` (LovyanGFX lê da PSRAM sem restrição em leitura). |
| F-06 | warning | `components/render/render.cpp:101` | `render_show_freeze` só entra no `startWrite/endWrite` na branch `blink == true`. Na branch `blink == false` o mutex é adquirido e liberado sem nenhuma operação LCD. Isso adiciona overhead de mutex (±5 µs) sem benefício 50% das chamadas, e `display_wait_dma()` (linha 99) é chamada incondicionalmente antes da branch, bloqueando o decode task por qualquer DMA em andamento mesmo quando o blink está desligado. | Mover `display_wait_dma()` para dentro da branch `if (blink)`, ou estruturar para que a lógica de DMA wait e LCD draw ocorra apenas quando realmente necessário. Melhor ainda: calcular `blink` antes de `xSemaphoreTake` e retornar cedo se a frame não precisar de update. |
| F-07 | warning | `main/app_main.cpp:97,103` | `link_ui_task` chama `link_state_query(now_ms)` e em seguida `link_state_idle_ms(now_ms)` com o mesmo `now_ms` capturado antes das duas chamadas. Entre as duas chamadas, um `link_state_mark_rx` de Core 0 pode atualizar `s_last_rx_ms`. Resultado: `link_state_query` retorna `LINK_DISCONNECTED` mas `link_state_idle_ms` retorna `0` (ou negativo em unsigned, ~49 dias), e o display imprime "offline: 0 s" na tela de DISCONNECTED. Este é o companheiro do F-02 no consumer side. | Adicionar `link_state_snapshot(uint32_t now_ms, link_status_t* out_st, uint32_t* out_idle)` que executa os dois loads sob uma única janela de tempo: `last = s_last_rx_ms.load(relaxed); *out_idle = now_ms - last; *out_st = ...`. A API pública fica mais segura e mais fácil de raciocinar. |
| F-08 | info | `main/app_main.cpp:122` | O boot log diz `"Sprint 2 boot"` — string herdada de sprint 2, não atualizada. Confunde logs em campo. | Alterar para `"Sprint 3 boot"` ou `"receptor v0.3 boot"`. |
| F-09 | info | `components/render/render.cpp:104` | `fillRect(250, 4, 66, 16)` define o background do badge. O texto `"FREEZE"` (6 caracteres × 12 px a textSize=2) começa em `cursor(254, 6)` e termina em x=326, 6 px além da borda direita da tela (320 px). LovyanGFX clippa internamente sem crash, mas o último trecho do badge extrapola o background de `66 px` e o caractere "E" final aparece sem background vermelho. | Ajustar para `fillRect(248, 4, 68, 16)` e `setCursor(252, 6)` para folga de 4 px, ou usar `"FRZ"` com textSize=2 (3 × 12 = 36 px) e badge menor. |
| F-10 | info | `components/link_state/host_tests/Makefile:3` | `CXXFLAGS = -std=c++17` passado como flag raw. O padrão do projeto (Sprint 1 review F8) recomenda `target_compile_features(cxx_std_17)` em CMake. O host_tests de link_state usa Makefile (copiando o padrão de reassembly/host_tests), então é consistente por ora, mas mantém a inconsistência arquitetural. | Longo prazo: migrar todos os host_tests para CMake. Por enquanto, aceitável. |

---

## Análise aprofundada dos tópicos solicitados

### 1. Inversão de prioridade: `link_ui_task` (prio 4) vs `decode_task` (prio 6)

`xSemaphoreCreateMutex()` no FreeRTOS do ESP-IDF implementa **priority inheritance**. Quando `decode_task` (prio 6) bloqueia em `xSemaphoreTake(s_mutex)` enquanto `link_ui_task` (prio 4) detém o mutex, o kernel eleva temporariamente a prioridade de `link_ui_task` para 6 até que ela libere o mutex. Como ambas as tasks estão no Core 1, o mecanismo de herança é aplicado corretamente pelo scheduler single-core.

**Conclusão: sem risco de inversão de prioridade com o mutex atual.** Usar `xSemaphoreCreateBinary` no futuro eliminaria a herança de prioridade — documentar essa restrição no comentário do `s_mutex`.

### 2. Semântica de wrap do timestamp (`memory_order_relaxed`)

A aritmética de subtração unsigned `now_ms - s_last_rx_ms` está correta para wraps de 32 bits. O teste `test_wraparound_idle_computation` cobre o caso `0x10 - 0xFFFFFFF0 = 0x20 = 32 ms` e passa. Isso é correto (veja F-02 para o problema de ordenamento, que é ortogonal ao wrap).

**O wrap em si está correto.** O problema é de ordenamento de memória entre os dois stores em `mark_rx`, não de aritmética.

### 3. `render_capture_thumb` — índice do front buffer

Após `render_present()`:
- `display_blit_full(s_fb[s_back_idx])` blit o buffer de índice `X`
- `s_back_idx ^= 1` → agora `s_back_idx = X^1`
- `render_capture_thumb` lê `s_fb[s_back_idx ^ 1] = s_fb[(X^1)^1] = s_fb[X]`

`s_fb[X]` é exatamente o buffer que acabou de ser blitado (front). **Índice correto.**

O comentário inline (`"The just-blitted frame is now the 'front' (index s_back_idx ^ 1 because render_present already flipped)"`) está preciso e é suficiente para o próximo leitor.

### 4. `reinterpret_cast` no `get_lcd()`

O cast `reinterpret_cast<lgfx::LGFX_Device*>(display_get_lgfx_ptr())` é sound porque:
- `display.cpp:9` declara `s_lcd` como `LGFX_ILI9341_Red*`
- `LGFX_ILI9341_Red` deriva de `lgfx::LGFX_Device` (LovyanGFX herança pública)
- `display_get_lgfx_ptr` retorna `static_cast<void*>(s_lcd)` — ponteiro do mesmo objeto

O round-trip `void* → LGFX_Device*` via `reinterpret_cast` é comportamento definido quando o tipo de destino é uma base do tipo original. O sprint 1 review F3 aceitou explicitamente esse padrão como trade-off correto para manter `display.h` como interface C pura.

**Aceitável.** Sugestão de melhoria: um `static_assert(std::is_base_of_v<lgfx::LGFX_Device, LGFX_ILI9341_Red>)` em `display.cpp` documenta o invariante sem custo de runtime (ver também S3-08 no audit de segurança).

### 5. addr window após sair de DISCONNECTED (Sprint 1 nit F6)

**Este é o F-01 deste review — crítico.** Sprint 1 F6 fixou o `setAddrWindow` para ser chamado uma vez em `display_init()` e nunca mais em `display_blit_full`. Sprint 3 introduz dois caminhos (`render_show_freeze` via `fillRect`, `render_show_disconnected` via `fillScreen` e `pushImage`) que emitem internamente `CASET`/`RASET` para as suas regiões, corrompendo o window pinado. O `display_blit_full` subsequente usa `writePixels` sem re-emitir setAddrWindow e produz output garbled.

---

## Spec Adherence Checklist (Sprint 3 Acceptance Criteria)

| Critério | Status | Notas |
|---|---|---|
| Estados transicionam corretamente nos 3 thresholds | PASS | Lógica correta, 7/7 testes host passam |
| FREEZE: vídeo congelado + ícone FREEZE piscando no canto | PARTIAL | Vídeo congelado: OK. Badge piscando: badge fica permanentemente aceso (F-03). Addr window corrompido na saída de FREEZE (F-01). |
| DISCONNECTED: tela de status + thumb 80×60 + contagem de tempo offline | PARTIAL | Tela de status e thumb renderizados corretamente durante DISCONNECTED. Saída de DISCONNECTED (reconexão) deixa addr window corrompido — primeiro frame de vídeo garbled (F-01 crítico). |
| Reconexão < 2 s nos 5 trials | NOT VALIDATED | Não foi executada a validação de hardware (Task 6 do plano não executada). |
| Testes Unity do `link_state` passam (host ou target) | PASS | 7/7 passam com `make test` no host. |

---

## Wins

1. **Aritmética wrap-safe sem branch**: A subtração unsigned direta `now_ms - s_last_rx_ms` em `link_state_idle_ms` e `link_state_query` é mais robusta que o ternário proposto no plano (`now >= t ? now - t : 0`), que por definição retornaria `0` em vez do idle correto durante o wrap. O teste de wraparound (extra em relação ao plano) documenta e valida isso.

2. **Namepace anônimo em `link_state.cpp`**: O uso de `namespace {}` em vez de `static` para as variáveis de módulo é idiomático em C++ moderno e correto para isolamento de linkage. Consistente com as recomendações de style do projeto.

3. **Mutex compartilhado correto**: Reutilizar `s_mutex` já existente em `render.cpp` para proteger os novos paths LCD é a decisão certa — um segundo mutex criaria risco de deadlock entre os dois. O modelo de concorrência documentado no header do arquivo é preciso e facilita auditoria futura.

4. **Separação de concerns `link_state` / `render`**: O componente `link_state` é puro C/C++ sem dependências de FreeRTOS, display ou rede — apenas `<atomic>`. Isso permite o host test simples com `g++` puro. O padrão de "state machine testável em host" introduzido em Sprint 2 para reassembly é seguido corretamente.

5. **`render.h` ganhou `extern "C"` block**: A adição dos três novos protótipos foi acompanhada pela inclusão do bloco `extern "C"` em `render.h` que estava ausente antes. Isso resolve uma inconsistência de Sprint 2 de forma silenciosa e correta.

---

## Tech Debt

| Item | Aceito? | Revisitar em |
|---|---|---|
| `s_thumb` em BSS (SRAM) em vez de PSRAM (spec §8) | Aceitável para V0 desenvolvimento | Sprint 4 (antes de soak test de 30 min) |
| Ausência de `link_state_snapshot` API — two-call TOCTOU (F-07) | Aceitável para V0 com comentário | Sprint 4 (pré-condição para feedback ao TX) |
| Validação de hardware da reconexão < 2 s (Task 6 do plano) | Não executada | Deve ser executada antes do merge para produção |
| `static const` em vez de `constexpr` para thresholds (F-04) | Funcional, anti-padrão menor | Sprint 4 cleanup |
| Boot log `"Sprint 2 boot"` (F-08) | Trivial | Próximo commit |
| Badge FREEZE não blinka quando decoder está pausado (F-03) | UX degradada mas funcional | Sprint 4 (UX polish) |
| Overhead de mutex em metade das chamadas `render_show_freeze` (F-06) | Sem impacto mensurável a 10 Hz | Sprint 4 se profiling mostrar problema |
| Herança de prioridade do mutex: não documentada — `xSemaphoreCreateBinary` no futuro quebraria | Risco latente | Adicionar comentário junto ao `xSemaphoreCreateMutex()` |

---

## Resumo

Sprint 3 entrega uma máquina de estados de link limpa e bem testada. A lógica de `link_state` (aritmética wrap-safe, atomics, 7 testes host) é a parte mais sólida do sprint. A integração com o pipeline de render, porém, tem dois bugs críticos: (F-01) as funções de overlay corrompem o addr window do ILI9341 que `display_blit_full` assume estar fixo em 320×240, tornando o primeiro frame após qualquer saída de FREEZE ou DISCONNECTED ilegível; e (F-02) os dois stores em `link_state_mark_rx` usam `memory_order_relaxed` sem barreira entre eles, permitindo que o reader veja `s_has_rx=true` antes que `s_last_rx_ms` seja visível, com potencial de flash falso de DISCONNECTED no primeiro pacote. Ambos têm correções de uma a duas linhas e devem ser resolvidos antes de qualquer teste em hardware real.

Os findings de warning (F-03 através F-07) são regressões de UX e de design — o badge que não pisca, o thumb em SRAM, a dupla leitura TOCTOU — que são aceitáveis como tech debt documentado para Sprint 4 desde que os dois críticos sejam fechados. A validação de reconexão < 2 s (Task 6 do plano) não foi executada e deve ser feita antes do merge de sprint production.
