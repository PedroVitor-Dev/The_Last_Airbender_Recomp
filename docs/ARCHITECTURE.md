# Arquitetura do recomp

Objetivo: transformar o código ARM7TDMI de *Avatar: The Last Airbender* (GBA, BQZE) em
código C nativo para PC. O executável sobe com uma reimplementação do hardware do GBA,
sem emular a CPU instrução por instrução no caminho principal.

```
            ┌──────────────┐  análise + tradução  ┌──────────────────┐
 ROM (.gba) │  recompiler  │ ───────────────────▶ │ generated/*.c    │
 ─────────▶ │  (C++20)     │                      │ (1 função C por  │
            └──────────────┘                      │  função ARM/Thumb)│
                                                  └────────┬─────────┘
                                                           │ compila junto
            ┌──────────────────────────────────────────────▼─────────┐
            │ runtime (C++20)                                        │
            │  cpu state · memória · PPU · APU · DMA · timers · IRQ  │
            │  BIOS HLE (SWI) · EEPROM · input · interpretador       │
            │  frontend SDL3 (janela, áudio, controle)               │
            └────────────────────────────────────────────────────────┘
```

## Módulos

| Pasta         | Papel |
|---------------|-------|
| `core/`       | Código comum: leitura da ROM, header, tipos, decodificador ARMv4T (ARM + Thumb). Usado pelo recompilador e pelo runtime. |
| `recompiler/` | Ferramenta offline. Descobre funções (entry point, alvos de `BL`, tabelas de ponteiros, *overrides* manuais em `config/`), monta o CFG e emite C. |
| `runtime/`    | Biblioteca que o código gerado chama: estado da CPU, barramento de memória, periféricos e BIOS. |
| `interp/`     | Interpretador ARMv4T de referência (dentro do runtime). Serve como *fallback* para código não recompilado (saltos indiretos desconhecidos, código copiado para IWRAM) e como oráculo de testes diferenciais. |
| `tools/`      | Scripts Python: validação da ROM, extração do sistema de arquivos interno, etc. |
| `config/`     | Dados por jogo: funções extras, tabelas de salto, *patches* e *hooks*. |

## Decisões

1. **C++20 + CMake + Ninja (MSVC no Windows).** O ambiente já tem as Build Tools 2022; o
   código gerado é C simples, que compila rápido mesmo com milhares de funções.
2. **Interpretador primeiro.** Ele faz o jogo rodar cedo, valida o runtime (PPU/APU/IRQ)
   e serve de referência para provar que cada função recompilada é equivalente.
3. **Granularidade por função**, com despacho por tabela `endereço → função C`. Saltos
   indiretos consultam a tabela; se a entrada não existir, caem no interpretador e são
   registrados para entrarem na próxima recompilação.
4. **Estado da CPU explícito** (`r0..r15`, CPSR/SPSR, bancos de modo) passado por ponteiro.
   Flags calculadas de forma preguiçosa só onde a análise mostrar que são lidas.
5. **Timing por orçamento de ciclos**: cada bloco soma seus ciclos aproximados; o runtime
   avança PPU/timers/DMA nos pontos de sincronização (fim de bloco, acessos a I/O, `SWI`).
6. **BIOS em HLE.** Reimplementação das chamadas `SWI` usadas pelo jogo; nenhum BIOS
   proprietário é necessário.
7. **A ROM nunca entra no repositório.** O usuário fornece o próprio dump; o build confere o
   SHA-1 antes de gerar código. `generated/` também fica fora do Git.

## Dados conhecidos da ROM

- Entrada ARM em `0x080000C0`.
- Salvamento: `EEPROM_V124` (biblioteca padrão da Nintendo).
- Contém um sistema de arquivos com nomes legíveis (`layers\fnp_buildings.idx`, `.set`, ...)
  em torno de `0x0823F000` — útil para extrair e, no futuro, substituir assets.
- ~89,6% da ROM está em uso (`0x72AB2F` bytes até o último byte não vazio).
