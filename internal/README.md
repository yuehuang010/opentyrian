# internal/

Project working knowledge for OpenTyrian development — notes, plan pointers, and
tracked issues that aren't obvious from the code or git history. Relocated here
from Claude Code's per-user memory so the knowledge is version-controlled and
shared.

## Notes

- [data-dir-flag-is-t-not-d.md](data-dir-flag-is-t-not-d.md) — run with `--data ./tyrian21`; `-d` means network delay, not data dir
- [sdl-dummy-video-hides-controllers.md](sdl-dummy-video-hides-controllers.md) — **`SDL_VIDEODRIVER=dummy` reports `no controllers detected` on macOS even with a pad connected** (GameController.framework discovery needs the Cocoa run loop); don't diagnose controller code from a headless run — use the real video driver, or `SDL_JoystickAttachVirtual` for a hardware-free test
- [remaster-plan-doc.md](remaster-plan-doc.md) — pointer to `plan/REMASTER_PLAN.md`; "HD skin, plays identical", truecolor engine is the crux
- [remaster-asset-tracker.md](remaster-asset-tracker.md) — pointer to `plan/REMASTER_ASSETS.md`; per-asset status/inventory
- [standalone-plan-doc.md](standalone-plan-doc.md) — pointer to `plan/STANDALONE_PLAN.md`; zero-external-data phases S0–S5
- [standalone-plan-progress.md](standalone-plan-progress.md) — execution state of S0–S5 (paused 2026-07-08)
- [use-sonnet-agents-for-implementation.md](use-sonnet-agents-for-implementation.md) — delegate implementation to subagents, tier the model by difficulty
- [commit-before-spawning-agents.md](commit-before-spawning-agents.md) — commit reviewed work before spawning agents; forbid state-changing git in prompts
- [plan/NAVIGATION_PLAN.md](plan/NAVIGATION_PLAN.md) — plan to improve code navigation (index tooling, CODEMAP, globals annotation); **all phases done** (`897c287`, `acceabf`)
- [plan/CONTROLLER_PLAN.md](plan/CONTROLLER_PLAN.md) — `SDL_Joystick` → `SDL_GameController` (Xbox/PlayStation named buttons, hotplug, remap screen); **done** (`c5bab0a`). Records the settled decisions (pure replace, no legacy-stick fallback), the default key map, the two bugs hotplug introduces (handle cached across `handleSdlEvents()`; `c_max` unclamped in `JE_playerMovement`), how to test controller paths with **no hardware** (`SDL_JoystickAttachVirtual` + linking `controller.c` against stubs), and why the menu label width is a **non**-issue (measured 67px of 83px, don't shorten labels on suspicion)
- [plan/SHIP_MODE_SWITCH_PLAN.md](plan/SHIP_MODE_SWITCH_PLAN.md) — **design draft**: single-player Fighter ⇄ Dragonwing in-flight mode switch; key finding: Dragonwing is a *player index* (`is_dragonwing`, `playerNum_==2`, `shipGr==0` sentinel), not a ship type, and `player[0].items` already owns the rear-weapon slot the charge cannon derives from — so no second inventory, no shop/save-format change. Phases M0–M3
- [plan/REMASTER_HUD.md](plan/REMASTER_HUD.md) — HD vectorized in-flight HUD (sidebar/status panel): state-driven overlay in `scale_and_flip`'s flight branch, `hdpic03.dat` panel art + vector bars/gauges, punch-out fallback model; phases H0–H4
- [CODEMAP.md](CODEMAP.md) — one-page subsystem map: files, entry points, key globals per subsystem; start here to orient in the codebase
- [hd-build-pipeline.md](hd-build-pipeline.md) — `tools/hd_build.py`, the cross-platform one-command orchestrator for the HD asset extraction + bundling pipeline; step list, opt-in tiles/sfx, and the hardcoded-data-dir / sfx-experiment-tool caveats found while wiring it
- [hd-music-compression.md](hd-music-compression.md) — **Decision (2026-07-12): keep shipped HD music at Vorbis ~q6 (~54 MB); savings too small to act on.** Encoder sweep (Opus 96k → 30 MB but breaks looping + needs a new decoder; Vorbis q4 → 34 MB drop-in). 35/41 tracks carry `LOOPSTART`/`LOOPLENGTH`; Opus's 48 kHz + pre-skip is the blocker
- [hd-music-track30.md](hd-music-track30.md) — HD music remaster of the title theme: pipeline, FluidR3 per-voice verdict (failed at ensemble level), four paths forward + experiment log
- [hd-music-opl-idioms.md](hd-music-opl-idioms.md) — **read before tuning any other LDS track**: OPL composition idioms that break under sample playback (delay-echo doubles, burst fusion, sub-grid detune, 37 ms repeats), the rewrite-don't-transcode principle, and the isolate/measure diagnosis workflow (`LDS_SOLO_FPS`, `compare_mix_balance.py`)
- [hd-music-pitch-report.md](hd-music-pitch-report.md) — automated pitch validation for track 30 (regenerate with `python3 tools/validate_pitch.py`): TEST A proves the OPL→MIDI converter is pitch-exact; TEST B measures per-soundfont preset tuning (solo renders + YIN f0). `tools/lds_to_midi.c` now dumps per-note `opl_hz/fnum/block/transpose` columns in the `.notes` file for TEST A
- [hd-music-ai-regen.md](hd-music-ai-regen.md) — AI audio-to-audio regeneration spike (ACE-Step 1.5 cover mode, local MPS/MLX): setup, batch scripts, first 4 track-30 covers + A/B artifact, loop-seam caveat
- [tools/tuner/README.md](../tools/tuner/README.md) — local zero-dependency web app (`python3 tools/tuner/server.py --song 30`) for ear-tuning `lds_gm_map.txt` by hand: live per-voice GM/transpose/cents/gate/echo + volume/mute/solo, A/B vs classic OPL, lossless map save; see also the tuner section in [hd-music-track30.md](hd-music-track30.md)

## Issues

Known issues, one file per issue, in [issue/](issue/). Each carries a `status` field.

- [issue/hd-text-vanish.md](issue/hd-text-vanish.md) — **Fixed (f064a6c)** — HD-mode text vanished on screens that draw once then hold/re-present a frame
- [issue/hd-music-untagged-loop-silence.md](issue/hd-music-untagged-loop-silence.md) — **⚠️ Refuted / fix reverted** — its premise ("4 untagged OGGs are looping tracks") was never measured and is **false**: all 6 untagged OGGs are genuine one-shots, and `tools/render_music.c` leaves one-shots untagged *by construction*, so **the LOOP tags are authoritative — never second-guess them**. Its fix (`4924808`) caused [issue/hd-secret-music-repeats.md](issue/hd-secret-music-repeats.md). Kept for the record; carries the measured 41-track loop/one-shot table
- [issue/hd-secret-music-repeats.md](issue/hd-secret-music-repeats.md) — **Fixed (7ecc398)** — the secret-level ZANAC3 cue looped forever in HD mode and level music never returned: `audioCallback()` never ticked the LDS sequencer on the HD path, so `playing` was frozen `true`, which both defeated `tyrian2.c`'s end-of-cue restore and made `4924808`'s untagged-loop heuristic always fire. Fix: trust the OGG tags + advance the sequencer (no `opl_update()`) in HD mode. Records the lldb recipe and the process-orphan trap
- [issue/vulcan-front-offcenter.md](issue/vulcan-front-offcenter.md) — **Reverted / Not-a-bug** — the `ed29a62` "recenter" was a misdiagnosis (`bx[]` is a symmetric sweep table indexed by `shotMultiPos`, not a constant offset); zeroing it killed the Vulcan's wave. Reverted in `src/episodes.c`; stock data restored
- [issue/highfps-starfield-zooming.md](issue/highfps-starfield-zooming.md) — **Fixed** — Smooth FPS made the in-flight starfield read as "zooming": speed was authentic (34.8 Hz sim, 3–5 px/tick, verified), but interpolation destroyed the 35 Hz strobed-trail illusion the star look depends on. A motion-streak emulation failed playtest; the fix excludes the star layer from interpolation (`draw_starfield_interp` draws raw tick positions). Lesson: strobing can be intended aesthetic — interp needs per-layer opt-outs
- [issue/hd-font-shade-bleed.md](issue/hd-font-shade-bleed.md) — **Open / Accepted (cosmetic)** — the HD vector font's 1px dark shade rim bilinearly smears ~4 output px into 12px-wide strokes, blotching curves and corners; fix is to nearest-sample the shade field in `bake_glyph` and soften by ~1px. Also records the headless font-A/B recipe and its two traps
