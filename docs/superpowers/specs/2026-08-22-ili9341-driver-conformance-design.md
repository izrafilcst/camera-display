# Sprint 9 Design Spec — Conformidade do driver ILI9341 + desbloqueio de boot

**Data**: 2026-08-22
**Estado**: Pronto para virar plano de implementação
**Origem**: auditoria comparativa entre `components/display` (LovyanGFX 1.2.21) e os drivers de referência do retro-go em `~/dev/robert/display/`, checada contra o datasheet ILI9341 V1.11 (`~/dev/robert/docs/ILI9341.pdf`)
**Hardware alvo**: ESP32-S3-N16R8, MAC `3C:DC:75:62:1F:3C`, USB-JTAG `/dev/ttyACM0`
**Branch**: `sprint-9-driver-conformance` · tag final `s9-done`

---

## 0. AVISO — o que os gates desta spec provam e o que não provam

**Host tests verdes nas waves 1–3 significam "a lógica de patterns está correta". Não significam "o display funciona".**

A placa hoje trava no boot antes do `app_main` (ver `docs/HANDOFF-rx-multicore-freeze.md`), portanto **nenhuma** correção desta spec terá sido observada num LCD real até a wave 5 fechar. Qualquer relatório de progresso que sugira o contrário está errado.

---

## 1. Contexto

O `components/display` é um wrapper fino sobre o LovyanGFX. Os drivers de referência do retro-go (`ili9341.h` bare-metal sobre `spi_master`, mais `sdl2.h` e `dummy.h`) implementam a mesma função por baixo de uma HAL com três backends plugáveis. A comparação expôs sete divergências entre o nosso caminho e o que o datasheet manda — cinco delas tocam o mesmo arquivo de 86 linhas (`components/display/display.cpp`).

Duas descobertas foram feitas lendo o código-fonte do LovyanGFX 1.2.21 e do ESP-IDF, não por inspeção do nosso repo, e são a razão de a spec existir em vez de um punhado de commits soltos:

1. `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)` **nunca** pode ter sucesso no ESP32-S3 — a PSRAM não é registrada com o cap de DMA.
2. `writePixels(..., swap=true)` **não** é o caminho de DMA assíncrono que o `display.h` documenta.

---

## 2. Decisões de arquitetura

| # | Decisão | Escolha | Razão |
|---|---|---|---|
| D1 | Escopo | Driver **+** desbloqueio do boot, na mesma spec | O driver é inobservável enquanto a placa não sobe |
| D2 | §C (blit/DMA) | Migrar para big-endian end-to-end | `esp_jpeg` faz o swap de graça via flag; elimina 76 800 conversões de CPU por frame |
| D3 | §A (clock SPI) | 40 MHz fixo, **sem** fallback | Não existe detecção de falha para gatear um fallback (ver 2.1) |
| D4 | Bancada | Waves 1–3 offline agora; 4–5 quando a placa voltar | Restrição operacional declarada pelo usuário |
| D5 | Empacotamento | Sprint única, 5 waves, uma branch | Cinco dos sete achados tocam `display.cpp`; branches paralelas = conflito garantido |
| D6 | Segurança do formato BE | Fechar os pixels atrás do `display.h` | Typedef/struct não pegam a falha real (ver 4.3) |

### 2.1 Por que D3 não tem fallback

Com `cfg.readable = false` (`components/display/include/lgfx_ili9341_config.h:59`), o LovyanGFX nunca lê nada do painel. `init_impl` sempre retorna `true`. Logo:

- o `if (!s_lcd->init())` de `components/display/display.cpp:25` é um teste que **nunca dispara**;
- um "cair para 40 MHz se 80 falhar" não teria gatilho para disparar.

Um fallback real exigiria `readable = true`, `freq_read ≤ 6 MHz` e leitura de `RDDID (04h)` / `RDID4 (D3h)`. Foi avaliado e **rejeitado por escopo**: 40 MHz é o valor que os módulos ILI9341 comprovadamente toleram, e a meta de fps não precisa de mais (ver 4.5).

