# Level Editor Plan

Status: **E0–E2 done** (2026-07-19); E3 (in-editor playtest) not started.
Usage: `./opentyrian --data ./tyrian21 --edit <episode 1-4>` (hidden flag).
In-editor: Tab layer, T tile palette, Enter/Space place, P pick, E event
editor, S save (one-time `tyrian?.lvl.bak` backup), Esc back.
Self-test: `--edit-roundtrip <ep>` byte-verifies parse/serialize of every level.
Extras beyond E0–E2: F12 BMP screenshots + headless `--edit-shot <ep>,<lvl>`;
full-map PNG export (X key, or `--edit-export <ep>,<lvl>`) via a
self-contained stored-deflate PNG writer (`lvledit_png.c`). Gotchas learned:
`mapSh` is big-endian on disk; flight palette is the fixed `palettes[5]`;
layer-1 blank-but-assigned slots render solid black in-game.
Goal: an in-engine level editor for the `tyrian?.lvl` archives — browse, edit
tilemaps and event scripts, save — reusing the existing load path
(`lvllib.c` offset table + the record layout consumed by `JE_loadMap()` in
`tyrian2.c:3254`).

## On-disk format (established from JE_loadMap + JE_analyzeLevel)

`tyrian?.lvl` = `u16 lvlNum` + `s32 lvlPos[lvlNum]` offset table + back-to-back
records. `lvlPos[lvlNum]` (virtual) = EOF. Two table entries per level:
even = record start, odd = map-section start within the record (redundant,
derivable). The final entry of `tyrian4.lvl` is the episode item data
(read at `lvlPos[lvlNum-1]` by `episodes.c:76`), not a level — preserve as blob.

One level record, all little-endian:

| field | type |
|---|---|
| mapFile, shapeFile | 2 × char (shapeFile selects `shapes?.dat`) |
| mapX, mapX2, mapX3 | 3 × u16 |
| levelEnemyMax | u16 |
| levelEnemy[] | u16 × levelEnemyMax |
| maxEvent | u16 |
| events[] | maxEvent × **11 bytes**: u16 time, u8 type, s16 dat, s16 dat2, s8 dat3, s8 dat5, s8 dat6, u8 dat4 (this order; verified byte-exact against all 62 records) |
| mapSh[3][128] | u16 — per-layer slot→shape# (1..600 into `shapes?.dat`, 0 = unused) |
| map1 | 14×300 bytes, slot indices 0..71 |
| map2 | 14×600 bytes |
| map3 | 15×600 bytes |

Only slots 0..71 are real (`ref[layer][72]`); layer 2 slot 71 and layer 3
slots ≥70 are forced NULL by the loader.

## Phases

- **E0 — archive I/O core** (`src/lvledit_io.c/.h`): editable in-memory level
  struct; `lvledit_load(episode, level)`; `lvledit_save()` that blob-copies
  untouched records, serializes the edited one, and rebuilds the full offset
  table (both entries per level). Hidden flag `--edit-roundtrip <ep>`:
  load+reserialize every level, write to scratch, byte-compare, print
  PASS/FAIL, exit 0/1 — proves the format understanding headlessly. Use
  `dir_fopen*`/`fread_*_die`/memwriter-style LE writes per repo conventions.
- **E1 — editor shell + tile editing** (`src/lvledit.c`): `--edit <ep> <lvl>`
  boots into the editor loop instead of `titleScreen()`. Draws the tilemap
  (self-loaded 24×28 tiles from `shapes?.dat`), scroll, layer 1/2/3 switch,
  cursor, tile palette of the layer's 72 slots, place/pick, status line,
  save (back up original to `.bak` once), quit. Keyboard-driven.
- **E2 — event editor**: toggleable pane: scrollable event list, edit
  time/type/dat fields, insert/delete, name common event types (from
  `JE_eventSystem()`'s switch in `tyrian2.c`).
- **E3 (later)** — playtest the edited level from inside the editor.

## Conventions for this feature

- New code drops the `JE_` prefix; tabs; C99; no new deps.
- Editor must not disturb normal gameplay paths — everything behind the
  `--edit*` flags; no changes to `JE_loadMap()` itself.
- Implementation delegated to Sonnet subagents, one phase each, reviewed in
  the main session before commit (repo agent conventions).
