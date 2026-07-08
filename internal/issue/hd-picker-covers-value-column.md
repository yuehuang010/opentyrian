---
title: HD Setup picker (dropdown) covers the whole value column when tall
status: Open / Deferred (redesign later)
area: menus / HD compositor
found: 2026-07-08 (dogfooding)
---

# Summary

In HD mode, opening a **tall** picker in the Setup → Graphics menu (e.g. the
**Scaler** picker, ~9 items) makes every other row's *value* disappear, whereas a
**short** picker (e.g. **Scaling Mode**, ~3 items) leaves the uncovered rows visible
but dimmed. The user expects the tall picker to behave like the short one — other
entries dimmed, not vanished.

# Why it happens (not a regression — parity with classic)

The picker box is opaque and only as wide as the value column. A tall picker gets
clamped upward (`yPicker = MIN(yPicker, 200 - 10 - (hPicker + 5 + 2))`, see
`src/opentyr.c` ~line 268) so its box spans the **entire** value column. Every value
row is therefore physically behind the box.

- **Classic**: values are drawn first, then the opaque box is painted over them
  (`fill_rectangle_wh` + border rings, `src/opentyr.c` ~line 367-370), so covered
  values are naturally occluded.
- **HD**: menu value glyphs are emitted to the topmost HD-font queue (drawn *after*
  the indexed box), so they would bleed on top of the box. `setupMenu()` suppresses
  any value row overlapping the box's outer extent (`valueHiddenByPicker`,
  `src/opentyr.c` ~line 297) to reproduce the classic occlusion.

Both paths hide exactly the rows the box covers, so **HD matches classic**. The
"vanish vs dim" asymmetry is purely box height: the tall box covers all rows, the
short one doesn't.

# Deferred fix (redesign)

Make the picker never obscure the whole value column so the other entries stay
dimmed-visible like the short picker — e.g. render the picker as an offset side
panel, or a scrolling list capped to N visible rows, instead of a column-width box
that grows unboundedly. This diverges from the classic layout, so it's a deliberate
HD-only redesign, tracked here rather than done inline during dogfooding.

Gate any change on `hd_mode` so the classic menu stays byte-identical.