A checagem `if (!s_lcd->init())` é **mantida** — custa zero e passa a valer se algum dia ligarmos leitura — mas o doc do `display.h` deixa de sugerir que ela detecta painel ausente.

---

## 3. Achados e rastreabilidade

| ID | Severidade | Achado | Evidência | Wave |
|---|---|---|---|---|
| §A | Alta | Clock SPI 8× acima do limite do datasheet | p.243 §18.3.4: `twc` mín 100 ns → 10 MHz máx. `main/pinout.h:25` = 80 MHz | 1 |
| §B | Alta | `MALLOC_CAP_SPIRAM \| MALLOC_CAP_DMA` falha em 100% dos boots | `esp_psram.c` (v5.2:316, v5.4:425) registra PSRAM sem `MALLOC_CAP_DMA`. `display.cpp:69` | 1 |
| §C | Média | O blit não é DMA assíncrono | `LGFXBase.hpp:343` passa `use_dma=false`; `swap=true` força `no_convert=false` | 3 |
| §D | Média | SLPOUT enviado ~64 ms após soltar RESX | p.230 §15.4 nota 7 exige ≥120 ms. `Panel_Device.cpp:65-75` | 2 |
| §E | Média | Backlight acende antes de limpar a GRAM | p.114: "Contents of memory is set randomly" no Power On. `display.cpp:33-34` | 1 |
| §F | Baixa | Comentário sobre `rgb_order` factualmente errado | p.127-128: MADCTL default `8'h00` = BGR **0** = painel RGB. `lgfx_ili9341_config.h:62` | 1 |
| §G | Baixa | Janela fixa é invariante global frágil | Legal por p.208 §9.3, mas atravessa dois componentes. `display.cpp:39-41` + `render.cpp:102-104` | 1 |

**Não é bug** (registrado para não ser "corrigido" por engano): o retro-go manda `COLMOD 0x05` e o LovyanGFX manda `0x55`. Ambos válidos — p.134 diz que os bits `DPI[2:0]` são ignorados quando a interface RGB não é usada, e nós só usamos a interface MCU.

---

## 4. Design detalhado

### 4.1 Ciclo de vida do `display_init`

```
display_init(spi_hz)
 1. guarda de reentrância        se já init: ESP_LOGW quando spi_hz difere, return true
 2. neutraliza SD_CS / TOUCH_CS  (inalterado — display.cpp:19-22)
 3. PIN_LCD_BL como output LOW   [NOVO]  backlight apagado antes do reset
 4. reset por hardware           [NOVO]  datasheet p.230 §15.4
       RST = 0 ; espera 10 ms            (tRW mín = 10 µs; margem para o rail)
       RST = 1 ; espera 120 ms           (nota 7: SLPOUT não pode ser enviado antes)
 5. new LGFX_ILI9341_Red(spi_hz)
    s_lcd->init_without_reset(false)     não pulsa RST de novo
 6. setRotation(1)                       emite COLMOD 0x55 + MADCTL|MAD_BGR
 7. fillScreen(0x0000)          [§E]     ANTES do brightness
 8. setBrightness(200)
```

**Passo 3 não é cosmético.** Com o reset manual no passo 4, ele passa a ocorrer *antes* de `Light_PWM::init(0)` rodar dentro do `Panel_Device::init`. Sem o passo 3 o pino de backlight fica indefinido exatamente durante o reset — que é o flash que o §E pretende eliminar. Como `cfg.invert = false` (`lgfx_ili9341_config.h:72`), nível LOW é apagado.

**Por que `init_without_reset` e não `init`.** Verificado em `LGFXBase.hpp:1425` — `init_without_reset(bool clear = false)` chama `init_impl(false, clear)`. Com `use_reset=false`, `Panel_Device::init` ainda executa `init_rst()`, `init_cs()`, `_light->init(0)`, `_bus->init()` e `rst_control(true)`; só não pulsa o RST nem aplica os `delay(8)` / `delay(64)` próprios. Mantemos toda a inicialização de bus e backlight e trocamos apenas a temporização do reset pela do datasheet.

