---
title: Secret-level music (ZANAC3) repeats forever in HD mode; level music never returns
status: Fixed
fixed-in: 7ecc398
component: HD remaster / audio (loudness.c audio callback + HD music path)
affects: hd_music only (classic OPL/LDS playback unaffected)
---

# Secret cue loops forever in HD mode

**Status: fixed in `7ecc398`** (confirmed by ear in-game). Regression introduced by `4924808`
("audio: loop untagged HD music tracks instead of falling silent"), which was
built on a premise that measurement has since refuted — see
[hd-music-untagged-loop-silence.md](hd-music-untagged-loop-silence.md).

## Symptom

With `hd_music` on (the default), picking up a secret-level portal plays the
ZANAC3 cue — and then it repeats forever. The level's own music never comes
back for the rest of the mission. Classic OPL (`hd_music = false`) is fine: the
cue plays once and the level music resumes.

## How it is meant to work

Picking up the portal calls `play_song(30)` ("ZANAC3", a **2.26-second one-shot
stinger**) at `src/mainint.c`. The level music is restored by the in-flight loop
in `src/tyrian2.c`, which waits for the cue to finish:

```c
else if (!playing && firstGameOver)
    play_song(levelSong - 1);
```

`playing` is the LDS sequencer's flag (`src/lds_play.c`). `lds_update()` is its
**only** writer of `false` (the `0xfc` terminator); `lds_load()` → `lds_rewind()`
sets it `true` on every `play_song()`.

## Root cause

Two bugs stacked, both from `4924808`:

1. **`playing` is frozen true in HD mode.** In `audioCallback()`
   (`src/loudness.c`), the `hd_active` branch called *only* `hd_music_decode()`.
   The pacing loop that calls `lds_update()` lived in the `else if` branch, so
   in HD mode the sequencer **never ticked** and `playing` never went false. The
   comment claiming the LDS "is always loaded and running as the master clock
   even in HD mode" was simply false.
2. **The untagged-loop heuristic then always fired.** `hd_music_decode()` ended
   with `else if (playing) { seek(0); }` — intended to catch "looping track whose
   OGG wasn't tagged". With `playing` frozen true, *every* untagged track looped
   forever at end-of-stream, and its `else { hd_ended = true; }` fallback was
   unreachable.

So the 2.26 s cue looped forever, and `tyrian2.c`'s restore was waiting on the
same frozen flag — hence also no level music. The heuristic was reading the very
flag its own code path had broken.

## Why the OGG tags are authoritative (the heuristic should never have existed)

`tools/render_music.c` links the game's **real** `lds_play.c`/`opl.c` and derives
the loop tags from that sequencer. Its own header:

> Tracks that terminate instead of looping (an explicit "stop" command in the
> LDS data) get no sidecar loop points.

"Untagged" therefore means "the sequencer terminates", **by construction**.
Measurement agrees 6-for-6 — see the table in
[hd-music-untagged-loop-silence.md](hd-music-untagged-loop-silence.md).

## Fix

`src/loudness.c`, engine-only, no asset change:

1. `hd_music_decode()`: deleted the `else if (playing)` branch. End-of-stream is
   now just: tagged → seek `hd_loop_start`; untagged → `hd_ended = true`. Trust
   the tags.
2. `audioCallback()`: hoisted the sequencer pacing loop into
   `lds_advance(Sint16 *out, int count)`; the classic path passes `out = samples`
   (synthesizing, arithmetic unchanged), the HD path passes `NULL` (advance the
   sequencer, skip `opl_update()` — the same trick `synth_seek_locked()` uses).
   `playing`/`songlooped` are now truthful in both modes.

No `!playing` call site changed — the point is that they all start working in HD
mode unchanged. This also repairs **Game Over Solo → ZANACS** (`src/mainint.c`,
the identical `!playing` pattern), which was broken the same way. *(Inferred from
code reading; not verified at runtime.)*

## Verify

Runtime, `hd_music` on, under lldb (park the main thread after `init_audio()` so
`titleScreen()` can't call `play_song()` and clobber the test; force
`expr play_song(30)`; advance the audio thread past ~2.26 s; **do not** use a
conditional breakpoint on the audio callback — use `breakpoint modify -i N`):

| | before | after |
|---|---|---|
| `playing` past 2.26 s | `true` forever (sequencer never ticked, `ldsFrameCounter == 0`) | **`false`** |
| `hd_ended` | `false` forever | **`true`** |
| `hd_pos` | wraps to 0 every ~99666 samples | freezes at 99621 |

Controls: `hd_music` off is unchanged (`playing` false at ~2.24 s via OPL); a
loop-tagged track (idx 1) still wraps to `hd_loop_start` with `hd_ended` false —
guards against over-correcting.

**lldb orphans `opentyrian` processes here** (reparented to PPID 1, and they hold
a real audio device). After any run: `ps -eo pid,ppid,command | grep opentyrian`
and kill survivors.
