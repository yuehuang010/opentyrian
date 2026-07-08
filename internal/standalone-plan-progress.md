---
description: Execution state of plan/STANDALONE_PLAN.md phases S0–S5 (paused 2026-07-08)
---

# Standalone plan progress

Progress on the zero-external-data standalone plan ([standalone-plan-doc.md](standalone-plan-doc.md)).
Paused by user on 2026-07-08. Branch/worktree map for resuming:

- **S0 — Bundle VFS: DONE, committed & verified.** Commit `eb77195` on
  `hd-remaster` (main checkout). Added `tools/mkbundle.py`, `src/bundle.c/.h`,
  `dir_fopen` fallback in `file.c`. Boots headless with no `--data` from
  `tyrian.base` alone; `--data` and loose files still win. Format: magic
  `TYBUNDL1`, LE index (name→compression byte→usize→csize→offset) + STORE blobs;
  compression byte reserved for S5.
- **S4 — Data bundling: SATISFIED by S0** (the inclusive bundle already packs the
  byte-exact set). Marked done in the plan doc.
- **S2 — SFX/voice: PARTIAL, paused, UNVERIFIED.** WIP checkpoint commit `d6edec7`
  on branch `s2-sfx-migration` (worktree `.claude/worktrees/s2-migration`). Has
  `tools/mkhdsnd.py` + in-progress `hd_sfx` loader wiring in `nortsong.c/.h` +
  `config.c`. NOT built. Chosen method: DC-removal + soxr VHQ + TPDF dither, no
  low-pass. HD bank format `HSND`. Duration-parity exit criterion not yet proven.
- **S1 — Music: PARTIAL, paused, UNVERIFIED.** WIP checkpoint commit `33fb28e` on
  branch `worktree-agent-ad97ff89426988852`
  (worktree `.claude/worktrees/agent-ad97ff89426988852`). Has `tools/render_music.c`,
  `tools/encode_music.sh`, vendored `src/stb_vorbis.c`, Makefile + `lds_play.c/.h`
  hooks. Offline OPL renderer incomplete; streamed `loudness.c` path + `hd_music`
  toggle NOT added; no OGGs rendered.
- **S3 — tilesets (`shapes?.dat`): NOT STARTED.** Riskiest (seam-matching); needs
  Opus review + human visual A/B.
- **S5 — packaging/compression: NOT STARTED.** Depends on S1–S3 assets existing.

**Human-signoff gate:** S1 and S3 cannot reach their plan exit criteria without the
user's perceptual pass ("listen to every effect / all 41 tracks", "visual A/B per
level"). Build/headless/duration checks are automatable; the listen/look is not.

**Do NOT touch the main-worktree HD-remaster WIP** (the user's own in-progress HD
menu/font work — unrelated to S0–S5). See [commit-before-spawning-agents.md](commit-before-spawning-agents.md).
