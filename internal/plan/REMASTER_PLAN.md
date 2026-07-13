# OpenTyrian HD Remaster — Phased Plan

> **Vision:** *"The game you remember, but sharp."* Upscaled HD visuals over
> gameplay that plays **identically** to the original. The classic build stays
> fully playable at every step; the remaster ships behind a toggle so we can
> always A/B against the original.
>
> **Per-asset status lives in [`REMASTER_ASSETS.md`](REMASTER_ASSETS.md)** — this
> doc is the architecture & phases; that one is the inventory & wiring checklist.

Status: **Phase 0 + Phase 1 complete and verified** (title-screen vertical slice;
Go/No-Go gate = **GO**). **Phase 2 spine landed**: the asset pipeline now extracts
& upscales *all 13* full-screen backdrops, and the compositor is a per-PIC HD
*registry* (any backdrop screen is one `hd_set_backdrop(n)` call away from HD).
**All full-screen backdrops are now wired** to HD (title, episode/difficulty/
gameplay selects, setup, instructions, load-game, high-score table + entry,
destruct intro) with an *automatic* fade (the palette fade functions drive the HD
backdrop brightness, so wiring a screen is just `clr256` + `hd_set_backdrop(N)` +
`hd_clear_backdrop()` on exit). **Phase 4 CRT/scanline mode landed** (`[video]
crt`, default off). Remaining: HD *sprites* (Phase 2) and palette-effect parity
(Phase 3) — the in-game truecolor mountain — plus swapping the placeholder
Lanczos upscaler for a real AI model. Two new phases were added: **5** (free
scaling & fullscreen) and **6** (60 fps+ retune). Target approach: *HD skin,
gameplay-identical*. Regenerate all HD backdrop assets with `python3
tools/hd_extract.py` (or a subset via `--pics 4,6`).

---

## 1. The core problem (why this is an engine change, not a setting)

The renderer is already resolution-agnostic — it's cleanly parameterized on
`vga_width`/`vga_height` (`video.h:26-27`) and the `scalers[]` table
(`video_scale.c:44`). That is **not** where the difficulty lives.

The difficulty is that the engine is **8-bit indexed-color**, and much of the
game's *look* is built on palette manipulation, not RGB:

- **Fades** (level transitions, title fade-in) = ramping the palette over N
  steps (`palette.c` `init_step_fade_palette` / `step_fade_palette` /
  `fade_black`), not alpha.
- **Ship / enemy recoloring** (the shop hue previews, damage flashes, team
  colors) = remapping *palette index ranges*, not tinting RGB.
- **Palette cycling** animates glows by rotating palette entries.

AI-upscaled art is **truecolor RGBA**. The instant an upscaled sprite enters the
frame, none of those palette tricks apply to it. So "upscale the visuals" really
means **move rendering from indexed software surfaces to truecolor GPU
compositing, and reimplement every palette effect as a tint / alpha / shader
operation.** That is the real project. The upscaled art sits on top of it.

## 2. The insight that keeps it tractable

**Separate _rendering resolution_ from _gameplay coordinate space._**

The ~100 hardcoded `320`/`200` assumptions the survey found — starfield stride
of 320 (`starlib.c`), background blitters (`backgrnd.c`), `320/2` centering and
the `y=192` HUD bar across `mainint.c` / `menus.c` / `opentyr.c` / `destruct.c` —
are **gameplay/layout math**. We do **not** rip those out.

- Game logic keeps *thinking* in 320×200 logical units → every gameplay rule,
  hitbox, and spawn stays valid and balanced.
- A **render scale factor** is applied at draw time; HD assets are composited
  into a high-res GPU target.

Result: an HD remaster that plays byte-for-byte like the original. True
widescreen (seeing *more* world) is explicitly **out of scope** for this vision —
it's a separate future project because it *does* require touching gameplay bounds.

## 3. Invariants (hold these through every phase)

