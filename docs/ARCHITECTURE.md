# Arquitetura do recomp

Objetivo: transformar o código ARM7TDMI de *Avatar: The Last Airbender* (GBA, BQZE) em
código nativo para PC. O executável roda sobre uma reimplementação do hardware do GBA,
sem emular a CPU instrução por instrução no caminho principal.

```
            ┌──────────────┐  análise + tradução  ┌────────────────────┐
 ROM (.gba) │  tlarecomp   │ ───────────────────▶ │ generated/recomp/  │
 ─────────▶ │  (C++20)     │                      │ 1 função C++ por   │
            └──────────────┘                      │ função ARM/Thumb   │
                   ▲ config/functions.txt          └─────────┬──────────┘
                                                             │ compila junto
            ┌────────────────────────────────────────────────▼─────────┐
            │ runtime (C++20)                                          │
            │  CPU · memória · PPU · APU · DMA · timers · IRQ · EEPROM │
            │  BIOS HLE · agendador por eventos · interpretador        │
            └───────────────────────────┬──────────────────────────────┘
                                        │
                  tlagba (janela Win32 + XAudio2 / headless)  ·  tlaverify
```

## Módulos

| Pasta         | Papel |
|---------------|-------|
| `core/`       | Leitura da ROM e decodificador ARMv4T. ARM e Thumb viram a mesma IR. Usado pelo recompilador e pelo runtime. |
| `recompiler/` | `tlarecomp`: análise de funções (`analysis.cpp`), emissor de C++ (`emitter.cpp`) e o desmontador `dis`. |
| `runtime/`    | Hardware do GBA, interpretador ARM7TDMI (`cpu.cpp`), agendador e despacho híbrido (`gba.cpp`), e as funções auxiliares do código gerado (`recomp_rt.h`). |
| `frontend/`   | `tlagba` (o jogo) e `tlaverify` (teste diferencial). |
| `config/`     | Conhecimento por jogo: tabelas, overlays, entradas extras, falsos positivos. |
| `tools/`      | Python: validação da ROM e extrator/montador do pak de assets. |

## Análise de funções

A análise faz uma travessia recursiva a partir de várias fontes:
- o entry point e os alvos de `BL`;
- as entradas de `config/functions.txt`;
- ponteiros Thumb (bit 0 = 1) para um `push {..., lr}`, tanto em literais quanto varrendo a ROM inteira.

Uma segunda passada aceita ponteiros para **funções folha**, desde que o alvo decodifique até
um retorno, fique na região de código e não esteja no meio de outra função.

Dentro de cada bloco há propagação simples de constantes e de "marcas" por registrador. Com
elas a análise reconhece:
- retornos `pop {rN}; bx rN` e `mov pc, lr`;
- tabelas de `switch`, inclusive com o `lsl #2` antes do `cmp`, e `ldr pc, [pc, rX, lsl #2]` em ARM;
- chamadas `mov lr, pc; bx rN` e veneers `_call_via_rN`;
- **`bl` usado como salto longo**: o `b` do Thumb só alcança ±2 KB, então o GCC usa `bl` para
  ir a epílogos compartilhados e para os laços de funções grandes. Esses saltos viram `goto`, e
  as pseudo-funções que eles criariam são removidas por uma poda até o ponto fixo;
- o fim de bloco em pools de literais e no padding `0x0000` depois de chamadas que não retornam.

`config/functions.txt` aceita as diretivas `func`, `table`, `skip`, `overlay <ram> <rom> <tam>`
(código da ROM copiado para a RAM) e `entry <função> <endereço>` (entradas extras de código
encadeado, como o mixer de som).

## Modelo de execução do código recompilado

- **Uma função C++ por função do jogo:** `void f_08061744(gba::Gba&)`. Os registradores e as
  flags ficam no estado da CPU (`g.cpu`), não em variáveis locais. Assim uma IRQ pode ser
  atendida em qualquer ponto de sincronização sem salvar nada.
- **Tempo:** cada bloco básico soma seus ciclos estimados em `g.cycles` no rótulo de entrada.
  Se `g.cycles >= g.next_event`, chama `g.sync_at(pc)`, que processa vídeo e timers, atende
  HALT e IRQs e devolve cada quadro pronto ao frontend por callback.
- **Chamadas:** um `BL` vira chamada direta (`f_X(g)`). Ao voltar, o código confere se
  `r15` é o endereço de retorno esperado. Se não for, a função retorna também, propagando,
  até chegar ao quadro que reconhece o endereço. É a semântica de desempilhar a pilha.
- **Saltos indiretos:** uma tabela de `switch` vira `switch`/`goto`. Os demais usam
  `g.jump_indirect`: despacho por endereço numa tabela, ou o interpretador se o endereço não
  tiver código recompilado.
- **Literais da ROM** viram constantes. Os acessos a EWRAM e IWRAM têm caminho rápido inline.
- **IRQ:** `service_irq()` faz em C++ o que o stub do vetor de IRQ faz, e chama o handler do
  jogo (recompilado, inclusive a cópia dele na IWRAM).
- **Modo híbrido:** o interpretador chama código recompilado sempre que chega a uma entrada
  conhecida, e o código recompilado cai no interpretador para endereços desconhecidos. Na
  prática o jogo roda com 2 instruções interpretadas em 7000 quadros.

## Verificação

O interpretador é o oráculo, e a comparação é sempre recompilado × interpretador a partir do
mesmo estado. São três frentes:
- **replay** de chamadas reais do jogo;
- **fuzz** de todas as funções com registradores sorteados;
- um **fuzzer de instruções** sobre uma ROM sintética, que testa o emissor instrução a instrução.

Um teste de mutação confirma que essas frentes detectam bugs conhecidos. Detalhes em
[TESTING.md](TESTING.md).

## Decisões

1. **C++20 + CMake + Ninja (MSVC).** O código gerado tem ~270 mil instruções em ~35 arquivos e
   compila em cerca de 1 minuto com `/O2`.
2. **Interpretador primeiro.** Ele fez o jogo rodar cedo e hoje é o oráculo do teste
   diferencial e o fallback de segurança.
3. **Estado da CPU em memória, não em variáveis locais.** É mais simples e permite IRQ em
   qualquer bloco. Otimizações como flags preguiçosas e registradores em locais ficam para depois.
4. **Timing por orçamento de ciclos por bloco**, sincronizando só quando vence um evento.
5. **BIOS em HLE.** Nenhum BIOS proprietário é necessário.
6. **A ROM nunca entra no repositório**, nem nada derivado dela (`generated/`,
   `assets/extracted/`).

## Dados conhecidos da ROM

- Entrada ARM em `0x080000C0` (crt0): configura as pilhas, grava o handler de IRQ e chama o
  `main` Thumb em `0x08061744`.
- Handler de IRQ ARM em `0x0800012C`, que o jogo depois copia para a IWRAM em `0x03004BE8`. Ele
  despacha pelo bit de `IF` usando a tabela de 13 ponteiros Thumb em `0x080A4B20`.
- **Mixer de som** em ARM na IWRAM (`0x03000000`, copiado de `0x08097C30`), escrito como código
  encadeado: cada canal guarda o ponteiro da sua rotina, e elas saltam umas para as outras
  com `bx r8`.
- O código é quase todo Thumb, compilado com GCC. A análise acha ~3 960 funções.
- Salvamento: `EEPROM_V124`.
- Os assets ficam num pak **HBPak** da Halfbrick em `0x082323DC` (1 555 arquivos, ~5 MB). Ver
  [PAK_FORMAT.md](PAK_FORMAT.md).
