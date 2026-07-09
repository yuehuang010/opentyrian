# CODEMAP.md — OpenTyrian subsystem map

One-page orientation for humans and agents. Written for
[`internal/plan/NAVIGATION_PLAN.md`](plan/NAVIGATION_PLAN.md) Phase 2 item 4.
Every claim below was verified by reading the source or grepping `tags`
(ctags, repo root) as of 2026-07-09 — treat it as more reliable than
`CLAUDE.md`'s "## Architecture" prose, which has at least one confirmed error
(see below).

**Regenerating/refreshing this understanding:** `make compdb` (writes
`compile_commands.json` for clangd/LSP "go to definition") and `make tags`
(ctags index, grep-free fallback) are already wired up in the `Makefile` —
both Phase 1 targets exist today. Doxygen is also live (`Doxyfile` has
`EXTRACT_ALL=YES`, `HAVE_DOT=YES`, `OUTPUT_DIRECTORY=doc/doxygen/`); run
`doxygen` from the repo root (no dedicated `make` target for it) to
regenerate browsable HTML with call graphs. When in doubt,
`grep -n '^SYMBOL' tags` beats reading this file's prose.

## Known discrepancy vs. CLAUDE.md

CLAUDE.md's "## Architecture" section says `JE_main` (in-flight gameplay
loop) lives in `mainint.c`. **It does not** — `JE_main` is defined in
**`src/tyrian2.c:641`**. `mainint.c` contains related-but-different
functions (`JE_mainKeyboardInput`, `JE_mainGamePlayerFunctions`, etc.) but
not `JE_main` itself. Likewise `titleScreen()` lives in **`src/tyrian2.c`**
(there is no `src/title.c`). All other file/function attributions in this
document were spot-checked against the source and matched CLAUDE.md's
claims unless noted otherwise.

## Control flow

`main()` — `src/opentyr.c:878` — initializes subsystems, then loops
(`src/opentyr.c:980-1014`):

1. `titleScreen()` (`src/tyrian2.c:3386`) — title/menu/attract-mode demo.
   Returns `false` on player quit, breaking the loop.
2. If `loadDestruct` is set: `JE_destructGame()` (`src/destruct.c:660`) —
   the Destruct minigame.
3. Otherwise: `JE_main()` (`src/tyrian2.c:641`) — the in-flight gameplay
   loop (enemies, shots, backgrounds, player input).

## Video / Rendering

SDL2 window, the 320x200 8-bit `VGAScreen` surface, upscaling, palette
fades, sprite/image loading, text, starfield/parallax backgrounds.

- Files: `video.c/h`, `video_scale.c/h`, `video_scale_hqNx.c` (no own
  header; declared in `video_scale.h`), `palette.c/h`, `vga_palette.c/h`,
  `vga256d.c/h`, `sprite.c/h`,
  `pcxload.c/h`, `picload.c/h`, `font.c/h`, `fonthand.c/h`, `starlib.c/h`,
  `backgrnd.c/h`, `interp.c/h` (Phase-6 render interpolation).
- Entry points: `init_video()` — `src/video.h:156`; `blit_sprite()` /
  `blit_sprite_hv()` / `blit_sprite_dark()` — `src/sprite.c`;
  `set_palette()` / `fade_palette()` — `src/palette.c`; `JE_outText()` /
  `JE_dString()` — `src/fonthand.c`; `draw_background_1/2()` —
  `src/backgrnd.c`.
- Key state: `SDL_Surface *VGAScreen, *VGAScreenSeg` (`src/video.h:149`),
  `Palette palettes[]` (`src/palette.h:30`), `bool highfps_mode` (Phase-6
  interpolation toggle, `src/interp.c:46`).

## Game Logic

Level scripting/data tables, player mechanics, shots/weapons, the
gameplay/interlevel driver, shop/menu UI, minigames.

- Files: `mainint.c/h` (5132 lines — monolith, see NAVIGATION_PLAN Phase 3),
  `tyrian2.c/h` (5443 lines — also a monolith; owns `titleScreen()` and
  `JE_main()`), `player.c/h`, `shots.c/h`, `episodes.c/h` (level scripting —
  see `doc/files.txt` for `levels?.dat`), `varz.c/h` (1195/364 lines — large
  shared game-state pile), `nortvars.c/h` (75/29 lines — small UI-drawing
  helper pair, no globals of its own), `menus.c/h`, `game_menu.c/h`
  (3296 lines), `destruct.c/h` (2812 lines), `editship.c/h`,
  `helptext.c/h`, `lvlmast.c/h`.
- Entry points: `JE_main()` — `src/tyrian2.c:641`; `titleScreen()` —
  `src/tyrian2.c:3386`; `JE_destructGame()` — `src/destruct.c:660`;
  `simulate_player_shots()` — `src/shots.c:37`; `power_up_weapon()` —
  `src/player.c:36`; `JE_initEpisode()` / `JE_loadItemDat()` —
  `src/episodes.c`; `JE_itemScreen()` — `src/game_menu.c:154`.
