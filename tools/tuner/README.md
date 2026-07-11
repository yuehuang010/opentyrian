# HD music tuner

A local, zero-dependency web app for ear-tuning the HD (soundfont) remaster
of an OpenTyrian LDS music track: swap GM instruments, adjust per-voice
volume/transpose/cents/gate/echo, remix, and A/B against the classic OPL
render in real time — no AI in the loop, just your ears and a browser.

## Run it

```sh
python3 tools/tuner/server.py --song 30
```

Then open the printed `http://localhost:8765` in a browser. Flags:

```
--song N        1-based song number (default 30)
--port N        HTTP port (default 8765)
--data DIR      Tyrian data dir (default /Users/felixhuang/source/opentyrian/tyrian21)
--sf2 FILE      Soundfont (default hdmusic_work/sf/FluidR3_GM.sf2)
--map FILE      GM voice map to edit (default tools/lds_gm_map.txt)
--trim SECS     Override loop-trim length; defaults to the hardcoded 113.669048s
                for song 30 (see internal/hd-music-track30.md), untrimmed otherwise
```

Requires the same toolchain as `tools/hd_music_pipeline.sh`: `ffmpeg`,
`ffprobe`, `fluidsynth` on `PATH`, and a C99 compiler to build
`hdmusic_work/bin/{render_music,lds_to_midi}` on first run if they're not
already built.

## What it does

On startup the server runs `lds_to_midi` once (into
`hdmusic_work/tuner/<song>/base/`) to get the `.notes`/`.patches` dumps, then
kicks off a background worker pool (4 parallel `fluidsynth`/`render_music`
processes) that renders every voice as an isolated mono stem plus the full
classic OPL reference. The page polls `/api/state` every 2s and streams
stems into the mixer as they finish — you can start listening immediately
with whatever's ready.

Each table row is one OPL voice fingerprint. Edit GM program / transpose /
cents / gate / echo and hit **Apply** to re-render just that voice and
hot-swap it into the currently-playing mix without a glitch. The **volume**
slider, mute, and solo are instant — pure client-side gain, no re-render.
The two special track-30 voices (the OPL-chip intro drone and the rewritten
percussion part, see `internal/hd-music-opl-idioms.md`) only expose a gain
control, since their audio doesn't come from a GM program at all.

The transport bar A/B-switches between the classic OPL render and your HD
mix with a fast crossfade, without stopping playback, so you can flip back
and forth on the same beat.

## Saving

**Save to map** rewrites `tools/lds_gm_map.txt` (or whatever `--map` points
at) — losslessly: line order, the header comment block, and every line's
trailing comment are preserved byte-for-byte; only the numeric
fields/options of fingerprints you actually changed are rewritten. A
one-deep backup is kept at `<map>.bak` (overwritten each save, not
accumulated). The save bar shows a dirty indicator and a preview of exactly
which fingerprints/fields differ from the last load.

Saving does **not** re-render anything — it only writes the map. Rerun
`tools/hd_music_pipeline.sh gm <song> <sf2>` (or restart the tuner) to
produce a full mix from the saved map.

## Cache

Stems are cached under `hdmusic_work/tuner/<song>/stems/` as
`<fp>_<settings-hash>.opus` (48kHz mono, keyed by fingerprint + GM program +
transpose + cents + gate + echo + song; volume is NOT part of the hash since
it's applied client-side). The tuner never touches `hdmusic_work/midi/` or
other `hd_music_pipeline.sh` caches — it renders through its own temp maps
and out-dirs under `hdmusic_work/tuner/`, so the two tools can't stomp on
each other's state.
