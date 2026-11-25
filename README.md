# The Last Airbender Recomp

Recompilação estática de **Avatar: The Last Airbender** (Game Boy Advance, EUA, `BQZE`)
para PC. O código ARM/Thumb da ROM vira C++ nativo, que roda sobre uma reimplementação do
hardware do GBA.

> Este repositório **não contém a ROM nem assets do jogo**. Você precisa do seu próprio dump.

## Status

Fase 3: o jogo roda **recompilado**, com imagem e som, a ~16x a velocidade do GBA quando sem
limite. Só 2 instruções passam pelo interpretador em 7000 quadros, e o teste diferencial dá
0 divergências. Veja [docs/ROADMAP.md](docs/ROADMAP.md) e [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Requisitos

- Windows 10/11 com Visual Studio 2022 Build Tools (C++, CMake, Ninja)
- Python 3.10+
- ROM `BQZE`, SHA-1 `9d864e9b3ccce4e5e1b1c566afa0d06088e88dfd`, em `roms/`

## Build

```powershell
.\scripts\build.ps1                                                # 1) ferramentas e runtime
.\build\release\tlarecomp.exe emit "roms\<sua-rom>.gba"            # 2) gera generated\recomp\
.\scripts\build.ps1                                                # 3) compila o código gerado (~1 min)
```

Sem o passo 2, o `tlagba` funciona só com o interpretador (cerca de 3x mais lento). O código
gerado e os relatórios (`generated/`) ficam fora do Git. Para compilar com o jogo aberto, use
outra pasta: `.\scripts\build.ps1 -BuildDir build\dev`.

## Jogar

```powershell
.\build\release\tlagba.exe "roms\<sua-rom>.gba"
```

Teclado: setas, **X** = A, **Z** = B, **Enter** = Start, **Backspace** = Select, **A**/**S** = L/R,
**Esc** = sair. O save (EEPROM) fica em `roms\<sua-rom>.sav`. O som sai pelo XAudio2.

| Opção | Efeito |
|-------|--------|
| `--interp` | ignora o código recompilado |
| `--mute` | sem áudio (ritmo pelo relógio) |
| `--headless --frames N` | sem janela, para testes |
| `--shot f.png` / `--shot-every K prefixo` | capturas PNG |
| `--press TECLA@ini-fim` | roteiro de botões |
| `--wav f.wav` | grava o áudio |
| `--fallback-log f.txt` | endereços executados pelo interpretador |

## Ferramentas de desenvolvimento

```powershell
.\build\release\test_decoder.exe                                    # testes do decodificador
.\build\release\tlarecomp.exe analyze "roms\<sua-rom>.gba" --disasm # relatórios + disassembly
.\build\release\tlarecomp.exe dis "roms\<sua-rom>.gba" 0x08097C30 56 arm --at 0x03000000
.\build\release\tlaverify.exe "roms\<sua-rom>.gba" --frames 3000 --press START@900-905
```

O `tlaverify` compara cada função recompilada com o interpretador, a partir de estados reais
do jogo.

## Assets

Os dados do jogo ficam num pak HBPak dentro da ROM ([formato](docs/PAK_FORMAT.md)):

```powershell
python tools\pak.py verify  "roms\<sua-rom>.gba"   # confere a ida e volta byte a byte
python tools\pak.py extract "roms\<sua-rom>.gba"   # -> assets\extracted\ (fora do Git)
python tools\pak.py build   assets\extracted build\data.pak
```

## Licença

GPL-3.0 (veja [LICENSE](LICENSE)). *Avatar: The Last Airbender* é marca de seus respectivos donos;
este projeto não tem afiliação com eles.
