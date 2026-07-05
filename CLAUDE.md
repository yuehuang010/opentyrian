# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

OpenTyrian is a C (C99) cross-platform port of the DOS vertical-scrolling shooter *Tyrian*. It renders to a 320x200 8-bit paletted VGA surface via SDL2 and upscales it. The engine is a direct, heavily-refactored port of the original Pascal source, so much of the code preserves the original's data formats, control flow, and naming.

The game itself ships with **no game data**. It requires the freeware Tyrian 2.1 data files (https://camanis.net/tyrian/tyrian21.zip) placed in the data directory. `data_dir()` in `src/file.c` resolves data from, in order: `custom_data_dir` (set via `-d`/`--data` arg), the compiled-in `TYRIAN_DIR`, then the current directory.

## Build & Run

Build system is a plain GNU Makefile. Dependencies: SDL2 and SDL2_net (via `pkg-config`), plus a C99 compiler.

```sh
make              # optimized build -> ./opentyrian
make debug        # -O0 -g3 -Werror, disables NDEBUG (enables asserts + intro logos are skipped under NDEBUG)
make clean
make -j           # parallel build

make WITH_NETWORK=false   # build without SDL2_net / multiplayer

./opentyrian -d /path/to/tyrian/data    # run pointing at data files
```

There is no test suite and no linter config beyond the compiler warnings (`-pedantic -Wall -Wextra`, `-Werror` in the debug target). "Passing" means it compiles clean and runs. Windows builds use `visualc/opentyrian.sln` (MSVC).

Object files land in `obj/` (mirrors `src/`); the Makefile globs `src/*.c`, so **new source files are picked up automatically** — no Makefile edit needed. `-MMD` generates header dependency files (`obj/*.d`).

## Architecture

Flat `src/` layout (~50 `.c` files, each with a matching `.h`). Entry point is `main()` in `src/opentyr.c`, which initializes subsystems then loops: `titleScreen()` → `JE_main()` (in `mainint.c`, the in-flight gameplay loop) or `JE_destructGame()` (the Destruct minigame).

Rough subsystem grouping:

- **Video / rendering**: `video.c` (SDL window + the 320x200 `VGAScreen` surface), `video_scale.c` + `video_scale_hqNx.c` (upscalers), `palette.c` / `vga_palette.c` (VGA palette + fades), `vga256d.c` (low-level blits), `sprite.c` / `pcxload.c` / `picload.c` (image loading), `font.c` / `fonthand.c` (text), `starlib.c` / `backgrnd.c` (parallax starfield & backgrounds).
- **Game logic**: `mainint.c` (main gameplay/interlevel driver), `player.c`, `shots.c`, `episodes.c` (level scripting — see `doc/files.txt` for the `levels?.dat` format), `varz.c` / `nortvars.c` (large piles of shared game-state globals), `menus.c` / `game_menu.c` / `mainint.c` for shop/menu UI, `destruct.c` and `editship.c` (minigames).
- **Audio**: `nortsong.c` (sound API), `sndmast.c` / `musmast.c` (sound/music tables), `loudness.c` (mixer), `lds_play.c` + `opl.c` (OPL2 FM synth for the LDS music format), `jukebox.c`.
- **Input**: `keyboard.c`, `mouse.c`, `joystick.c` (all SDL2-based; note commit history shows input was recently redesigned).
- **Persistence / IO**: `file.c` (all data access goes through `dir_fopen*` helpers), `config.c` + `config_file.c` (INI-style config + save-game read/write — recently rewritten), `memreader.c` / `memwriter.c` (endian-safe in-memory serialization), `animlib.c` (ANM cutscene player), `lvllib.c` / `pcxmast.c` etc. (asset table masters).
- **Misc**: `arg_parse.c` / `params.c` (command-line parsing), `network.c` (SDL2_net UDP multiplayer, guarded by `WITH_NETWORK` / `#ifdef WITH_NETWORK`), `mtrand.c` (Mersenne Twister RNG — use `mt_rand()`, not `rand()`), `xmas.c` (Christmas-themed assets).

## Conventions

- **`JE_` prefix**: functions/vars carried over from the original Pascal source (`JE_` = the original author's initials). New code tends to drop it. Both coexist; match the surrounding file.
- **Data is little-endian on disk**. When reading/writing binary game files, go through `memreader.c`/`memwriter.c` or the `SDL_SwapLE*` macros rather than raw struct reads, for cross-platform correctness.
- **All file IO goes through `data_dir()` + `dir_fopen*()`** (`file.c`) so custom data dirs and missing-file diagnostics work. Don't `fopen` game assets directly.
- Networking code must be wrapped in `#ifdef WITH_NETWORK` — the build can exclude it entirely.
- `.editorconfig` is present; follow it (tabs for indentation in `.c`/`.h`).
- Global game state is real and pervasive (`varz.c`, `nortvars.c`, `config.h`). Expect to read and mutate shared globals rather than pass everything through parameters.

## Reference

- `doc/files.txt` — describes the original Tyrian data file formats (levels, shapes, music, palettes, etc.). Essential when touching asset loading or level scripting.
- `README` — controls, network multiplayer invocation (`--net`), data file source.
- Doxygen config in `Doxyfile` (`make` docs land in `doc/doxygen/`).
