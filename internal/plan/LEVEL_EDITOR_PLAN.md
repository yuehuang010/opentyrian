# Level Editor Plan

Status: **E0–E2 done** (2026-07-19); **E7 (in-editor "F5" playtest) done**
(2026-07-23). **E4–E6 designed** (2026-07-23, below): add levels/episodes —
archive record-append, a full **semantic** `levels<ep>.dat` script editor,
add-episode scaffolding. E7 (fly the edited level, then return to the editor)
was the concrete realization of the old E3 placeholder. Opcode schema for the
script lives in [SCRIPT_REFERENCE.md](SCRIPT_REFERENCE.md).
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
  *(Realized as **E7 — "F5" playtest**, designed in full below.)*

## E4–E6: adding levels & episodes (designed 2026-07-23)

### The core insight — two files, only one of them writable today

"Add a level / add an episode" is not one data structure but two, and the editor
currently writes only the first:

| | `tyrian<ep>.lvl` (archive) | `levels<ep>.dat` (script) |
|---|---|---|
| Holds | tilemap + events + enemies per level *record* | interlevel command language: play order, level→record binding, shop menus, map-branch choices, cutscenes, story cubes, end-of-episode |
| Editor today | read + write (E0–E2) | read-only (title/play-order parse) |
| Consumed by | `JE_loadMap()` | interpreter at `tyrian2.c:2676` |

A record is **inert** until a `]L` line in the script points at it
(`tyrian2.c:2865`, `lvlFileNum = atoi(s+25)`). So making a new level *playable*
requires **writing the encrypted script** — that is the real new capability and
everything below hinges on it. The full opcode schema the script editor
implements is in [SCRIPT_REFERENCE.md](SCRIPT_REFERENCE.md).

### E4 — Archive can grow a record (`src/lvledit_io.c`)

Today `lvledit_save_archive()` only *replaces* one record in place. Add an
insert/append path:

- Grow `lvlNum` by **2** (the record-start + map-section entry pair per level),
  serialize the new record just before the trailing unpaired blob (item data
  for tyrian4/5, EOF marker otherwise), rebuild the whole offset table. The
  serializer + offset-table rebuild already exist and are round-trip-proven; this
  is a bounded extension, not a rewrite.
- **Template, not zero-fill.** A zero-filled level has no end event and won't
  terminate in-game. "Add level" **clones an existing record** (default: the
  currently selected level, or a bundled minimal template) — sidesteps "what is
  a minimal valid level" entirely and reuses the proven serializer.
- New CLI self-test `--edit-addlevel <ep>`: load, clone-append, save to scratch,
  reload, assert count+1 and byte-identical clone — headless proof like
  `--edit-roundtrip`.

### E5 — Semantic script editor (the crux; `src/lvledit_script.c/.h` + UI in `src/lvledit.c`)

