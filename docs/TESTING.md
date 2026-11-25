# Testes e verificação

O código recompilado precisa se comportar exatamente como o interpretador, que é a referência
de semântica do ARM7TDMI, e a PPU otimizada precisa produzir a mesma imagem que a versão por
pixel original. Cinco camadas de teste garantem isso, da mais barata à mais completa.

| Camada | Ferramenta | O que pega | Tempo |
|--------|-----------|------------|-------|
| Decodificador | `test_decoder` | codificação/desmontagem de instruções conhecidas | instantâneo |
| PPU | `test_ppu` | composição e sprites otimizados × implementação original por pixel (25 mil casos) | ~1 s |
| Instruções | `scripts\test_synth.ps1` | erros do emissor e dos helpers em cada forma de instrução, nos casos-limite | ~1–2 min (inclui build) |
| Funções do jogo | `tlaverify` | erros de análise e de tradução no código real (fluxo, chamadas, tabelas, overlays) | ~1 min |
| Os próprios testes | `scripts\mutation_test.ps1` | se as camadas acima ainda detectam bugs conhecidos | ~4 min |

## Decodificador

```powershell
.\build\release\test_decoder.exe
```

## Fuzzer de instruções (`test_synth`)

```powershell
.\scripts\test_synth.ps1 -Functions 8000 -Runs 30 -Seed 2
```

1. `tlarecomp synth` gera uma ROM sintética com N funções curtas: sequências aleatórias de
   instruções ARM e Thumb válidas, sem desvios nem escritas em PC, `sp` ou `lr`, terminadas em
   `bx lr`. Formas imprevisíveis no ARM7 (PC como `Rs`, PC em multiplicação) ficam de fora.
2. A ROM passa pelo mesmo `analyze --emit` do jogo e é compilada num executável à parte.
3. Cada função roda K vezes, interpretada e recompilada, com registradores escolhidos para
   atingir casos-limite: 0, 31, 32, 33, `0x80000000`, `0xFFFFFFFF` e ponteiros alinhados ou não
   para EWRAM e IWRAM. As memórias começam com conteúdo pseudoaleatório.

## Verificação do jogo (`tlaverify`)

```powershell
.\build\release\tlaverify.exe "roms\<sua-rom>.gba" --frames 7000 --per-function 3 --fuzz 5 --press START@900-905 ...
```

- **Replay:** o jogo roda no interpretador seguindo o roteiro de teclas. Nas primeiras K chamadas
  de cada função, o estado é copiado para duas instâncias que executam a função até ela retornar.
- **Fuzz (`--fuzz K`):** a partir do estado final do jogo, todas as funções são chamadas K vezes
  com registradores sorteados e um endereço de retorno mágico.
- `--only 0xADDR` isola uma função. O relatório completo fica em `build/verify_report.txt`.

Como ler os resultados:

| Veredito | Significado |
|----------|-------------|
| idênticas | registradores, flags, EWRAM, IWRAM, VRAM, paleta e OAM batem |
| **DIVERGENTES** | bug: falha o teste (código de saída 1) |
| inconclusivas (IO) | diferença ou timeout numa execução que acessou registradores de IO; os modos contam ciclos de forma diferente, então VCOUNT, timers e DMA podem variar |
| inconclusivas (tempo) | nem o interpretador retornou dentro do limite (laços com IRQ mascarada, `main`, entradas sem sentido) |
| inconclusivas (erro) | o próprio interpretador parou (ex.: instrução indefinida com entradas aleatórias) |
| não eram chamadas | a referência "voltou" com outra pilha: era um `bl` usado como salto |

## Teste de mutação

```powershell
.\scripts\test_synth.ps1          # gera a ROM sintética, se ainda não existir
.\scripts\mutation_test.ps1
```

O teste injeta, um de cada vez, bugs sutis em `runtime/include/gba/recomp_rt.h`
(`tools/mutate.py list`): `SBC` sem carry, carry do `LSR #32` por registrador e `LDRH`
desalinhado. Cada um precisa gerar divergências no `test_synth`. No fim o código é restaurado
e recompilado.

Isso garante que um "0 divergências" significa algo. Na primeira rodada, o carry do `LSR #32`
escapou do fuzz por função, e foi o que motivou o fuzzer de instruções.

## Resultados de referência (ROM BQZE)

- `tlaverify` (replay de 7000 quadros + fuzz ×5): 1472 + 17 156 execuções idênticas, cobrindo
  todas as 3963 funções, com 0 divergências.
- `test_synth` (8000 sequências × 30): 239 820 execuções idênticas, com 0 divergências.
- Mutações M1, M2 e M3: todas detectadas.