1. **The classic build always runs.** No phase leaves `master` unplayable.
2. **Remaster is opt-in** via config (`[video] hd = true`) and can be toggled at
   runtime for A/B.
3. **Gameplay is untouched.** Logical coordinate space stays 320×200. If a change
   would alter a hitbox or spawn position, it's out of scope.
4. **Assets are additive.** HD assets live alongside the originals; a missing HD
   asset falls back to the upscaled classic pixels, never a crash.
5. **Everything is measured against the original**, side by side.

---

## 4. Phase breakdown

| Phase | Name | Goal | Effort | Risk | Status |
|------:|------|------|:------:|:----:|:------:|
| **0** | Presentation polish | Retina-sharp, correct-aspect output on today's pipeline | S | Low | ✅ done |
| **1** | Truecolor engine | Replace indexed present path with a truecolor GPU compositor; reimplement fade | **L** | **High** | ✅ done (title slice) |
| **2** | HD asset pipeline | Extract → AI-upscale → repackage → composite HD backgrounds & sprites | M | Med | 🔨 backdrops done; sprites pending |
| **3** | Palette-effect parity | Reimplement recoloring & cycling as GPU tint/shader so HD art matches classic behavior | M–L | High | — |
| **4** | Text, UI & polish | Redrawn fonts/menus, optional shaders (bloom/CRT), cutscene handling | M | Low | 🔨 CRT/scanline done |
| **5** | Free scaling & fullscreen | Any window size + real fullscreen, aspect-correct, no gameplay-space change | S–M | Low–Med | ✅ done |
| **6** | High-framerate retune | Decouple sim tick from 60/70 Hz origin; smooth 60 fps+ without altering game speed/balance | **L** | **High** | 🔨 render interpolation live (opt-in) |

**Phase 0/1 verification (done):** built clean on macOS/arm64; the title screen
composites a 4× Lanczos-upscaled backdrop (`hdtitle.dat`, via
`tools/hd_extract.py`) behind the color-keyed indexed logo/version/menu, with a
matched fade-in, at Retina 1280×800. Confirmed by reading back the composited
framebuffer (`SDL_RenderReadPixels`). HD is toggled by `[video] hd` in
`opentyrian.cfg` (default on); with it off, the game is byte-for-byte the classic.
Known follow-ups: the Lanczos upscale is a **placeholder** for a real AI upscaler
(Phase 2); the demo-idle `fade_black` transition doesn't ramp the HD backdrop
(cosmetic); mouse/DPI math is implemented but wants a hands-on check on a
high-DPI display.

Effort: S ≈ days · M ≈ weeks · L ≈ month(s), hobby-pace.

---

### Phase 0 — Presentation polish (foundation, low risk)

Make *today's* image as good as the current architecture allows. Pure
rendering-layer; no gameplay or asset work.

- Add `SDL_WINDOW_ALLOW_HIGHDPI` (`video.c:91`) so the window is Retina-native
  instead of OS-blurred.
- Use `SDL_RenderSetLogicalSize` + `SDL_RenderSetIntegerScale` so scaling is
  crisp and correctly proportioned; reconcile with the existing
  `calc_dst_render_rect` aspect logic (`video.c:319`).
- Correct the 4:3 non-square-pixel aspect as the default.

**Exit criteria:** title screen is pixel-sharp and correctly proportioned on a
Retina display; no regressions to scaler/fullscreen behavior.

---

### Phase 1 — Truecolor engine + **vertical slice** (the crux)

This is where the architecture actually changes, and where we prove the whole
remaster is viable **before** investing in assets. Scope Phase 1 as a single
**vertical slice** first:

> **Slice target — the title screen** (`titleScreen()`, `tyrian2.c:3235`). It
> loads a full-screen backdrop — **`tyrian.pic` image #4**, palette
> `palettes[pcxpal[3]] = palettes[8]` from `palette.dat` (`tyrian2.c:3271` →
> `picload.c:30`) — and fades it in via `fade_palette(colors, 10, 0, 239)`
> (`palette.c:130`). It exercises *truecolor target + one HD asset + one
> reimplemented fade* with **zero** gameplay entanglement.

