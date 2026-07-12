# tools/acestep — AI music regeneration (ACE-Step 1.5 covers)

Regenerates the OPL soundtrack as HD audio by feeding the classic render to
ACE-Step 1.5's audio-to-audio cover mode. Full history, verdicts, and API
gotchas: `internal/hd-music-ai-regen.md`.

**Locked recipe** (user-approved 2026-07-11): `task_type="cover"`, PyTorch/MPS
DiT (`use_mlx_dit=False` — the MLX path silently ignores the melody knob),
`cover_noise_strength=0.80`, seed 4242, generic orchestral caption.

## Setup (one-time)

```sh
git clone https://github.com/ACE-Step/ACE-Step-1.5.git ~/source/ACE-Step-1.5
cd ~/source/ACE-Step-1.5 && uv sync    # brew install uv first if needed
```

Model checkpoints (~9 GB) auto-download on first run. Override the install
location with `ACESTEP_ROOT=/path/to/ACE-Step-1.5`.

## Render the full soundtrack

```sh
cd ~/source/ACE-Step-1.5
uv run python /path/to/opentyrian/tools/acestep/render_all.py
```

Per song 1..41: classic OPL render (via `tools/hd_music_pipeline.sh classic`)
-> loop-aware cover source -> ACE-Step cover -> cut to one pass, 44.1 kHz
stereo, mean-volume-matched to classic, LOOPSTART/LOOPLENGTH-tagged ->
`hdmusic_work/variants/hdmusic_NN.acestep.ogg`. Idempotent: existing covers
are skipped, so a re-run only processes missing/failed songs. Logs an
envelope-correlation score per song (~0.65+ is a faithful cover; near zero
means the model strayed — re-roll with another seed or caption).

Install into the game data dir (with automatic backup):

```sh
for nn in $(seq 1 41); do tools/hd_music_pipeline.sh install $nn acestep; done
```

In-game A/B: jukebox — Up/Down change song, Left/Right seek, H switches
HD remix vs classic OPL synth in place (see src/jukebox.c).

## A/B listening page

`build_ab_page.py <song-key>` builds a self-contained HTML player
(`hdmusic_work/acestep/ab_page_<key>.html`, audio base64-embedded) from the
track lists in its `SONGS` dict — used for the pre-integration listening
rounds; edit `SONGS` to add tracks/variants.
