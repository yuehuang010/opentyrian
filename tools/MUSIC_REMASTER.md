# HD music remaster (Phase S1)

How the streamed OGG music (`hdmusic_NN.ogg`) shipped in the data dir is
produced, and how the engine plays it. See `internal/plan/STANDALONE_PLAN.md`
§"Phase S1 — Music remaster" for the design rationale.

## Overview

`music.mus` holds 41 LDS/OPL2-sequenced tracks that the engine normally
synthesizes at runtime through `src/lds_play.c` + `src/opl.c`. The remaster
renders each track **once, offline**, at the full device sample rate using the
game's *own* sequencer/synth (no reimplementation), encodes to OGG Vorbis with
loop metadata, and streams it back at runtime via `stb_vorbis` behind the
`[audio] hd_music` toggle. Missing/unreadable track → transparent LDS fallback.

Pipeline: `music.mus` → **render_music** → `hdmusic_NN.wav` (+ `.loop`) →
**encode_music.sh** → `hdmusic_NN.ogg` (with `LOOPSTART`/`LOOPLENGTH` Vorbis
comments) → dropped into the data dir.

## 1. Build the offline renderer

`tools/render_music.c` links the engine's unmodified `lds_play.c` / `opl.c` /
`musmast.c` against a WAV writer. It provides its own `file.h` stubs and
`audioSampleRate`, so it needs no other engine objects:

```sh
cc -O2 -std=iso9899:1999 $(pkg-config --cflags sdl2) \
   tools/render_music.c src/lds_play.c src/opl.c src/musmast.c \
   $(pkg-config --libs sdl2) -lm -o render_music
```

(SDL2 is pulled in only for its integer typedefs; no audio device is opened.)

## 2. Render every track to WAV + loop sidecar

```sh
SDL_AUDIODRIVER=dummy ./render_music ./tyrian21 ./wav
```

Args: `render_music [data_dir] [out_dir]` (defaults `tyrian21` / `.`). For each
of the 41 songs this writes:

- `hdmusic_NN.wav` — 44.1 kHz **16-bit stereo** PCM. The OPL synth is mono; the
  two channels are identical (not new content). The file ends exactly at the
  sequencer's loop-back point (intro + one loop iteration).
- `hdmusic_NN.loop` — a sidecar with `LOOPSTART <samples>` / `LOOPLENGTH
  <samples>`. Tracks that terminate instead of looping (an explicit LDS "stop")
  get **no** sidecar.

Loop detection watches `songlooped` (set by `lds_play.c` when the sequencer
jumps backward) and maps the loop-target order position to its sample offset via
a per-tick position→offset table. `LOOPSTART` is 0 when a track loops from the
very top, non-zero when it has a distinct intro (e.g. track 4 "CAMANIS",
`LOOPSTART=1135178`).

## 3. Encode to OGG Vorbis

```sh
tools/encode_music.sh ./wav ./tyrian21   # optional 3rd arg: libvorbis -qscale (default 6, ~192 kbps)
```

Requires `ffmpeg` with `libvorbis`. Copies each `.loop` sidecar's values into
`LOOPSTART` / `LOOPLENGTH` Vorbis comments on the OGG. Output lands as
`hdmusic_NN.ogg` in the target data dir.

## Engine playback (`src/loudness.c`)

- `hd_music_load_song()` opens `hdmusic_(song+1).ogg` via `dir_fopen` (so the
  bundle/`--data`/cwd chain applies), decodes with `stb_vorbis`, and reads the
  loop comments. On any failure it silently leaves the synth path active.
- The audio callback pulls decoded stereo frames; at end-of-stream it seeks back
  to `LOOPSTART` for a seamless loop. Volume/fade multipliers, `restart_song`,
  and the jukebox all drive this path unchanged.
- `[audio] hd_music` (default **on**) gates the whole thing; Setup → Sound also
  exposes a live On/Off toggle. Off → the classic LDS synth, byte-for-byte as
  before.

## Known limitation — device sample rate

The loader requires `info.sample_rate == audioSampleRate` (the SDL-negotiated
device rate) and does **not** resample; a mismatch falls back to the synth with
a `warning: hd_music: … want 2ch/NHz` line. The shipped OGGs are rendered at
**44100 Hz**. This matches the common case (and the negotiated rate on the dev
Mac), but on a device that negotiates e.g. 48000 Hz the HD tracks won't load.
To target another rate, re-render (the renderer follows `audioSampleRate`, which
`render_music` hard-sets to 44100 — change `SAMPLE_RATE` there) and re-encode.
A future robustness pass could resample in `hd_music_load_song` instead.
