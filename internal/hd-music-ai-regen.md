# HD music via AI regeneration (ACE-Step 1.5 cover spike)

Status: **awaiting listening verdict** (2026-07-11). Direction: instead of
note-level transcoding (LDS -> MIDI -> soundfont, see `hd-music-track30.md`),
feed the *audio* of the classic OPL render to an audio-to-audio model and let
it regenerate the whole piece in an HD style. Directly attacks the
ensemble-coherence failure of the transcode path (the model generates one
coherent mix); gives up pitch-exactness, the shared tick clock, and per-voice
control.

## Setup (done, local)

- **ACE-Step 1.5** cloned at `~/source/ACE-Step-1.5`, deps via `uv sync`
  (brew-installed uv). Runs natively on Apple Silicon: DiT on MPS/MLX,
  `acestep-v15-turbo` (2B) auto-downloaded to its `checkpoints/`.
  Model init ~5 s warm; a 227 s cover renders in **~40-60 s** on the M5 Pro.
- Cover mode **skips the 5 Hz LM entirely** — an uninitialized `LLMHandler`
  is fine; no LM checkpoint needed (`use_lm` checks `llm_initialized`).
- Key API: `GenerationParams(task_type="cover", src_audio=..., caption=...,
  lyrics="[Instrumental]", audio_cover_strength=0..1)`; strength 1.0 =
  tightest adherence to source structure. Docs: `docs/en/INFERENCE.md`.

## Spike scripts (in gitignored `hdmusic_work/acestep/`, worktree ai-music-regeneration)

- `run_cover.py` / `run_cover_batch.py` — headless cover renders, single model
  load for a batch. Run from the ACE-Step repo root:
  `cd ~/source/ACE-Step-1.5 && uv run python .../run_cover_batch.py <src.wav> <outdir>`
- Source audio = `hd_music_pipeline.sh classic 30` output concatenated x2
  (`classic/hdmusic_30_x2.wav`, 227.3 s) — two loops so the model internalizes
  the repeat and a loop can be cut from the output later.
- `build_ab_page.py` + `ab_template.html` — self-contained A/B artifact page
  (opus 64k base64-embedded; artifact size cap is 16 MB). Published at
  <https://claude.ai/code/artifact/a2c1cdf5-0e27-49b8-8c01-fbf112312e11>:
  classic + 4 covers, playhead-preserving switching, loop-seam marker.

## First batch (seed 4242, caption = orchestral per the v6 instrument map)

| variant | strength | note |
|---|---|---|
| orch_s100 | 1.00 | tightest structure adherence |
| orch_s085 | 0.85 | |
| orch_s070 | 0.70 | |
| synth_s085 | 0.85 | synth-family caption (one-family palette idea) |

Objective checks: output duration matches source exactly (227.32 s); no
clipping (max -1.0 dB); loudness mean -15..-17 dB vs classic -20.9.
**Loop-half self-similarity 0.58-0.74 vs 0.96 for classic** — the covers do
NOT repeat exactly across the two loops, so a seamless game loop will need
manual seam work (crossfade or pick-one-loop), or repaint-mode experiments.

## Round-1 verdict + two hard-won API gotchas (2026-07-11)

User verdict on the first batch: "sounds nothing like the original — more like
a single [standalone] track than an actual remake." Root cause found in the
ACE-Step source, two separate traps:

1. **`audio_cover_strength` is NOT the melody knob.** It only sets how many
   diffusion steps keep cover *conditioning* (`cover_steps = steps * strength`).
   The melody-retention knob is **`cover_noise_strength`** (img2img-style: src
   latents blended into the initial noise). Its default is **0.0 = pure
   caption-driven style transfer, zero melody retention** — the round-1
   mistake. The gradio help text documents 0.1-0.25 as the recommended range,
   1.0 = max retention (may resist style change).
