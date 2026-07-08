---
title: Front Vulcan Cannon "fires left of center" recenter — REVERTED (was a misdiagnosis)
status: Reverted / Not-a-bug
component: gameplay / weapon data (src/episodes.c JE_loadItemDat)
affects: classic AND HD (data-level)
superseded-by: this file (the ed29a62 recenter was wrong and has been reverted)
---

# Front Vulcan Cannon recenter was a misdiagnosis — reverted

**Status: REVERTED.** Commit `ed29a62` added a block in `JE_loadItemDat`
(`src/episodes.c`) that zeroed `bx[]` for the front Vulcan's power weapons to
"recenter" it. That fix was based on a wrong reading of the weapon data and it
**broke the Vulcan's characteristic waving spray**. The block (and its
`#include <string.h>`) has been removed; stock data is restored.

## What the earlier fix got wrong

The `ed29a62` reasoning was: "the front Vulcan is `multi==1` at every power, so
`bx` is a pure per-shot offset; `bx[0]` is negative (-4..-8), so the stream fires
left of center; zero it to recenter." Two premises are both false:

1. **`bx` is NOT a single offset — it is an 8-entry sweep table indexed by
   `shotMultiPos`, not by `multi`.** In `player_shot_create` (`src/shots.c`), each
   spawned shot does `shot->shotX = PX + weapon->bx[shotMultiPos-1]` and
   `shot->shotXM = weapon->sx[shotMultiPos-1]`, where `shotMultiPos` cycles
   `1..weapon->max` on every shot (`shots.c:351-354`). So consecutive shots walk
   across `bx[]`/`sx[]`, tracing a woven left-right ribbon — **even when
   `multi==1`** (`multi` only controls how many shots spawn per trigger; the
   per-shot index still advances). The earlier fix inspected only `bx[0]` (the
   *leftmost sample* of the sweep) and mistook it for a constant offset.

2. **The data is already symmetric / centered on average.** Parsed directly from
   `tyrian21/tyrian.hdt` (record layout in `JE_loadItemDat`; `bx` at record
   offset +42, `sx` at +26, 80-byte records after the 14-byte `itemNum[7]`
   header at `episode1DataLoc`):

   | weap | multi | max | bx[] | sx[] | sum(bx) |
   |------|-------|-----|------|------|---------|
   | 221 (pow1) | 1 | 2 | (-4, 4) | (0,0) | 0 |
   | 223 | 1 | 4 | (-6, 0, 6) | (-2, 0, 2) | 0 |
   | 226 | 1 | 8 | (-8,-4,0,4,8,4,0,-4) | (-2,-1,0,1,2,1,0,-1) | 0 |
   | 231 | 1 | 8 | (-8,-4,0,4,8,4,0,-4) | (-2,-1,0,1,2,1,0,-1) | 0 |

   Every `bx[]` sums to 0 — the stream is symmetric about the ship centerline.
   The Vulcan does **not** fire left of center on average; it sweeps side to side.

## The actual reported bug (this dogfooding round)

"Front Vulcan Cannon fires from the center in the ship shop but doesn't wave."
That is precisely the *symptom of the ed29a62 fix*: zeroing all 8 `bx[]` collapsed
the sweep, so at power 1 (where `sx` is all-zero) every shot spawns dead center
and goes straight up — no wave. Reverting restores the sweep in both the shop
preview and gameplay.

The wave is **entirely data-driven** (the `bx[]`/`sx[]` sweep with a stationary
ship), not produced by the ship moving. The shop parks the ship
(`JE_initWeaponView`, `game_menu.c`, `player[0].x = 72`, `delta_x_shot_move = 0`)
and the wave still shows — because the sweep is in the weapon data. (The earlier
version of this note claimed the opposite; that claim was wrong.)

## The revert

Removed from `src/episodes.c`: the `// Recenter the front Vulcan Cannon's shots`
loop that followed the `weaponPort` loading loop, plus the now-unused
`#include <string.h>`. No other change.

## If a genuine off-center still shows in HD

It is **not** in the weapon data (verified symmetric above). Look at the HD
sprite-centering / compositor path — the `5241cff` compositor batch and the shot
sprite `+1` offsets (e.g. `shots.c:117`) — not `JE_loadItemDat`. Note the
`5241cff` change to `simulate_player_shots` that mirrors the `shotXM > 100` X-wrap
onto the in-flight path is unrelated to the Vulcan (it fixes the Laser `sx=101`
and Zica `sx=120` shop previews) and was left in place.