**Layering caveat (important):** the backdrop PIC is only *part* of the title.
The animated Tyrian logo (`PLANET_SHAPES` sprite 146), the version string, and
the menu text are drawn into the **indexed** `VGAScreen` *on top of* the PIC each
frame — they are not in the PIC. So the HD backdrop must sit **behind** those
dynamic indexed elements (composite HD first, then let the indexed layer draw
over it), or they'll be occluded. This is exactly the kind of compositing-order
question the slice exists to answer.

Slice work:

1. **Truecolor compositor.** Add an RGBA layer at the single present choke point,
   `scale_and_flip` (`video.c:382`) — inject between the existing
   `SDL_RenderCopy(main_window_texture …)` and `SDL_RenderPresent`
   (`video.c:396→397`). Draw the HD texture into the same `dst_rect` so
   geometry/aspect/mouse-mapping (`last_output_rect`) stay consistent. A flag set
   by `titleScreen()` and read here selects HD-vs-classic for the current screen.
   Objects in scope: `main_window_renderer`, `main_window_texture`,
   `main_window_tex_format` (`video.c:48-50`).
2. **One HD asset.** Extract PIC #4 to PNG (Phase 2 tooling: RLE-decode
   `picload.c:60-78`, color through `palettes[8]` with the 6→8-bit expansion
   `(c<<2)|(c>>4)`), upscale it, load as an `SDL_Texture`.
3. **One reimplemented effect — the fade.** Replace the palette-ramp fade-in with
   `SDL_SetTextureColorMod` / `SDL_SetTextureAlphaMod` on the HD texture, ramped
   over the same step count (10) as the classic `fade_palette` loop
   (`palette.c:130-151`) so timing matches.

**Go/No-Go gate:** if the slice looks right and the code feels clean, the rest of
the remaster is largely repetition. If the compositor fights the engine, we've
learned it cheaply — before touching a single sprite.

After the gate, generalize: a screen/layer registry mapping game screens to HD
assets, and the fallback path for everything not yet remastered.

---

### Phase 2 — HD asset pipeline

The "upscaled visuals" itself. Tooling-heavy but low architectural risk once
Phase 1 exists.

Asset inventory & formats:

| Asset | File(s) | Format on disk | Notes |
|-------|---------|----------------|-------|
| Full-screen pics | `tyrian.pic` | Header offset table + RLE, 320×200 8-bit indexed, palette via `pcxpal[]`/`palettes[]` | Title, interludes |
| Sprites/shapes | `tyrian.shp`, `tyrianc.shp`, `estsc.shp`, … | Shape tables (`sprite.h` `Sprite{width,height,…}`) | Transparency = color 0; **must** preserve alpha edges |
| Big PCX | `tshp2.pcx` | PCX | `pcxload.c` |
| Cutscenes | `*.anm` (`tyrend.anm`) | ANM frames (`animlib.c`) | Video-like; treat separately |
| Palettes | `palette.dat` | 256-color palettes | Source of truth for indexed→RGB |

Pipeline:

1. **Extractor** (offline tool, reuses the decode logic in `picload.c` /
   `sprite.c` / `animlib.c`): decode each asset, apply its palette → RGBA PNG.
2. **Upscale**: Real-ESRGAN or a pixel-art-tuned model. Sprites need
   alpha-aware upscaling so color-0 transparency doesn't bleed.
3. **Repackage**: new HD asset bundle (PNG/atlas) loaded alongside originals;
   naming maps 1:1 to classic asset IDs for the Phase-1 fallback.
4. **Wire into the compositor** screen by screen: backgrounds first (easiest,
   biggest visual payoff), then sprites.

