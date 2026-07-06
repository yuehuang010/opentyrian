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

## 3. Compiled sprite sheets (`Sprite2`) — 🔨 extracted (wiring pending)

RLE comp shapes (`JE_loadCompShapesB`, `sprite.c:501`). These are the in-flight
sprites — the invasive **display-list compositor** (plan Track 2b) and the bulk
of **recoloring parity** (Phase 3) live here. Extracted offline by
`tools/hd_extract_comp.py` (`Sprite2` = `u16 offsets[]` header, 1-based; frames
are 12×14 RLE; `2x2` = tiles `i,i+1,i+19,i+20`). **11,856 HD frames** written as
`hdcomp_<sheet>_NN.dat` (48×56, xBRZ+alpha) + `hd_comp_manifest.json`. Wiring per
[`REMASTER_FLIGHT_COMPOSITOR.md`](REMASTER_FLIGHT_COMPOSITOR.md).

| Sheet var | Source file(s) | Contents | In-flight blit | HD status |
|---|---|---|:--:|:--:|
| `spriteSheet8` | `tyrian.shp` blk 7 | player shots | plain | ✅ extracted |
| `spriteSheet9` | `tyrian.shp` blk 8 | player ships | plain + `_darken`/`_blend` | ✅ extracted |
| `spriteSheet10` | `tyrian.shp` blk 9 | power-ups | plain | ✅ extracted |
| `spriteSheet11` | `tyrian.shp` blk 10 | coins/pickups | plain | ✅ extracted |
| `spriteSheet12` | `tyrian.shp` blk 11 | misc | plain | ✅ extracted |
| `enemySpriteSheets[0..3]` | **`newsh?.shp`** per-level via `shapeFile[]` (`tyrian2.c` evt 5, `lvlmast.c`) — 31 banks `enemy_<suffix>` | enemies | plain + **`_filter`** (16-band hue) | ✅ extracted |
| `shopSpriteSheet` | `newsh1.shp` (`game_menu.c:159`) | shop icons/arrows | plain (menu) | ✅ extracted |
| `explosionSpriteSheet` | `newsh6.shp` (`tyrian2.c:793`) | explosions | `_blend` | ✅ extracted |
| `destructSpriteSheet` | `newsh~.shp` (`destruct.c:685`) | destruct minigame | plain | ✅ extracted |

> **Correction:** enemy sheets are **`newsh?.shp`** loaded per-level, NOT
> `shapes*.dat` (those are level-tileset graphics, a different format).
>
> **Recolor (Phase-3 crux) — narrowed by the compositor design:** in-flight
> sprites use only 4 modes — plain, `_blend`, `_darken`, `_filter`. There is **no
> hue-rotation blit**; "ship hue" is plain frames, "enemy hue" is `_filter`'s
> 16-band nibble remap (bake band variants OR retain band structure for enemy
> banks only). Everything else = colormod/alpha/blendmode. See the design doc.

---

## 4. Large PCX images — 🔨 extracted (wiring pending)

Loaded via `pcxload.c` / `JE_loadPCX`. Extracted by `hd_extract.py` using each
file's own embedded PCX palette (no 6→8 expansion), Lanczos 4x → `hdpcx_<name>.dat`.

| File | Where | Upscaler | Status |
|---|---|:--:|:--:|
| `tshp2.pcx` | big interlude image (`tyrian2.c:2784`) | L | ✅ extracted |
| `shipedit.pcx` | ship editor bg | L | ✅ extracted |
| `tyrset.pcx` | setup screen | L | ✅ extracted |
| `netarena.pcx`, `netset.pcx`, `netmega.pcx`, `netfont1/2.pcx` | network UI | L | ✅ extracted |

---

## 5. Cutscenes (`.anm`) — 🔨 extracted (wiring pending)

Extracted by `tools/hd_extract_anim.py` (`.anm` = OpenTyrian page-based delta
container, NOT FLI/FLC; 256B header + 1024B BGR palette + page descriptors +
cumulative run/skip/dump deltas over a 320×200 framebuffer).

| File | Where | Approach | Status |
|---|---|---|:--:|
| `tyrend.anm` | ending cutscene (`tyrian2.c:2500`, `animlib.c`) | 111 frames → `hdanim_tyrend_NNNN.dat` (1280×800, Lanczos) | ✅ extracted |

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
| Sprite tables | 8 | 🔨 logo wired; 4 more extracted | title logo `#146` live; OPTION/WEAPON/EXTRA/FACE extracted, wiring pending; 3 fonts = redraw |
| Comp sprite sheets | 39 sheets / 11,856 frames | 🔨 extracted | wiring = display-list compositor (Track 2b), see design doc |
| Large PCX | 8 | 🔨 extracted | wiring pending, backdrop-like |
| Cutscenes | 1 (111 frames) | 🔨 extracted | wiring pending |

**State:** all offline extraction done (Phase A). Remaining: wire static assets
(Phase B), then the invasive in-flight compositor (Phase C, Track 2b) +
recoloring parity (Phase D) per
[`REMASTER_FLIGHT_COMPOSITOR.md`](REMASTER_FLIGHT_COMPOSITOR.md), then font
redraw. Backdrops still Lanczos (real AI upscaler unavailable on this host).
