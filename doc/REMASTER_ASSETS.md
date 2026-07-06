# OpenTyrian HD Remaster — Asset Tracker

> Companion to [`REMASTER_PLAN.md`](REMASTER_PLAN.md). The plan doc owns
> *architecture & phases*; **this doc owns the asset inventory & per-asset
> status** — what has been extracted, upscaled, packaged, and wired.
>
> Keep this in sync when an asset changes state. One row per asset (or asset
> group). Regenerate HD assets with `python3 tools/hd_extract.py`.

## Legend

Pipeline stages an asset moves through (see plan §Phase 2):

1. **Extract** — decode original format → RGBA (via palette / alpha-key)
2. **Upscale** — enlarge to HD (Lanczos placeholder → real model)
3. **Package** — write engine asset (`HDPX`/atlas) into the data dir
4. **Wire** — composited by the engine behind the `[video] hd` toggle

Status values: ✅ done · 🔨 in progress · ⬜ not started · ➖ n/a ·
🎨 *needs redraw, not upscale* (fonts/UI)

Upscaler tags: `L` = Lanczos placeholder · `E` = Real-ESRGAN (photo/anime) ·
`X` = xBRZ/hqx (pixel-art) · `H` = hand-drawn/redrawn

---

## 1. Full-screen backdrops — ✅ pipeline complete

`tyrian.pic`, 13 images, 320×200 8-bit indexed, palette via `pcxpal[]`
(`pcxmast.c`). Extracted/upscaled/packaged/wired end-to-end.

| # | Screen (where it shows) | Palette | Asset | Extract | Upscale | Package | Wire |
|--:|---|:--:|---|:--:|:--:|:--:|:--:|
| 1 | interlude / story | 0 | `hdpic01.dat` | ✅ | L | ✅ | ✅ |
| 2 | interlude | 7 | `hdpic02.dat` | ✅ | L | ✅ | ✅ |
| 3 | interlude | 5 | `hdpic03.dat` | ✅ | L | ✅ | ✅ |
| 4 | **title screen** | 8 | `hdpic04.dat` (+`hdtitle.dat`) | ✅ | L | ✅ | ✅ |
| 5 | episode/difficulty select | 10 | `hdpic05.dat` | ✅ | L | ✅ | ✅ |
| 6 | gameplay/ship select | 5 | `hdpic06.dat` | ✅ | L | ✅ | ✅ |
| 7 | setup / instructions | 18 | `hdpic07.dat` | ✅ | L | ✅ | ✅ |
| 8 | load-game | 19 | `hdpic08.dat` | ✅ | L | ✅ | ✅ |
| 9 | high-score table | 19 | `hdpic09.dat` | ✅ | L | ✅ | ✅ |
| 10 | high-score entry | 20 | `hdpic10.dat` | ✅ | L | ✅ | ✅ |
| 11 | destruct intro | 21 | `hdpic11.dat` | ✅ | L | ✅ | ✅ |
| 12 | interlude | 22 | `hdpic12.dat` | ✅ | L | ✅ | ✅ |
| 13 | interlude | 5 | `hdpic13.dat` | ✅ | L | ✅ | ✅ |

> **Follow-up:** all backdrops are still `L` (Lanczos placeholder). Re-run
> through the real upscaler (`E`) once the model is chosen — tooling-only change,
> no engine impact.

---

## 2. Sprite tables (`Sprite`) — 🔨 title logo done

Loaded by `JE_loadMainShapeTables("tyrian.shp")` (or `tyrianc.shp` at Xmas),
`opentyr.c:796`. Transparency = color 0 → must become real alpha.
See plan §Phase 2 Tracks A/B/C and §Phase 3 (recoloring).