2. **The native MLX DiT backend silently ignores `cover_noise_strength`.**
   Three renders at 0.2/0.5/0.8 came out byte-identical (md5-equal) on MLX.
   The PyTorch path implements it (`modeling_acestep_v15_turbo.py` ~line 2050).
   Fix: `initialize_service(..., use_mlx_dit=False)` to run the DiT on torch/MPS.
3. Also useful: `task_type="cover-nofsq"` conditions on raw source latents
   instead of FSQ-quantized 5 Hz semantic codes; on the out-of-distribution OPL
   timbre it raised envelope corr vs classic from 0.35 to 0.51 even on MLX.

Objective pre-listen gate that caught #2: mono 8 kHz RMS-envelope correlation
vs classic + md5 of variants (see session log; worth scripting if this
direction continues).

## Round 2b (torch/MPS, knob live) — awaiting listening verdict

Same seed/caption, `use_mlx_dit=False`, sweep of `cover_noise_strength`
(page updated in place, same artifact URL). Envelope corr vs classic /
loop-half self-corr (classic self-corr = 0.96; broken round 1 was 0.35/0.58):

| variant | task | noise | corr | self-corr |
|---|---|---|---|---|
| orch_n020 | cover | 0.20 | 0.641 | 0.815 |
| orch_n050 | cover | 0.50 | 0.656 | 0.722 |
| orch_n080 | cover | 0.80 | 0.686 | 0.780 |
| nofsq_n050 | cover-nofsq | 0.50 | **0.800** | **0.914** |

Torch/MPS render speed ≈ MLX (50-60 s per 227 s cover) — no cost to leaving
MLX off for cover work. no-FSQ + noise 0.5 is objectively the closest remake;
higher noise on no-FSQ may pull closer still (risk: keeps OPL timbre).

## Track-30 round-2b verdict (user, 2026-07-11) + track 28

User: **melody 0.50 and 0.80 are "very good to the original"** (leans 0.50;
couldn't split them blind on current speakers). melody 0.20 dropped;
no-FSQ dropped ("too many artifacts and too similar to the original").
**Recipe locked: task_type="cover", torch/MPS DiT, cover_noise_strength
0.5-0.8, orchestral caption, seed 4242.**

Track 28 "Torm - The Gathering" (90.2 s loop) run with the same recipe,
generic orchestral caption (no track-30 instrument specifics): envelope corr
vs classic 0.727 (n050) / 0.769 (n080). A/B page (separate artifact):
<https://claude.ai/code/artifact/7142286b-6de5-43f7-9531-2534e7e5578e>.
`build_ab_page.py` now takes a song key (30/28) and emits `ab_page_<song>.html`.

Metric caveat: loop-half self-corr is only valid when the loop length is a
near-multiple of the 0.25 s envelope window — for track 28 even the classic
scores 0.19 (windows misalign across the seam). Corr-vs-classic is unaffected.

## Track-28 verdict + track-18 stress test (2026-07-11)

Track 28 user verdict: **melody 0.80 better** (0.50 close; either is "a
meaningful uplift" vs classic without A/B). So far 0.80 leads on faster
material, 0.50 on the anthem.

Track 18 "Tyrian, The Level" (77.3 s loop) chosen as stress test: most-heard
gameplay track, fast/driving rather than anthemic. Same recipe. **Envelope
corr dropped to 0.350 (n050) / 0.460 (n080)** — the recipe tracks fast
material notably worse than the anthems (30: 0.64-0.69, 28: 0.73-0.77).
A/B page: <https://claude.ai/code/artifact/e4dcc450-93b4-4bfe-8a3c-c8d887fe4b2a>.
If the ear verdict matches the numbers, knobs to try: noise 0.9, a
tempo/genre-matched caption (driving synth/rock rather than orchestral),
other seeds.

## Open questions for the verdict

- Does any cover beat "enhanced classic" (path 1) on ensemble coherence?
- Melodic fidelity: does it keep the V14 counter-melody and bass lines?
- Licensing: composition is Alexander Brandon's; local generation avoids
  hosted-service TOS but distribution of regenerated audio is still murky.
