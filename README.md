# The Last Airbender Recomp

Recompilação estática de **Avatar: The Last Airbender** (Game Boy Advance, EUA, `BQZE`)
para PC. O código ARM/Thumb da ROM vira C nativo, que roda sobre uma reimplementação do
hardware do GBA.

> Este repositório **não contém a ROM nem assets do jogo**. Você precisa do seu próprio dump.

## Status

Fase 1: análise do código (decodificador ARMv4T e descoberta de funções). Veja [docs/ROADMAP.md](docs/ROADMAP.md) e [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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

## Licença

GPL-3.0 (veja [LICENSE](LICENSE)). *Avatar: The Last Airbender* é marca de seus respectivos donos;
este projeto não tem afiliação com eles.
