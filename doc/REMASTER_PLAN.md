# OpenTyrian HD Remaster — Phased Plan

> **Vision:** *"The game you remember, but sharp."* Upscaled HD visuals over
> gameplay that plays **identically** to the original. The classic build stays
> fully playable at every step; the remaster ships behind a toggle so we can
> always A/B against the original.

Status: **Phase 0 + Phase 1 complete and verified** (title-screen vertical slice;
Go/No-Go gate = **GO**). **Phase 2 spine landed**: the asset pipeline now extracts
& upscales *all 13* full-screen backdrops, and the compositor is a per-PIC HD
*registry* (any backdrop screen is one `hd_set_backdrop(n)` call away from HD).
Remaining Phase 2 work is wiring individual screens (screen-by-screen, each visually
QA'd) and swapping the placeholder Lanczos upscaler for a real AI model. Phases
3–4 remain. Target approach: *HD skin, gameplay-identical*. Regenerate all HD
backdrop assets with `python3 tools/hd_extract.py` (or a subset via `--pics 4,6`).

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
| **2** | HD asset pipeline | Extract → AI-upscale → repackage → composite HD backgrounds & sprites | M | Med | 🔨 spine done; wiring screens |
| **3** | Palette-effect parity | Reimplement recoloring & cycling as GPU tint/shader so HD art matches classic behavior | M–L | High | — |
| **4** | Text, UI & polish | Redrawn fonts/menus, optional shaders (bloom/CRT), cutscene handling | M | Low | — |

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
  purists.
- **Cutscenes** (`.anm`): upscale frames or apply a runtime upscaler.

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
