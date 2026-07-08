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

## 2. Sprite tables (`Sprite`) — ✅ fonts + logo wired; menu sprites extracted

Loaded by `JE_loadMainShapeTables("tyrian.shp")` (or `tyrianc.shp` at Xmas),
`opentyr.c:796`. Transparency = color 0 → real alpha.

| Table | `sprite.h` id | Contents | Upscaler | HD status |
|--:|---|---|:--:|:--:|
| 0 | `FONT_SHAPES` | large font | X | ✅ wired (HD font rendering, `hd_font_emit`) |
| 1 | `SMALL_FONT_SHAPES` | small font | X | ✅ wired |
| 2 | `TINY_FONT` | tiny font | X | ✅ wired |
| 3 | `PLANET_SHAPES` | title logo, planets, cube | X | ✅ logo `#146` live; planets wired (dormant, §note) |
| 4 | `FACE_SHAPES` | menu portraits | X | ⬜ extracted (per-face palette); wiring needs persistent overlay |
| 5 | `OPTION_SHAPES` | option/help icons | X | 🔨 wired but dormant (shop has no HD backdrop) |
| 6 | `WEAPON_SHAPES` | weapon icons | X | ⬜ extracted; one-shot draw → needs persistent overlay |
| 7 | `EXTRA_SHAPES` | ending pics (`estsc.shp`) | X | ⬜ extracted |

**Fonts (was 🎨 redraw): done via xBRZ brightness maps + runtime recolor**, not a
TTF redraw — glyphs are recolored per draw call (`hue<<4 | (brightness+value)`) to
match `blit_sprite_hv`. HD text shows on all full-screen VGA screens (title,
menus, shop, interlude); in-flight HUD stays classic by design (composite/interp
can't be reproduced by a full-screen quad safely). See
[`REMASTER_FLIGHT_COMPOSITOR.md`](REMASTER_FLIGHT_COMPOSITOR.md) for the shared
recolor-synthesis approach.

**Menu-sprite dormancy:** OPTION/PLANET menu sprites are wired but only composite
where an HD backdrop is active; the shop screen (`JE_itemScreen`) has none, so
they fall back to classic today. FACE/WEAPON draw once per menu-state (not per
frame) → need a *persistent* HD overlay (current overlay is immediate-mode).
Both are follow-ups; assets are ready.

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

## 4. Large PCX images — ➖ mostly not engine-hooked

Extracted by `hd_extract.py` (each file's own embedded PCX palette, no 6→8
expansion, Lanczos 4x → `hdpcx_<name>.dat`). **Investigation finding:** 7 of 8
are **not loaded by the OpenTyrian engine at all** — they're assets from separate
standalone DOS utilities in the Tyrian 2.1 distro (`SHIPEDIT.EXE`,
`NETARENA.EXE`, etc.) that were never ported. OpenTyrian's own ship editor
(`editship.c`) builds its UI from sprites, no PCX. Infra to composite full-screen
HD assets exists (`hd_set_backdrop_asset`), but there are no call sites to wire.

| File | Where | Wireable? | Status |
|---|---|:--:|:--:|
| `tshp2.pcx` | interlude image (`JE_loadPCX`, `tyrian2.c:2804`) | in a generic script branch, no clean enter/exit | ✅ extracted · wiring deferred (leak risk) |
| `shipedit.pcx` | — | ➖ not loaded (DOS `SHIPEDIT.EXE` asset) | extracted, dead |
| `tyrset.pcx` | — | ➖ not loaded (DOS setup tool asset) | extracted, dead |
| `netarena/netset/netmega/netfont1-2.pcx` | — | ➖ not loaded (DOS net tool assets) | extracted, dead |

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
| Backdrops | 13 | ✅ wired | Lanczos placeholder (real AI upscaler unavailable on host) |
| Fonts | 3 tables / 272 glyphs | ✅ wired | HD text on all full-screen VGA screens; HUD classic by design |
| In-flight sprites | 39 sheets / 11,856 frames | ✅ wired | shots, ship, powerups, coins, explosions, enemies (plain+darken+blend+`_filter` hue) |
| Title logo | `PLANET_SHAPES` #146 | ✅ wired | xBRZ, gold |
| Cutscene | `tyrend.anm` (111 fr) | ✅ wired | HD streaming playback |
| Menu sprites | FACE/OPTION/WEAPON/PLANET | 🔨 extracted; partly wired | dormant (needs shop HD backdrop + persistent overlay) |
| Large PCX | 8 | ➖ | 7/8 not engine-hooked (DOS-tool assets); tshp2 deferred |

**State — HD wiring complete.** The gameplay (all in-flight sprites incl.
recolored enemies), title, backdrops, cutscene, and text render in HD; the whole
pipeline regenerates from `tools/hd_extract*.py`. Known follow-ups (assets ready):
menu-sprite dormancy (shop HD backdrop + persistent overlay for FACE/WEAPON
portraits), swapping backdrop/PCX Lanczos → a real AI upscaler when one is
available, and (optional) HD in-flight HUD text.
