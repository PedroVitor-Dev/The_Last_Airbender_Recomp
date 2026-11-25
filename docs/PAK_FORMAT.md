# Formato HBPak (dados do jogo)

Todos os assets do jogo ficam num único arquivo **HBPak** (Halfbrick) embutido na ROM.
Na ROM BQZE ele fica em `0x082323DC`, ocupa 5 170 020 bytes (~61% da ROM) e tem 1 555 arquivos.

Ferramenta: [`tools/pak.py`](../tools/pak.py) (`list`, `extract`, `build`, `verify`).

## Como o jogo encontra o pak

`func_08098B78` varre a ROM a partir de `0x08200000`, de 4 em 4 bytes, até achar os 12
primeiros bytes da assinatura. O endereço encontrado vai para `0x0200AA48` e `0x0200AA4C`.
Como o endereço não é fixo, um pak maior ou movido continua funcionando. No port de PC,
também dá para carregar o pak de um arquivo em disco.

A busca de arquivos (`func_08090510`) percorre a tabela comparando **nomes**
(`strncmp` com 120 caracteres), então o hash não é usado para localizar arquivos.

## Layout (little-endian)

### Cabeçalho (`header_size` = 0x80)

| Offset | Tipo      | Conteúdo |
|--------|-----------|----------|
| 0x00   | char[16]  | `"HBPakFileV10\r\n\x1a\n"` |
| 0x10   | u32       | tamanho total do pak (cabeçalho + tabela + dados) |
| 0x14   | u16       | `header_size` (0x80) |
| 0x16   | u16       | número de entradas (1555) |
| 0x18   | char[104] | caminho de build: `D:\Build\Avatar\data` |

### Entrada (0x84 bytes, logo após o cabeçalho)

| Offset | Tipo      | Conteúdo |
|--------|-----------|----------|
| 0x00   | char[120] | nome com `\` como separador, ex.: `actors\cameos\aang.set` |
| 0x78   | u32       | hash do nome (algoritmo desconhecido; não bate com CRC32/FNV/djb2/sdbm/ELF) |
| 0x7C   | u32       | tamanho |
| 0x80   | u32       | offset relativo ao início do pak |

### Dados

Os arquivos são contíguos, sem alinhamento e sem lacunas. A ordem dos dados **não** é a
ordem da tabela, então o `manifest.json` guarda as duas para remontar o pak byte a byte.

## Tipos de arquivo

| Ext.  | Qtde | Bytes     | Hipótese |
|-------|-----:|----------:|----------|
| `set` | 377  | 2 721 464 | conjuntos de tiles/gráficos (sprites e cenários) |
| `sp`  | 208  | 1 274 404 | definições de sprite / animação |
| `la`  | 217  |   247 772 | layers de fase |
| `str` |  49  |   216 852 | tabelas de texto |
| `ent` |  96  |   190 008 | entidades por sala |
| `ro`  |  96  |    97 904 | salas (rooms) |
| `idx` | 150  |    76 888 | índices de tilemap |
| `hbs` |  63  |    67 092 | scripts (Halfbrick script) |
| `tsp` | 150  |    31 900 | propriedades de tileset |
| `pal` |  45  |    18 900 | **paletas**: 4 bytes de cabeçalho + N × 16 cores BGR555 |
| `dts` |  96  |    15 120 | ? |
| `pat`, `txt`, `fnt`, `fx` | 8 | ~6 KB | padrões, tabelas (`damagetbl.txt` é binário), fontes, efeito |

Só `.pal` está decodificado por enquanto (o extrator gera prévias PNG em `previews/`).
Os outros formatos serão documentados conforme o runtime e o recompilador os exercitarem.

## Validação

`pak.py verify` extrai e remonta o pak em memória; o resultado é idêntico ao original.
`extract` + `build` em disco também reproduz exatamente os bytes da ROM.

> Os arquivos extraídos (`assets/extracted/`) e qualquer `.pak` ficam fora do Git.