| Table | `sprite.h` id | Contents | Blit modes used | Upscaler | Status |
|--:|---|---|---|:--:|:--:|
| 0 | `FONT_SHAPES` | large font | — | 🎨 H | ⬜ redraw |
| 1 | `SMALL_FONT_SHAPES` | small font | — | 🎨 H | ⬜ redraw |
| 2 | `TINY_FONT` | tiny font | — | 🎨 H | ⬜ redraw |
| 3 | `PLANET_SHAPES` | title logo, planets, cube | plain, blend | X | 🔨 logo (#146) done · rest extracted |
| 4 | `FACE_SHAPES` | menu portraits | plain | X | ⬜ (per-face `facepal[]` recolor) |
| 5 | `OPTION_SHAPES` | option/help icons | plain | X/H | ⬜ |
| 6 | `WEAPON_SHAPES` | weapon icons | plain | X | ⬜ |
| 7 | `EXTRA_SHAPES` | ending pics (`estsc.shp`, `mainint.c:2475`) | plain | E | ⬜ |

**Recommended first target (Track 2a):** `PLANET_SHAPES` #146 (title logo) and
`FACE_SHAPES` — static, plain-blit, no recolor → behave like backdrops, reuse
the `hd_set_backdrop`-style compositor. Fonts are 🎨 **redraw** (Phase 4), not
upscale.

---

## 3. Compiled sprite sheets (`Sprite2`) — ⬜ not started

RLE comp shapes (`JE_loadCompShapesB`, `sprite.c:501`). These are the in-flight
sprites — the invasive **display-list compositor** (plan Track 2b) and the bulk
of **recoloring parity** (Phase 3) live here.

| Sheet var | Source file(s) | Contents | Recolor? | Upscaler | Status |
|---|---|---|:--:|:--:|:--:|
| `spriteSheet8` | `tyrian.shp` blk | player shots | value/flash | X | ⬜ |
| `spriteSheet9` | `tyrian.shp` blk | player ships | **hue** (ship color) | X | ⬜ |
| `spriteSheet10` | `tyrian.shp` blk | power-ups | value | X | ⬜ |
| `spriteSheet11` | `tyrian.shp` blk | coins/pickups | value | X | ⬜ |
| `spriteSheet12` | `tyrian.shp` blk | misc weapons | value | X | ⬜ |
| `enemySpriteSheets[0..3]` | `shapes).dat`, `shapesw.dat`, `shapesx.dat`, `shapesy.dat`, `shapesz.dat` (per-level banks, `tyrian2.c:4355`) | enemies | value/dark | X/E | ⬜ |
| `shopSpriteSheet` | `newsh1.shp` (`game_menu.c:159`) | shop icons/arrows | plain | X | ⬜ |
| `explosionSpriteSheet` | `newsh6.shp` (`tyrian2.c:793`) | explosions | blend/add | E | ⬜ |
| `destructSpriteSheet` | `newsh~.shp` (`destruct.c:685`) | destruct minigame | plain | X | ⬜ |

> **Recolor column is the Phase-3 crux.** `hue` (ship/team color) = pre-bake the
> finite hue set OR HSV shader. `value`/flash/dark = `SDL_SetTextureColorMod`.
> `blend`/explosions = alpha/additive. Start Track 2b with plain+value only,
> defer hue.

---

## 4. Large PCX images — ⬜ not started

Loaded via `pcxload.c` / `JE_loadPCX`.

| File | Where | Upscaler | Status |
|---|---|:--:|:--:|
| `tshp2.pcx` | big interlude image (`tyrian2.c:2784`) | E | ⬜ |
| `shipedit.pcx` | ship editor bg | E | ⬜ |
| `tyrset.pcx` | setup screen | E | ⬜ |
| `netarena.pcx`, `netset.pcx`, `netmega.pcx`, `netfont1/2.pcx` | network UI | E / 🎨 | ⬜ |

---

## 5. Cutscenes (`.anm`) — ⬜ not started

| File | Where | Approach | Status |
|---|---|---|:--:|
| `tyrend.anm` | ending cutscene (`tyrian2.c:2500`, `animlib.c`) | upscale frames offline **or** runtime upscaler; treat as video | ⬜ |

---

## 6. Palettes / non-visual — ➖ n/a

`palette.dat` is the **source of truth** for indexed→RGB during extraction, not
an upscale target. `levels*.dat`, `cubetxt*.dat`, `*.snd` etc. are data/audio —
out of scope for asset upscaling.

---

## Roll-up

| Group | Assets | Done | Notes |
|---|:--:|:--:|---|
| Backdrops | 13 | ✅ 13/13 wired | all still Lanczos placeholder |
| Sprite tables | 8 | ⬜ 0/8 | 3 are font-redraw, not upscale |
| Comp sprite sheets | ~13 | ⬜ 0 | needs display-list compositor (Track 2b) |
| Large PCX | ~9 | ⬜ 0 | backdrop-like, low risk |
| Cutscenes | 1 | ⬜ 0 | separate treatment |

**Next asset action:** settle the upscaler model on a test batch (backdrops re-run
`E` + a sprite batch `E` vs `X` for halo/tiny-sprite quality), then Track 2a
(static plain sprites: title logo, faces, large PCX).
