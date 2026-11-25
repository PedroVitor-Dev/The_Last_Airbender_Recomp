# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/).

## [0.0.1] — 2026-11-25

First public release. The whole game runs natively on PC through static recompilation.

### Added
- `AvatarRecomp.exe`: no console window, finds the ROM next to the executable or in `roms\`,
  accepts a ROM dragged onto it, or asks for it once and remembers it.
- The game's code recompiled to C++ (3,640 functions discovered), with an interpreter fallback
  for anything not covered.
- Video (all GBA modes, sprites, windows, blending) and sound (FIFOs + PSG at 48 kHz, XAudio2).
- In-game settings screen styled after the game (Esc, F1, Xbox/home button or Back+Start),
  with the game frozen while it is open.
- Keyboard and XInput gamepad remapping, analog stick, triggers.
- Save states with 10 slots, borderless fullscreen, window size, volume, turbo, pause and
  PNG screenshots.
- Settings saved to `tlagba.ini`.

### Requirements
- Windows 10/11, 64-bit. Your own ROM dump of the USA cartridge (`BQZE`).

[0.0.1]: https://github.com/PedroVitor-Dev/The_Last_Airbender_Recomp/releases/tag/v0.0.1
