# Handoff — RX trava em `cpu_start: Multicore app`

**Data**: 2026-05-31
**Estado**: Sprint 4 SOFTWARE-COMPLETE (91/91 host tests, build limpa). Hardware bring-up do caminho real de vídeo está **bloqueado** por trava silenciosa do chip durante boot do IDF.

---

## 1. Objetivo final (não cumprido ainda)

Estabelecer link TX ↔ RX e mostrar vídeo da câmera do TX na tela do RX (ILI9341 320×240) a ~24 fps via ESP-NOW canal 6.

- **TX** (camera-drone-tx, IDF 5.5.3, firmware do usuário, NÃO neste repo): pronto. BT MAC `98:A3:16:F5:2C:96`, Wi-Fi STA MAC `98:A3:16:F5:2C:94`. Adverte BLE como `CAM-TX` com PIN 1234.
- **RX** (este projeto, IDF 6.0.1, ESP32-S3-N16R8): firmware compila e flasha mas **trava no boot** antes do `app_main` ser chamado.
- **RX MAC** (Wi-Fi STA, ESP-NOW): `3C:DC:75:62:1F:3C` — usuário precisa hardcodar isso no peer ESP-NOW do firmware do TX.

---

## 2. Sintoma exato

O monitor serial do RX mostra os logs do ROM bootloader + 2nd stage bootloader + PSRAM init e **para em**:

```
I (356) esp_psram: Found 8MB PSRAM device
I (359) esp_psram: Speed: 80MHz
I (362) cpu_start: Multicore app
[SILÊNCIO TOTAL — sem panic, sem WDT, sem mais nada]
[após ~35s o chip reseta sozinho com rst:0xc (RTC_SW_CPU_RST)]
```

A próxima linha esperada (não aparece) seria `esp_psram: SPI SRAM memory test OK` e depois `cpu_start: Pro cpu start user code`. Ou seja, o chip morre entre `Multicore app` e o teste de PSRAM, **antes de o IDF spawning do main_task**, **antes de `app_init: Application information:`**, **antes do `app_main` ser chamado**.

`Saved PC: 0x40384590` no boot seguinte → addr2line → `esp_restart_noos` em `system_internal.c:165`. Ou seja, `esp_restart()` está sendo chamado de algum lugar. Como `app_main` nunca roda, o `esp_restart` deve estar vindo de:
- panic handler silencioso (antes do panic printer estar pronto)
- abort/assertion em alguma init do IDF
- WDT que não loga (mas WDT loga normalmente)

Observação importante: o monitor do usuário no pio device monitor vê EXATAMENTE o mesmo — não é problema de buffering de quem captura. O chip de fato não está emitindo bytes após `cpu_start: Multicore app`.

---

## 3. O que está **funcionando**

| Item | Como verificar |
|---|---|
| Build (PIO + IDF 6.0.1 + framework-espidf 6.0.1) | `pio run` → SUCCESS, 0 warnings |
| Host tests (todos os 6 suites) | 91/91 PASS |
| Flash | `pio run -t upload --upload-port /dev/ttyACM0` → SUCCESS |
| Chip vivo em download mode | `esptool read_mac` → `3c:dc:75:62:1f:3c` |
| ROM bootloader logs | até `Multicore app` aparecem normalmente |
| Octal PSRAM detectado | `Found 8MB PSRAM device` `Speed: 80MHz` |

---

## 4. O que JÁ FOI tentado e descartado

| Hipótese | Como testou | Resultado |
|---|---|---|
| Buffering Python serial | Testou com pio device monitor interativo do usuário (TTY real) | Usuário vê o mesmo silêncio — **NÃO é buffering** |
| BT controller em Core 0 starva console | `CONFIG_BT_CTRL_PINNED_TO_CORE_1=y` + `CONFIG_BT_NIMBLE_PINNED_TO_CORE_1=y` | Sem mudança |
| USB-Serial-JTAG blocking | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_LOG_NO_BLOCK=y` | Sem mudança |
| BT em si é o problema | Removeu CONFIG_BT_ENABLED inteiro (ble_pair.cpp gated com `#if CONFIG_BT_NIMBLE_ENABLED`) | **AINDA TRAVA** — BT NÃO era a causa |
| Reset cycle de USB | DTR/RTS toggle, esptool reset, hard reset | Indiferente |
| app_main reached? | Adicionou `printf(">>>>>> APP_MAIN ENTERED <<<<<<\n")` no início + vTaskDelay+fflush | Nunca aparece — chip morre antes do main_task |
| Re-flash com firmware "conhecido bom" smoke=y | Usuário relatou que smoke=y voltou a funcionar em pio monitor uma vez | Não conseguiu reproduzir confiavelmente |

---

## 5. Hipóteses ABERTAS para investigar

1. **PSRAM init falha silenciosamente entre `Multicore app` e `SPI SRAM memory test OK`**.
   - `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM=y` pode estar criando layout inválido sem Wi-Fi habilitado ainda.
   - `CONFIG_SPIRAM_SPEED_80M=y` no Octal pode estar instável (módulo "vermelho clone").
   - **Teste**: reduzir SPIRAM_SPEED para 40M; remover TRY_ALLOCATE_WIFI_IN_PSRAM.

