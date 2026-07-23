# Level event reference — the `JE_eventSystem()` field map

Reference data for the level editor's event script: for every event `type`,
which of the six `dat` fields it reads and what each one **means**. This is what
lets the editor show meaning ("Load enemy art: banks 3, 7") instead of a raw row
of eight numbers, and it's the source of truth for the editor's per-type field
schema (`src/lvledit.c`).

Companion to [LEVEL_EDITOR_PLAN.md](LEVEL_EDITOR_PLAN.md) (which documents the
`tyrian?.lvl` on-disk container). Scope here is the *semantics* of one event
record's fields, not the file container.

## Provenance / how to regenerate

Extracted directly from the `switch (eventRec[eventLoc-1].eventtype)` in
`JE_eventSystem()` — `src/tyrian2.c`, roughly lines 4599–5457. If the switch is
ever edited, re-read those case bodies and update the affected rows here; every
entry below corresponds to exactly one `case N:` label. When a field's meaning
is unclear from this table, the case body in that switch is authoritative.

## One event record

On disk (little-endian, **11 bytes**, this exact order — see LEVEL_EDITOR_PLAN):

    u16 time, u8 type, s16 dat, s16 dat2, s8 dat3, s8 dat5, s8 dat6, u8 dat4

Note the disk order interleaves `dat5`/`dat6` before `dat4`. The editor works in
a **logical field-id order** instead — `0=time, 1=type, 2=dat, 3=dat2, 4=dat3,
5=dat4, 6=dat5, 7=dat6` (see `event_field_get/set/range` in `src/lvledit.c`); the
serializer maps logical→disk. Keep the two orders straight: the table below names
fields `dat`..`dat6` logically.

`time` is the level's scroll position at which the event fires (not wall-clock);
event dispatch (`eventLoc` walk) assumes records are stored in non-decreasing
`time` order.

## Field conventions seen throughout

- **`-99` sentinel** on `dat`/`dat2` (and sometimes `dat6`) in the
  Enemy-Global-\* cases = "leave this enemy field unchanged."
- **`dat3` (or `dat4`) value 80–89** in the Enemy-Global-\* cases (19, 20, 27,
  55) = read the real linknum *indirectly* from `newPL[dat3-80]`, a scripting
  "variable slot" written by type 75 (`RAND LINK PICK`).
- Several cases **write back** into the record at runtime (noted per-type below):
  type 12 zeroes `dat6`; the Enemy-Global indirect cases overwrite `dat4`; type
  33/45 rewrite `dat` for arcade/superTyrian; types 49–52 temporarily mutate then
  restore. The editor treats all fields as plain stored values — these runtime
  mutations don't affect what's saved.

### Enemy-spawn helper

Spawn cases call `JE_createNewEventEnemy(enemyTypeOfs, enemyOffset,
uniqueShapeTableI)` (`src/tyrian2.c:4487`). It does **not** take the dat fields as
args — it reads them off the current record:

| field | meaning in a spawn |
|---|---|
| dat  | enemy type index (`+ enemyTypeOfs`) |
| dat2 | X spawn position (`-99` = keep default / off-screen) |
| dat3 | added to `enemy.eyc` (Y velocity) |
| dat4 | `enemy.linknum` (group/link id) |
| dat5 | added to `enemy.ey` (Y position offset) |
| dat6 | `enemy.fixedmovey` (fixed movement pattern id) |

`enemyOffset` picks the layer/lane: `0`=Sky (bg2), `25`=Ground (bg1), `50`=Top
(bg3), `75`=Ground2 (bg1). Result index is left in the global `b` (`0` = spawn
failed, table full).

## Per-type field map

Types are grouped by role. "no fields" = the case reads none of `dat`..`dat6`.
Types **58, 59** are not handled by the switch; the `default` case warns and
reads nothing.

### Background / scrolling / starfield

