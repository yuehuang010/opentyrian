---
title: HD vector font — shade rim bleeds into stroke interiors (blotching on curves/corners)
status: Open / Accepted (cosmetic; human A/B signed off 2026-07-09)
area: fonts / hd_vectorize_font.py
found: 2026-07-09 (headless A/B of the title menu, flat-vs-shaded bake)
---

# Summary

`tools/hd_vectorize_font.py` (`bake_glyph`) now carries the original glyphs'
per-pixel shading — but the dark shade rim **bleeds too far into the stroke
interior**, pooling as brown blotches. Worst on curves and corners: the bowl of
"a", the shoulders of "G", the joins on "e" and "w". Reads as grime rather than
as a lit bevel.

Cosmetic only. Shipped in `c23466c` after human sign-off; the bug it replaced
(completely flat, wrongly-bright glyphs) was far worse.

# Why it happens

The source dark shades (nibbles 10–11 on FONT_SHAPES) live in a **1-source-pixel
rim** on the glyph's outer boundary. `bake_glyph` edge-extends the nibble field,
then **bilinearly upsamples it 4x**, so that 1px rim smears across roughly ±4
output pixels.

A typical stroke is ~3 source px = 12 output px wide. The rim therefore bleeds
about a third of the way in **from each side**, and where two edges meet (a
corner, a tight curve) the two bleeds overlap and compound. Hence blotches
concentrated exactly at corners and curves.

The classic 8-bit renderer never does this: the dark pixel *is* the outermost
pixel, with a hard step to the body shade. There is no gradient to reproduce.

# Fix (not attempted)

Confined to the shade resample in `bake_glyph`. **Does not touch the engine or
the alpha channel.**

Sample the shade field with **nearest-neighbor** (faithful to the hard step, but
blocky at 4x), then soften by **~1 output pixel** — enough to kill stairstepping,
far short of the current ~4px smear.

The key insight the current bake misses: the alpha channel already carries the
crisp anti-aliased silhouette, so **the shade field does not need to be smooth
for the glyph to look smooth.** Smoothing it is what causes the bleed.

Leave the alpha path alone — it traces the *presence* mask through a padded fine
grid, and the shade field must keep resampling through that same grid/crop/
box-downsample so the two channels stay pixel-registered (see the geometry note
in `bake_glyph`'s comments).

# Verifying a fix

Headless A/B via the `OT_SHOTDIR`/`OT_KEYS` hooks (`src/video.c`, `cc80d42`):

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OT_SHOTDIR=<dir> OT_SHOTEVERY=1 \
      OT_SHOTQUIT=130 timeout 120 ./opentyrian --data <data>

Title menu is **present frame 122** (static, deterministic). Crop "Start New
Game" and zoom 4x nearest-neighbor; the blotches are obvious.

Two traps found while capturing:

- **The attract-demo HUD frame cannot be A/B'd.** `titleScreen()` waits on real
  wall-clock, so two runs of the *same* build differ by ~6500 playfield pixels at
  frame 440. Only static menu/text screens are frame-reproducible.
- **"AN EPIC MEGAGAMES PRODUCTION ©1994" is painted into the backdrop PCX**, not
  drawn through the font pipeline. It looks like TINY_FONT body text but shows
  zero pixel difference between bakes. It is *not* evidence about the tiny font.

# Related

Baked assets (`tyrian21/hdfont_*.dat`) live outside the repo, so `hd-remaster`
carries the baker but not its 272 outputs. Re-run
`hd_vectorize_font.py --data <dir>` after changing the bake.
