---
title: HD music falls silent mid-mission on looping tracks that lack LOOP tags
status: Refuted / fix reverted
fixed-in: 4924808 (reverted in the hd-secret-music-repeats fix)
component: HD remaster / audio (loudness.c HD music path)
affects: hd_music only (classic OPL/LDS playback unaffected)
---

# HD music stops mid-mission on untagged looping tracks

> **⚠️ THIS ISSUE'S PREMISE WAS WRONG. Do not use it as a reference.**
>
> The central claim below — that tracks 19/25/31/34 are *looping tracks whose
> OGGs were left untagged* — is **false**, and was never measured. All six
> untagged OGGs are **genuine one-shot stingers**. The fix built on that claim
> (`4924808`) caused a real bug: the secret-level cue looped forever and level
> music never returned. See
> [hd-secret-music-repeats.md](hd-secret-music-repeats.md). `4924808`'s
> `else if (playing)` heuristic has been reverted.
>
> The document is kept for the record. Corrected findings are below; the
> original text follows, struck through in spirit.

## Corrected findings (measured, 2026-07-16)

Two independent methods, in exact agreement:

1. **Harness** — compiled the real `src/lds_play.c` + `src/opl.c` against the
   actual `tyrian21/music.mus`, drove `lds_update()` for all 41 songs at 69.5 Hz
   with a 4-minute cap, recording `playing`/`songlooped`.
2. **OGG durations** — read off the shipped `hdmusic/*.ogg`.

**Exactly 6 of 41 songs self-terminate** (set `playing = false` via the `0xfc`
terminator), and they are **exactly** the 6 OGGs with no loop tags, with
durations matching to within milliseconds:

| idx | file | title | LDS | OGG | verdict |
|--:|--:|---|--:|--:|---|
| 9 | 10 | End of Level | 5.57s | 5.56s | one-shot ✓ |
| 10 | 11 | Game Over Solo | 10.53s | 10.52s | one-shot ✓ |
| 18 | 19 | The MusicMan | 5.48s | 5.48s | **one-shot** (doc claimed looping ✗) |
| 24 | 25 | The final edge | 78.46s | 78.40s | **one-shot** (doc claimed looping ✗) |
| 30 | 31 | ZANAC3 | 2.24s | 2.259s | **one-shot** (doc claimed looping ✗) |
| 33 | 34 | High Score Table | 7.29s | 7.28s | **one-shot** (doc claimed looping ✗) |

The other 35 songs loop forever via the `0xf9` jump-back (`jumppos < posplay`)
and **are** loop-tagged.

This is not a coincidence: `tools/render_music.c` links the game's real
sequencer and, per its own header, *"Tracks that terminate instead of looping
(an explicit 'stop' command in the LDS data) get no sidecar loop points."*
**Untagged means self-terminating, by construction. The tags are authoritative
— never second-guess them.**

The claim below that *"The classic LDS player loops all of them"* is therefore
false; the classic player terminates all six.

## What about the original "falls silent" symptom?

**Unresolved — it was real but misattributed, and this fix was the wrong
answer.** *(Inferred from code reading; not verified at runtime.)*

If a level's `levelSong` is one of the self-terminating tracks, the music stops
at the song's natural end in **both** modes: `tyrian2.c`'s
`!playing && firstGameOver` → `play_song(levelSong - 1)` is a **no-op**, because
`play_song()` early-outs when `song_num == song_playing`. So classic goes silent
too — HD was never the outlier.

If the symptom resurfaces, the candidate fixes are:
- a **data** fix — re-render + tag that specific OGG (see
  [../hd-music-compression.md](../hd-music-compression.md) for why we avoid
  touching the ~54 MB of assets); or
- a `levelSong`-restart fix — make the restore path actually restart a finished
  song rather than no-op.

**Not** a `playing`-based heuristic in the decoder.

---

## Original text (retained for the record — premise refuted, see above)

## Symptom

With `hd_music` on (the default), the background music on some levels stops
partway through a mission and never resumes — silence for the rest of the
level. Classic OPL/LDS playback (`hd_music = false`) is unaffected: those tracks
loop normally. *(← false; see Corrected findings.)*

## Root cause (as originally believed)

The HD path streams a pre-rendered OGG per song (`hd_music_decode`,
`src/loudness.c`). At end-of-stream it either seeks back to `LOOPSTART` (if the
OGG carries `LOOPSTART`/`LOOPLENGTH` Vorbis comments) or latches `hd_ended` and
outputs silence.

**6 of the 41 shipped OGGs carry no loop tags** — tracks 10, 11, 19, 25, 31, 34.
They were assumed to be one-shot stingers (see
[../hd-music-compression.md](../hd-music-compression.md)) — *and that assumption
was correct after all; the "correction" below is the error.*

For the four looping tracks, `hd_has_loop == false` → `hd_ended = true` at EOF →
permanent silence. *(← premise false: those four are one-shots.)*

## Fix (as originally applied in 4924808 — now reverted)

```c
if (hd_has_loop)      { seek(hd_loop_start); }   // tagged: loop at LOOPSTART
else if (playing)     { seek(0); }               // untagged + LDS still playing -> loop whole OGG
else                  { hd_ended = true; }        // genuine one-shot: LDS stopped, so do we
```

The reasoning was: *"The LDS song is always loaded and running as the master
clock even in HD mode, so `playing` is the source of truth."* **This was false.**
`audioCallback()` only ran the sequencer on the classic path, so in HD mode
`playing` was frozen `true` and the `else` was unreachable. Both the premise
(these tracks loop) and the mechanism (`playing` is meaningful in HD mode) were
wrong.
