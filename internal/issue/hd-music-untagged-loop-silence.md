---
title: HD music falls silent mid-mission on looping tracks that lack LOOP tags
status: Fixed
fixed-in: hd-remaster (pending commit)
component: HD remaster / audio (loudness.c HD music path)
affects: hd_music only (classic OPL/LDS playback unaffected)
---

# HD music stops mid-mission on untagged looping tracks

**Status: Fixed on branch `hd-remaster`.**

## Symptom

With `hd_music` on (the default), the background music on some levels stops
partway through a mission and never resumes — silence for the rest of the
level. Classic OPL/LDS playback (`hd_music = false`) is unaffected: those tracks
loop normally.

## Root cause

The HD path streams a pre-rendered OGG per song (`hd_music_decode`,
`src/loudness.c`). At end-of-stream it either seeks back to `LOOPSTART` (if the
OGG carries `LOOPSTART`/`LOOPLENGTH` Vorbis comments) or latches `hd_ended` and
outputs silence.

**6 of the 41 shipped OGGs carry no loop tags** — tracks 10, 11, 19, 25, 31, 34.
They were assumed to be one-shot stingers (see
[../hd-music-compression.md](../hd-music-compression.md)), but that was wrong:

| File | Song (`musicTitle[NN-1]`) | Actually |
|--:|---|---|
| 10 | Level End | true one-shot ✓ |
| 11 | Game Over Solo | true one-shot ✓ |
| 19 | The MusicMan | **looping — was untagged** |
| 25 | The final edge | **looping — was untagged** |
| 31 | ZANAC3 | **looping — was untagged** |
| 34 | High Score Table | **looping — was untagged** |

For the four looping tracks, `hd_has_loop == false` → `hd_ended = true` at EOF →
permanent silence. The classic LDS player loops all of them (`songlooped`,
backward `jumppos`, `src/lds_play.c`), so silence in HD is a clear regression.

## Fix

Engine-only, **no asset change** (the frozen `hdmusic/` OGGs are untouched — no
re-render, no re-tag, no re-commit of the ~54 MB). In `hd_music_decode`, when a
track has no loop tags, mirror the LDS master sequencer instead of falling
silent:

```c
if (hd_has_loop)      { seek(hd_loop_start); }   // tagged: loop at LOOPSTART
else if (playing)     { seek(0); }               // untagged + LDS still playing -> loop whole OGG
else                  { hd_ended = true; }        // genuine one-shot: LDS stopped, so do we
```

The LDS song is always loaded and running as the master clock even in HD mode, so
`playing` is the source of truth: it stays `true` for a looping track and goes
`false` for a genuine one-shot (which is also what `tyrian2.c`'s `!playing` jingle
check relies on). Level End / Game Over therefore still stop correctly.

**Tradeoff:** an untagged track loops from sample 0, so a non-repeating intro bar
(if any) replays each loop, whereas a tagged track resumes at `LOOPSTART`.
Negligible for these four level tracks. If a specific track's intro-replay ever
proves audible, the follow-up is the *data* fix: re-render + tag just that OGG
(the compression note documents why we avoided touching the assets).

## Verify

Play a level that uses one of tracks 19/25/31/34 (e.g. a ZANAC3 level) and let it
run past the OGG length; music should loop instead of going silent. Toggle
`hd_music` off to confirm the classic path was always fine.