O `clear` fica `false` porque o passo 7 já limpa explicitamente.

### 4.2 Clock e alocação

`main/pinout.h`: `LCD_SPI_HZ_TARGET` e `LCD_SPI_HZ_SAFE` são substituídos por um único

```c
#define LCD_SPI_HZ  40000000   // 4x acima dos 10 MHz do datasheet (p.243 §18.3.4).
                               // Desvio consciente: e o valor que os modulos ILI9341
                               // toleram na pratica. Ver spec 2026-08-22 secao 2.1.
```

As duas chamadas em `main/app_main.cpp:153` e `:246` acompanham. `LCD_SPI_HZ_SAFE` era código morto — nenhum call site o referenciava.

`lgfx_ili9341_config.h:30`: `freq_read` cai de 16 MHz para 6 MHz. Mesmo com `readable=false` hoje, deixar 16 MHz ali é uma armadilha armada para quem ligar leitura amanhã (`trc` mín 150 ns → 6,67 MHz máx, p.243).

`display.cpp:66-78`: a alocação vira uma linha — `heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)` — sem o cap `DMA` que nunca casa, sem fallback e sem o `ESP_LOGW` que hoje dispara em todo boot, duas vezes (`render_init` aloca dois framebuffers). Mantém-se um `assert` de alinhamento de 4 bytes para preservar o que o `display.h` promete.

### 4.3 Contrato de pixel format — big-endian end-to-end

**A ordem de bytes do framebuffer deixa de ser a nativa do ESP32 (little-endian) e passa a ser a do fio do ILI9341 (big-endian).**

Produtores:

| Onde | Mudança |
|---|---|
| `components/decoder/decoder.cpp:52` | `cfg.flags.swap_color_bytes` `0` → `1` — o esp_jpeg swapa dentro do loop de decode, custo zero |
| `components/display/test_patterns.cpp` | helper `rgb565()` passa a montar BE |
| `components/display/host_tests/test_patterns.cpp` | expectativas acompanham |

Consumidores:

| Onde | Mudança |
|---|---|
| `display_blit_full` | `writePixels(buf, n, true)` → `writePixelsDMA(..., false)` fatiado |
| `render.cpp:132` (apagar badge FREEZE) | passa a chamar `display_blit_rect` |
| `render.cpp:155` (thumb) | passa a chamar `display_blit_rect` |
| `render_capture_thumb` | cópia crua, sem lógica nova — só o tipo de `s_thumb` vira `rgb565be_t` e o conteúdo passa a significar BE |

**D6 — a mitigação.** Nem um `typedef` nem um wrapper struct pegam a falha real, porque a falha real é um argumento `bool swap` num call site do LovyanGFX, e nos dois casos o chamador acabaria fazendo cast e passando `uint16_t*` do mesmo jeito. O que elimina a classe de bug é não deixar ninguém fora do `display.cpp` empurrar pixel:

```c
typedef uint16_t rgb565be_t;   // RGB565 big-endian — ordem do fio do ILI9341

void display_blit_full(const rgb565be_t* fb);
void display_blit_rect(const rgb565be_t* src, int src_stride,
                       int x, int y, int w, int h);
```

Uma função nova cobre os dois caminhos de pixel do `render.cpp`:

- apagar o badge FREEZE → `display_blit_rect(&fb[4*320 + 248], 320, 248, 4, 64, 16)`
- desenhar o thumb → `display_blit_rect(s_thumb, 80, 120, 140, 80, 60)`

Depois disso, `swap`, `writePixels` e `pushImage` existem em **um arquivo só**. O typedef fica como documentação; quem faz enforcement é a fronteira da API.

**Semântica de bloqueio das duas funções** (para não deixar ambíguo): `display_blit_full` retorna com a última fatia em voo — é para isso que existe o `display_wait_dma()`. `display_blit_rect` **espera o DMA terminar antes de retornar**, porque os dois call sites do `render.cpp` emendam outros desenhos logo depois e não teriam onde encaixar um wait.

