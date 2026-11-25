# Roadmap

## Fase 0 — Fundação ✅
- [x] Estrutura do repositório, `.gitignore` protegendo a ROM
- [x] Validação da ROM (`tools/rom_check.py`)
- [x] Build CMake/Ninja com MSVC e leitura do header em C++

## Fase 1 — Entender o código
- [ ] Decodificador ARMv4T (ARM + Thumb) com disassembler textual e testes
- [ ] Descoberta de funções a partir do entry point e dos alvos de `BL`
- [ ] Mapa de código × dados; relatório de cobertura
- [ ] Extrator do sistema de arquivos interno

## Fase 2 — Runtime + interpretador
- [ ] Barramento de memória (BIOS/EWRAM/IWRAM/IO/PAL/VRAM/OAM/ROM/EEPROM)
- [ ] Interpretador ARM7TDMI completo
- [ ] BIOS HLE (SWIs usadas pelo jogo)
- [ ] PPU (modos 0–2 e sprites primeiro), DMA, timers, IRQ
- [ ] Frontend SDL3: janela, input, áudio
- [ ] **Marco: jogo inicia e chega ao menu pelo interpretador**

## Fase 3 — Recompilação
- [ ] Emissor de C por função, despacho por tabela
- [ ] Testes diferenciais recompilado × interpretador
- [ ] Fallback e registro de saltos indiretos desconhecidos
- [ ] **Marco: jogo completo rodando 100% recompilado**

## Fase 4 — Melhorias de PC
- [ ] Resolução interna maior / widescreen (onde der)
- [ ] 60 fps estável, save states, remapeamento de controles
- [ ] Mods e substituição de assets
