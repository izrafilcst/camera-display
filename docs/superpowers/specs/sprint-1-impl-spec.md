# Sprint 1 Implementation Spec — Display Bring-up

**Data**: 2026-05-27
**Estado**: Aprovado para implementacao
**Cobre**: Task 1–7 do plan `2026-05-26-sprint-1-bringup-display.md`
**Hardware alvo**: ESP32-S3-N16R8 + ILI9341 SPI 320x240 "modulo vermelho"

---

## 1. Validacao da Pinagem Proposta

### 1.1 Conflitos com USB-CDC (GPIO 19, 20)

Os GPIOs 19 e 20 sao os pinos D- e D+ do USB-CDC interno do ESP32-S3. Nenhum dos pinos do plano (GPIO 5–13 para SPI, 1–4 para entrada) usa 19 ou 20. **Sem conflito.** O USB-CDC permanece funcional em paralelo ao barramento SPI — util para `idf.py monitor` durante desenvolvimento.

Atencao: ao usar `CONFIG_ESP_CONSOLE_USB_CDC=y` (padrao do target esp32s3 no IDF 5.2), o boot log sai pelo USB. Se a placa nao tiver D+/D- conectados a um host USB, o boot pode emperrar ate o timeout de 3 s. Para evitar, adicionar `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` e `CONFIG_ESP_CONSOLE_USB_CDC=n` no `sdkconfig.defaults` durante debug via UART (GPIO 43/44).

### 1.2 Strapping Pins (GPIO 0, 3, 45, 46)

Strapping pins do ESP32-S3 em boot:

| GPIO | Funcao strapping | Valor desejado no boot | Proposta do plan |
|------|-------------------|-----------------------|-----------------|
| 0    | Boot mode (0=download, 1=normal SPI boot) | HIGH (normal) | Nao usado — OK |
| 3    | Tensao interna JTAG (0=1.8V, 1=3.3V) | Depende do target | **Usado: PIN_JOY_Y** — ver abaixo |
| 45   | VDD_SPI (0=1.8V, 1=3.3V) | HIGH (3.3V) | Nao usado — OK |
| 46   | ROM messages (0=enabled, 1=disabled) | LOW (habitual) | Nao usado — OK |

**PROBLEMA IDENTIFICADO: GPIO 3 e strapping pin.**

GPIO 3 e usado no plan como `PIN_JOY_Y` (ADC1_CH2). Durante o reset/boot, o nivel logico em GPIO 3 influencia a configuracao de tensao do JTAG interno. Se o divisor resistivo do joystick PSP-1000 apresentar nivel intermediario ao ligar, pode haver comportamento inesperado.

**Mitigacao recomendada**: mover `PIN_JOY_Y` para GPIO 14 (ADC1_CH13, livre, sem funcao de strapping). Ajustar `PIN_BTN_BACK` de GPIO 1 para manter coerencia numerica se necessario.

GPIO 1 em si nao e strapping pin no ESP32-S3 (diferente do ESP32 classico onde GPIO 1 e TXD0). No ESP32-S3, GPIO 1 e de uso geral — uso como botao e seguro.

**Quadro de pinos ajustados recomendados:**

| Funcao | GPIO original | GPIO recomendado | Motivo |
|--------|--------------|------------------|--------|
| PIN_JOY_Y | 3 | 14 | Evita strapping pin |
| PIN_JOY_X | 4 | 4 | Sem conflito, mantido |
| PIN_BTN_ADVANCE | 2 | 2 | Sem conflito, mantido |
| PIN_BTN_BACK | 1 | 1 | GPIO 1 ok no S3 |

Se hardware ja estiver soldado com GPIO 3 para joystick Y: adicionar pull-down externo de 10k entre GPIO 3 e GND para garantir nivel LOW ao boot (nivel do ADC em repouso no joystick PSP-1000 e tipicamente ~1.65 V central, podendo ser amostrado como HIGH pelo strapping). Avaliar na bancada.

### 1.3 CS Trocado em Clones do Modulo Vermelho