2. **Versão do framework-espidf é 6.0.1 (pré-release/RC do IDF 6).**
   - O TX roda 5.5.3 (estável) e funciona. Bug específico do IDF 6.0.1 com Octal PSRAM 80MHz + 16MB flash QIO é plausível.
   - **Teste**: forçar PIO usar IDF 5.5 (`platform = espressif32 @ 6.13.0` no platformio.ini que usa IDF 5.4) e ver se boot completa.

3. **NVS partition corrompida pelos múltiplos flashes** (Sprint 4 fluxo cria/apaga namespace `pairing`).
   - Mas isso normalmente loga "no free pages" antes de erro.
   - **Teste**: `esptool erase_flash` → reflash → ver se boot completa.

4. **`CONFIG_COMPILER_OPTIMIZATION_PERF=y` + IDF 6 introduz bug**.
   - Otimização -O2 com GCC 15 pode estar gerando código quebrado em algum init.
   - **Teste**: trocar pra `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` ou `_O0`.

5. **`CONFIG_COMPILER_STACK_CHECK=y` em GCC 15 com IDF 6 está disparando canário falso**.
   - Audit S-02 adicionou isso. Se canário antes de stack ser válido → panic silencioso.
   - **Teste**: remover stack canaries temporariamente.

6. **Brownout (audit S-01) disparando**.
   - `CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y` é o mais sensível. Octal PSRAM @ 80MHz consome corrente alta no boot.
   - **Teste**: subir o threshold (mais permissivo) ou desabilitar BROWNOUT_DET.

---

## 6. Próximos passos sugeridos (em ordem de menor → maior esforço)

1. **`esptool erase_flash` + reflash** (5 min). Elimina hipótese 3.
2. **Desabilitar brownout** (`# CONFIG_ESP_BROWNOUT_DET` em sdkconfig.defaults) + reflash (10 min). Elimina 6.
3. **Desabilitar stack canaries** (remover linhas `CONFIG_COMPILER_STACK_CHECK*`) + reflash (10 min). Elimina 5.
4. **`CONFIG_SPIRAM_SPEED_40M=y` em vez de 80M** + reflash (10 min). Elimina parte de 1.
5. **Remover `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_IN_PSRAM=y`** + reflash (10 min). Elimina o resto de 1.
6. **Trocar `CONFIG_COMPILER_OPTIMIZATION_PERF=y` por DEBUG** + reflash (10 min). Elimina 4.
7. **Forçar PIO usar IDF 5.x** via `platform = espressif32 @ 6.13.0` no platformio.ini (30 min, vai reinstalar toolchain). Elimina 2.

Bisseção: aplicar uma de cada vez, flashar, ver se passa de `Multicore app`. A primeira que destravar é a causa.

---

## 7. Setup atual no repositório (estado a preservar)

- Branch: `main`, último commit: `8d4b683` ("build(s4): keep BT_NIMBLE + USB-JTAG non-blocking console")
- Diff não-commitado:
  - `sdkconfig.defaults`: BT desabilitado, smoke=n, `CONFIG_RECEIVER_PEER_MAC="98:A3:16:F5:2C:94"`, `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_LOG_NO_BLOCK=y`
  - `components/ble_pair/ble_pair.cpp`: gated com `#if CONFIG_BT_NIMBLE_ENABLED` (stubs quando BT desativado)
  - `components/ble_pair/CMakeLists.txt`: `bt` em REQUIRES só quando BT enabled
  - `main/app_main.cpp`: limpo (sem diagnóstico)
- MAC RX para hardcoding no TX: **`3C:DC:75:62:1F:3C`**
- MAC TX para hardcoding no RX (já feito via Kconfig): **`98:A3:16:F5:2C:94`**

---

## 8. O que NÃO mexer

- Sprint 4 software entregue: pair_nvs (13 tests), ble_pair state machine (25 tests), NimBLE wiring. **Não regrida** — o problema é orthogonal ao código de Sprint 4.
- Wire format fixes do `ef75431` (uint8 payload_len, calc_offset asymmetric 236/240). Bate com TX real.
- Sprint 3 link state machine + render overlays.
- Sprint 2 reassembly + decoder + display.
- Sprint 1 display bring-up.

---

## 9. Como continuar (script para próxima sessão)

```bash
cd /home/linux/dev/droner6/camera-display
git status                       # confirmar diff atual
ls /dev/ttyACM*                  # ACM0 = RX, ACM1 = TX
# Próximo passo: aplicar hipótese 1 (erase_flash)
python3 /home/linux/.platformio/packages/tool-esptoolpy/esptool.py \
    --port /dev/ttyACM0 --chip esp32s3 erase_flash
pio run -t upload --upload-port /dev/ttyACM0
# Pedir ao usuário que rode no terminal dele:
#   pio device monitor --port /dev/ttyACM0 --baud 115200
# Se ver "app_init: Application information" → bisseção achou
# Se não → próxima hipótese (brownout, depois stack check, depois PSRAM speed, etc)
```

Critério de sucesso: ver no log do RX no monitor do usuário:

```
I (xxx) main: Sprint 4 boot — free heap=...
W (xxx) main: using CONFIG_RECEIVER_PEER_MAC override (no BLE pairing)
I (xxx) espnow_link: esp-now ready, channel=6
I (xxx) display: lcd ok 320x240 @ 80000000 Hz
I (xxx) main: fps=N drops_timeout=0 ...
```

E vídeo no LCD do RX, se TX já estiver enviando frags para o MAC `3C:DC:75:62:1F:3C`.
