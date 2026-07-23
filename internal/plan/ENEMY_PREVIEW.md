# Enemy Preview in the Event Editor

Status: **Design** (2026-07-23). Scope chosen: **Option A only** — an enemy
sprite thumbnail + a few decoded stats in the event editor's inspector panel.
Options B (full enemy browser/picker) and C (shape-bank-loaded check) are
recorded at the end as follow-ups; the A renderer is written to be reusable by
both.

## Problem

The event editor shows an enemy-spawn event's target as a bare number
(`Enemy #234`). The author has no way to see *what that enemy is* — its sprite
or its stats — without launching the level. All the data is already in memory;
it just isn't surfaced.

## What the data is (all already loaded in the editor)

`enemyDat[N]` — `JE_EnemyDatType`, `src/episodes.h:118`. Loaded by
`JE_loadItemDat()` (`src/episodes.c:63`, from `tyrian.hdt`, or the level-file
tail for episode ≥4), which the editor already triggers via
`JE_initEpisode()` at `src/lvledit.c:2653`. Relevant fields:

- `shapebank` — which sprite bank the graphics live in (1..34).
- `egraphic[20]` — animation frame indices into that bank; `egraphic[0]` is the
  base/representative frame.
- `esize`, `armor`, `value`, `explosiontype`, `animate`, `xmove/ymove`,
  `xaccel/yaccel`, `elaunchtype` — the stat block.

Enemies have **no name field** in the data — only a number. (A curated name
table, like `event_type_name()`, would be a separate effort; A shows the number
+ sprite + stats, no names.)

## The render recipe (exactly what the game does)

In-flight draw is one blit, `src/tyrian2.c:210`:

```c
blit_sprite2(surface, x, y, *enemy[i].sprite2s,
             enemy[i].egr[enemy[i].enemycycle-1] + sprite_offset);
```

Replicated for a static thumbnail of enemy `N`:

1. `bank = enemyDat[N].shapebank`
2. `char c = shapeFile[bank-1]` (`src/lvlmast.c:27`) → the file `newsh<c>.shp`
   (lowercased; see `JE_loadCompShapes`, `src/sprite.c:609`)
3. load that bank into a `Sprite2_array` (on demand + cache — see below)
4. `blit_sprite2(VGAScreen, x, y, sheet, enemyDat[N].egraphic[0])`

The editor does **not** currently load enemy banks (only tile shapes). A is the
first code to pull `newsh*.shp` into the editor.

## Which events are previewable, and where the enemy # lives

The inspector schema already tags the field: `EF(2, "Enemy #")` — field id 2 =
`dat1` = `eventdat`. For the standard spawn types the spawn helper reads
`tempW = eventdat` directly (`JE_createNewEventEnemy`, `src/tyrian2.c:4517`;
`enemyTypeOfs` is 0 for 6/7/10/15/…, and `enemyOffset` 25/50/75 selects the
*slot lane*, not the `enemyDat` index) — so **enemy N = dat1**.

Previewable when the event's field schema is one of:

- `fl_enemy_spawn` (types 6, 7, 10, 15, 17, 18, 23)
- `fl_enemy_spawn_noyoff` (types 32, 56)
- `fl_12` (type 12, GROUND 4X4 — dat1 is the type base)

**Not** previewable as a clean `enemyDat` index: `fl_enemy_custom` (49–52),
whose field 2 is "Graphic" and field 4 "Shape Idx" — those override the sprite
directly rather than naming an `enemyDat` row. Skip them in v1.

Detection: compare the selected event's resolved schema pointer (from
`build_inspector_fields()`) against those three arrays, or add a small
`event_preview_enemy_num(const lvledit_event *ev, int *out_n)` helper that
returns true + the enemy number for exactly those types.

## Integration point & layout

`draw_inspector_sidebar()` — `src/lvledit.c:2002`. The panel spans
`ED_EVENT_SIDEBAR_LABEL_X` (212) .. `ED_EVENT_SIDEBAR_VALUE_RIGHT` (314),
~102px wide. Inspector fields occupy y≈22..70 (≤7 fields). Everything below
that down to the status line (y≈174) is empty — put the preview block there,
e.g. starting y≈96:

```
  ┌─ (x 212..314) ─────────────┐
  │ <existing header + fields>  │  y 13..~70
  │                             │
  │  Enemy #234   bank 7 'C'    │  y 96
  │  ┌────┐  armor 20  sz 1     │  sprite box + stats,
  │  │spr │  val 1500           │  sprite ~24px, stats to
  │  └────┘  expl 3  mv -2,1    │  the right / below
  └─────────────────────────────┘
```

- Draw a 1px frame, then `blit_sprite2` the base frame inside it. Enemy sprites
  are drawn on a 12px column cadence; multi-cell / `esize`≥1 enemies show only
  their primary cell — acceptable for a thumbnail (note it in a comment).
- Stat lines via `JE_outText` (armor, size, value, explosion, x/y move). Keep it
  to ~4–5 short lines to fit the panel width.
- Only render the block when `event_preview_enemy_num()` succeeds **and** the
  bank loaded; otherwise show nothing (or a one-line "bank X not available").

## Risks / must-handle

1. **`JE_loadCompShapes` is *dying*** (`dir_fopen_die`) and asserts the target
   slot is empty. Write a cached, non-dying loader keyed by bank char:
   `static Sprite2_array enemy_preview_bank; static char loaded_bank_char;` —
   `dir_fopen` (not `_die`), free-then-load on a char change, tolerate a missing
   file by leaving the slot empty and skipping the blit. Free it on editor exit.
2. **`blit_sprite2` does NO index bounds-check** (`src/sprite.c:650`,
   `data + offsets[index-1]`). A bad `egraphic[0]` reads a garbage offset →
   out-of-bounds read / crash. Guard before blitting: sprite count =
   `SDL_SwapLE16(((Uint16*)sheet.data)[0]) / 2` (the first offset points past the
   offset table), require `1 <= index <= count`, and `sheet.data != NULL`.
3. **Enemy index range**: `ENEMY_NUM = 850` (`src/lvlmast.h:36`); clamp/validate
   `N` before indexing `enemyDat[N]`.
4. **Palette**: the editor already runs the game palette (it draws tiles), so
   enemy colors should be correct — confirm visually with `--edit-shot`. (The
   `--edit-shot` events dump renders the inspector, so a spawn-event selection
   will exercise the preview headlessly for a pixel check.)
5. **`egraphic[0] == 0`**: some rows may have a 0/blank base frame; treat 0 as
   "nothing to draw" rather than blitting index 0.

## Plan (Option A)

- **A1.** Cached non-dying bank loader + a `bool enemy_preview_sprite(int n, int
  *bank_out)` that resolves bank → file → cache, validates the index, and blits
  into a given (x,y). Pure renderer, reusable by B later.
- **A2.** `event_preview_enemy_num(ev, &n)` predicate (the three schemas above).
- **A3.** Wire into `draw_inspector_sidebar()`: when the predicate holds, draw
  the sprite box + stat lines in the lower panel.
- **A4.** Verify: `make` + `make debug` clean; `--edit-shot <ep>,<lvl>` on a
  level with a spawn event selected, read back the events BMP, confirm the
  sprite renders in the panel (bank load + no crash on odd indices).

Implementation delegated to a Sonnet subagent, reviewed from the main
(Opus) session per repo agent conventions.

## Follow-ups (not in this scope)

- **B. Enemy browser / visual picker** — a modal grid of all `enemyDat` rows
  with sprites + stats, scroll + filter by bank, pick-to-assign onto the
  `Enemy #` field. Builds directly on A1's renderer.
- **C. Shape-bank-loaded check** — cross-reference each spawn's
  `enemyDat[N].shapebank` against the ≤4 banks declared by type-5 "ENEMY SHAPES"
  events earlier in the level (`newEnemyShapeTables`, `src/tyrian2.c:4666`); flag
  `⚠ bank not loaded`. Same data as A, no rendering; catches a real class of
  invisible-enemy authoring bugs.