| type | name | fields |
|---|---|---|
| 1  | starfield speed | dat: speed |
| 2  | bg scroll speeds (+reset delay) | dat: bg1 speed · dat2: bg2 speed (also drives explode-move if >0) · dat3: bg3 speed |
| 3  | reset bg scroll to defaults | no fields |
| 4  | stop backgrounds | dat (enum): 0/1→layer 1, 2→layer 2, 3→layer 3 |
| 8  | starfield off | no fields |
| 9  | starfield on | no fields |
| 21 | background3over = 1 | no fields |
| 22 | background3over = 0 | no fields |
| 28 | topEnemyOver = false | no fields |
| 29 | topEnemyOver = true | no fields |
| 30 | bg scroll speeds (variant, no guard) | dat: bg1 · dat2: bg2 (also explode-move) · dat3: bg3 |
| 42 | background3over = 2 | no fields |
| 43 | set background2over | dat: value |
| 44 | screen filter / brightness | dat (enum): >0 on, 2 also fade · dat2: filter · dat3: brightness · dat4: target filter · dat5: brightness step · dat6 (enum): 0→fade-start |
| 48 | background2 not transparent | no fields |
| 65 | background3x1 toggle | dat (enum): 0→on else off |
| 72 | background3x1b toggle | dat (enum): 1→on else off |
| 73 | skyEnemyOverAll toggle | dat (enum): 1→on else off |
| 77 | set bg1/bg2 scroll pointers | dat: bg1 scroll offset · dat2: bg2 scroll offset (>0, else follows dat) |
| 81 | WRAP2 (bg2 wrap region) | dat: wrap start offset · dat2: wrap end offset |

### Enemy spawning

All read the six spawn fields via the helper above unless noted.

| type | name | notes |
|---|---|---|
| 5  | load enemy shape banks | dat,dat2,dat3,dat4: sprite-sheet bank ids for slots 0–3 (>0 load, 0 free) |
| 6  | spawn Ground enemy | offset 25 |
| 7  | spawn Top enemy | offset 50 |
| 10 | spawn Ground enemy 2 | offset 75 |
| 12 | custom 4×4 ground enemy (2×2 block of 4) | dat6 (enum, **written to 0**): 0/1→Ground, 2→Sky, 3→Top, 4→Ground2 · dat=type base, dat2=X, dat3=Yvel, dat4=link, dat5=Yoff |
| 15 | spawn Sky enemy | offset 0 |
| 17 | spawn Ground Bottom enemy | offset 25; then `ey = 190 + dat5` (dat5 = ground Y offset) |
| 18 | spawn Sky enemy on bottom | offset 0; then `ey = 190 + dat5` |
| 23 | spawn Sky enemy on bottom (top variant) | offset 50; then `ey = 180 + dat5` |
| 32 | create enemy (Top, forced bottom) | offset 50; then `ey = 190` (dat5 ignored) |
| 49–52 | custom ground-enemy variant | offset = type−48 (49 Ground,50 Sky,51 Top,52 Ground2) · dat: custom graphic id · dat6: custom armor · dat3: custom shape-table index (3rd helper arg) · dat2=X, dat4=link, dat5=Yoff (dat/dat3/dat6 restored after; net no-op) |
| 56 | spawn Ground2 Bottom enemy | offset 75; then `ey = 190` (dat5 ignored) |

### Enemy-global modifiers (affect a group by linknum)

`dat4` is the linknum filter throughout (`0` or `99` often = "all"); `dat3`
80–89 = indirect linknum via `newPL`.

