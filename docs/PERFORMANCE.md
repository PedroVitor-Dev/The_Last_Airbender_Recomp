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
| **Recompilado, PPU otimizada** | **~2 070** | **~35x** |
| Interpretador sem renderização | ~383 | ~6x |
| **Recompilado sem renderização** | **~12 900** | **~216x** |

Sem renderização, o código recompilado é **~34x mais rápido** que o interpretador. Jogando a
60 quadros/s, o jogo inteiro usa cerca de 3% de um núcleo.

## Para onde vai o tempo (`--profile`)

| Subsistema | Fatia |
|------------|------:|
| PPU (renderização) | ~80% (composição ~47%, BGs ~20%, sprites ~9%) |
| Sincronização (eventos de vídeo/timer, ~658 por quadro) | ~9% |
| Código recompilado e o resto | ~8% |
| APU | ~3% |

## O que foi feito

- **PPU** (`runtime/src/ppu.cpp`):
  - BGs de texto decodificados por tile, em vez de por pixel;
  - a composição percorre só as camadas ativas, já ordenadas por prioridade;
  - a segunda camada só é buscada quando o pixel realmente usa alpha (o jogo mantém o
    efeito de cor ligado em 100% das linhas);
  - tabelas para brilho e para a conversão BGR555 → ARGB;
  - as camadas não são mais limpas a cada linha.

  A saída é idêntica pixel a pixel: 70 quadros de referência conferidos por hash.

- **Tentativa descartada:** separar o laço da composição em versões com e sem janela
  (template) ficou ~5% mais lento com o MSVC e foi revertido.

## Próximos passos possíveis (por retorno esperado)

1. **Composição em blocos de 8 pixels / SIMD.** A composição ainda é ~47% do tempo total.
2. **Sprites por linha.** Listar uma vez por linha os sprites que a cruzam e decodificar por tile.
3. **Menos syncs.** Hoje os overflows do timer de som geram um sync cada (~220 por quadro). Dá
   para calcular as amostras do FIFO por aritmética na próxima sincronização de vídeo.
4. **Código recompilado** (flags preguiçosas, registradores em variáveis locais). Só ~8% do
   tempo: o ganho total máximo seria de poucos por cento, então fica por último.

## Como validar uma otimização

```powershell
# antes: capturar referências
.\build\release\tlagba.exe <rom> --headless --frames 7000 --shot-every 100 build\golden\f <roteiro>
# depois: capturar em build\check e comparar os hashes dos PNGs (precisam ser iguais)
```

A execução é determinística: duas rodadas iguais produzem os mesmos 70 quadros. Para mudanças
no código gerado ou no runtime da CPU, rode também os testes de [TESTING.md](TESTING.md).
