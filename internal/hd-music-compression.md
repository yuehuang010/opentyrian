# HD music compression — experiment + decision (2026-07-12)

**Decision: leave the shipped HD music at Vorbis ~q6 (~54 MB) for now.** The
achievable savings aren't large enough to justify the work/risk yet. Revisit if
the repo-size cost of the committed `hdmusic/` OGGs becomes a real problem, or if
Opus integration lands for another reason.

## Context

The 41 HD music tracks (`hdmusic/hdmusic_NN.ogg`, ~54 MB) are committed in-repo
as regular git blobs (LFS was blocked — this repo is a public fork, and GitHub
refuses LFS uploads to forks; see commit `17892f0`). That one-time 54 MB in
history prompted the question: can we make it meaningfully smaller?

## Experiment

Ran an agent to sweep encoders, encoding from **lossless re-renders**
(`tools/render_music.c` → WAV, not lossy→lossy off the shipped OGGs). Full
41-track totals:

| Config | Total | % of baseline | Decoder change? |
|---|---:|---:|---|
| Baseline (shipped Vorbis ~q6) | 54 MB | 100% | — |
| Opus 64k | 22 MB | 40% | new decoder + loop rework |
| Opus 96k | 30 MB | 54% | new decoder + loop rework |
| Opus 128k | 40 MB | 72% | new decoder + loop rework |
| Vorbis q3 | 29 MB | 53% | none |
| Vorbis q4 | 34 MB | 63% | none |
| Vorbis q5 | 40 MB | 74% | none |

## Why not Opus (the crux)

Opus is the only option with a big win (30 MB @96k, ~46% smaller), but it breaks
seamless looping and needs real engineering:

- **35 of 41 tracks carry `LOOPSTART`/`LOOPLENGTH`** Vorbis comments (sample
  offsets, under ffprobe `stream_tags` not `format_tags`; 6 lack them — tracks
  10, 11, 19, 25, 31, 34). `src/loudness.c` `hd_music_load_song()` seeks to
  `LOOPSTART` at end-of-stream.
  - **Correction (2026-07-12):** these 6 are NOT all one-shot stingers — that
    earlier assumption was wrong and it hid a bug. Only tracks 10 (Level End)
    and 11 (Game Over) are true one-shots. Tracks **19 (The MusicMan), 25 (The
    final edge), 31 (ZANAC3), 34 (High Score Table) are looping tracks whose
    OGGs simply were never tagged** — so in HD mode they fell silent at EOF
    mid-mission. Fixed engine-side (loop untagged tracks from 0 while the LDS
    master is still `playing`); assets untouched. See
    [[issue/hd-music-untagged-loop-silence]].
- Opus is hard-locked to **48000 Hz** and carries a **312-sample pre-skip**, so
  44100-domain loop offsets don't survive a naive re-encode — they'd need
  recomputing. `ffmpeg -c:a libopus` also drops the LOOP* comments by default.
- **`stb_vorbis` cannot decode Opus at all** (different id packet). Would need a
  new decoder (libopus + opusfile; `opusfile` wasn't even available via
  pkg-config on the dev Mac).
- `hd_music_load_song()` requires exact `sample_rate == audioSampleRate` (44100)
  with **no resampling** — Opus @48k fails that unconditionally (the same
  limitation noted in `tools/MUSIC_REMASTER.md` §"Known limitation").

Estimated Opus integration = multi-day, architecture-tier work, not a flag.

## The cheap option, if we ever want it

**Vorbis q4 → ~34 MB (~37% smaller), zero code changes** — same `stb_vorbis`
decoder, same 44.1 kHz, loop tags stay in-domain. Just re-run `encode_music.sh`
at q4 instead of q6 (encode from the lossless re-renders, preserve loop
sidecars). Rejected for now only because ~20 MB saved isn't worth even a
re-encode + re-audition pass; a small quality regression on frozen assets.

**Quality caveat:** objective metrics were trustworthy for Vorbis but unreliable
for Opus (the 44.1→48→44.1 resample round-trip dominates the numbers) — any Opus
quality call needs human listening. See also [[hd-music-opl-idioms.md]] for why
these tracks are quality-sensitive.

## Reproduce

Experiment artifacts (encodes, 41 lossless WAVs, 15 A/B audition clips, sweep
scripts) were produced under a session scratchpad, not committed. To redo: build
`render_music` per `tools/MUSIC_REMASTER.md` §1, render to WAV, then
`ffmpeg -c:a libopus -b:a <R>` / `-c:a libvorbis -q:a <Q>` per track and sum
sizes. Related: [[hd-build-pipeline.md]] (the `music` staging step).
