---
description: "HD remaster per-asset status is tracked in plan/REMASTER_ASSETS.md (inventory), separate from REMASTER_PLAN.md (architecture)"
---

# HD asset tracker

Per-asset HD remaster status lives in `plan/REMASTER_ASSETS.md` (in-repo), a companion to `plan/REMASTER_PLAN.md` ([remaster-plan-doc.md](remaster-plan-doc.md)). The plan doc owns architecture & phases; the asset tracker owns the inventory & wiring checklist — one row per asset with Extract/Upscale/Package/Wire columns and an upscaler tag (L=Lanczos placeholder, E=Real-ESRGAN, X=xBRZ/pixel-art, H=hand-redraw).

State as of 2026-07-06: **HD wiring complete.** Backdrops, title logo (xBRZ gold), all in-flight sprites (`Sprite2` sheets — shots/ship/powerups/coins/explosions/enemies incl. `_filter` hue recolor), the `tyrend.anm` cutscene, and text (fonts via xBRZ brightness maps + runtime recolor) all render HD. Extraction tools: `tools/hd_extract*.py` (backdrops/sprites, `_comp` sheets, `_anim`, `_filter` brightness maps, `_font`). In-flight compositor rides the Phase-6 `interp.c` display list; design in `plan/REMASTER_FLIGHT_COMPOSITOR.md`.

Non-obvious follow-ups (assets ready, deliberately deferred): (1) menu sprites (FACE/OPTION/WEAPON portraits) are wired-but-dormant — need an HD backdrop on the shop screen (`JE_itemScreen`) + a *persistent* (not immediate-mode) HD overlay for once-drawn glyphs; (2) in-flight HUD text stays classic by design (the game_screen −24/clip/interp composite can't be reproduced by a full-screen HD quad safely); (3) backdrops/PCX still Lanczos — swap to a real AI upscaler when available (none installable on this host); (4) 7/8 standalone PCX (tyrset/shipedit/net*) are dead DOS-tool assets, never loaded by the engine. See [commit-before-spawning-agents.md](commit-before-spawning-agents.md) — this campaign committed each increment before spawning the next.