O "modulo vermelho" (PCB 2.8" ILI9341 com XPT2046 e slot SD) tem pelo menos 3 revisoes de PCB conhecidas na comunidade. Em algumas revisoes, o CS do TFT e CS do SD estao trocados no silkscreen ou na trilha versus o pinout impresso.

**Procedimento de verificacao com multimetro (continuidade):**

1. Com placa desligada, colocar multimetro em modo buzzer (continuidade).
2. Toque uma ponta no pino rotulado `CS` (ou `TFT_CS`) no header do modulo.
3. Toque a outra ponta no pino 1 do CI ILI9341 (pino CS do chip — identificado pelo triangulo de orientacao no encapsulamento TQFP). Deve bipar.
4. Se nao bipar, tente o pino rotulado `T_CS` (touch CS) em vez do `CS` — se bipar aqui, os rotulos estao trocados.
5. Repita para SD: pino rotulado `SD_CS` deve fazer continuidade com o pino 1 do slot SD.

**Consequencia pratica**: se os CS estiverem trocados:
- Inicializar display com `PIN_LCD_CS = 5` e `PIN_SD_CS = 10` (valores trocados no `pinout.h`).
- Manter ambos `PIN_TOUCH_CS` e `PIN_SD_CS` em HIGH na init para evitar que o chip errado responda durante o init do ILI9341.

**Dica adicional**: durante o `display_init`, antes de `s_lcd->init()`, colocar ambos GPIO 5 e GPIO 6 em saida HIGH manualmente via `gpio_set_level()` para neutralizar SD e XPT2046, independente da logica do LovyanGFX.

---

## 2. API Signatures Finais do Componente `display`

### Convencoes gerais

- Convencao de erro: `esp_err_t` para funcoes que podem falhar em runtime; `bool` apenas para `display_init` (mapa direto do plan, custo zero de mudanca no app_main).
- Thread-safety: **nenhuma funcao e thread-safe**. Todas as chamadas devem ocorrer na mesma task (task_render no Core 1). O objeto LovyanGFX interno nao e protegido por mutex.
- Pre-condicao universal: todas as funcoes (exceto `display_init`) exigem que `display_init` tenha sido chamado com sucesso anteriormente. Comportamento com `s_lcd == nullptr` e undefined (crasha). A implementacao pode adicionar assert em modo debug.

---

### `display_init`

```c
/**
 * @brief Inicializa o barramento SPI, configura o ILI9341 e liga o backlight.
 *
 * @param spi_hz  Frequencia SPI em Hz. Valores tipicos: 40000000 ou 80000000.
 *                O driver reclampeia ao maximo suportado pelo hardware se
 *                o valor fornecido exceder o limite do SPI2_HOST.
 *
 * @return true   Init bem-sucedido; display mostra tela preta, backlight aceso.
 * @return false  Falha: log ESP_LOGE com detalhe. Causa provavel: pinagem errada,
 *                modulo sem alimentacao, ou SPI_DMA_CH_AUTO esgotado.
 *
 * @pre   Nenhuma. Pode ser chamada a qualquer momento apos app_main().
 * @post  s_lcd != nullptr; setRotation(1) aplicado; tela preenchida com 0x0000.
 * @note  Nao e thread-safe. Chamar uma unica vez, em app_main antes de criar tasks.
 * @note  Aloca o objeto LGFX_ILI9341_Red no heap interno (nao PSRAM) via new.
 */
bool display_init(uint32_t spi_hz);
```

Valores de retorno expandidos para log (interno, nao exposto na API):
- `ESP_ERR_INVALID_STATE`: chamada dupla (s_lcd ja != nullptr) — logar e retornar true.
- `ESP_ERR_NO_MEM`: heap insuficiente para objeto LGFX.
- Falha silenciosa do `init()`: log do LovyanGFX ja indica o erro.

---

### `display_blit_full`

```c
/**
 * @brief Envia um framebuffer RGB565 completo (320x240) ao display via DMA SPI.
 *
 * @param rgb565_buf  Ponteiro para buffer de 320*240*2 = 153600 bytes em RGB565
 *                    little-endian (formato nativo do ILI9341 com swap=true).
 *                    O buffer DEVE estar alinhado a 4 bytes.
 *                    Pode residir em DRAM interna (MALLOC_CAP_INTERNAL) ou PSRAM;
 *                    ver secao 3 deste doc para implicacoes de DMA.
 *
 * @pre   display_init() retornou true.
 * @pre   display_wait_dma() foi chamada apos o ultimo display_blit_full() — ou
 *        este e o primeiro blit. Chamar blit_full sem wait_dma anterior causa
 *        race: o buffer anterior ainda pode estar sendo transmitido.
 * @post  DMA iniciado; retorno da funcao NAO significa blit completo.
 *        Chamar display_wait_dma() para bloquear ate conclusao.
 *
 * @note  Nao e thread-safe.
 * @note  buf == nullptr: retorno silencioso (no-op).
 */
void display_blit_full(const uint16_t *rgb565_buf);
```

---

### `display_wait_dma`

```c
/**
 * @brief Bloqueia ate o DMA SPI do ultimo blit terminar.
 *
 * Internamente chama lgfx->waitDMA(), que aguarda em busy-loop o flag
 * de conclusao do periferico SPI. Tipicamente < 1 us de espera se chamada
 * apos o tempo esperado do blit (15–30 ms).
 *
 * @pre   display_init() retornou true.
 * @post  DMA concluido; o buffer passado ao ultimo display_blit_full() pode
 *        ser reutilizado ou liberado com seguranca.
 *
 * @note  Chamar antes de modificar o buffer de blit para evitar corrupcao visual.
 * @note  Nao e thread-safe.
 * @note  Chamar sem blit pendente e seguro (retorna imediatamente).
 */
void display_wait_dma(void);
```

---

### `display_alloc_framebuffer_psram`

```c
/**
 * @brief Aloca um framebuffer RGB565 de 320*240*2 bytes na PSRAM.
 *
 * Tenta primeiro com MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA.
 * Se falhar (PSRAM octal com restricao DMA — ver secao 3), tenta somente
 * MALLOC_CAP_SPIRAM e loga aviso sobre necessidade de staging buffer.
 *
 * @return Ponteiro alinhado a 4 bytes em PSRAM, ou nullptr se PSRAM
 *         indisponivel ou memoria insuficiente.
 *         Endereco esperado: 0x3C000000–0x3FFFFFFF (mapa PSRAM do S3).
 *
 * @pre   CONFIG_SPIRAM=y e CONFIG_SPIRAM_MODE_OCT=y em sdkconfig.
 * @post  Buffer nao inicializado. Conteudo indefinido.
 *
 * @note  Nao e thread-safe.
 * @note  Capacidade total PSRAM 8 MB; dois framebuffers consomem 307.2 KB (< 4%).
 */
uint16_t *display_alloc_framebuffer_psram(void);
```

---

### `display_free_framebuffer`

```c
/**
 * @brief Libera framebuffer previamente alocado por display_alloc_framebuffer_psram.
 *
 * @param fb  Ponteiro retornado por display_alloc_framebuffer_psram().
 *            fb == nullptr: no-op.
 *
 * @pre   display_wait_dma() foi chamada se fb estava em uso por DMA ativo.
 * @note  Nao e thread-safe.
 */
void display_free_framebuffer(uint16_t *fb);
```

---

### `display_get_lgfx_ptr`

```c
/**
 * @brief Retorna ponteiro ao objeto LGFX_Device interno (uso em Sprint 3+).
 *
 * Expoe o objeto LovyanGFX para uso direto de primitivas graficas (drawString,
 * fillRect, etc.) no overlay HUD sem duplicar wrappers.
 *
 * @return Ponteiro valido ao objeto LGFX_ILI9341_Red apos display_init() OK.
 *         nullptr se display_init() nao foi chamado ou falhou.
 *
 * @note  O caller NAO deve chamar startWrite/endWrite sem parear corretamente.
 *        O caller NAO deve chamar waitDMA diretamente se display_wait_dma()
 *        ja estiver sendo chamada — duplo wait e seguro mas desperdicador.
 * @note  Tipo de retorno exposto como void* no header C para evitar incluir
 *        LovyanGFX.hpp no header publico. Caster para LGFX_ILI9341_Red* no
 *        arquivo .cpp que inclui lgfx_ili9341_config.h.
 *
 * Assinatura no header publico (display.h):
 *   void *display_get_lgfx_ptr(void);
 *
 * Uso tipico (Sprint 3, em arquivo .cpp que conhece o tipo completo):
 *   auto *lcd = static_cast<LGFX_ILI9341_Red*>(display_get_lgfx_ptr());
 */
void *display_get_lgfx_ptr(void);
```

Motivacao do `void*`: manter `display.h` puro C (sem includes C++) e evitar exposicao do template LovyanGFX para units que nao precisam. Sprint 3 cria um header separado `display_lgfx.h` com o cast tipado, incluido apenas nos arquivos de overlay.

---

## 3. PSRAM DMA-capable — Analise Tecnica

### 3.1 Restricao DMA + PSRAM Octal no ESP32-S3

O ESP32-S3 usa um bus interno denominado PSRAM_BUS (via MSPI). A PSRAM Octal (modo OPI) e acessada pelo controlador MSPI0, enquanto o SPI geral (SPI2/SPI3) usa MSPI1+. O DMA do SPI2 (usado pelo LovyanGFX para o display) pertence ao GDMA (General DMA), cujos canais podem acessar tanto DRAM interna quanto PSRAM, **desde que a restricao de alinhamento seja respeitada** (buffer alinhado a 4 bytes, tamanho multiplo de 4 bytes).

**Resposta direta**: `writePixels()` do LovyanGFX pode tecnicamente enviar DMA de PSRAM no ESP32-S3 com IDF 5.2, **mas ha um caveat critico**:

O GDMA no ESP32-S3 exige que o descritor DMA e o buffer de dados estejam em memoria acessivel pelo GDMA. A PSRAM e acessivel pelo GDMA no S3 (diferente do ESP32 classico onde PSRAM nao era DMA-capable). O IDF 5.2 configura o cache da PSRAM Octal com line size de 64 bytes; o GDMA faz flush automatico via cache. Portanto, **PSRAM -> DMA -> SPI e suportado no S3 + IDF 5.2**.

Condicoes obrigatorias:
- `CONFIG_SPIRAM_USE_MALLOC=y` (ja no sdkconfig.defaults do plan)
- `CONFIG_ESP32S3_SPIRAM_SUPPORT=y` (implicito em `CONFIG_SPIRAM=y` no target s3)
- Buffer alinhado: `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)` garante alinhamento adequado.

### 3.2 Staging Buffer — Quando Necessario

Um staging buffer em DRAM interna e necessario **somente** se:
1. A versao especifica do LovyanGFX usada tem bug que nao respeita o flag DMA-capable da PSRAM (verificar changelog da v1.1.16+).
2. O bloco PSRAM alocado nao satisfaz o alinhamento exigido pelo GDMA (improvavel com `heap_caps_malloc`).
3. Benchmark mostrar que a penalidade de latencia da PSRAM (cache miss + acesso via MSPI0) supera o custo de copiar 153.6 KB para DRAM.

**Custo de staging copy**: `memcpy(staging_dram, psram_fb, 153600)` em LX7 @ 240 MHz com PSRAM Octal @ 80 MHz leva aproximadamente **1.5–2.5 ms** (PSRAM bandwidth teorica ~80 MB/s, mas DMA do proprio blit ja ocorre em paralelo com CPU). Este custo e aceitavel dentro do orcamento de 41.6 ms.

### 3.3 Tradeoff de Latencia vs Copia

| Cenario | Blit time (estimativa) | CPU ocupada durante blit | Risco |
|---------|----------------------|--------------------------|-------|
| PSRAM direto + DMA | ~15 ms @ 80 MHz | Livre (DMA async) | Cache miss pode causar stutter |
| DRAM staging + DMA | ~15 ms + 2 ms copia | ~2 ms (memcpy) | Consome 153.6 KB de DRAM interna |
| CPU pixel-a-pixel | ~60 ms | 100% | Inaceitavel (ultrapassa orcamento) |

**Recomendacao**: tentar PSRAM direto na Task 7 do Sprint 1. Se `pattern_tearing_stripes` exibir glitches periodicos que nao ocorrem com buffer em DRAM interna, adotar staging. A funcao `display_alloc_framebuffer_psram` ja implementa o fallback silencioso (tenta DMA flag, depois sem DMA flag) — isso cobre o caminho de compatibilidade sem mudanca de API.

**Capacidade de DRAM interna disponivel para staging** (se necessario): O ESP32-S3 tem ~512 KB de DRAM interna total; com pilhas de tasks, objetos LovyanGFX e Wi-Fi stack (Sprint 2+), disponivel para staging seria ~100–150 KB. Um framebuffer completo (153.6 KB) pode nao caber. Solucao alternativa: blit em duas metades (dois blocos de 76.8 KB), mas isso complica o codigo e so deve ser feito se PSRAM direto falhar.

---

## 4. Gotchas Conhecidas: ILI9341 Modulo Vermelho + ESP32-S3

### G1 — Polaridade do Backlight (PWM)

O modulo vermelho usa um transistor PNP (ou MOSFET P-channel) para o backlight, resultando em **logica invertida**: GPIO BL em LOW acende o backlight, GPIO BL em HIGH apaga.

Na configuracao LovyanGFX (`lgfx_ili9341_config.h`):
```cpp
cfg.invert = false;  // Valor no plan — PODE ESTAR ERRADO
```

Se ao ligar o backlight nao acender, inverter para `cfg.invert = true`.

**Verificacao sem osciloscópio**: apos `display_init()`, chamar `s_lcd->setBrightness(0)`. Se a tela apagar, o `invert` esta correto. Se a tela acender ao maximo, trocar o valor.

Referencia: o LovyanGFX trata `invert=true` como "nivel alto = apagado" (sinal PWM invertido). A maioria dos clones chineses do modulo vermelho com TR2 PNP precisa de `invert=true`.

### G2 — MADCTL Byte Exato para Rotation=1 (320x240 Paisagem, BGR vs RGB)

O ILI9341 usa o registrador MADCTL (0x36) para controlar a orientacao e ordem das cores. Para 320x240 paisagem com origem no canto superior esquerdo:

```
MADCTL = 0x28 para MY=0, MX=1, MV=1, ML=0, BGR=0 (RGB order)
MADCTL = 0xA8 para MY=1, MX=0, MV=1, ML=0, BGR=0 (RGB order)  
```

O ILI9341 **sempre armazena pixels em BGR** internamente. O campo BGR do MADCTL controla a ordem de envio dos dados (nao o armazenamento). O LovyanGFX com `cfg.rgb_order = false` (valor no plan) envia em BGR, que e o correto para o ILI9341.

**Sintoma de BGR/RGB trocado**: cores complementares — vermelho aparece azul, verde fica verde (coincide), azul aparece vermelho. Se isso ocorrer, trocar `cfg.rgb_order = true`.

**Rotation=1 no LovyanGFX** aplica internamente `setRotation(1)` que envia `MADCTL = 0x60` (MY=0, MX=1, MV=1, BGR=0) para o chip. Isso resulta em 320 pixels horizontais e 240 verticais — confirmar com `s_lcd->width()` retornando 320 e `s_lcd->height()` retornando 240 no log.

### G3 — DMA Channel Exhaustion (Wi-Fi vs SPI)

O ESP32-S3 tem 5 canais GDMA. A pilha Wi-Fi aloca 2–3 canais durante a inicializacao. O SPI2 (display) alocado com `SPI_DMA_CH_AUTO` ocupa mais 1 canal. Isso deixa 1–2 canais livres para SPI3 (se usado) ou outros perifericos.

**No Sprint 1** (sem Wi-Fi), nao ha risco de esgotamento.

**No Sprint 2+** (Wi-Fi ativo): verificar se `display_init()` continua retornando true apos `esp_wifi_start()`. Se `SPI_DMA_CH_AUTO` falhar em alocar canal, o LovyanGFX cairia para modo sem DMA (CPU polling), degradando o blit para ~60 ms.

**Mitigacao**: inicializar Wi-Fi ANTES do display para que os canais GDMA do Wi-Fi sejam alocados primeiro, e o SPI2 pega o que sobrar com `SPI_DMA_CH_AUTO`. O IDF 5.2 garante que `SPI_DMA_CH_AUTO` nao conflita com canais ja reservados desde que haja canal disponivel.

Se esgotar: usar `CONFIG_GDMA_ENABLE_DEBUG_LOG=y` e inspecionar alocacao de canais no boot log.

### G4 — Strapping Pins na Boot (GPIO 0, 3, 45, 46)

Ja coberto em secao 1.2, resumido aqui para referencia rapida:

- **GPIO 0**: pull-up externo obrigatorio (normalmente ja existe no modulo de desenvolvimento). Deixar flutuante = modo download em alguns resets.
- **GPIO 3**: usado como `PIN_JOY_Y` no plan — **risco**. Ver secao 1.2 para mitigacao.
- **GPIO 45**: se nao conectado, nivel definido pelo pull interno. Checar o modulo ESP32-S3 usado — alguns devboards tem pull-down em GPIO 45 que forcam VDD_SPI a 1.8V, incompativel com a logica 3.3V do ILI9341.
- **GPIO 46**: nivel LOW ao boot necessario. Nao usar GPIO 46 como saida com nivel incerto no boot.

**Acao preventiva**: no `sdkconfig.defaults`, adicionar:
```
CONFIG_ESPTOOLPY_BEFORE_DEFAULT=y
```
E verificar se o modulo ESP32-S3 tem pull-ups/pull-downs adequados nos strapping pins antes de soldar o display.

### G5 — Byte Swap (Endianness) no `writePixels`

O ILI9341 espera pixels em big-endian (byte alto primeiro: RRRRRGGG GGGBBBBB). O ESP32-S3 e little-endian. O LovyanGFX tem um parametro `swap` em `writePixels`:

```cpp
s_lcd->writePixels(buf, count, true /* swap */);  // true = troca bytes
```

O plan usa `swap=true` (correto para buffers RGB565 em little-endian). Se esquecer o `swap=true`, a tela mostrara cores erradas com padroes de bits invertidos (nao complementares, mas "embaralhados" — diferente do bug BGR do G2).

**Regra pratica**: sempre usar `swap=true` com buffers alocados via `malloc` no ESP32-S3. Se usar um encoder JPEG que ja emite big-endian, usar `swap=false`.

### G6 — Reset Hard (PIN_LCD_RST) e Timing

O ILI9341 exige sequencia de reset correta:
1. RST para LOW por pelo menos 10 µs.
2. RST para HIGH.
3. Espera de pelo menos 120 ms antes do primeiro comando SPI.

O LovyanGFX executa esse timing automaticamente no `init()`. Se houver reset externo (botao de reset no devboard) durante operacao, o display pode ficar em estado indefinido — o `display_init()` deve ser chamado novamente.

**Implicacao para Sprint 3 (reconexao de link)**: o display sobrevive a reconexao de rede sem necessidade de reinit. Somente reset do MCU exige `display_init()` novamente.

### G7 — PSRAM e `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM`

O sdkconfig.defaults do plan inclui `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM=y`. Isso move buffers internos do Wi-Fi para a PSRAM, liberando DRAM interna — **bom para Sprint 2+**.

Porem, durante o Sprint 1 (sem Wi-Fi), essa config pode causar aviso no boot sobre PSRAM nao totalmente mapeada se o SPIRAM cache nao estiver configurado corretamente. Nao e erro fatal. Verificar no boot log por mensagens `E (xxx) spiram:`.

---

## 5. Ajustes Recomendados ao Plan

### 5.1 Task 2 — Pinagem: Mover PIN_JOY_Y de GPIO 3 para GPIO 14

**Justificativa**: GPIO 3 e strapping pin no ESP32-S3. Risco de boot instavel se o joystick apresentar nivel intermediario ao ligar.

**Mudanca minima em `pinout.h`**:
```cpp
// Antes:
#define PIN_JOY_Y  3   // ADC1_CH2
// Depois:
#define PIN_JOY_Y  14  // ADC1_CH13 — livre, sem strapping
```

Impacto: zero em Sprint 1 (joystick nao usado). Previne problema futuro em Sprint 4.

### 5.2 Task 4 — LovyanGFX Config: Adicionar `invert = true` como TODO comentado

**Justificativa**: a polaridade do backlight e incerta ate o hardware real ser testado. O plan define `cfg.invert = false` sem ressalva. Adicionar comentario explicitando o risco poupa tempo de debug.

Sugestao de comentario no `lgfx_ili9341_config.h`:
```cpp
cfg.invert = false;  // VERIFICAR no hw: se backlight nao acender, trocar para true
                     // Modulos vermelhos PNP geralmente precisam invert=true
```

### 5.3 Task 4 — LovyanGFX Config: `bus_shared = false` esta correto, nao mudar

O plan define `cfg.bus_shared = false`. Isso esta correto porque SD e touch nao sao usados via LovyanGFX. Se `bus_shared = true` fosse definido, o LovyanGFX aplicaria locking de mutex a cada transacao, adicionando ~5–10 µs overhead por frame — desnecessario quando o barramento e dedicado ao display.

### 5.4 Task 7 — PSRAM Allocator: Adicionar Teste de Alinhamento

**Justificativa**: a secao 3 deste spec mostra que DMA de PSRAM requer alinhamento a 4 bytes. O smoke test atual apenas verifica que a alocacao nao e nula. Adicionar verificacao de alinhamento:

```cpp
if (psram_fb) {
    assert(((uintptr_t)psram_fb & 0x3) == 0);  // alinhamento 4 bytes
    ESP_LOGI(TAG, "psram fb ok at %p (aligned=%d, free spiram=%u)",
             psram_fb,
             (((uintptr_t)psram_fb & 0x3) == 0),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    display_free_framebuffer(psram_fb);
}
```

### 5.5 Adicionar Task 8 (nova): Verificar Pinos CS do Modulo Vermelho com Multimetro

**Justificativa**: o swap de CS entre TFT/SD e um problema real em clones. Descobrir isso depois do Task 4 (apos implementar init completo) desperdicaria mais tempo. Inserir como **Task 2.5** (entre Task 2 e Task 3):

**Task 2.5**: Verificar pinagem fisica com multimetro (procedimento na secao 1.3 deste spec).
- Se CS trocado: atualizar `pinout.h` antes de prosseguir para Task 3.
- Dura ~5 min. Previne debug de horas em Task 4.

### 5.6 Sem Mudanca na Ordem das Tasks 1–7

A ordem Task 1 → 2 → 3 → 4 → 5 → 6 → 7 e logica e correta. As mudancas acima sao acrescimos e ajustes dentro das tasks existentes, nao reordenacoes.

---

## 6. Resumo das Decisoes de Arquitetura

| Decisao | Escolha | Razao |
|---------|---------|-------|
| Erro em display_init | bool | Compatibilidade com plan; simples para app_main |
| Erro em funcoes de alloc | nullptr | Padrao C; caller verifica e loga |
| Thread-safety | Nenhuma (single-task) | Custo zero; task_render e unica caller |
| display_get_lgfx_ptr retorno | void* | Mantem display.h puro C |
| DMA de PSRAM | Tentativa direta com fallback | S3 suporta, verificar com benchmark real |
| GPIO para joystick Y | 14 (nao 3) | Evitar strapping pin |
| Byte swap writePixels | swap=true | ESP32-S3 e little-endian; ILI9341 e big-endian |
