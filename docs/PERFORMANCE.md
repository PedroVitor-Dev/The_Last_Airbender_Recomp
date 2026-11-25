# Desempenho

Todas as medições vêm do mesmo benchmark: `scripts\bench.ps1`, com 7000 quadros headless
cobrindo abertura, menus, diálogos e gameplay, num único núcleo. O GBA roda a 59,73 quadros por
segundo; "35x" quer dizer que um quadro custa 1/35 do tempo disponível.

```powershell
.\scripts\bench.ps1                         # recompilado, com perfil por subsistema
.\scripts\bench.ps1 -Interp                 # só interpretador
.\build\release\tlagba.exe <rom> --headless --frames 7000 --no-render ...   # só CPU/timers/som
```

## Resultados (ROM BQZE)

| Configuração | Quadros/s | × tempo real |
|--------------|----------:|-------------:|
| Interpretador | ~310 | ~5x |
| Recompilado, PPU original | ~976 | ~16x |
| Recompilado, PPU 1ª rodada (por tile, camadas ordenadas, tabelas) | ~2 070 | ~35x |
| Recompilado, PPU 2ª rodada (composição SSE2, tiles transparentes, sprites) | ~2 540 | ~42x |
| Recompilado, PPU 3ª rodada (efeitos de cor em SSE2) | ~3 070 | ~51x |
| **Recompilado, PPU 4ª rodada (BGs de texto com paleta pré-convertida)** | **~3 350** | **~56x** |
| Interpretador sem renderização | ~383 | ~6x |
| **Recompilado sem renderização** | **~12 900** | **~216x** |

Sem renderização, o código recompilado é **~34x mais rápido** que o interpretador. Jogando a
60 quadros/s, o jogo inteiro usa cerca de 2% de um núcleo.

## Para onde vai o tempo (`--profile`)

| Subsistema | Fatia |
|------------|------:|
| PPU (renderização) | ~64% (composição ~33%, BGs ~21%, sprites ~11%) |
| Sincronização (eventos de vídeo/timer, ~658 por quadro) | ~14% |
| Código recompilado e o resto | ~14% |
| APU | ~5% |

## O que foi feito

**1ª rodada** (`runtime/src/ppu.cpp`):
- BGs de texto decodificados por tile, em vez de por pixel;
- a composição percorre só as camadas ativas, ordenadas por prioridade;
- tabelas para brilho e para a conversão BGR555 → ARGB;
- as camadas não são mais limpas a cada linha.

**2ª rodada:**
- **Composição por camadas com SSE2 ("pintor").** Cada camada é aplicada sobre a linha
  inteira, de trás para frente: onde o pixel é opaco (e a janela libera), o topo atual desce
  para "segunda camada". A seleção é feita por máscara (`and`/`andnot`/`or`), 8 pixels por
  instrução. Um passe escalar final aplica os efeitos de cor.
- **Tiles transparentes:** uma linha de tile toda transparente é preenchida de uma vez.
- **Sprites não-afins:** a faixa visível é recortada uma vez e a base da linha de tiles é
  calculada uma vez por sprite.

**3ª rodada:**
- **Efeitos de cor em SSE2.** Alpha e brilho são calculados por canal para 8 pixels de uma vez
  (`mullo`/`srli`/`min`). A escolha entre topo, alpha e brilho é feita por máscaras que
  reproduzem a cadeia de decisão da versão escalar (sprite semitransparente, janela, 1º e 2º
  alvos, modo), sem desvios. A versão escalar fica como fallback fora do x86-64.
  Resultado: ~2 540 → ~3 070 fps. Uma mutação (EVA trocado por EVB) é detectada pelo
  `test_ppu` em 4 558 de 20 000 casos.

**4ª rodada — BGs de texto:**
- **Paleta pré-convertida:** duas tabelas (4 bpp e 8 bpp) já com o bit de opaco e com 0 nos
  índices transparentes. Cada pixel vira uma leitura de tabela, sem desvio. A PPU compara a
  paleta de BG com uma cópia em cache uma vez por linha (512 bytes) e reconstrói as tabelas
  só quando ela muda, o que cobre qualquer caminho de escrita (CPU, DMA, BIOS, cópia de estado).
- **Flip horizontal por tile:** a ordem dos nibbles é invertida uma vez, em vez de testar a
  cada pixel.
- **Tiles inteiros desenrolados** (8 escritas diretas).

  Resultado: ~3 070 → ~3 350 fps, com os BGs caindo de ~28% para ~21%. O `test_ppu` ganhou
  20 000 casos de BG de texto contra a primeira implementação por pixel. Mutações no flip e
  na cache de paleta são detectadas.

**5ª rodada — sprites:**
- **Cache da OAM:** cada linha percorria os 128 registros da OAM. Agora a PPU mantém uma lista
  compacta dos sprites ativos com a faixa vertical de cada um, reconstruída quando a OAM muda
  (comparação de 1 KB com uma cópia em cache, uma vez por linha). O desenho de cada sprite
  continua idêntico. Resultado em A/B intercalado (5 rodadas): mediana +4%. Uma mutação que
  faz a cache ignorar mudanças é detectada pelo `test_ppu` (4 805 de 5 000 casos).

