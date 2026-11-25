# The Last Airbender Recomp

Recompilação estática de **Avatar: The Last Airbender** (Game Boy Advance, EUA, `BQZE`)
para PC. O código ARM/Thumb da ROM vira C nativo, que roda sobre uma reimplementação do
hardware do GBA.

> Este repositório **não contém a ROM nem assets do jogo**. Você precisa do seu próprio dump.

## Status

Fase 2: o runtime com interpretador já roda o jogo (menu e gameplay) com imagem e som. Veja [docs/ROADMAP.md](docs/ROADMAP.md) e [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Requisitos

- Windows 10/11 com Visual Studio 2022 Build Tools (C++, CMake, Ninja)
- Python 3.10+
- ROM `BQZE`, SHA-1 `9d864e9b3ccce4e5e1b1c566afa0d06088e88dfd`, em `roms/`

## Build

```powershell
.\scripts\build.ps1
.\build\release\test_decoder.exe
.\build\release\tlarecomp.exe info    "roms\<sua-rom>.gba"
.\build\release\tlarecomp.exe analyze "roms\<sua-rom>.gba" --disasm
```

A análise grava relatórios em `generated/analysis/` (fora do Git).

## Jogar (interpretador)

```powershell
.\build\release\tlagba.exe "roms\<sua-rom>.gba"
```

Teclado: setas, **X** = A, **Z** = B, **Enter** = Start, **Backspace** = Select, **A**/**S** = L/R,
**Esc** = sair. O save (EEPROM) fica em `roms\<sua-rom>.sav`. O som sai pelo XAudio2
(`--mute` desliga); `--wav arquivo.wav` grava o áudio no modo headless.

Modo de teste sem janela (capturas PNG e roteiro de botões):

```powershell
.\build\release\tlagba.exe "roms\<sua-rom>.gba" --headless --frames 2400 --shot-every 200 build\shots\run --press START@900-905
```

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