**Exit criteria:** backgrounds and the main sprite set render from HD assets,
with automatic fallback to upscaled classic pixels for anything not yet done.

**Spine (done).** The full-screen-backdrop half of this is now generalized:

- *Pipeline:* `tools/hd_extract.py` extracts, colorizes (via `pcxpal[]`), and 4×
  Lanczos-upscales **all 13** `tyrian.pic` images to `hdpicNN.dat` (NN = 1-based
  PIC number, e.g. `hdpic04.dat`) in the `HDPX` format (`"HDPX"` + u32 LE w + u32
  LE h + RGBA rows). `--pics 4,6` regenerates a subset; `--no-preview` skips the
  human-inspect PNGs (`tools/hdpic_previews/`, gitignored). Lanczos is still the
  **placeholder** for a real AI upscaler — swapping it is a tooling-only change,
  no engine impact.
- *Compositor:* `video.c` holds a per-PIC texture cache `hd_backdrop_tex[1..13]`
  with per-id lazy load + graceful fallback (missing asset → classic path, warns
  once). State: `hd_backdrop_active` / `hd_backdrop_id` / `hd_backdrop_fade`.
- *Wiring a screen* is now one line. A screen that does
  `JE_loadPic(VGAScreen, N, …)` then draws its dynamic layer (text/sprites) on
  top becomes HD by, inside `if (hd_mode)`: `JE_clr256(VGAScreen);` (wipe the
  classic backdrop to the transparent index so the HD asset shows behind the
  keyed overlay) + `hd_set_backdrop(N);`, ramp `hd_backdrop_fade` 0→255 over the
  screen's existing fade step count, and `hd_clear_backdrop();` on every exit
  path. The title screen (PIC 4) is the reference implementation in
  `titleScreen()`.

