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

## Issues

Known issues, one file per issue, in [issue/](issue/). Each carries a `status` field.

- [issue/hd-text-vanish.md](issue/hd-text-vanish.md) — **Fixed (f064a6c)** — HD-mode text vanished on screens that draw once then hold/re-present a frame
- [issue/vulcan-front-offcenter.md](issue/vulcan-front-offcenter.md) — **Fixed** — front Vulcan Cannon fires left of center (stock-data `bx` asymmetry); recentered in `src/episodes.c` `JE_loadItemDat` (front port only; rear Vulcan untouched)
