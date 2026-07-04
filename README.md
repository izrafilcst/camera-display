# camera-display

Módulo receptor para o robô: recebe o stream de vídeo transmitido pelo módulo
embarcado no robô, decodifica e renderiza em um display TFT 320×240, com meta
de 24 fps sem tearing/stutter e latência de exibição abaixo de 30 ms.

## Hardware

- **MCU**: ESP32-S3-N16R8 (16 MB flash QIO, 8 MB PSRAM Octal, Xtensa LX7 dual-core @ 240 MHz, Wi-Fi + BLE 5)
- **Display**: TFT 320×240, RGB565, interface SPI com DMA

## Arquitetura

- Core 0: recepção via ESP-NOW, reassembly de pacotes, pareamento BLE
- Core 1: decodificação de frame e renderização no display (DMA SPI, double buffer)

## Estrutura do projeto

```
main/            app_main.cpp — boot, inicialização e loop principal
components/
  ble_pair/      pareamento via BLE (NimBLE)
  pair_nvs/      persistência do pareamento em NVS
  espnow_link/   link de recepção via ESP-NOW
  link_state/    estado/qualidade do link
  reassembly/    reassembly de pacotes / jitter buffer
  decoder/       decodificação de frame (JPEG/RGB565)
  display/       driver do display TFT (LovyanGFX)
  render/        pipeline de renderização / blitting
docs/            specs, planos e relatórios de auditoria/review
```

## Build

Projeto ESP-IDF com wrapper PlatformIO (`platformio.ini`).

```
pio run                              # build
pio run -t menuconfig                # Kconfig (ex.: CONFIG_RECEIVER_PEER_MAC)
pio run -t upload                    # flash
pio device monitor                   # monitor serial
pio run -t upload -t monitor         # flash + monitor
pio run -t clean                     # limpar build
pio run -t fullclean                 # remover .pio/
```

## Critérios de sucesso

- [ ] 24 fps renderizados sem stutter perceptível por ≥ 10 min contínuos
- [ ] Sem tearing visível em movimento horizontal de alta frequência
- [ ] Latência de display medida < 30 ms (p95)
- [ ] Recuperação graciosa de perda de link: sem freeze > 200 ms
- [ ] Reconexão automática transparente ao usuário

Detalhes de arquitetura, restrições técnicas e decisões em aberto: ver `CLAUDE.md`.
