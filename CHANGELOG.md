# Changelog

All notable changes to this project. Versions follow [Semantic Versioning](https://semver.org/).

## [0.0.3] — 2026-11-26

Works with [RecompLauncher](https://recomplauncher.com).

### Added
- RecompLauncher support: `recomplauncher.json` in every release (Easy Setup with the player's
  ROM, video, audio and controls settings on the game's page, saves and save states backed up).
- `AvatarRecomp.exe --game <rom>` starts that ROM straight away and `--data <folder>` keeps saves,
  save states and screenshots in that folder. Started by RecompLauncher, it skips its own launcher
  and opens full screen in Big Picture mode. Without these, nothing changes.

### Changed
- Release zips are named `AvatarRecomp-vX.Y.Z-Windows-x64.zip`.
- `tlagba.ini` keeps its comments on their own lines (same keys and values).

## [0.0.2] — 2026-11-25

Fixes a freeze when talking to NPCs and slowdown while walking. **Everyone on 0.0.1 should update.**

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
- Freeze when talking to the village children at the start of the game (the dialogue box never
  appeared) and slowdown while walking: the recompiled code charged more CPU cycles than the interpreter (code
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

[0.0.3]: https://github.com/PedroVitor-Dev/The_Last_Airbender_Recomp/releases/tag/v0.0.3
[0.0.2]: https://github.com/PedroVitor-Dev/The_Last_Airbender_Recomp/releases/tag/v0.0.2
[0.0.1]: https://github.com/PedroVitor-Dev/The_Last_Airbender_Recomp/releases/tag/v0.0.1
