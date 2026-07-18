---
title: Smooth FPS made the in-flight starfield read as "zooming" fast dots
status: Fixed
fixed-in: fc39ad8
component: Phase-6 render interpolation (interp.c / backgrnd.c)
affects: highfps_mode only (classic rendering unaffected)
---

# Smooth-FPS starfield read as "zooming" — fix: exclude the star layer from interpolation

**Status: Fixed** — `draw_starfield_interp()` now draws every star at its raw
current-tick position (no interpolation).

## Symptom

With Smooth FPS (`highfps_mode`) on, the in-flight starfield (asteroid fields and
all other space levels) appeared to "zoom" downward far too fast. With Smooth FPS
off, the same stars looked slower and left the familiar trailing line.

## Diagnosis (verified, not a speed bug)

- The simulation is authentic: measured 34.8 Hz tick rate; stars move
  `(rand 2..4) + starfield_speed(1)` = 3–5 px/tick (~105–175 px/s). No episode-1..4
  level overrides the speed (no event type 1 anywhere in `tyrian?.lvl`); the update
  code is byte-identical to upstream OpenTyrian (unchanged since 2013).
- The interpolation itself was mathematically correct (logged: 5 rows per 29 ms
  tick, alpha monotone 0→1, no double-stepping).
- The real cause is perceptual: the classic renderer presents once per 35 Hz tick,
  so a star jumps 3–5 px per presented frame and the eye (plus sample-and-hold LCD
  persistence) fuses the discrete jumps into a **strobed multi-image trail** — the
  original starfield "look" depends on this emergent artifact. Smooth interpolation
  reconstructs true continuous motion, which destroys the trail and turns each star
  into a single sharp dot honestly covering 175 px/s → reads as "zooming".

## Fix (and the failed first attempt)

1. **Tried first, rejected by playtest:** drawing each star as a short fading
   motion streak (head + `color-4*k` tail along the per-tick travel) in the
   interpolated path. Looked reasonable in theory but did **not** restore the
   intended look — the layer still read as zooming.
2. **Landed fix:** `draw_starfield_interp()` ignores `prev`/`alpha` and draws each
   star at `curr[i]` — the starfield is deliberately **excluded from
   interpolation**. Stars hold their 35 Hz tick positions across the smooth
   presents, exactly reproducing the classic strobed trail, while ship/enemies/
   backgrounds stay interpolated. Confirmed fixed by playtest.

## Scope notes

- The layer is shared: every level starts `starActive = true` (`tyrian2.c`), ground
  levels disable it via script event 8. ~19 levels across the four episodes keep it
  on; ep3 lvl 6 and ep4 lvls 15/20 enable it mid-level via event 9. The Astral Zone
  special weapon also shows this layer on any level (`astralDuration`).
- Unaffected by design: the options-menu starfield (`game_menu.c`, classic path, no
  interp) and the title/jukebox 3-D starfield (`starlib.c`, separate system).
- Lesson for future interp work: motion that the 35 Hz presentation renders as
  strobing/flicker can be part of the game's intended aesthetic — interpolating it
  "correctly" can be a visual regression. Consider per-layer opt-outs.
