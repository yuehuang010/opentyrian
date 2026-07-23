# Level Editor Plan

Status: **E0–E2 done** (2026-07-19); E3 (in-editor playtest) not started.
Usability pass **2026-07-22**: toggleable tile sidebar + undo/redo, in-app
episode picker, level-select sort + titles (below). Queued: left mini-map
scrollbar.
Usage: `./opentyrian --data ./tyrian21 --edit` (hidden flag, no argument --
boots an in-app episode picker (1-4), then that episode's level-select).
In-editor: Tab layer, T toggle tile sidebar, `[`/`]` prev/next tile slot,
Enter/Space place, P pick, U undo, R redo, E event editor, S save (one-time
`tyrian?.lvl.bak` backup), X export, Esc back.

Tile sidebar (T): right-hand 2-col scrolling grid of the active layer's 72
slots (replaces the old modal palette overlay). Open shrinks the map
viewport 13→11 cols (`view_cols()`); `[`/`]` step the brush through usable
slots with the highlight auto-scrolling to follow. Undo/redo: bounded
whole-level snapshot ring (`ED_UNDO_CAP` 64, static BSS ~6.6MB), `undo_push()`
before each real mutation (tile place + all event edits); event editor uses
`Y` for redo since `R` is sort-by-time there. All in `src/lvledit.c` only.

Level-select sort + titles: the list defaults to **play order** and `O`
toggles to raw archive-index order. Play order and the shown level titles are
parsed from the episode script `levels<ep>.dat` (encrypted pascal strings;
`read_script_line()` is a non-dying decrypt copy of helptext.c's, since
`read_encrypted_pascal_string` exits at EOF). Play-level lines are `]L…`
(two-char prefix; `s[1]=='L'`): `lvlFileNum = atoi(s+25)` (1-based) → archive
index `lvlFileNum-1`; title = 9 chars at `s+13`. Order is by first script
appearance, orphan records appended by index, identity fallback if the script
is missing. Sorting is **display-only** (`display_order[row]→archive_index`);
on-disk record order is never touched (the script references records by
index). `last_level_sel` tracks the archive index so the highlight sticks
across re-sorts/episode switches.

Mouse support (2026-07-22): the mouse **selects and scrolls only -- it never
places a tile**, so a stray click (e.g. right after opening a level) can't
drop an asset by accident; placement stays on Enter/Space. Left-click/drag in
the map viewport moves the cursor to that cell; right-click is the eyedropper
(same as `P`). The left mini-map strip is click/drag-to-scroll (jumps
`cursor_y` to the clicked row). The tile sidebar is click-to-select. Drag
continuation is gated on a `mouse_dragging` flag that is only armed by a press
INSIDE the map screen and cleared at map-editor entry, so a button still
physically held from the level selector as the map opens (mouseClearInput()
flushes queued click events but NOT the held-button bitmask) can't hijack the
cursor on the first frame -- this was the "clicking a level jumps to the top
of the map" bug. The level-select and episode-select screens are
click-to-highlight-a-row, click-again-to-open; the event editor is
click-to-select-row-and-field. Mouse wheel scrolls vertically everywhere (map
cursor, event selection, level/episode list). A drawn crosshair (`draw_mouse_
pointer()`, suppressed while `mouseInactive`) stands in for the OS cursor,
which stays hidden inside the 320x200 area (see keyboard.c's `SDL_ShowCursor`
toggle) — kept out of `render_map_screen()`/`draw_event_screen()` so the
headless `--edit-shot` BMPs and F12 screenshots stay pointer-free. One
shared-file change outside `lvledit.c`: a `mouseWheelY` accumulator
(`SDL_MOUSEWHEEL` case) in `keyboard.c`/`keyboard.h`, read and zeroed once per
editor frame. The level-select list draws its columns (NUM/NAME/MAP/SHP) at
fixed pixel x's under a header row, since JE_outText's proportional font made
the old space-padded single-string layout never line up.

Queued (task chip): **left mini-map scrollbar** — slim left-edge vertical
mini-map of the active layer showing full level height, a band for the current
6-row viewport, and a cursor tick. Main risk: the map viewport is anchored at
x=0 today, so it needs a left-origin offset + a recomputed `view_cols()` that
fits left strip + map + right sidebar within 320px.
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
| events[] | maxEvent × **11 bytes**: u16 time, u8 type, s16 dat, s16 dat2, s8 dat3, s8 dat5, s8 dat6, u8 dat4 (this order; verified byte-exact against all 62 records). For what `type` + the `dat` fields *mean*, see [EVENT_REFERENCE.md](EVENT_REFERENCE.md) |
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
