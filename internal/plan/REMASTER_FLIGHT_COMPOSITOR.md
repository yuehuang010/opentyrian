# HD In-Flight Sprite Compositor + Recoloring Parity — Design

> Blueprint for **Phase 2 Track B** (in-flight display-list compositor) and
> **Phase 3** (recoloring parity) of the HD remaster. Companion to
> [`REMASTER_PLAN.md`](REMASTER_PLAN.md) (architecture/phases) and
> [`REMASTER_ASSETS.md`](REMASTER_ASSETS.md) (asset inventory). Derived from a
> read-only architecture pass over `interp.c`, `sprite.c`, `tyrian2.c`.

## 0. The two load-bearing discoveries

**(A) The capture layer already exists.** Phase 6 (`src/interp.c`) already
intercepts *every* in-flight playfield draw at the blit choke points and records
it into a display list of `Op` structs, each carrying exactly what an HD emit
needs: `{type, variant (which blit fn), filter byte, key (entity tag+occurrence),
x, y, s2 (the Sprite2_array, whose .data identifies the sheet), index}`.
`interp_draw()` re-issues those ops at **interpolated** positions. The HD
in-flight compositor is **a second consumer of this existing list**, not a new
capture system. This retires the capture and interpolation design questions
together.

**(B) In-flight `Sprite2` sprites use only FOUR recolor modes — no hue rotation.**
Every gameplay `blit_sprite2*` call site (`shots.c`, `tyrian2.c` `blit_enemy`/
explosions, `mainint.c` player ship) uses only `blit_sprite2` (plain),
`blit_sprite2_blend` (translucent/explosions), `blit_sprite2_darken` (shadow),
and `blit_sprite2_filter` (color-band remap), plus `_clip`/`2x2` fan-outs.
**There is no `blit_sprite2_hv`.** The player ship (`spriteSheet9`) draws
`2x2` / `_darken` / `_blend` — plain frames, no hue blit. The "hue" the asset
tracker names is `_filter`'s op: `*pixels = filter | (*data & 0x0f)` — replace
the high nibble (Tyrian's 16 hue bands), keep the low nibble (brightness),
driven by `enemy[i].filter`. So in-flight recoloring is a **closed 4-mode
problem**, not open-ended HSV.

## 1. Capture — reuse the interp display list (not a parallel list, not VGA diff)

- **VGA diff — rejected:** the 8-bit playfield is flattened with blend/darken/
  filter already baked in and z-order lost; per-sprite identity is unrecoverable.
- **Parallel explicit list — rejected as redundant:** `interp.c` already builds
  exactly this, entity-tagged, in draw order, with prev/curr interpolation pairs.
- **Intercept at `blit_sprite2*` — recommended, realized through the existing
  interp hooks.** Extend the *consumer* (`interp_draw`/dispatch), not the sites.

**Required change to capture outside high-fps mode:** recording is gated on
`highfps_mode` (`interp_record_begin`). Generalize to
`highfps_mode || (hd_mode && hd_flight_active)`. With `hd_mode` on and `highfps`
off, capture the tick list and drive one HD present per tick at `alpha=1.0`
(exact tick positions). With both on, HD sprites ride the interpolated `dx,dy`
`interp_draw` already computes.

**Do NOT suppress the indexed blit in flight** (opposite of the menu
`hd_set_sprite` model): `_darken`/`_blend` read *destination* pixels, so the
8-bit blit must run for a correct composited base and pixel-exact fallback. The
HD sprite draws on top at higher res, fully covering the silhouette; the low-res
sprite underneath is the natural per-sprite fallback. Suppression would punch
index-0 holes that break neighbouring sprites' destination reads. Flight capture
is **additive-overlay, never subtractive.**

**Sheet identity:** `op->s2.data` is a stable pointer. Add
`hd_sheet_id_for(const Uint8 *data)` in `sprite.c` comparing against the sheet
globals (`spriteSheet8..12`, `explosionSpriteSheet`, `shopSpriteSheet`,
`enemySpriteSheets[0..3]`). That stem + `op->index` keys the HD atlas lookup.
**Note (corrected during extraction):** the `enemySpriteSheets[0..3]` are runtime
slots filled *per level* from `newsh?.shp` files via `shapeFile[]`
(`tyrian2.c` event type 5, `lvlmast.c`) — NOT a static 4-file map to
`shapes*.dat` (those are level-tileset graphics, a different format/loader). So
`hd_sheet_id_for` must map the loaded sheet back to its `newsh` source; the HD
atlas is keyed by `newsh` file (extractor emits `hdcomp_enemy_<suffix>_NN.dat`
for all 31 enemy banks), and the runtime resolves which bank each slot holds.

## 2. Draw integration in `scale_and_flip`

**Coordinate chain (subtle):** Op positions are in `game_screen` space; the
visible playfield is 264×184 starting at column **24** (`JE_starCompositeShow`,
`tyrian2.c:91`). So:

```
logical VGA x = op.x - 24 ;  logical VGA y = op.y   (clip to [0,264) x [0,184))
window x = dst_rect.x + (op.x - 24) * dst_rect.w / vga_width
window y = dst_rect.y +  op.y       * dst_rect.h / vga_height
```

Reuse the `hd_sprite_queue` rect math (`video.c`), with the −24 x-shift and a
per-op source rect into the sheet atlas.

**New present branch (gameplay), before the backdrop branch** — gameplay has a
scrolling background, no PIC backdrop, so `hd_backdrop_active` doesn't apply:

```
if (hd_mode && hd_flight_active) {
    // 1. base playfield: software-scale 8-bit VGAScreen -> main_window_texture,
    //    RenderCopy to dst_rect (the classic tail path).
    // 2. HD flight sprites on top, in list order (z-order preserved):
    for each entry in hd_flight_queue:
        RenderCopy(sheet_atlas[entry.sheet], &entry.src_rect, &entry.window_rect)
        with per-entry blendmode/colormod
    // 3. (deferred) re-draw index-0-keyed in-playfield HUD glyphs on top.
    apply_crt_overlay(&dst_rect); RenderPresent; return;
}
```

Background/starfield stay in the 8-bit base layer this phase (Track B is
*sprites*). Keep the 16-slot menu `hd_sprite_queue` as-is; the flight queue is a
**separate atlas-backed queue** (`hd_flight_queue`, capacity in the hundreds)
whose entries add `sheet id`, `src_rect`, `blendmode`, `colormod`.

**Emit from `interp_draw()`** (where interpolated `dx,dy` are known and the list
is walked per presented frame):

```
if (hd_mode) {
    int sheet = hd_sheet_id_for(op->s2.data);
    if (sheet >= 0 && hd_flight_lookup(sheet, op->index))
        hd_flight_set(sheet, op->index, op->variant, op->filter, dx - 24, dy);
}
blit_sprite2...(game_screen, dx, dy, ...);   // 8-bit base still drawn, no suppression
```

Non-high-fps HD runs the same list once at `alpha=1`. Route flight present
through an interp-owned function that clears the flight queue, replays the list,
composites, presents. Neither hd nor highfps on → classic `JE_starShowVGA`
untouched (byte-identical).

## 3. Recoloring parity table (the crux)

| Classic blit | 8-bit op | HD SDL2 mechanism | Pre-bake? |
|---|---|---|---|
| `blit_sprite2` (plain) | copy opaque | `RenderCopy` + `BLENDMODE_BLEND`, colormod (255,255,255) | No |
| `blit_sprite2_darken` (shadow) | `dest_low /= 2` in silhouette | draw frame's alpha mask black ~50%: `ColorMod(0,0,0)` + `AlphaMod(~128)` + BLEND | No (reuse plain frame alpha) |
| `blit_sprite2_blend` (translucency/explosions) | `dest_low=(dest_low+sprite_low)/2` | `BLENDMODE_BLEND` ~50% (`AlphaMod ~128`); consider `BLENDMODE_ADD` for explosions (start alpha, add as tuning knob) | No |
| `blit_sprite2_filter` ("hue" band remap) | `*pixels = filter \| (sprite_low)` | **not doable with SDL2 colormod** (multiply-only) — see below | **Yes / special** |
| `_hv` value (HUD text only in flight) | `low += value` clamp | `ColorMod(f,f,f)` | No |
| `JE_filterScreen` band+brightness (whole playfield) | band replace + brightness | leave in 8-bit base layer initially | No |

**The `_filter` case — recommended: on-demand recolored-frame cache keyed
`(sheet, index, filter)`.** For exact parity the HD asset for filter-using sheets
must retain the **band structure** (HD paletted image, or RGBA + a low-res band
map). At load, synthesize the filtered RGBA once per distinct `(index,filter)`
actually seen and cache it as a small static texture. `enemy[i].filter` is stable
across an enemy's life, so the cache stays tiny (tens of entries). Fallback if
band data isn't retained: **pre-bake 16 band variants for the enemy sheets only**
(`enemySpriteSheets`, the only in-flight `_filter` users). Player/powerup/coin
sheets are plain/blend/darken → **no filter pre-bake**. Ship "hue" needs nothing
special (plain frames).

## 4. Interpolation

Solved by emitting from `interp_draw()`: it blends prev/curr tick positions per
op and runs once per presented frame. `hd_flight_set` gets `dx-24, dy`, so HD
sprites ride the interpolated path in lockstep. Prev/curr pairing
(`op->key = tag<<10 | occurrence`) and `MAX_INTERP_JUMP` snap apply for free. HD
sprites are not framebuffer-read effects, so they interpolate cleanly (look
*better* than the tick-rate 8-bit base).

## 5. Fallback + toggle

- **Per-sprite:** `hd_flight_set` only fires on an atlas hit; the 8-bit blit is
  never suppressed → a missing HD frame shows the upscaled classic sprite. No
  holes, no crash.
- **Missing sheet/atlas:** `load_hd_sheet` warns once, marks failed, returns
  null → all its sprites fall back (mirror `load_hd_backdrop`).
- **`hd_mode` off:** recording gate excludes it → byte-identical classic path.
  hd-on/highfps-off → exactly one HD present per tick at `alpha=1`.
- **Hard bail to scaled 8-bit (no HD overlay) when:** `starShowVGASpecialCode != 0`
  (mirror/light levels transform the composite), `playerEndLevel`/paused/
  `skipStarShowVGA`, and 2p-mirror/networked frames (default bail if
  `starShowVGASpecialCode` set).

## 6. Performance / caching mandate

- **One `SDL_Texture` atlas per sheet**, uploaded once via `SDL_UpdateTexture` at
  load. **Never** `CreateTextureFromSurface` per frame. Each op = one
  `RenderCopy` from an atlas src rect.
- Per-sheet **index → src `SDL_Rect`** table built at load from the extractor
  manifest.
- Filter variants: bounded lazy cache keyed `(sheet,index,filter)`, LRU-capped
  (~128).
- Budget: ~100–300 sprites/frame worst case; ~300 quads is trivial. SDL2
  (≥2.0.10) auto-batches consecutive `RenderCopy`s sharing texture+blendmode+
  colormod — track current mod state and only re-set on change (avoid churn).

## 7. Asset-pipeline requirements (feedback to the extractor)

1. **Atlas + manifest per in-flight sheet** (`hdcomp_<sheet>` + index→src-rect):
   `sheet8..12`, `explosion`, the 31 `enemy_<suffix>` banks (from `newsh?.shp`),
   `shop`, `destruct`. (Extractor currently emits per-frame HDPX + a manifest;
   an atlas pack is a later optimization — see §6.)
2. **Clean color-0 → alpha edges** (critical: darken uses alpha as shadow mask).
3. **Band-structure retention for `_filter` sheets (enemy banks + any filtering
   shot sheet):** HD paletted or RGBA + band map, so exact 16-band remap is
   reproducible; OR pre-bake 16 band variants for those sheets only. **No hue
   pre-bake for ship/powerup/coin sheets.**
4. Frame cell geometry must match classic (`Sprite2` 12×14 base; `2x2` =
   indices `i, i+1, i+19, i+20`).

## 8. Phased, reviewable implementation checklist

Each step independently A/B-verifiable via the headless attract-mode demo +
`SDL_RenderReadPixels` at `scale_and_flip` (see `CLAUDE.md` §Debugging).

1. **Plumbing, no visual change.** `hd_flight_active`, `hd_flight_queue`,
   `hd_flight_begin/set/clear`, `load_hd_sheet` (atlas cache) + src-rect table in
   `video.c`; `hd_sheet_id_for()` in `sprite.c`; generalize the record gate; add
   the `scale_and_flip` flight branch presenting the scaled base with an **empty**
   queue (must be pixel-identical to classic — verify readback equality).
2. **One sheet, plain: player shots (`spriteSheet8`).** Smallest real win.
3. **Player ship (`spriteSheet9`) plain + `_darken` shadow.**
4. **Enemies (`enemySpriteSheets`) plain** — highest sprite count; validate
   atlas/batching/perf here.
5. **Blend + explosions (`explosionSpriteSheet`, `_blend`).**
6. **`_filter` parity (enemy hue bands)** — the Phase-3 crux; gate behind its own
   sub-flag until A/B-clean.
7. **Interpolation join** (highfps on) — confirm HD rides interpolated positions.
8. **HUD-on-top overlay** (optional polish) if occlusion shows.
9. **Guards + toggle matrix** — `starShowVGASpecialCode` bail, hd-off byte-
   identical, hd-on/highfps-off single present, 2p.

## 9. Top regression risks + verification

| Risk | Why | Verify |
|---|---|---|
| −24 shift / 264×184 clip wrong | Op coords are `game_screen` space | readback: HD vs 8-bit sprite centroid must coincide |
| Classic path drift (hd off, or empty queue) | new branch in the present choke point | readback-equality: hd-off vs baseline; hd-on/empty-queue vs classic |
| `_filter` hue wrong | colormod can't rotate hue | A/B harness; sample enemy band pixels vs classic |
| darken/blend fallback breaks if suppression added | tempting optimization | keep additive-overlay; assert no index-0 holes in `game_screen` |
| interp pairing mismatch HD vs 8-bit | lookup miss could desync | highfps on: HD and 8-bit silhouettes move together (readback deltas) |
| per-frame texture creation slips in | hundreds of sprites | assert atlas textures created only at load; `CreateTexture` calls = 0 in present loop |
| mirror/light levels (`starShowVGASpecialCode`) | HD quads ignore composite transform | force those levels; confirm HD bails to scaled base |

## Critical files
- `src/video.c` — gameplay HD present branch, `hd_flight_*` queue, `load_hd_sheet`
  atlas cache, src-rect tables
- `src/interp.c` — emit HD draws from `interp_draw`/dispatch at interpolated
  positions; generalize the recording gate
- `src/sprite.c` — `hd_sheet_id_for`; the `blit_sprite2*` variants + `2x2` frame
  geometry the atlas must match
- `src/tyrian2.c` — flight present seam, `JE_starCompositeShow` coord offset +
  `starShowVGASpecialCode` guards, enemy/explosion draw + `enemy.filter`
- `tools/hd_extract*.py` — per-sheet atlas + manifest, band-structure retention
  for `_filter` sheets, color-0 alpha edges