**Status: E5a done; E5b/c IMPLEMENTED (2026-07-23).** The script editor is
reachable via **`C`** ("sCript") from the level-select screen (per-episode).
Model-layer mutations + fixed-width field access live in `lvledit_script.c`
(headlessly testable); `lvledit.c` holds only `run_script_editor()`'s
render/input glue. Section ordinals are kept correct by **rewriting every
jump/`]G` target on a marker insert/delete** (`lvledit_script_insert_section`/
`_delete_section`), rather than a symbolic-reference model — the flat `script_doc`
stays the single source of truth (as E5a established), and the retarget is proven
byte-exact by the hidden **`--edit-script-retarget-test <ep>`** gate (opcode 264):
step 0 field-API no-op, steps 1–4 insert/delete-invert target preservation,
step 5 codec round-trip — ALL PASS on episodes 1–4. Full field support: `]L`
(next/name/song/record#), `]J/]2/]w/]t/]l/]H` (section targets), `]G`
(origin/count/per-choice planet+section), `]M/]i/]P/]?/]!/]+/]W`. Descoped to
free-text/sub-line rows (not structured fields): `]I` 9 shop rows, `]W`/`]Q`
`#`-terminated text blocks, `]h`'s line — editable as indented raw lines.

Make `levels<ep>.dat` writable with a **structured, validated editor** over the
full command language (not a raw-line editor). Decompose:

- **E5a — codec + model.** Encoder (inverse of `read_script_line`'s XOR, ~6
  lines — see SCRIPT_REFERENCE.md), plus a parser into the **3-level tree**:
  sections → commands → owned sub-line blocks (`]I` = next 9 lines; `]W`/`]Q` =
  lines until `#`; `]h` = next 1 line). Inert non-`]` lines and the `]S` no-op
  round-trip **verbatim** (blob-preserve posture, same as the archive editor).
  Ship a `--edit-script-roundtrip <ep>` self-test (decrypt→parse→serialize→
  encrypt, byte-identical) *first* — nothing is trusted until it passes.
- **E5b — structural editing.** Insert/delete/reorder sections and commands.
  **Section numbers are symbolic**: jump/`]G` targets are stored as references
  to a section object and only lowered to 1-based ordinals at serialize time, so
  a reorder/insert can't silently break every jump. This is the single biggest
  correctness hazard — call it out in review.
- **E5c — per-opcode field forms.** A semantic inspector like the event
  editor's `ef_field` tables, one schema per opcode from the SCRIPT_REFERENCE
  table (`]L` name/song/record#, `]G` branch choices, `]I` 9 shop rows, jump
  targets as a section picker, `]P`/`]M`/`]W` args). Reuse the event-editor's
  list+inspector UI idiom already in `lvledit.c`.
- One-time `levels<ep>.dat.bak` backup on first script save (same pattern as the
  archive `.bak`).

E5 is Opus-reviewed (or Fable for E5a+E5b end-to-end): the symbolic-section
rewrite and the block framing are subtle, and a mis-serialized script silently
corrupts the whole episode's sequencing.

### E6 — Add episode (scaffolding; `src/lvledit.c`)

Engine is already episode-count-dynamic: `EPISODE_MAX 5` (`episodes.h:34`),
`JE_scanForEpisodes()` probes `tyrian<n>.lvl`, `JE_findNextEpisode()` wraps on
availability — **a 5th episode needs no core-engine change**. "New episode" is a
scaffold built from E4 + E5:

- Create `tyrian5.lvl` = one cloned level (E4) + the **item-data blob** appended
  to the archive tail (episodes ≥4 read item data from there, `episodes.c:76`;
  copy episode 4's blob so the shop/enemy tables are valid).
- Emit a minimal `levels5.dat` (one section: `]I` menu, `]L` play, `]Q` end) via
  the E5 encoder, plus a stub `cubetxt5.dat`.
- Replace the hardcoded 1–4 loop in `run_episode_select()` (`lvledit.c:3213`)
  with a dynamic scan + a "＋ New episode" row.
- **Ceiling:** `EPISODE_MAX 5` is the natural stop. Episode 6+ would mean
  bumping that constant and auditing save-slot / next-episode logic — out of
  scope; flag if requested.

### E7 — In-editor playtest, the "F5" experience (`src/lvledit.c` + carve-outs in `tyrian2.c`)

**Status: IMPLEMENTED (2026-07-23)** — interactive path done, built clean
(`make` + `make debug`), shared gameplay paths verified unbroken headlessly
(attract demo still reaches flight; `--edit-roundtrip` still PASSes). The
optional headless `--edit-playtest <ep>,<lvl>` smoke test is **deferred** (live
input can't be driven headlessly; the shared paths it would exercise are already
covered by demo playback). Actual carve-outs landed in `tyrian2.c` only — the
in-game-menu Quit already routes to the return, so `mainint.c` needed no edits.

Press **F5** in the map editor → fly the level you're editing → to return, open
the in-game menu (**Esc**) and pick **Quit** (or die / reach the level's own
end) — you land back in the map editor with your edits intact.

**As built (differs slightly from the design sketch below):**
- One new global `editorPlaytest` (`varz.c`/`.h`), false on every normal path.
- **4 strict-superset carve-outs in `tyrian2.c`**: skip the campaign
  end-of-level block (`JE_main` ~739), return-to-caller at level end (~768),
  skip the on-level autosave (~1046), and skip the whole `levels<ep>.dat`
  interpretation in `JE_loadMap` (~2677) — each `... && !editorPlaytest` or
  `play_demo || editorPlaytest`, so a normal run (`editorPlaytest==false`) is
  byte-identical in behavior.
- **No `mainint.c` change**: the existing in-game-menu Quit sets
  `reallyEndLevel`/`playerEndLevel`, which flows to the `JE_main` early return.
- `playtest_current_level()` in `lvledit.c` stages the current `cur_level`
  (incl. unsaved edits) to `_edtest.lvl` in `data_dir()` via the existing
  `lvledit_save_archive`, sets up state (`JE_initEpisode` → `JE_initPlayerData`
  → repoint `levelFile`/`lvlPos`/`lvlFileNum` → default difficulty/song/name),
  runs `JE_main()`, then deletes the scratch file and restores the editor
  (mouse absolute, `palettes[5]`, `tileset_loaded=false`). `lvledit_io`'s own
  archive blob and `cur_level` are untouched by the game-side globals, so edits
  survive the round trip.

The original design write-up follows.

Press **F5** in the map editor → fly the level you're editing → on death /
level-end / Esc, land back in the map editor exactly where you left. This is the
concrete design of the old E3 placeholder.

**Why it's tractable.** The editor is its own top-level mode (`opentyr.c:1025`,
`lvledit_run()`); it never touches `JE_main()`. And the game's own **demo
playback already does almost exactly what a playtest needs**:

- `JE_loadMap()` under `play_demo` **skips the entire episode-script
  interpretation** (`tyrian2.c:2672`, the `if (!play_demo)` block) and jumps
  straight to the record read at `lvlPos[(lvlFileNum-1)*2]` (`tyrian2.c:3255`).
  So a playtest needs **no `levels<ep>.dat` at all** — it loads a record by
  index, the same way the demo does.
- `JE_main()` **returns to its caller** when `play_demo` (`tyrian2.c:765`)
  instead of chaining to the item screen / next section — exactly the
  "come back to the editor" behavior we want.
- `load_next_demo()` (`mainint.c:2385`) is the **state template**: `JE_initEpisode(ep)`,
  `lvlFileNum`, `levelName`, `levelSong`, and a full player loadout
  (ship/weapons/sidekicks/generator/shield/special/powers).

**The one difference from demo mode:** a demo replays canned input
(`replay_demo_keys`); a playtest takes **live** input. So E7 is a *sibling
flag*, `editorPlaytest`, that shares `play_demo`'s script-skip / no-save /
return-early skeleton but leaves input live.

**Design:**

- **`playtest_current_level()` in `lvledit.c`**, invoked on F5 from
  `run_map_editor()`. It:
  1. **Stages the edited record to a scratch archive.** `JE_loadMap()` always
     reads `dir_fopen(data_dir(), levelFile)` seeking `lvlPos[…]`, so the record
     under test must be on disk *and reachable through `data_dir()`*. Write the
     editor's in-memory archive (edited record included, **even if unsaved**) to
     a reserved name in `data_dir()` (e.g. `_edit_playtest.lvl`) via
     `lvledit_save_archive()`, then point `levelFile` at it and `JE_analyzeLevel()`
     to fill `lvlPos`. This tests unsaved edits **without** touching the real
     archive or its `.bak`. Delete the scratch file on return. (Rejected
     alternative: "save then play the real archive" — forces a disk write on
     every playtest; rejected: temp dir — `data_dir()` composition can't reach
     outside it.)
  2. Sets `lvlFileNum` to the record index, `levelName`/`levelSong` from the
     level, `difficultyLevel` to a default, and a **fixed fully-kitted loadout**
     (a sane default so the level is flyable/inspectable; a loadout picker is a
     later nicety, not MVP).
  3. Snapshots editor-owned global state (palette, `hd_mode` already handled,
     `twoPlayerMode`, song), sets `editorPlaytest = true`, calls `JE_main()`.
  4. On return: clears `editorPlaytest`, deletes the scratch archive, restores
     the snapshot, re-applies the editor palette (`palettes[5]`) and forces a
     tileset reload (`tileset_loaded = false`) — flight loads its own palette and
     frees sprite sheets, so the editor must re-establish its own on the way
     back.

- **Core carve-outs (the invasive, review-worthy part).** Group the existing
  `play_demo` gates by intent and extend only the right ones:
  - *Script-skip / no-save / no-backup* gates (`tyrian2.c:2672`,
    `1043`, `1049`; and the loadmap fade `3249`): treat as
    `script_bypass = play_demo || editorPlaytest`. A local `script_bypass`
    keeps the diff legible instead of `|| editorPlaytest` sprinkled everywhere.
  - *Input* gates (`mainint.c:3795/3799/3863/4142`, the `replay_demo_keys` path,
    and `tyrian2.c:2342` "input kills demo"): stay `play_demo` **only** — live
    input must flow. Add **one** new rule: under `editorPlaytest`, **Esc aborts
    the level** and returns (set `endLevel`/`reallyEndLevel` so JE_main falls
    through its normal end path to the `play_demo`-style return).
  - *Return-to-caller* gate: JE_main already returns when `play_demo` at `765`;
    extend that single early-return to `play_demo || editorPlaytest`. A level
    with no end-event won't terminate on its own — Esc (above) is the guaranteed
    way out.
  - Suppress demo recording, autosave/backup, high-score entry, and the
    end-of-level item/map screens under `editorPlaytest` (all already gated on
    `!play_demo` or reachable via the early return).

- **Smoke test:** `--edit-playtest <ep>,<lvl>` that stages, enters `JE_main()`
  for a fixed frame budget with synthetic "no input", then returns — proves the
  enter/return plumbing headlessly (can't validate gameplay feel, but catches
  crashes and state-restore regressions), mirroring `--edit-shot`.

**Risk / review posture.** E7 is the *only* phase that edits core gameplay files
(`tyrian2.c`, `mainint.c`), against the "don't disturb gameplay paths"
convention — so it's **Opus-reviewed**, and every carve-out must be a strict
superset (a plain campaign run has `editorPlaytest == false`, so behavior is
provably unchanged). The `script_bypass` local + the single Esc/return rule keep
the surface small and auditable.

### Sequencing & risk

- Order: **E4 → E5a (codec/roundtrip) → E5b/E5c → E6**, with **E7 (playtest)
  buildable right after E4** — it needs only record-append staging, not the
  script editor (it bypasses the script entirely). A good early win: E4 + E7
  gives "edit tiles/events → F5 → fly it" before any script work.
- E5a's byte-identical round-trip gate is the go/no-go for the script feature;
  E7's carve-out-is-a-strict-superset check is the go/no-go for touching core.
- Backups + round-trip self-tests are the safety net; everything except E7's
  audited carve-outs stays behind `--edit*`, no changes to `JE_loadMap()`'s
  record read or the script interpreter's logic.

## Conventions for this feature

- New code drops the `JE_` prefix; tabs; C99; no new deps.
- Editor must not disturb normal gameplay paths — everything behind the
  `--edit*` flags; no changes to `JE_loadMap()` itself.
- Implementation delegated to Sonnet subagents, one phase each, reviewed in
  the main session before commit (repo agent conventions).
