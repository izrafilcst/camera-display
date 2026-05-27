# Plans — Receptor V0

Implementation plans derivados do spec em [`../specs/2026-05-26-receptor-design.md`](../specs/2026-05-26-receptor-design.md).

O V0 é entregue em **7 sprints**. Cada sprint é independentemente testável e produz um artefato verificável. Total estimado: ~10–14 dias de trabalho focado.

## Sprints

| # | Arquivo | Objetivo | Duração estimada |
|---|---|---|---|
| 1 | [sprint-1-bringup-display](2026-05-26-sprint-1-bringup-display.md) | Bring-up ESP-IDF + ILI9341 + DMA + padrões de teste | 1–2 dias |
| 2 | [sprint-2-video-pipeline](2026-05-26-sprint-2-video-pipeline.md) | ESP-NOW RX + reassembly + esp_jpeg + double buffer | 2–3 dias |
| 3 | [sprint-3-link-handling](2026-05-26-sprint-3-link-handling.md) | Estados CONNECTED / FREEZE / DISCONNECTED | 1 dia |
| 4 | [sprint-4-control-pipeline](2026-05-26-sprint-4-control-pipeline.md) | Joystick + 2 botões + telemetry + adaptive | 2 dias |
| 5 | [sprint-5-hud-camada-a](2026-05-26-sprint-5-hud-camada-a.md) | HUD sempre-visível (link + bateria robô) | 1 dia |
| 6 | [sprint-6-hud-camada-b-menu](2026-05-26-sprint-6-hud-camada-b-menu.md) | Itens configuráveis + menu + NVS | 2–3 dias |
| 7 | [sprint-7-hardening](2026-05-26-sprint-7-hardening.md) | Soak + critérios + decisão anti-tearing fase 2 | 1–2 dias |

## Dependências

```
Sprint 1 ──► Sprint 2 ──► Sprint 3
                │
                ├──► Sprint 4 ──► Sprint 6
                │                  ▲
                └──► Sprint 5 ─────┘
                                   │
                                   ▼
                              Sprint 7
```

- Sprint 2 depende de Sprint 1 (display funcionando)
- Sprints 3, 4, 5 podem rodar em paralelo após Sprint 2 (em pessoas diferentes)
- Sprint 6 depende de Sprint 4 (input) e Sprint 5 (hud base)
- Sprint 7 fecha tudo (hardening + validação)

## Como executar

Cada plan file tem no topo:
> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

Recomenda-se invocar `superpowers:executing-plans` ou `superpowers:subagent-driven-development` ao começar cada sprint.

## Convenções

- **Branch por sprint**: `sprint-1-display`, `sprint-2-video`, ...
- **Commits convencionais**: `feat:`, `fix:`, `test:`, `docs:`, `perf:`, `chore:`, `obs:`
- **Tags ao fim de cada sprint**: `s1-done`, `s2-done`, ... e `v0.1.0` no fim
- **Testes Unity host-side** quando viável (reassembly, telemetry, link_state)
- **Validação manual** documentada em cada task de critério de aceitação

## Critérios de sucesso V0 (do spec §10)

A validação final ocorre no Sprint 7. Pontos esperados:

- [ ] 24 fps renderizados sem stutter perceptível por ≥ 10 min
- [ ] Latência display p99 < 100 ms
- [ ] Drop rate < 1% em canal limpo
- [ ] fps_min ≥ 20
- [ ] Reconexão automática < 2 s
- [ ] Soak térmico 30 min: temp média < 80 °C
- [ ] Sem tearing severo em movimento normal
- [ ] Menu funcional + settings persistem após reboot

## Fora do V0 (Plano 2+)

- Pairing BLE inicial + encriptação ESP-NOW (PMK/LMK)
- Hardware real de GPS / IMU / compass
- Modos de degradação L3/L4 automáticos
- Solda do TE pad do ILI9341 para vsync hardware
- Bateria do RX com hardware real
- Funcionalidade do touchscreen e SD card
