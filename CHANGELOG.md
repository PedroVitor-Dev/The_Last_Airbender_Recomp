# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Launcher: title screen with the cover art, with Play, Settings, Select ROM… and Quit. Shows the
  current ROM and whether it is the expected one; navigable with keyboard or gamepad.
- Settings can be opened from the launcher; "Exit to launcher" in the in-game menu.
- "Launcher at startup" option (`launcher = no` in `tlagba.ini` starts the game directly).
- Video page in the settings: fullscreen, window size, **frame rate** (60 fps or the GBA's original
  59.73) and **V-Sync** (frames shown on the monitor refresh at 60/120/240 Hz; on other rates,
  such as 144/160 Hz, a precise timer keeps the frame rate).
- Audio follows the picture with dynamic rate control (pitch change under 0.5%) instead of being
  the clock, so frames are evenly spaced.

### Fixed
- Slowdown while walking: the recompiled code charged more CPU cycles than the interpreter (code
  in IWRAM, like the sound mixer, was charged as if it ran from ROM), so the game lost about 1
  frame in 10 when the character moved. It now runs a full 60 logic frames per second.

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
