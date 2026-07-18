---
title: HD-mode keyboard-config menu text vanishes while rebinding a key
status: Fixed
fixed-in: 1b3b37b
component: HD remaster / video compositor (game_menu.c keyboard-config rebind)
affects: hd_mode only (classic rendering unaffected)
---

# HD-mode keyboard-config text vanishes during key rebind

## Symptom

In HD mode, opening the keyboard-config menu (`MENU_KEYBOARD_CONFIG`) and
starting to rebind a key blanks the whole menu -- labels, bound key names, and
the bottom help-text line -- until a key is pressed. Only the small pulsing
selection rectangle stays visible during the wait.

## Root cause

This is another instance of the "draw once, then hold/re-present a frame" class
documented in [hd-text-vanish.md](hd-text-vanish.md). The rebind wait loop in
`src/game_menu.c` (`JE_menuFunction`, `MENU_KEYBOARD_CONFIG` case, "change key"
branch) draws the menu text once, then enters a `while (true)` loop that, every
iteration, only redraws a pulsing rectangle and calls `JE_showVGA()` --
it never re-emits the text. In HD mode, text drawn via `JE_textShade` goes
through the per-present HD glyph queue (`hd_font_emit`, `src/video.c`), which is
rebuilt and drained on every present. Once the wait loop starts presenting
frames without re-drawing the text, the queue comes up empty and the text
disappears from the composited output.

The analogous controller-config binding wait loop
(`detect_controller_assignment` in `src/controller.c`) was also checked and
does **not** have this bug: it never calls `JE_showVGA()`/presents at all while
waiting for pad input, so the display simply holds the last-presented frame
(text included) for the whole wait -- there is nothing to fix there.

## Fix

`src/game_menu.c`:
- Factored the `MENU_KEYBOARD_CONFIG` row-drawing block (labels + bound key
  names, previously inline in `JE_itemScreen`) into a new static helper,
  `JE_drawKeyboardConfigRows()`, so it can be called from the rebind path too.
  `JE_itemScreen`'s per-frame draw now just calls the helper -- pixel-identical
  output, same call site.
- In the "change key" rebind branch, immediately before entering the wait loop,
  when `hd_mode` is set: flip `hd_font_force_classic` to `true` (see
  `src/video.h`, and the existing save/restore idiom in
  `JE_drawShipModeIndicator`, `src/mainint.c`), re-draw the row block via
  `JE_drawKeyboardConfigRows()`, redraw the help text via
  `JE_drawMainMenuHelpText()`, then draw the "waiting for key" blank-name text
  for the row being rebound, and restore `hd_font_force_classic` before the
  first `JE_showVGA()`. This bakes all of that text as persistent classic
  8-bit pixels in `VGAScreenSeg`, which (unlike the HD glyph queue) survive
  every subsequent present the wait loop performs, without needing to be
  redrawn each iteration.
- The whole block is gated on `hd_mode`, so classic (non-HD) rendering takes
  the exact same code path as before -- no behavior change when HD is off.

## How to reproduce / verify

Headless can't show it -- run the real app in HD (config `item 'hd' 'true'` in
`~/.config/opentyrian/opentyrian.cfg`) and navigate to Options > Keyboard
Configuration, then select any key to rebind. Before the fix, all menu text
vanished the instant the rebind wait loop started; after the fix it stays
visible while waiting for a key press.
