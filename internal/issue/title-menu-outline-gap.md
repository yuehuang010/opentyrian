---
title: Title-menu text — hairline gap in the 4-corner outline (HD mode)
status: Fixed
fixed-in: 61b1f30
area: fonts / HD glyph compositor (src/video.c, src/font.c, src/tyrian2.c)
found: 2026-07-28 (user screenshot: white hairlines inside "Instructions" over the planet's cloud band)
---

# Summary

The title menu draws each item as **five** classic passes — four dark copies of
the glyph at `(±1,±1)` plus the glyph itself (`tyrian2.c`, `titleScreen()`). In
HD mode each pass became its own anti-aliased quad, so the four outline edges
**alpha-accumulated** (`1-(1-a)^4` ≈ 0.94) instead of unioning, and where the
copies only barely met they stranded a half-covered sliver. Read on screen as a
white hairline cutting across the outline — only visible against a light
backdrop, which is why it showed up over the planet's cloud band and nowhere
else.

Fixed by compositing the outline and the glyph into **one** quad
(`hd_font_emit_outlined` → `synth_hd_font_outlined` in `video.c`,
`drawFontHvOutline` in `font.c`), with the outline shape unioned at **classic**
resolution from the sprite's exact mask (`sprite_shade_mask`, new in
`sprite.c`), then upscaled with a sharpened bilinear ramp so the corners stay
round. The union is the whole point: classic's copies are opaque, so their union
is `max`, never "over".

# The finding that matters most

**The counters and apertures the outline closes are closed in the classic 8-bit
renderer too.** The 'c' in "Instructions" has no sky behind its mouth at
320x200; the four diagonal copies fill it. Verified by capturing the same menu
with `hd` off (see below) — top row of the 3-way capture is identical in that
respect to the HD build.

So *sky visible inside a letter is the artifact, not the intended look.* Don't
"restore" it. Only the enclosed 1px counter of 'o' shows background, in both
renderers.

# Rejected approach (do not retry as-is)

An attempt to make the outline thinner — union the **anti-aliased** silhouettes
at HD resolution (`max`, not "over") and plug interior slivers with the classic
mask eroded by one pixel — **reintroduced the hairlines** on the 'c'. The traced
silhouette sits up to ~half a logical pixel inside its hard mask, so a union of
anti-aliased copies cannot cover what the classic union covers, and a
one-pixel-eroded plug does not reach far enough out to close the difference.
Sealing like classic requires the outline to be as fat as the classic mask.

# Capturing the title menu headlessly

The `OT_SHOTDIR` readback hook in `video.c` is the **only** way to see this —
screen capture of the running window returns pure black (`CopyFromScreen` and
`PrintWindow` both, fullscreen or windowed; the SDL renderer is GPU-composited
and DWM won't hand it over).

    cd <a scratch dir holding its own opentyrian.cfg>
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
      OT_SHOTDIR=shots OT_SHOTEVERY=1 OT_SHOTQUIT=40 \
      opentyrian --data <data>

Traps:

- **The menu presents exactly once**, then sits in a 30-second idle loop
  (`titleScreen()`, "Play demo after idle for 30 seconds") that never calls
  `JE_showVGA` again. Coarse `OT_SHOTEVERY` values skip straight from the logo
  fade-in to the attract demo and you conclude, wrongly, that the menu never
  renders.
- **The frame index is not stable across builds or configs.** It was present
  #11 with `hd true`, #31 with `hd false`, and 122 in the 2026-07-09 run
  recorded in [hd-font-shade-bleed.md](hd-font-shade-bleed.md). Find it by
  scanning the dumps for gold text in the menu band rather than hardcoding:

      band = frame[143*S : 156*S, 100*S : 220*S]      # S = output px per logical px
      gold = ((band[...,0] > 150) & (band[...,1] > 90) & (band[...,2] < 140)).sum()

- Put a **private `opentyrian.cfg` in the run directory** (`get_user_directory()`
  is `"."` on Windows) so you can force `fullscreen -1`, `scaler None`,
  `scaling_mode Integer` and an integer-multiple window — the readback is then
  an exact NxN of the 320x200 surface and crops land on logical pixel
  boundaries. Flipping `hd` there gives the classic reference render.

# Lesson

Two fixes here were built on an offline reimplementation of the compositor in
Python and shipped without ever looking at a real frame; both were wrong about
what the engine actually put on screen. The capture recipe above costs about a
minute. **Use it before writing the fix, not after the second complaint.**