**Propagação do typedef.** `rgb565be_t` sobe pela cadeia para as assinaturas não mentirem: `render_back_buffer()` passa a devolver `rgb565be_t*` (`components/render/include/render.h`), o parâmetro `out_buf` do decoder acompanha (`components/decoder/include/decoder.h`) e `s_fb[2]` / `s_thumb` em `render.cpp` mudam de tipo. É a mesma largura de sempre — `typedef uint16_t` — então nenhum cast novo aparece; só o nome do tipo muda.

O `render.cpp` continua usando `display_get_lgfx_ptr()` para `fillRect` / `print` / `setTextColor` dos badges — primitivas de desenho, cor convertida internamente pelo LGFX, imunes a ordem de byte.

**Aninhamento é seguro.** `display_blit_rect` faz seu próprio `startWrite()`/`endWrite()` dentro do bloco que `render_show_freeze` já abriu. Verificado em `Panel.hpp:87-88`: as duas são contadas por referência via `_start_count`; só o par mais externo abre e fecha transação de fato.

### 4.4 O blit novo

```c
display_blit_full(fb):
    startWrite()
    setAddrWindow(0, 0, W, H)                      // §G — volta para cá, por frame
    for (5 fatias de 48 linhas)                    // 48 * 320 * 2 = 30 720 B
        writePixelsDMA(&fb[fatia], 48*320, false)
    endWrite()
```

Duas razões para fatiar em vez de mandar os 153 600 B de uma vez:

1. Acima de ~32 KB o `Bus_SPI::writeBytes` fatia sozinho **e insere `vTaskDelay(1)` entre os pedaços**. Com `CONFIG_FREERTOS_HZ=1000` (`sdkconfig.defaults:13`) são ~4 ms de tick jogados fora por frame. Fatiando nós, ficamos abaixo do limite e nenhum `vTaskDelay` entra no caminho.
2. 240 / 48 = 5 fatias exatas, sem resto.

Encadear é seguro porque `writePixelsDMA` não re-emite RAMWR e cada `writeBytes` abre com `while (*cmd & SPI_USR) {}` — a fatia N+1 espera a N. A última fica em voo no retorno, o que finalmente dá sentido ao `display_wait_dma()` e ao double buffer do `render.cpp`.

O ganho: com `swap=false` e o framebuffer já em BE, `src` e `dst` viram o mesmo `swap565_t`, então `no_convert` passa a ser `true` e somem as 76 800 conversões de CPU por frame.

### 4.5 Orçamento de tempo — por que 40 MHz basta

| Clock | Tempo de fio por frame cheio | Teto teórico |
|---|---|---|
| 40 MHz | 153 600 B × 8 / 40 MHz = **30,7 ms** | ~32,5 fps |
| 80 MHz | 15,4 ms | ~65 fps |

A meta V0 do `docs/superpowers/plans/README.md` é **24 fps** (orçamento de 41,7 ms/frame). A 40 MHz sobra ~25% de folga, e agora com DMA de verdade o decode do frame N+1 sobrepõe o blit do frame N. Baixar o clock não compromete a meta.

Esse mesmo número contextualiza o §G: o F6 original economizava ~5 µs/frame, ou **0,016%** de um frame de 30,7 ms — pagos ao custo de um invariante que atravessava dois componentes.

### 4.6 §G — a janela deixa de ser invariante global

`display_blit_full` e `display_blit_rect` passam a emitir cada um o seu `setAddrWindow`. Com isso:

- `restore_full_blit_window` (`render.cpp:102-104`) e suas três chamadas são **deletadas**;
- o comentário de 7 linhas em `render.cpp:95-101`, que existe só para explicar o perigo, sai junto;
- o `setAddrWindow` "pinado" em `display.cpp:39-41` sai do `display_init`.

O comportamento continua legal pelo datasheet de qualquer forma — p.208 §9.3 garante que o contador volta a Start Column / Start Page quando passa de End Page — mas deixa de ser algo que qualquer chamador futuro de `display_get_lgfx_ptr()` possa quebrar em silêncio.

### 4.7 §F — o comentário correto

