# internal/

Project working knowledge for OpenTyrian development — notes, plan pointers, and
tracked issues that aren't obvious from the code or git history. Relocated here
from Claude Code's per-user memory so the knowledge is version-controlled and
shared.

## Notes

- [data-dir-flag-is-t-not-d.md](data-dir-flag-is-t-not-d.md) — run with `--data ./tyrian21`; `-d` means network delay, not data dir
- [remaster-plan-doc.md](remaster-plan-doc.md) — pointer to `plan/REMASTER_PLAN.md`; "HD skin, plays identical", truecolor engine is the crux
- [remaster-asset-tracker.md](remaster-asset-tracker.md) — pointer to `plan/REMASTER_ASSETS.md`; per-asset status/inventory
- [standalone-plan-doc.md](standalone-plan-doc.md) — pointer to `plan/STANDALONE_PLAN.md`; zero-external-data phases S0–S5
- [standalone-plan-progress.md](standalone-plan-progress.md) — execution state of S0–S5 (paused 2026-07-08)
- [use-sonnet-agents-for-implementation.md](use-sonnet-agents-for-implementation.md) — delegate implementation to subagents, tier the model by difficulty
- [commit-before-spawning-agents.md](commit-before-spawning-agents.md) — commit reviewed work before spawning agents; forbid state-changing git in prompts
- [plan/NAVIGATION_PLAN.md](plan/NAVIGATION_PLAN.md) — plan to improve code navigation (index tooling, CODEMAP, globals annotation); **all phases done** (`897c287`, `acceabf`)
- [CODEMAP.md](CODEMAP.md) — one-page subsystem map: files, entry points, key globals per subsystem; start here to orient in the codebase
- [hd-music-track30.md](hd-music-track30.md) — HD music remaster of the title theme: pipeline, FluidR3 per-voice verdict (failed at ensemble level), four paths forward + experiment log
- [hd-music-opl-idioms.md](hd-music-opl-idioms.md) — **read before tuning any other LDS track**: OPL composition idioms that break under sample playback (delay-echo doubles, burst fusion, sub-grid detune, 37 ms repeats), the rewrite-don't-transcode principle, and the isolate/measure diagnosis workflow (`LDS_SOLO_FPS`, `compare_mix_balance.py`)
- [hd-music-pitch-report.md](hd-music-pitch-report.md) — automated pitch validation for track 30 (regenerate with `python3 tools/validate_pitch.py`): TEST A proves the OPL→MIDI converter is pitch-exact; TEST B measures per-soundfont preset tuning (solo renders + YIN f0). `tools/lds_to_midi.c` now dumps per-note `opl_hz/fnum/block/transpose` columns in the `.notes` file for TEST A

## Issues

Known issues, one file per issue, in [issue/](issue/). Each carries a `status` field.

- [issue/hd-text-vanish.md](issue/hd-text-vanish.md) — **Fixed (f064a6c)** — HD-mode text vanished on screens that draw once then hold/re-present a frame
- [issue/vulcan-front-offcenter.md](issue/vulcan-front-offcenter.md) — **Reverted / Not-a-bug** — the `ed29a62` "recenter" was a misdiagnosis (`bx[]` is a symmetric sweep table indexed by `shotMultiPos`, not a constant offset); zeroing it killed the Vulcan's wave. Reverted in `src/episodes.c`; stock data restored
- [issue/hd-font-shade-bleed.md](issue/hd-font-shade-bleed.md) — **Open / Accepted (cosmetic)** — the HD vector font's 1px dark shade rim bilinearly smears ~4 output px into 12px-wide strokes, blotching curves and corners; fix is to nearest-sample the shade field in `bake_glyph` and soften by ~1px. Also records the headless font-A/B recipe and its two traps
