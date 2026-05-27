#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pinout.h"

/**
 * Configuracao LovyanGFX para o modulo vermelho ILI9341 SPI 2.8" 320x240.
 * Hardware: ESP32-S3-N16R8, SPI2_HOST, DMA automatico.
 *
 * NOTAS DE HARDWARE:
 *  - bus_shared = false: SD e touch nao sao usados via LovyanGFX.
 *    Evita overhead de mutex (~5-10 us/frame) desnecessario.
 *  - swap = true em writePixels: ESP32-S3 e little-endian, ILI9341 espera
 *    big-endian (byte alto primeiro). Sem swap = cores embaralhadas.
 *  - rgb_order = false: ILI9341 aceita BGR (ordem nativa do chip).
 *    Se cores aparecerem complementares (vermelho vira azul), trocar para true.
 */
class LGFX_ILI9341_Red : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    explicit LGFX_ILI9341_Red(uint32_t spi_hz) {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = LCD_SPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = spi_hz;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = PIN_LCD_MISO;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = PIN_LCD_CS;
            cfg.pin_rst          = PIN_LCD_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = 240;
            cfg.panel_height     = 320;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            // VERIFICAR no hw: se backlight nao acender, trocar para true.
            // Modulos vermelhos com transistor PNP no backlight precisam invert=true.
            // Teste: apos display_init(), chamar setBrightness(0).
            //   Se tela apagar -> invert=false esta correto.
            //   Se tela acender ao maximo -> trocar para invert=true.
            cfg.invert           = false;
            cfg.rgb_order        = false;  // BGR — nativo do ILI9341
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;  // SD/touch nao usados; sem mutex overhead
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;  // ver nota cfg.invert acima
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};