- Key state: `Player player[2]` (`src/player.h:120`) — each `Player` holds
  `ulong cash`, `PlayerItems items`, `Uint8 *lives`, etc.; flight-state
  flags `endLevel, reallyEndLevel, quitRequested, ...` and enemy-tracking
  `totalEnemy` — `varz.h:248,264`; difficulty/power globals
  `difficultyLevel, power, shotRepeat[11]` — `config.h:101-120` (note: some
  gameplay globals live in `config.h`, not `varz.h`).

## Audio

Sound API, mixer, OPL2 FM synth for the LDS music format, sound/music
tables, jukebox.

- Files: `nortsong.c/h` (sound API), `sndmast.c/h`, `musmast.c/h` (tables),
  `loudness.c/h` (mixer), `lds_play.c/h` + `opl.c/h` (LDS/OPL2 synth),
  `jukebox.c/h`, `stb_vorbis.c` (third-party, no matching `.h` in `src/` —
  only `.c` file in the tree without one).
- Entry points: `load_music()` / `play_song()` — `src/loudness.c:459,488`;
  `JE_playSampleNum()` — `src/nortsong.c:501`; `lds_update()` —
  `src/lds_play.c:264`; `jukebox()` — `src/jukebox.c:43`.
- Key state: `bool hd_sfx` (`src/nortsong.c:48`), `int audioSampleRate`
  (`src/loudness.c:46`).

## Input

SDL2-based keyboard, mouse, joystick handling.

- Files: `keyboard.c/h`, `mouse.c/h`, `joystick.c/h`.
- Entry points: `init_keyboard()` / `keyboardGetInput()` —
  `src/keyboard.c:85,101`; `JE_mouseStart()` — `src/mouse.c:120`;
  `poll_joysticks()` — `src/joystick.c:217`.
- Key state: `bool keysactive[SDL_NUM_SCANCODES]` (`src/keyboard.c:47`),
  `bool has_mouse` (`src/mouse.c`), `bool joydown` (any joystick button
  down, `src/joystick.c:51`).

## Persistence / IO

All data access (`data_dir()` + `dir_fopen*`), INI-style config, save-game
read/write, endian-safe in-memory (de)serialization, ANM cutscene player,
asset table masters.

- Files: `file.c/h` (`data_dir()`, `dir_fopen*` — all asset IO must go
  through these), `config.c/h` + `config_file.c/h` (config + save-game),
  `memreader.c/h` / `memwriter.c/h`, `animlib.c/h`, `lvllib.c/h`,
  `pcxmast.c/h`.
- Entry points: `data_dir()` — `src/file.c:43`; `dir_fopen()` /
  `dir_fopen_warn()` / `dir_fopen_die()` — `src/file.c:80,99,110`;
  `load_opentyrian_config()` / `save_opentyrian_config()` —
  `src/config.c:266,352`; `JE_saveGame()` / `JE_loadGame()` —
  `src/config.c:451,507`; `playAnim()` — `src/animlib.c:154`;
  `JE_analyzeLevel()` — `src/lvllib.c:35`.
- Key state: config-file schema types in `config_file.h`; save-slot data
  driven through `Player player[2]` (see Game Logic) and `varz.h`/`config.h`
  globals.

## Misc / Support

Command-line parsing, UDP multiplayer (optional), Mersenne Twister RNG,
Christmas-themed asset variant, standalone-bundle loader.

- Files: `arg_parse.c/h`, `params.c/h`, `network.c/h` (guarded by
  `#ifdef WITH_NETWORK` throughout), `mtrand.c/h`, `xmas.c/h`, `bundle.c/h`
  (zero-external-data support, see `internal/plan/STANDALONE_PLAN.md`).
- Entry points: `parse_args()` — `src/arg_parse.c:46` (getopt-style
  parser); `JE_paramCheck()` — `src/params.c:49` (uses `parse_args()` to
  interpret Tyrian's own CLI flags); `network_init()` —
  `src/network.c:756` (only compiled with `WITH_NETWORK`); `mt_rand()` /
  `mt_srand()` — `src/mtrand.c:76,61` (use this, not `rand()`);
  `xmas_time()` — `src/xmas.c:42`; `bundle_available()` / `bundle_has()` —
  `src/bundle.c:291,300`.
- Key state: `bool isNetworkGame`, `int network_delay` — `src/params.c:63-64`;
  `bool xmas` — `src/xmas.c:40`.

## File count sanity check

53 `.c` files in `src/`, 53 `.h` files, but the pairing isn't 1:1: `stb_vorbis.c`
and `video_scale_hqNx.c` (declared via `video_scale.h`) have no matching
header, while `qoi.h` and `opentyrian_version.h` are header-only (no `.c`).
