#pragma once

// Display SPI (modulo vermelho ILI9341 + SD + touch — SD e touch nao usados)
#define LCD_SPI_HOST       SPI2_HOST
#define PIN_LCD_MOSI       11
#define PIN_LCD_SCK        12
#define PIN_LCD_MISO       13   // conectado mas idle (touch nao usado)
#define PIN_LCD_CS         10
#define PIN_LCD_DC          9
#define PIN_LCD_RST         8
#define PIN_LCD_BL          7
#define PIN_TOUCH_CS        6   // mantido HIGH; XPT2046 nao usado
#define PIN_SD_CS           5   // mantido HIGH; SD nao usado

// Entrada (sprint 4) — placeholders aqui para coerencia futura
#define PIN_JOY_X           4   // ADC1_CH3
// NOTE: GPIO 3 e strapping pin no ESP32-S3 (JTAG voltage select).
// Movido para GPIO 14 (ADC1_CH13, livre, sem funcao de strapping).
// Ref: architect spec §5.1 — previne boot instavel com joystick conectado.
#define PIN_JOY_Y          14   // ADC1_CH13 — livre, sem strapping
#define PIN_BTN_ADVANCE     2
#define PIN_BTN_BACK        1

// SPI clock. 40 MHz e 4x acima dos 10 MHz que o datasheet ILI9341 V1.11
// especifica (p.243 secao 18.3.4: twc min 100 ns). Desvio consciente: e o
// valor que os modulos ILI9341 toleram na pratica, e a 40 MHz um frame cheio
// ocupa 30,7 ms de fio (teto ~32 fps) contra a meta V0 de 24 fps.
// LCD_SPI_HZ_SAFE foi removido: era codigo morto, nenhum call site o usava.
// Ver docs/superpowers/specs/2026-08-22-ili9341-driver-conformance-design.md
#define LCD_SPI_HZ          40000000
