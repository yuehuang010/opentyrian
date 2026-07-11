# HD music remaster — track 30 (title theme) progress log

Status: **in progress** (2026-07-10). Goal: an HD-sounding replacement for the
OPL2 render of track 30, judged by ear via an A/B artifact page.

## Pipeline (all in-tree, uncommitted as of this note)

- `tools/lds_to_midi.c` — taps the game's own `lds_play.c` sequencer and emits
  MIDI + a `.patches` voice report + a `.notes` per-note dump. OPL voices are
  identified by an 8-hex FNV-1a fingerprint over their timbre registers.
  `LDS_GM_MAP` env var overrides the map file (used to render per-voice stems
  by zeroing other voices' volumes).
- `tools/lds_gm_map.txt` — fingerprint → GM program [transpose] [volume 0-127
  as CC7]. The tuning knob for everything below.
- `tools/hd_music_pipeline.sh` — `build` / `midi 30` / `gm 30 <sf2> <variant>`
  (FLUID_GAIN env) / `install` / `restore`. **Gotcha:** it caches
  `hdmusic_work/midi/hdmusic_30.mid`; `rm hdmusic_work/midi/hdmusic_30.*`
  after any map edit.
- Soundfonts in `hdmusic_work/sf/`: FluidR3_GM.sf2, GeneralUser-GS.sf2.
- Loop = 7900 sequencer ticks = 113.669048 s (1 tick ≈ 14.4 ms). Verify render
  loudness with `ffmpeg -af volumedetect`; FluidR3 draft gain ≈ 0.3, later
  boosted-CC7 renders ≈ 0.48 (0.6 clips).
- A/B listening page: Claude artifact
  <https://claude.ai/code/artifact/627d1f28-99be-48d7-a892-15d56644f67d> —
  variant buttons, per-voice piano roll (V-numbers = voices sorted by first
  note-on), and an 8-stem mute mixer. Built from
  `scratchpad/player_template.html`.

## What we tried (v1–v6, FluidR3 orchestral)

Six iterations of per-voice GM substitution with FluidR3, tuned by the user's
ear per voice (map file has the full per-fingerprint comments):

- Main melody + riffs → Brass Section; 2nd melody → tried trumpet (distorts at
  max velocity), french horn (washed), brass section (muffled), piano, finally
  Overdriven Guitar.
- Removed voices that had no acceptable sampled counterpart: the 37 ms
  4751-note arpeggio (8251725a), the timpani pulse (efa39919), a bell tick
  (15a839b2).
- Learned: nothing acoustic articulates 37 ms notes except mallets (marimba);
  FluidR3's per-sample vibrato/detune/room makes stacked voices smear.

## Verdict (user, 2026-07-10)

**FluidR3 per-voice substitution failed at the ensemble level.** Individually
each voice sounds good; together they don't harmonize — the OPL original is
more coherent. Root cause: the piece is composed for FM voices (instant
attack, zero reverb, uniform tuning, mono/dry); sampled orchestral voices each
bring their own attack lag, vibrato, detune and room, so the stack never locks.

## Paths forward (agreed 2026-07-10)

1. **Enhanced classic** — remaster the OPL render itself (post-processing).
   **Already implemented by the user.** Baseline to beat.
2. **Period-correct GM soundfont** — GeneralUser GS tried (`gus` variant): not
   much better than path 1. Next: a real **SC-55 soundfont** (dry, uniform,
   built to blend — the authentic 1995 wavetable upgrade).
3. **One-family arrangement** — all voices from a single coherent palette
   (all-synth leads/pads/basses) instead of a mixed orchestra, so the ensemble
   glues.
4. **Hybrid** — classic OPL render as the bed, sampled lead voices layered on
   top (stems are sample-aligned to the same tick clock, mixing is linear).

## Experiment renders (2026-07-10, awaiting listening verdict)

All three are on the artifact page alongside classic and the FluidR3-v6 stems:

- **Path 2 / `sc55`** — `hdmusic_work/sf/SC-55.sf2` (nitro-shoe/sc-55-soundfont
  v1.24 GitHub release, 9.2 MB, an SC-55 imitation font — Kitrinx's converter
  needs ROM dumps, skipped). Same v6 voice map, FLUID_GAIN 0.48 →
  `variants/hdmusic_30.sc55.ogg` (mean −19.4 dB).
- **Path 3 / `synthfam`** — one-family all-synth arrangement,
  `hdmusic_work/maps/synthfam_30.txt` via `LDS_GM_MAP` override (FluidR3,
  gain 0.42, mean −18.0 dB). Palette: saw/square leads, Synth Brass, Synth
  Strings, Synth Bass, Synth Voice, warm pad, FX-3 crystal for runs. The two
  voices removed in the orchestral pass return quietly (37 ms arp → square
  lead vol 60; pulse → Synth Drum vol 70) since synths articulate them.
- **Path 4 / `hybrid`** — classic OPL render + sampled stems layered on top
  (melody ×0.6, 2nd melody ×0.5, choir ×0.45; ffmpeg amix, durations align
  because both derive from the same tick clock) →
  `variants/hdmusic_30.hybrid.ogg` (mean −20.5 dB, matches classic's −20.8).
- **Fusion (`fusion`)** — user idea: sum paths 2+3+4 into one wide modern mix.
  Hybrid stays center bed; SC-55 `extrastereo=m=2.0` ×0.5 and all-synth
  `extrastereo=m=2.6` ×0.45 fill the sides; ×0.7 trim + limiter →
  `variants/hdmusic_30.fusion.ogg` (mean −19.3 dB). Layering works because
  every render plays the identical performance off the same tick clock.