| type | name | fields |
|---|---|---|
| 13 | enemies inactive | no fields |
| 14 | enemies active | no fields |
| 19 | enemy global move | dat3 (enum scope): 80–89 indirect · 0 match linknum · 2 all 0–24 · 1 all 25–49 · 3 all 50–74 · 99 all 0–99 · dat4 linknum · dat X-vel (≠−99) · dat2 Y-vel (≠−99) · dat6 fixedmovey (−99→0) · dat5 enemycycle (>0) |
| 20 | enemy global accel | dat3 80–89 indirect · dat4 linknum · dat X-accel (≠−99) · dat2 Y-accel (≠−99) · dat5 enemycycle/animin · dat6 >0 starts animation (ani=dat6, animin=dat5) |
| 24 | enemy global animate | dat4 linknum (exact) · dat2 enemycycle/animin (>0 else 0) · dat ani (>0) · dat3 (enum): 1 freeze/loop, 2 animate-on-fire |
| 25 | enemy global damage (armor, galaga-scaled) | dat4 linknum (0=all) · dat armor (scaled by difficulty in galagaMode) |
| 27 | enemy global accel-rev + color filter | dat3 80–89 indirect, and if 1–16 also `enemy.filter` (color) · dat4 linknum · dat exrev (≠−99) · dat2 eyrev (≠−99) |
| 31 | enemy fire override | dat4 linknum (99=all) · dat freq[0] · dat2 freq[1] · dat3 freq[2] · dat5 launchfreq |
| 33 | enemy-from-other-enemies (death drop) | dat: enemy-to-spawn-on-death id (rewritten in arcade/superTyrian) · dat4 linknum |
| 39 | enemy linknum change | dat: old linknum · dat2: new linknum |
| 40 | enemyContinualDamage = true | no fields |
| 41 | reset enemy-availability table | dat (enum): 0→all 100 slots, else→0–24 only |
| 45 | arcade-only enemy-from-other-enemies | like 33 (arcade only) · dat4 linknum |
| 47 | enemy global armor set (direct) | dat4 linknum (0=all) · dat armor (no scaling) |
| 55 | enemy global accel (direct linear) | dat3 80–89 indirect · dat4 linknum (0=all) · dat xaccel (≠−99) · dat2 yaccel (≠−99) |
| 60 | assign special-enemy flag | dat4 linknum (exact) · dat flagnum · dat2 (enum): 1→setto=true else false |
| 74 | enemy global bounce params | dat4 linknum (0=all) · dat5 xminbounce · dat6 yminbounce · dat xmaxbounce · dat2 ymaxbounce · **dat3 unused** (only global-\* case that skips dat3) |
| 75 | pick random enemy variant → newPL slot | dat linknum range low · dat2 range high · dat3 newPL slot (index dat3−80) · dat4 events to skip if none available |
| 79 | set boss health-bar link numbers | dat: boss bar 0 linknum · dat2: boss bar 1 linknum |

### Level flow / conditionals / jumps

| type | name | fields |
|---|---|---|
| 11 | end level trigger | dat (enum): 1→force immediate end, else→normal end sequence |
| 36 | readyToEndLevel = true | no fields |
| 37 | set enemy spawn frequency | dat: levelEnemyFrequency |
| 38 | jump event cursor to level-time | dat: target level time (curLoc) |
| 53 | forceEvents toggle | dat (enum): 99→off else on |
| 54 | event jump | dat: jump target (65535 = return to returnLoc) |
| 57 | set superEnemy254Jump | dat: value |
| 61 | conditional skip on global flag | dat: globalFlags index (1-based) · dat2: compare value · dat3: events to skip if equal |
| 63 | skip events unless 2-player | dat: events to skip |
| 66 | conditional skip on difficulty | dat: threshold · dat2: events to skip if initialDifficulty ≤ dat |
| 67 | level timer setup | dat (enum): 1→timer on · dat2: jump-to on expiry · dat3: countdown (×100 ticks) |
| 70 | conditional jump on enemy-type presence | dat: jump target · dat2 (mode): 0→any type 1–19 present, else specific types dat2/dat3/dat4 · dat3,dat4: extra enemy types |
| 71 | conditional jump on map scroll position | dat: jump target · dat2: map-Y threshold |
| 76 | returnActive = true | no fields |
| 78 | increment galagaShotFreq | no fields |
| 80 | skip events if 2-player | dat: events to skip |

### Presentation / audio / pickups / misc

| type | name | fields |
|---|---|---|
| 16 | show text window | dat: text-window index 1–10 |
| 26 | smallEnemyAdjust | dat: value (small-enemy sprite anchor) |
| 34 | start music fade | no fields |
| 35 | play new song | dat: song number |
| 46 | change difficulty | dat: delta · dat2 (gate): 0 or 2P/action to apply · dat3: damageRate (≠0) |
| 62 | play sound effect | dat: sound id |
| 64 | set "smoothie" data | dat: smoothie index (1-based) · dat2: value · dat3: extra value |
| 68 | randomExplosions toggle | dat (enum): 1→on else off |
| 69 | set player invulnerability | dat: invulnerable ticks |
| 82 | give special weapon | dat: special weapon id |

## Editor schema built on this

`src/lvledit.c` turns the table above into a per-type field schema (labels +
enum decodes) that drives the event editor's readable list and right-sidebar
inspector. Types not covered here (58, 59, anything the schema author left
unmapped) fall back to raw `dat`..`dat6` rows so every field stays editable.
When adding named support for a new/changed type, update **both** this doc and
that schema.