**Timers e IRQs** (correções encontradas ao tentar reduzir syncs):
- Escrever no controle ou na recarga de um timer agora reagenda o próximo evento. Antes, o
  novo estouro só entrava na agenda no próximo evento qualquer.
- Limpar o bit I do CPSR (`msr`, retorno de handler) agora pede sincronização: uma IRQ
  pendente é atendida no bloco seguinte, como no hardware. Antes, ela esperava o próximo
  evento de vídeo ou timer (no caso encontrado, ~52 mil ciclos depois de o timer 2 estourar,
  com o atendimento variando conforme outros eventos).
- Cada amostra do FIFO entra no instante exato do estouro do timer, calculado por aritmética,
  e não no ciclo da sincronização (que pode estar um bloco à frente).

  Isso muda o momento de algumas IRQs, então o estado do jogo diverge do anterior (aparece na
  posição dos flocos de neve, que vêm do gerador aleatório) e as referências foram regeradas.
  Todos os testes de CPU e de PPU passam. No A/B ficou ~2,7% mais rápido.

**Tentativas medidas e descartadas:**
- separar o laço da composição em versões com e sem janela (template): ~5% mais lento;
- **conversão BGR555 → ARGB com SSE2** (separar canais, expandir 5 → 8 bits, intercalar):
  correta (`test_ppu` e 70 quadros idênticos), mas ~2% mais lenta no A/B de 9 rodadas
  (mediana 3 511 contra 3 583 fps). A tabela de 32 K entradas (128 KB) fica no cache L2, e
  uma leitura por pixel sai mais barata que as operações vetoriais;
- **timers "preguiçosos"**: estouros que só alimentam o FIFO deixam de ser eventos (658 → 456
  syncs por quadro), com saída idêntica (70 quadros, WAV byte a byte e 4 118 IRQs iguais à
  estratégia precisa, comparadas no mesmo binário). Mas no A/B sem `--profile` o ganho foi
  zero: uma sync custa pouco, e o ganho que o perfil mostrava vinha do custo do próprio perfil
  (ele cronometra cada sync). Foi removido para não manter complexidade sem retorno. As
  correções de IRQ acima saíram dessa investigação;
- paleta de OBJ pré-convertida + sprites decodificados por tile: correta (passou no
  `test_ppu` depois de corrigir a volta de endereço em 8 bpp perto do tile 1023), mas neutra
  a ~1,5% mais lenta no A/B. O custo dos sprites estava na varredura da OAM por linha, não
  nos pixels, e foi isso que a cache resolveu;
- "pintor" escalar, sem SIMD: ~20% mais lento que a busca por pixel, porque são vários passes
  e 4 arrays gravados por camada;
- decidir com SSE2 quais blocos de 8 pixels recebem efeito de cor, mantendo o cálculo
  escalar: ~8% mais lento, porque neste jogo a maioria dos pixels recebe o efeito. A solução
  foi vetorizar o cálculo em si (3ª rodada).

**Validação:**
- 70 quadros de referência do jogo, idênticos por hash;
- `test_ppu`: composição e sprites comparados com a implementação original por pixel, em
  25 000 configurações aleatórias. Isso inclui janelas e sprites semitransparentes, que o
  roteiro do jogo não exercita. Uma mutação na ordem das camadas é detectada.

## Próximos passos possíveis (por retorno esperado)

Com o jogo a ~60x o tempo real (menos de 2% de um núcleo a 60 fps), as últimas tentativas
deram ganho nulo ou negativo. O desempenho deixou de ser prioridade.

1. **Código recompilado** (flags preguiçosas, registradores em variáveis locais): ~10% do
   tempo, então o ganho total máximo é pequeno.

## Como validar uma otimização

```powershell
# antes: capturar referências
.\build\release\tlagba.exe <rom> --headless --frames 7000 --shot-every 100 build\golden\f <roteiro>
# depois: capturar em build\check e comparar os hashes dos PNGs (precisam ser iguais)
```

A execução é determinística: duas rodadas iguais produzem os mesmos 70 quadros.
**Meça ganhos com A/B intercalado e sem `--profile`**: o perfil cronometra cada sync e cada
linha, e isso distorce justamente as partes que se tenta otimizar. Mudanças na
composição ou nos sprites também precisam passar no `test_ppu`. Para mudanças
no código gerado ou no runtime da CPU, rode também os testes de [TESTING.md](TESTING.md).

## Lag do jogo (quadros de lógica × quadros de vídeo)

O jogo não usa HALT: espera o VBlank num laço. Para saber se ele está perdendo quadros, compare o
contador de quadros de lógica dele (EWRAM `0x0200_30F0`, avança uma vez por volta do laço
principal) com os quadros de vídeo: rode até o quadro N e N+160 com `--dump-ram` e subtraia.

Com o save state de teste (andando na neve), o código recompilado perdia **17 de 160 quadros**
(10,6%) e o interpretador nenhum: o emissor cobrava a busca de instruções sempre como ROM
(3/6 ciclos), inclusive no código dos overlays na IWRAM (1 ciclo), como o mixer de som. Com o
custo por região — igual ao de `Bus::access_cycles` — e os acessos à pilha (SP) e ao pool de
literais (PC) na região certa, o recompilado passou a 160 de 160. **O custo do código gerado
precisa acompanhar o do interpretador**: diferença ali vira lag (ou jogo rápido demais).
