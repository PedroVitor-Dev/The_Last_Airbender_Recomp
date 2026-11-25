# Roadmap

## Fase 0 — Fundação ✅
- [x] Estrutura do repositório, `.gitignore` protegendo a ROM
- [x] Validação da ROM (`tools/rom_check.py`)
- [x] Build CMake/Ninja com MSVC e leitura do header em C++

## Fase 1 — Entender o código
- [x] Decodificador ARMv4T (ARM + Thumb) com disassembler textual e testes
- [x] Descoberta de funções (entry, `BL`, ponteiros, tabelas de switch, config manual)
- [x] Mapa de código × dados; relatório de cobertura (3 640 funções, 1 salto indireto restante)
- [ ] Detectar funções folha alcançáveis só por ponteiro (sem `push {lr}`)
- [x] Extrator do sistema de arquivos interno (HBPak, ida e volta byte a byte — `tools/pak.py`)
- [ ] Decodificar formatos: `.set`/`.sp` (gráficos), `.la`/`.ro`/`.idx` (fases), `.str` (texto)

## Fase 2 — Runtime + interpretador
- [x] Barramento de memória (BIOS/EWRAM/IWRAM/IO/PAL/VRAM/OAM/ROM/EEPROM)
- [x] Interpretador ARM7TDMI completo
- [x] BIOS HLE (SWIs usadas pelo jogo + stub próprio do vetor de IRQ)
- [x] PPU (modos 0–5, sprites normais/afins, janelas, blending), DMA, timers, IRQ
- [x] Frontend Win32 (janela + teclado) e modo headless com capturas PNG
- [x] **Marco: jogo inicia, chega ao menu e ao gameplay pelo interpretador** (~5x tempo real)
- [ ] Áudio: mixer dos FIFOs A/B + canais PSG, saída pelo Windows (WASAPI/XAudio2)
- [ ] Mosaico, timing fino de DMA/waitstates, IntrWait/VBlankIntrWait (não usados pelo jogo)
- [ ] Frontend multiplataforma (SDL3) e controle

## Fase 3 — Recompilação
- [ ] Emissor de C por função, despacho por tabela
- [ ] Testes diferenciais recompilado × interpretador
- [ ] Fallback e registro de saltos indiretos desconhecidos
- [ ] **Marco: jogo completo rodando 100% recompilado**

## Fase 4 — Melhorias de PC
- [ ] Resolução interna maior / widescreen (onde der)
- [ ] 60 fps estável, save states, remapeamento de controles
- [ ] Mods e substituição de assets
