# Task 2.5 — Verificacao de Continuidade: LCD_CS vs SD_CS no Modulo Vermelho

**Sprint**: 1
**Posicao**: Entre Task 2 (pinout) e Task 3 (display skeleton)
**Duracao estimada**: 5 min
**Risco prevenido**: CS trocado entre TFT/SD — descoberto apos implementacao custaria horas de debug

---

## Por que fazer isso agora

O modulo vermelho (PCB 2.8" ILI9341 com XPT2046 e slot SD) existe em pelo menos 3
revisoes de PCB. Em algumas revisoes, o silkscreen ou a trilha do CS do TFT e do SD
estao trocados em relacao ao pinout impresso. Descubrir isso na Task 4
(apos implementar `display_init`) desperdicaria muito mais tempo.

Se os CS estiverem trocados, basta inverter `PIN_LCD_CS` e `PIN_SD_CS` em `pinout.h`
antes de seguir.

---

## Ferramentas necessarias

- Multimetro em modo continuidade (buzzer)
- Modulo vermelho ILI9341 desligado (sem alimentacao)

---

## Procedimento

### Passo 1 — Localizar o CI ILI9341

Identifique o CI ILI9341 no modulo (encapsulamento TQFP-80). O pino 1 e marcado por
um triangulo ou ponto no canto do encapsulamento. O pino CS do ILI9341 e o pino 1
nesse encapsulamento (consulte o datasheet ILI9341 pag 5, "Package Outline").

### Passo 2 — Testar LCD_CS

1. Coloque o multimetro em modo buzzer (continuidade).
2. Toque uma ponta no pino rotulado `CS` (ou `LCD_CS` / `TFT_CS`) no header do modulo.
3. Toque a outra ponta no pino CS do CI ILI9341 (pino 1 do TQFP).

**Resultado esperado (CS correto)**: buzzer soa.
**Resultado problema**: silencio — os pinos podem estar trocados.

### Passo 3 — Verificar se CS e TFT_CS estao trocados

Se o Passo 2 ficou silencioso:

1. Toque uma ponta no pino rotulado `T_CS` (touch CS / XPT2046 CS) no header.
2. Toque a outra ponta no pino CS do CI ILI9341.

Se **buzzer soar aqui**: os rotulos estao trocados — o pino fisico `T_CS` vai ao TFT,
e o pino fisico `CS` vai ao XPT2046.

### Passo 4 — Verificar SD_CS

1. Toque uma ponta no pino rotulado `SD_CS` (ou `SD_SS`) no header.
2. Toque a outra ponta no pino de ativacao do slot SD (pino de chip-select do slot,
   tipicamente pino 2 do conector micro-SD).

**Resultado esperado**: buzzer soa.

---

## Acoes conforme resultado

| Resultado | Acao |
|-----------|------|
| Todos os CS batem com o silkscreen | Prosseguir para Task 3 sem mudancas |
| LCD_CS e SD_CS trocados no silkscreen | Trocar `PIN_LCD_CS` e `PIN_SD_CS` em `main/pinout.h` antes da Task 3 |
| LCD_CS e TOUCH_CS trocados | Trocar `PIN_LCD_CS` e `PIN_TOUCH_CS` em `main/pinout.h` |

### Valores corretos em `pinout.h` apos correcao (exemplo CS trocado):

```c
// Se o modulo tiver LCD_CS e SD_CS trocados:
#define PIN_LCD_CS          5   // era 10 — verificado com multimetro
#define PIN_SD_CS          10   // era  5 — verificado com multimetro
```

---

## Dica adicional: neutralizar SD e XPT2046 durante init

Independente do resultado, adicionar no `display_init()` (Task 4), antes do
`s_lcd->init()`:

```c
// Neutralizar SD e XPT2046 — mantem CS alto para evitar interferencia
// durante sequencia de init do ILI9341
gpio_set_level(PIN_SD_CS,    1);
gpio_set_level(PIN_TOUCH_CS, 1);
```

Isso garante que o chip errado nao responda ao barramento SPI durante o init,
mesmo se os CS estiverem errados na primeira tentativa.

---

## Referencia

Spec `sprint-1-impl-spec.md` §1.3 — "CS Trocado em Clones do Modulo Vermelho"