*Caveat when wiring:* only screens whose foreground is drawn over a *static* PIC
convert cleanly. Screens that redraw the PIC every frame in a loop, or draw
opaque panels expected to sit on the classic backdrop, need per-screen handling
(don't `clr256` blindly) — hence screen-by-screen with visual A/B each time.

---

### Phase 3 — Palette-effect parity (the sharpest thorn)

HD (truecolor) art must reproduce the behaviors the palette used to provide:

- **Recoloring** — Tyrian's index-range remap becomes a per-sprite **hue-shift
  shader** driven by a mapping table (ship colors, damage flash, teams).
- **Palette cycling** — reproduce as animated shader params or pre-baked frames.
- **Global fades/tints** — already generalized from the Phase-1 fade work.

This is the highest-risk *correctness* work because recoloring is pervasive and
subtle. Build a side-by-side harness (classic vs HD) for each effect.

---

### Phase 4 — Text, UI & polish

- **Fonts** are better **redrawn** (TTF/vector) than upscaled — bitmap-font
  upscaling looks mushy; UI is where "cheap" shows. `font.c`/`fonthand.c` are
  already surface-relative, easing a higher-res text layer.
- **Menus/HUD** re-laid out crisply on the HD layer (still driven by 320×200
  logical positions).
- **Optional shaders**: bloom, subtle lighting, optional CRT/scanline mode for
  purists. **(done — scanline + vignette CRT mode, `[video] crt`, default off,
  cached overlay textures at the present choke point; no gameplay/image change.)**
- **Cutscenes** (`.anm`): upscale frames or apply a runtime upscaler.

---

### Phase 5 — Free scaling & fullscreen

**Status: done.** The window is `SDL_WINDOW_RESIZABLE`; the destination rect is
recomputed every frame from the renderer *output* size (Phase 0), so both the
classic scaler path and the HD backdrop path scale continuously and aspect-correct.
`FULLSCREEN_DESKTOP` toggling (Alt+Enter / options) round-trips correctly. Added:
persisted `[video] window_width` / `window_height`, so a resized window is restored
across scaler switches, fullscreen round-trips, and restarts (fullscreen-desktop
sizes are never recorded as the user's free size). Both `RESIZED` and `SIZE_CHANGED`
route to the resize handler. Mouse mapping is unchanged from Phase 0 and wants a
hands-on check at extreme sizes.

Today the window opens at a fixed integer multiple and the render rect is
computed in `calc_dst_render_rect` (`video.c`). Goal: the player can resize the
window to *any* size and toggle real fullscreen, always aspect-correct, with the
image sharp and the mouse mapping intact — **without touching the 320×200 logical
gameplay space** (Invariant #3).

- **Resizable window:** add `SDL_WINDOW_RESIZABLE`; handle `SDL_WINDOWEVENT_
  SIZE_CHANGED` by recomputing the destination rect (the letterbox math already
  lives in `calc_dst_render_rect`, which uses the renderer *output* size post
  Phase 0 — so most of this is enabling the flag + reacting to the event).
- **Fullscreen:** `SDL_WINDOW_FULLSCREEN_DESKTOP` toggle (there's an existing
  fullscreen path/keybind to reconcile), aspect-preserved with letterbox/pillar
  bars. Persist the choice + last window size in config (`[video]`).
- **Aspect policy:** keep the 4:3 correction from Phase 0 as the default; offer
  integer-scale and pixel-perfect options. The HD backdrop textures already
  scale continuously (they're drawn into `dst_rect`), so free scaling is nearly
  free for the HD path; the classic scaler path stays on its `scalers[]` steps.
- **Mouse mapping:** the window-point ↔ render-pixel reconciliation added in
  Phase 0 (`mapScreenPointToWindow` et al.) must be re-verified at arbitrary
  sizes and in fullscreen; this is the main correctness risk.

**Exit criteria:** drag-resize to any size and toggle fullscreen with a correct,
sharp, aspect-right image and accurate mouse hit-testing; gameplay unaffected.
Low risk because it's presentation-layer only — the hard part (decoupling render
resolution from logical space) was already done in Phases 0–1.

---

### Phase 6 — High-framerate retune (60 fps+)

**Status: render interpolation live behind `[video] highfps` (default off).** We took
recommended path (1) — *fixed-timestep simulation + rendered interpolation* — so the
simulation is byte-identical and only presentation is decoupled. Because the flight
loop fuses movement and drawing (and consumes RNG inside draw paths), a from-scratch
read-only re-render was rejected in favor of a **display-list capture/replay**
(`src/interp.c`):

- The sim still runs exactly once per tick. During its draw pass every playfield
  draw is recorded into a per-tick display list, tagged by entity, via hooks at the
  blit choke points (`blit_sprite2*`, the `blit_sprite*`/font family, background
  rows, a starfield marker). HUD on `VGAScreenSeg` is not recorded.
- Between ticks, `flight_present()` fills the wait to the tick deadline with
  interpolated frames: clear `game_screen`, replay the current list with each op
  offset toward its previous-tick position (paired by a `(tag, occurrence)` hash),
  re-apply the starfield and flat colored filter read-only, composite, present. The
  pacing (`setFrameCount`) is preserved, so game speed is unchanged.
- **Known degradations on interpolated frames** (documented in `interp.c`):
  framebuffer-reading effects can't be replayed op-by-op, so superpixel sparkles and
  the smoothie/lava/water filters render at tick-rate; the flat level tint is
  preserved. Mispaired/reused entity slots snap rather than smear.
- **Verified:** clean `make`/`make debug`; the attract-mode demo runs the full
  record→replay path headless with no assert/overflow/crash. **On-device visual A/B
  of smoothness done (2026-07-12): confirmed noticeably smoother.** Exposed in the UI
  as Setup → Graphics → **"Smooth FPS"** (not just the `[video] highfps` config item).
  One artifact found & fixed on-device: side-to-side scroll stuttered because the
  interpolator unwrapped only the *vertical* background tile wrap (28 px); added the
  symmetric *horizontal* unwrap (24 px = tile width, `mapXPos = mapXOfs % 24`) in
  `interp_op_pos` (`src/interp.c`). Enable with `[video] highfps = true` or the menu.
- **Not** the same as raising the *simulation* rate to 60/120 Hz (which retunes
  balance — option 2 below). That is a separate, opt-in follow-on now made far more
  tractable by this decoupling.

The original is locked to the DOS/VGA cadence (~70 Hz via `setFrameCount`/
`waitUntilElapsed` in `nortsong.c`; much game logic assumes "one tick = one
frame"). Goal: render and update smoothly at 60 fps and above **without changing
how fast the game actually plays or its balance** (Invariant #3 again — speed and
spawn timing are gameplay).

The crux: **separate the simulation tick from the display refresh.** Options,
cheapest-safest first:

1. **Fixed-timestep sim + rendered interpolation** *(recommended).* Keep the sim
   running at the original tick rate (so all `mt_rand` sequences, spawn cadence,
   and per-tick movement math stay byte-identical), but decouple presentation:
   run the sim in fixed steps off a real-time accumulator and interpolate sprite
   positions between the previous and current tick when drawing. Yields smooth
   60/120/144 fps with **zero** balance change. Needs prev/curr position state
   for everything drawn (players, shots, enemies, powerups) and an interpolation
   pass at draw time.
2. **Higher sim tick + rescaled constants.** Raise the actual tick rate and
   divide every per-tick delta/timer accordingly. Simpler, but **high risk**: it
   changes the integer-step movement and RNG cadence the game was tuned around,
   so hitboxes/patterns can drift. Requires the Phase-3-style A/B harness on
   gameplay, not just visuals.
3. **Hybrid** — interpolate (1) now; selectively raise tick rate later only where
   it's provably balance-neutral.

Why **L / High**: unlike everything above it, this reaches into *gameplay* timing
— the one thing the remaster has so far promised not to touch. It's kept as a
distinct, opt-in phase precisely so the "plays identical" guarantee holds for
everyone who doesn't want it. Recommended path is (1): identical simulation,
smoother presentation.

**Exit criteria:** smooth ≥60 fps with a frame-time-independent feel, and a
side-by-side confirming the *simulation* (positions per tick, RNG, deaths) is
unchanged from the classic cadence.

---

## 5. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Phase 1 compositor fights the engine | **Vertical slice first** — fail cheap at the gate |
| Recoloring parity (Phase 3) is subtle & pervasive | Side-by-side A/B harness per effect; table-driven mapping |
| AI upscaling looks inconsistent across asset types | Per-type treatment; hand-redo fonts/UI; curate, don't bulk-run |
| Sprite transparency bleeds when upscaled | Alpha-aware extraction & upscaling; validate color-0 edges |
| Scope creep into widescreen/gameplay | Invariant #3: logical space stays 320×200; widescreen is a separate project |
| Mid-refactor breakage | Invariant #1/#2: classic path always runs; remaster behind a toggle |
| Free scaling breaks mouse hit-testing (Phase 5) | Re-verify the Phase-0 window-point↔render-pixel mapping at arbitrary sizes + fullscreen |
| High-framerate retune alters game feel/balance (Phase 6) | Prefer fixed-timestep sim + render interpolation (identical simulation); A/B the sim per tick before any tick-rate change |

## 6. Open decisions

- **Upscaler choice & model** (Real-ESRGAN vs pixel-art-specific) — settle during
  Phase 2 tooling with a test batch.
- **HD asset bundle format** (loose PNG vs atlas) — driven by load-time perf.
- **Scale factor / target internal resolution** (e.g. 4×→1280×800 logical, or
  free) — decided at Phase 1 compositor design.
- **Fonts**: which TTF pairing matches Tyrian's tone (Phase 4).

## 7. Immediate next step

Land **Phase 0** (safe, visible win), then build the **Phase 1 title-screen
vertical slice** and evaluate the Go/No-Go gate before anything else.