`lgfx_ili9341_config.h:62` afirma que BGR é "ordem nativa do chip". É o contrário: p.127-128 mostra que o MADCTL default após Power On **e** após HW Reset é `8'h00`, ou seja bit BGR = 0 = painel com filtro **RGB**. Com `rgb_order = false` o LovyanGFX escreve ativamente `MAD_BGR` (0x08, `Panel_LCD.hpp:72`). O valor está certo para a maioria dos módulos vermelhos; a justificativa é que está invertida, e quem depurar cor com esse comentário na mão vai para o lado errado.

---

## 5. Waves

| Wave | Conteúdo | Gate | Bancada? |
|---|---|---|---|
| 1 | §A, §B, §E, §F, §G — correções isoladas | `pio run` limpo + host tests | não |
| 2 | §D — reset manual (p.230) + `init_without_reset()` | `pio run` limpo | não |
| 3 | §C — BE end-to-end + `display_blit_rect` | host tests reescritos (RED→GREEN) | não |
| 4 | Desbloqueio do boot | `app_init: Application information:` no log | **sim** |
| 5 | Validação em hardware | olho no LCD | **sim** |

Merge após a wave 3. As waves 4 e 5 seguem como checklist vivo no plano de implementação.

### 5.1 Wave 4 — procedimento

Bisseção na ordem do `docs/HANDOFF-rx-multicore-freeze.md`, uma hipótese por flash, critério de sucesso único: aparecer `app_init: Application information:` no log.

1. `esptool erase_flash` + reflash
2. desabilitar brownout (`CONFIG_ESP_BROWNOUT_DET`)
3. desabilitar stack canaries (`CONFIG_COMPILER_STACK_CHECK*`)
4. `CONFIG_SPIRAM_SPEED_80M` → `40M`
5. remover `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM`
6. `CONFIG_COMPILER_OPTIMIZATION_PERF` → `DEBUG`
7. forçar IDF 5.x via `platform = espressif32 @ 6.13.0`

**Ressalva contra confusão:** baixar o SPI do LCD para 40 MHz (§A) **não pode** consertar esse freeze. Ele ocorre antes do `app_main`, e o barramento do LCD só é inicializado depois. Os dois tocam o mesmo rail de alimentação, mas em momentos diferentes do boot.

### 5.2 Wave 5 — checklist de bancada

Itens que nenhum host test alcança:

- [ ] `setBrightness(0)` apaga a tela? → valida `Light_PWM.invert = false`
- [ ] barras de cor com vermelho vermelho? → valida `rgb_order = false` (BGR)
- [ ] badge FREEZE apaga com a cor certa → **consumidor BE nº 3**, não coberto por CI
- [ ] thumb em `AGUARDANDO LINK` com a cor certa → **consumidor BE nº 4**, não coberto por CI
- [ ] 40 MHz estável em soak de 10 min, sem artefatos
- [ ] fps medido ≥ 24
- [ ] sem flash de lixo da GRAM no boot → valida §E
- [ ] sem tela branca em cold boot repetido (10×) → valida §D

---

## 6. Testabilidade

**Cobertos por host tests** (`components/display/host_tests/`): os patterns em BE. A wave 3 começa reescrevendo as expectativas para big-endian (RED), depois vira o helper `rgb565()` (GREEN) — o fluxo do `superpowers:test-driven-development`.

**Não cobertos, e a spec não finge que são**: tudo que precisa do LovyanGFX ou de hardware — os dois consumidores BE dentro do `render.cpp`, a temporização do reset, a ordem BGR, a polaridade do backlight, a estabilidade do clock. Esses fecham na wave 5 e estão listados nominalmente em 5.2.

---

## 7. Riscos

