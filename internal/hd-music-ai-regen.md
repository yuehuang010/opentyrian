# HD music via AI regeneration (ACE-Step 1.5 cover spike)

Status: **landed** (2026-07-11) — recipe locked, all 41 tracks rendered+installed, jukebox A/B shipped; see the final sections. Direction: instead of
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

## Spike scripts (landed in `tools/acestep/` — see its README; originals were in gitignored `hdmusic_work/acestep/`)

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

## Recipe locked + full-soundtrack render + in-game A/B (2026-07-11)

Track-18 verdict: user prefers melody 0.80 ("the more I listen, the more I
prefer" it); **recipe locked: cover, torch/MPS, noise 0.80, seed 4242,
generic orchestral caption.**

- `tools/acestep/render_all.py` (run under the ACE-Step venv) rendered
  ALL 41 songs: classic render -> loop-aware cover source (full + looped
  region; single pass for the 6 non-looping jingles 10/11/19/25/31/34) ->
  cover -> cut to one pass, 44.1 kHz stereo, mean-volume-matched to classic,
  LOOPSTART/LOOPLENGTH tags -> `variants/hdmusic_NN.acestep.ogg`. Fixed en
  route: pipeline classic cmd errored on non-looping songs under bash 3.2
  (empty-array expansion, `6b9296c`).
- All 41 **installed** into the shared tyrian21 DATA_DIR (previous
  enhanced-classic oggs backed up in this worktree's `hdmusic_work/backup/`;
  `hd_music_pipeline.sh restore <NN>` reverses). Format verified against the
  engine's requirements (2ch/44100, vorbis stream comments).
- **Jukebox H key** (`1e531c5`) A/Bs HD remix vs classic OPL synth live, with
  an [HD]/[OPL] indicator (`hd_music_playing()` accessor).

Envelope corr vs classic per song (recipe determinism confirmed: track 30
re-rendered at 0.688 vs 0.686 in the sweep). The two statistical outliers —
6 Deli Shop Quartet (-0.15) and 22 Come Back again to Savara (-0.09) — were
**cleared by ear (user, 2026-07-11: "no oddities")**, i.e. very low envelope
corr can be a legitimate reinterpretation, not a failure; treat the metric as
a triage hint only. Next-lowest band (36/37/18/17, 0.42-0.53) also shipped. Best: 5 (0.98), 10 (0.95), 27 (0.95), 32 (0.93), 40 (0.91).
Full table in the render log; large negative gains (31/32/40, -11..-14 dB)
are quiet ambient classics the cover rendered hot.

## Genre-remake experiment (2026-07-11, in progress) + gotcha #4

User (post-landing): the landed remixes are faithful; now try an experimental
"synthony" remake (symphony-orchestra-meets-synths) and other genres.
`tools/acestep/render_genres.py` sweeps captions on one track with the locked
cover mechanics (torch/MPS, seed 4242).

**Gotcha #4 — the caption is a NO-OP at cover_noise_strength 0.80 on turbo**
(user caught it by ear: round-1 genre variants "all sound the same"; pairwise
raw-waveform corr 1.000 across five different captions). The turbo sampler
has a fixed 8-step schedule ending [..., 0.5, 0.3]; cover-noise truncation
starts at the nearest timestep to (1-noise), so 0.80 -> t=0.3 -> ONE denoise
step over 70%-source latents: enough to HD-ify timbre, zero room for style.
Corollary: **the landed soundtrack's orchestral caption did nothing** — the
approved faithful-HD sound is caption-independent source refinement.
Style-vs-melody map (track 30, raw-waveform corr between genre captions):
0.80 -> identical (1.000); 0.50 -> 2 steps; 0.35 -> 3 steps, subtle
differences (0.94-0.96); 0.20 -> 6 steps, real genre separation (0.73-0.94)
with envelope corr vs classic still ~0.60. Two dead ends measured: a dense
20-timestep custom schedule (more low-noise steps) changes nothing (0.998 —
style is decided at the high-t steps the truncation removes), and
audio_cover_strength=0.5 (text-only late steps) is nearly a no-op (0.97-
0.996). So on turbo, **cover_noise_strength is the only style lever**, and
genre transfer costs melody: usable range ~0.20-0.35. The proper tool would
be `flow_edit_morph` (source-caption -> target-caption V_delta integration),
but it requires a non-turbo base DiT checkpoint (not downloaded).

Round-2 A/B page (same artifact, one loop pass per variant): classic,
faithful-0.80 baseline, synthony at 0.50/0.35/0.20, synthwave/metal/trance
at 0.20, and the two cs05 hybrids:
<https://claude.ai/code/artifact/25c055a1-95a9-4ac7-b7a6-990a44833633>.
Awaiting listening verdict; next knobs if a genre wins: sweep more tracks,
per-track genre assignment (calm vs combat), or the base-model flow-edit.

## Exploration round: operatic symphony, realistic instruments (2026-07-11)

User direction: explore at low retention, captions encouraging realistic
instruments from an "operatic synthony"; old genre samples cleared.
Three captions (operatic symphony w/ wordless choir; Hollywood film score,
realistic acoustic instruments, no choir; operatic synthony = orchestra +
choir over analog synth pulse) at melody 0.20 (6 steps, envelope corr
0.58-0.62) and 0.10 (7 steps, corr 0.42-0.44), plus a seed-1337 take.
New lever measured: **at low retention the seed is a huge variation knob**
(raw corr 0.40 between seeds at 0.20 — different takes of the same recipe);
same-family orchestral captions differ only modestly (0.94-0.95).
Same A/B artifact updated with: classic, landed-HD reference, the six
exploration variants. Awaiting verdict.

**Round-2 verdict (user):** film score 0.20 and operatic synthony 0.20 won;
opera-choir captions and the 0.10 takes dropped (samples deleted). Iteration
round rendered around the winners: seed re-rolls (1337/9001 — corr vs
classic 0.52-0.59, all healthy), tighter melody 0.25 takes (5 steps,
corr 0.62), and a filmsynth hybrid caption merging both winners (0.62).
Same artifact updated (11 tracks, opus 56k to fit the cap). Awaiting verdict.

## Open questions for the verdict

- Does any cover beat "enhanced classic" (path 1) on ensemble coherence?
- Melodic fidelity: does it keep the V14 counter-melody and bass lines?
- Licensing: composition is Alexander Brandon's; local generation avoids
  hosted-service TOS but distribution of regenerated audio is still murky.
