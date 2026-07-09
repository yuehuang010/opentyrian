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
- **S2 — SFX/voice: DONE, merged & validated (2026-07-09).** Merged into
  `hd-remaster` (main checkout). `tools/mkhdsnd.py` + `hd_sfx` loader in
  `nortsong.c/.h` + `config.c`; `hd_sfx = true` by default. Method: DC-removal +
  soxr VHQ + TPDF dither, no low-pass. HD banks (`HSND` magic) baked at
  `tyrian21/hdsnd_sfx.dat`, `hdsnd_voices.dat`, `hdsnd_voicesc.dat`.
  Both `make` and `make debug` (`-Werror`) build clean. Loader confirmed to
  overlay the HD banks (valid magic, no `warning: hd_sfx:` fallback in the
  startup log). **Human A/B sign-off done**: user played the title-screen demo
  live and confirmed HD SFX/voices sound clean — no distortion or artifacts.
  Duration parity proven by the pipeline (all outputs exactly 4× source length).
- **S1 — Music: DONE, merged & validated (2026-07-09).** Merged into
  `hd-remaster` (main checkout) via `7e4fc28`; both parts
  landed as `9b7db4e` (offline renderer + encoder + vendored `stb_vorbis`) and
  `c5a0fb2` (streamed OGG path in `loudness.c` behind `hd_music`), plus `b548140`
  (Setup → Sound live toggle). `hd_music = true` by default.
  All **41 `hdmusic_NN.ogg`** assets are baked in `tyrian21/` (44100 Hz 16-bit
  stereo; 35 carry `LOOPSTART`/`LOOPLENGTH`, 6 terminate). Reproduce pipeline
  documented in `tools/MUSIC_REMASTER.md` (was referenced but missing).
  Verified 2026-07-09: `make` + `make debug` (`-Werror`) build clean; the vendored
  `stb_vorbis` decode path opens the real assets (stereo/44100, loop comments
  parse); re-rendering from `render_music` reproduces **byte-exact loop points**
  matching the shipped OGGs; this Mac's SDL device rate negotiates 44100 → exact
  match, so HD music loads (no synth fallback). **Known caveat:** the loader
  requires `sample_rate == audioSampleRate` and does not resample — a device
  negotiating 48000 Hz would silently fall back to the synth (see
  `tools/MUSIC_REMASTER.md` §"Known limitation"). **Human A/B sign-off done
  (2026-07-09)**: user validated HD music quality live — tracks and loops sound
  clean. The old WIP checkpoint `33fb28e` (worktree branch, now pruned) is
  superseded.
- **S3 — tilesets (`shapes?.dat`): DONE — human seam A/B signed off (2026-07-09).**
  User ran the live game (real Cocoa renderer, `hd_tiles = true`) and confirmed the
  HD tile backgrounds look good — no objectionable edge seams; the default nearest
  atlases pass. Landed on `hd-remaster` as `89aa5a2` (compositor + extractor
  behind `hd_tiles`), with follow-up fixes in `9b8992f`. Present in-tree:
  extractor `tools/hd_extract_tiles.py` + provenance `tools/hd_tile_manifest.py`;
  compositor `flight_emit_bg_row`/`hd_tile_z_for` in `interp.c` and
  `load_hd_tile_atlas`/`hd_tile_atlas_src`/`current_palette_index` in `video.c`;
  `bool hd_tiles` in `video.c` now surfaced as a live in-game menu toggle
  (`MENU_ITEM_HD_TILES`, "HD Tiles:", in `opentyr.c`) as well as `[video]` config.
  Key finding: gameplay palette is always `palettes[5]`, so only **5 atlases**
  exist (`)wxyz × p05`, baked in `tyrian21/`), plus a staged `hdtile_lanczos/`
  variant set for the A/B. Verified headless (attract demo: atlas loads, `hd_bg=1`,
  177k tile lookups 0 misses, clean over 90 s; `make` + `make debug` build clean).
  **Remaining: human seam-quality A/B (nearest vs lanczos) on a real display** —
  see full detail in [plan/STANDALONE_PLAN.md](plan/STANDALONE_PLAN.md) "S3
  IMPLEMENTED". Sibling S3c items: `estsc.shp` credits pics wired (needs a manual
  ending playtest); `tshp2.pcx` interlude consciously deferred (ships classic).
- **S5 — packaging/compression: NOT STARTED.** Depends on S1–S3 assets existing.

**Human-signoff gate:** CLEARED. S3's perceptual pass ("visual A/B per level") was
signed off by the user on 2026-07-09 (live run, tiles look good). S1's "listen to
all 41 tracks" and S2's SFX/voice listens were signed off the same day. All three
remaster tracks (S1/S2/S3) now have their human sign-offs.

**Do NOT touch the main-worktree HD-remaster WIP** (the user's own in-progress HD
menu/font work — unrelated to S0–S5). See [commit-before-spawning-agents.md](commit-before-spawning-agents.md).
