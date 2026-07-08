---
title: HD-mode text vanishes on screens that draw once then hold/re-present a frame
status: Fixed
fixed-in: f064a6c
component: HD remaster / video compositor
affects: hd_mode only (classic rendering unaffected)
---

# HD-mode text vanishes on "draw once, then hold a frame" screens

**Status: Fixed (commit `f064a6c`, branch `hd-remaster`).**

## Symptom

In HD mode ([remaster-plan-doc.md](../remaster-plan-doc.md)), menu/UI text disappears
on screens that draw text once and then re-present the same frame while idling for
input. Non-text elements (ship illustration, cubes, panels) stay visible; only the
text is gone. Most visible case: the shop / Game Menu (`JE_itemScreen`) showed a blank
right-hand panel with no menu items.

## Root cause

Text drawn to `VGAScreenSeg` goes through the composited HD path (`hd_font_emit` in
`src/video.c`) rather than being held in the 8-bit frame. So a screen that draws text
**once** and then **re-presents** the same frame loses that text after the first
present — classic-mode pixels persist across presents, HD's do not.

Note: the HD font *glyph* assets are frequently absent, so `hd_font_emit` declines and
the text is really classic 8-bit — yet it still fails to survive under HD compositing.
So the fix is about **redrawing every presented frame**, not about the glyph queue.

## Fix

Pick per screen; always gate on `hd_mode` so classic rendering stays pixel-identical:

1. **Per-frame redraw** — break the idle inner loop in HD mode so the outer loop
   redraws the text each frame. This is what fixed the shop:
   `src/game_menu.c` `JE_itemScreen` inner loop →
   `if (hd_mode || hasInput(INPUT_NO_MOTION)) break;`
2. **HD-mode early fade** — `memcpy VGAScreen2→VGAScreen` + `fade_palette` BEFORE the
   text draws, with the classic post-draw fade gated `if (!hd_mode)`.
3. **Break inner motion loops** in HD mode.

Supporting mechanisms added in the same commit:
- `hd_font_force_classic` (`video.c`/`video.h`) — makes `hd_font_emit` decline so text
  paints persistent classic pixels when it must survive many presents (e.g. the
  level-start HUD, drawn before the flight loop begins).
- Held-text registry `JE_holdTextClear/Redraw/DString/TextShade` (`fonthand.c`/`.h`) for
  screens built incrementally by `JE_outTextGlow`, whose per-step presents would
  otherwise erase earlier lines.

## Screens fixed in f064a6c

`mainint.c` (help system, load screen, high-score screen + name entry, episode-start,
results, end-level cube animation, `JE_outCharGlow` skip path), `tyrian2.c`
(level-start HUD, SuperArcade/SuperTyrian unlock, network start), `opentyr.c` (setup
menu), `destruct.c` (intro + help), `game_menu.c` (shop / Game Menu). Previously fixed
in earlier commits: title menu, gameplay/episode/difficulty selects.

## How to reproduce / verify

Headless can't show it — run the real app in HD (config `item 'hd' 'true'`) and
navigate to the screen. See [data-dir-flag-is-t-not-d.md](../data-dir-flag-is-t-not-d.md)
for run args (`./opentyrian --data ./tyrian21`).

## Open follow-up

The shop only lands on the composited path because difficulty-select **leaks its HD
backdrop** (the shop has no backdrop of its own — see the comment near
`JE_updateNavScreen` in `game_menu.c`). The per-frame-redraw fix resolves the symptom;
clearing the leaked backdrop on shop entry would let the shop render on the plain
classic path and is a reasonable robustness follow-up.
