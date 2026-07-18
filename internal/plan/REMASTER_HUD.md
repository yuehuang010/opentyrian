# REMASTER_HUD — HD vectorized in-flight HUD

Status: **planned 2026-07-17** — phases H0–H2 in progress, H3–H4 later.
Reverses the "in-flight HUD stays classic by design" deferral recorded in
`REMASTER_ASSETS.md`. Goal: the flight sidebar/status panel renders at native
output resolution — HD panel art plus **procedurally drawn (vector) dynamic
elements** — while the 8-bit HUD keeps drawing underneath as the always-correct
fallback. "HD skin, plays identical": zero game-logic changes.

## Current state (verified 2026-07-17)

- Panel art: `JE_loadPic(VGAScreen, twoPlayerMode ? 6 : 3, false)` at
  `tyrian2.c:816` — PIC #3 (1P) / #6 (2P) from `tyrian.pic`, baked into
  `VGAScreenSeg` once per level. **HD versions already exist**: `hdpic03.dat` /
  `hdpic06.dat` in the data dir (same asset family `hd_set_backdrop` loads).
- Flight present: `scale_and_flip()` flight branch (`video.c:2230`) draws the
  scaled 8-bit base, then clips the HD sprite overlay to the 264×184 playfield
  (`video.c:2235`) — the sidebar strip (VGA x∈[264,320)) and bottom bar
  (y∈[184,200)) get nothing but nearest-neighbor upscale today.
- Dynamic classic draws (all into `VGAScreenSeg`, all geometry except icons):
  - Shield bar `JE_drawShield` → `JE_dBar3(surf, 270, 194, shield, 144)`
    (`varz.c:1112`, `nortvars.c:31`); max-shield marker `varz.c:1126`.
  - Armor bar `JE_drawArmor` → `JE_dBar3(surf, 307, 194, armor, 224)`
    (`varz.c:1130`).
  - `JE_dBar3` semantics: segments are 9px-wide rects (x..x+8), 1px tall rows
    at y-1..y, stepping y by −2 per unit; color starts `col+2` and increments
    every 2 segments (a brightness ramp within the 16-color bank).
  - Main power bar: inline `tyrian2.c:1288-1300`, filled rect x269..276.
  - Weapon-power dots: inline `tyrian2.c:1256-1276`, `115+j` colored 2×3 rects
    at x≈286/289.
  - Sidekick icons + ammo gauges: `JE_drawOptions` (`varz.c:396`) —
    `blit_sprite(OPTION_SHAPES)` icons at x=284 (real bitmaps) +
    `draw_segmented_gauge(…, 284, y+13, …)`; per-frame refill/discharge at
    `mainint.c:4687`, `4705`. `draw_segmented_gauge` (`vga256d.c:158`): full
    segments `color+12`, partial segment `color + 12*part/segment_value`.
  - Rear-config buttons: `blit_sprite(OPTION_SHAPES, 18/19)` at (285/302, 44)
    (`mainint.c:202`).
  - Message bar: `JE_drawTextWindow` / `JE_outCharGlow` (`mainint.c:99,109`) —
    sprite-erase + text at (20,190), persistent until countdown erase.
  - Level name: baked once at `tyrian2.c:824` (268,…), forced classic via
    `hd_font_force_classic`.
- Cash / lives / superbombs / special icon are drawn on the **playfield**
  (`JE_inGameDisplays`, `mainint.c:3163`) and already have HD text handling
  (`hd_font_flight_hud`) — **out of scope** here.

## Architecture

**State-driven overlay, not draw interception.** A new HD HUD layer draws the
whole panel region every present from live game state; the classic 8-bit panel
keeps rendering underneath untouched. Any failure (missing asset, 2P, queue
trouble) → skip the overlay → classic HUD shows. No classic draw call is
removed or conditionalized.

- **Hook point**: in the `hd_mode && hd_flight_active` branch of
  `scale_and_flip()` (`video.c`), immediately after
  `SDL_RenderSetClipRect(…, NULL)` and **before** `draw_hd_font_queue()` —
  i.e. above flight sprites, below HD text/cursor.
- **Region**: two strips — sidebar `x∈[264,320) × y∈[0,200)` and bottom bar
  `y∈[184,200) × x∈[0,264)`. All output rects computed from `dst_rect`
  exactly like the flight-queue math (`video.c:2255-2259`).
