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
| **Recompilado, PPU 2ª rodada (SSE2, tiles transparentes, sprites)** | **~2 540** | **~42x** |
| Interpretador sem renderização | ~383 | ~6x |
| **Recompilado sem renderização** | **~12 900** | **~216x** |

Sem renderização, o código recompilado é **~34x mais rápido** que o interpretador. Jogando a
60 quadros/s, o jogo inteiro usa cerca de 2–3% de um núcleo.

## Para onde vai o tempo (`--profile`)

| Subsistema | Fatia |
|------------|------:|
| PPU (renderização) | ~75% (composição ~41%, BGs ~22%, sprites ~9%) |
| Sincronização (eventos de vídeo/timer, ~658 por quadro) | ~11% |
| Código recompilado e o resto | ~10% |
| APU | ~4% |

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

**Tentativas medidas e descartadas:**
- separar o laço da composição em versões com e sem janela (template): ~5% mais lento;
- "pintor" escalar, sem SIMD: ~20% mais lento que a busca por pixel, porque são vários passes
  e 4 arrays gravados por camada;
- decidir com SSE2 quais blocos de 8 pixels recebem efeito de cor: ~8% mais lento, porque
  neste jogo o efeito fica ligado em 100% das linhas e a maioria dos pixels o recebe.

**Validação:**
- 70 quadros de referência do jogo, idênticos por hash;
- `test_ppu`: composição e sprites comparados com a implementação original por pixel, em
  25 000 configurações aleatórias. Isso inclui janelas e sprites semitransparentes, que o
  roteiro do jogo não exercita. Uma mutação na ordem das camadas é detectada.

## Próximos passos possíveis (por retorno esperado)

1. **Passe final de efeitos com SIMD de verdade**, calculando alpha e brilho em paralelo e não
   só a decisão. A composição ainda é ~41%, e quase todo pixel recebe efeito.
2. **BGs de texto:** paleta pré-convertida por linha, ou cache de linhas de tile decodificadas.
3. **Menos syncs:** os overflows do timer de som geram um sync cada (~220 por quadro). As
   amostras do FIFO podem ser calculadas por aritmética na próxima sincronização de vídeo.
4. **Código recompilado** (flags preguiçosas, registradores em variáveis locais): ~10% do
   tempo, então o ganho total máximo é pequeno.

## Como validar uma otimização

```powershell
# antes: capturar referências
.\build\release\tlagba.exe <rom> --headless --frames 7000 --shot-every 100 build\golden\f <roteiro>
# depois: capturar em build\check e comparar os hashes dos PNGs (precisam ser iguais)
```

A execução é determinística: duas rodadas iguais produzem os mesmos 70 quadros. Mudanças na
composição ou nos sprites também precisam passar no `test_ppu`. Para mudanças
no código gerado ou no runtime da CPU, rode também os testes de [TESTING.md](TESTING.md).