| Risco | Mitigação |
|---|---|
| Um consumidor BE esquecido → cor errada só naquele elemento, falha parcial e silenciosa | D6 concentra todo push de pixel no `display.cpp`; os dois casos não cobertos por CI estão nominalmente em 5.2 |
| Wave 4 não converge e as waves 1–3 ficam sem validação real | Merge acontece após a wave 3; o código fica em `main` e o checklist de bancada segue vivo |
| 120 ms extras no boot (§D) | Irrelevante — ocorre uma vez, e o boot já leva ordens de grandeza mais |
| Fatiar em 48 linhas assume o limite de ~32 KB por transação | Verificar no primeiro soak: se aparecer `vTaskDelay` no perfil do blit, reduzir a fatia |
| 40 MHz ainda é 4× o datasheet | Aceito e documentado (2.1). Se aparecer artefato na wave 5, o próximo degrau é 26 MHz — ainda acima dos 24 fps |

---

## 8. Fora de escopo

- Fallback automático de clock por leitura de ID (`readable=true` + `RDDID`) — avaliado e rejeitado em 2.1
- Reintroduzir a HAL multi-backend do retro-go (`sdl2` / `dummy`) para testar o display no desktop
- Fade de backlight no estilo `ledc_set_fade_time_and_start` do driver de referência
- Caminho de `deinit` para painel e barramento
- Qualquer coisa do Sprint 4 (BLE pairing) — o handoff marca como "não regrida"
- Investigar por que o `esp_jpeg` não é o gargalo (não medido; presume-se que não é)

---

## Apêndice A — Referências verificadas

**Datasheet ILI9341 V1.11**

| Página | Seção | Fato usado |
|---|---|---|
| 101 | 8.2.12 SLPOUT | Esperar 5 ms antes do próximo comando; 120 ms para entrar em Sleep Out |
| 114 | 8.2.22 RAMWR | Power On Sequence: "Contents of memory is set randomly" |
| 127-128 | 8.2.29 MADCTL | Default `8'h00` após Power On e HW Reset → BGR = 0 = painel RGB |
| 134 | 8.2.33 COLMOD | `DPI[2:0]` ignorado quando a interface RGB não é usada; default 18 bpp |
| 208 | 9.3 | Page counter > End Page → volta a Start Column / Start Page |
| 230 | 15.4 | `tRW` mín 10 µs; 5 ms após soltar RESX; SLPOUT não antes de 120 ms |
| 243 | 18.3.4 | 4-line SPI: `twc` mín 100 ns (10 MHz); `trc` mín 150 ns (6,67 MHz) |

**LovyanGFX 1.2.21**

| Arquivo:linha | Fato usado |
|---|---|
| `LGFXBase.hpp:343` | `writePixels(..., swap)` passa `use_dma = false` |
| `LGFXBase.hpp:347` | `writePixelsDMA(..., swap)` passa `use_dma = true` |
| `LGFXBase.hpp:408` | `pushImage` é templated no tipo de pixel |
| `LGFXBase.hpp:1425` | `init_without_reset(bool clear = false)` → `init_impl(false, clear)` |
| `Panel.hpp:87-88` | `startWrite`/`endWrite` contados por `_start_count` |
| `Panel_LCD.hpp:72` | `MAD_BGR = 0x08` |
| `Panel_LCD.hpp:137` | `getColMod` → `RGB565_2BYTE` (0x55) |
| `Panel_LCD.cpp` (`writePixels`) | `no_convert` decide entre `writeBytes(..., use_dma)` e `_bus->writePixels` |
| `Panel_Device.cpp:65-75` | Reset: RST low, `delay(8)`, bus init, RST high, `delay(64)` |
| `Panel_ILI9341.hpp:82-84` | `SLPOUT` + 120 ms, `IDMOFF`, `DISPON` + 100 ms |
| `Bus_SPI.cpp` (`writeBytes`) | `use_dma=false && length<1024` → flip buffer; acima de ~32 KB → laço com `vTaskDelay(1)` |

**ESP-IDF**

| Arquivo:linha | Fato usado |
|---|---|
| `esp_psram.c:316` (v5.2) / `:425` (v5.4) | PSRAM registrada com `MALLOC_CAP_SPIRAM \| MALLOC_CAP_DEFAULT \| 8BIT \| 32BIT` — **sem** `MALLOC_CAP_DMA` |

**Drivers de referência**: `~/dev/robert/display/{ili9341,sdl2,dummy}.h` (retro-go)