- **Punch-out model** (incremental safety): the layer owns a list of *cells*
  (logical VGA rects). The panel background is drawn as the strip rects minus
  the cells of elements not yet vector-implemented, so their classic pixels
  show through the holes. As each element gets a vector renderer, its cell
  flips from punch-out to drawn-on-top. Nothing ever disappears.
- **Colors**: sample the live 8-bit palette (`colors[]`) at present time using
  the same classic indices (144/224 ramps, `115+j`, `color+12`), so palette
  fades and flashes stay in sync with the playfield.
- **Placement of code**: new `src/hd_hud.c` + `hd_hud.h`. It needs renderer
  internals (`main_window_renderer`, dst_rect math, hdpic texture cache);
  expose the minimum via a small internal header or additions to `video.h` —
  implementer's choice, but do NOT duplicate the hdpic loader: factor
  `hd_set_backdrop`'s loader (`video.c:614-663`) so both paths share the
  fail-once cache.

## Phases

### H0 — infrastructure (no visible change when asset missing)
- `src/hd_hud.c/.h`: `hd_hud_draw(SDL_Renderer *, const SDL_Rect *dst_rect)`
  called from the flight branch of `scale_and_flip()`.
- Gating: `hd_mode && hd_flight_active && !twoPlayerMode` (2P deferred to H4)
  and hdpic03 texture available (fail-once, like every other HD cache).
- Rect-scaling helper (logical VGA rect → window rect via `dst_rect`).

### H1 — HD panel background
- Draw the two strips from the `hdpic03.dat` texture (source rects scaled
  from the asset's resolution; read the asset header the same way
  `hd_set_backdrop` does — don't assume a fixed scale factor).
- Initial punch-outs (classic shows through): shield bar, armor bar, power
  bar, weapon dots, sidekick icon+gauge cells, rear-config buttons, message
  bar (16,188)-(263,199), level-name block (268,…).
- Verify: headless attract-mode demo + `SDL_RenderReadPixels` at
  `scale_and_flip` (the Phase 0/1 recipe) — sidebar pixels differ from
  classic only where HD art differs; punch-out cells byte-identical.

### H2 — vector dynamic elements (the core deliverable)
Replace punch-outs with procedural draws, faithful to classic geometry and
color ramps but crisp at output resolution:
- Shield + armor bars: reproduce `JE_dBar3`'s segment layout & 2-step color
  ramp as scaled rects (optionally sub-segment smooth height as polish —
  keep the ramp look either way). Include the max-shield marker.
- Main power bar incl. its color logic (`tyrian2.c:1288-1300`).
- Weapon-power dots (`115+j` ramp).
- Sidekick ammo gauges (`draw_segmented_gauge` semantics: `+12` full,
  proportional partial).
- Read live globals directly (`shield`, `armor`, `power`, weapon/port power,
  sidekick ammo — see `player.h` / `varz.h`); no mirroring hooks needed, no
  interpolation (HUD values step per logic tick by design).
- Icons (sidekick, rear-config) and all text **remain punch-outs** in H2.

### H3 — HD text (later)
- Level name + message bar re-rendered per present via the HD glyph machinery
  (needs an immediate-mode glyph draw at present time, or state captured from
  `JE_drawTextWindow`/`JE_outCharGlow` incl. per-char glow values). The
  classic baked text stays underneath (covered), so `hd_font_force_classic`
  at `tyrian2.c:824` stays as-is.

### H4 — bitmaps + 2P (later)
- HD sidekick icons / rear-config buttons through the HD sprite pipeline.
- Two-player layout (`hdpic06.dat`, different element positions,
  `JE_drawOptionLevel` at x=268).

## Risks / notes
- The in-flight pause/menu and game-over paths present with different flags —
  the layer must only draw when `hd_flight_active` is genuinely the flight
  present (same gate the sprite overlay already trusts).
- `JE_dBar3` is shared with menus; do not modify it — the HUD layer
  reimplements its *look*, classic code is untouched.
- Palette: in-flight flashes (damage, pickups) mutate `colors[]`; sampling at
  present time keeps the vector elements in sync automatically.
- Verification is headless: attract demo drives the real flight loop;
  `make && make debug` clean is the bar, plus readback A/B for H1.
