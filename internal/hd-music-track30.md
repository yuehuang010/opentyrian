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
## Pitch correction fix (2026-07-10, "right instrument, wrong keys")

The automated validator (see `internal/hd-music-pitch-report.md`) proved the
MIDI itself is pitch-exact and found the real cause: GM preset tuning in the
bass register — Contrabass ~+0.9 st sharp and String Ensemble ~+0.77 sharp in
ALL three fonts, Synth Bass ~−0.4..−0.5 flat → layered bass lines disagreed by
>1.2 st. Also: the LDS data intentionally detunes some voices off the grid
(d877be97 +0.45 st, 7fda9b3c +0.40), so fidelity target = OPL frequencies,
not equal temperament.

Fix: `lds_gm_map.txt` gained a 5th column `cents` (−99..99) emitted as MIDI
RPN 1 channel fine-tuning when a fingerprint takes a channel (fluidsynth
honors it — verified empirically: a d877be97 solo note went from +0.48 st to
+0.00 vs the chip frequency). Correction rule per voice:
`cents = 100 × (TestA_median_bias − TestB_median_offset)`. Applied:
d877be97 −48, e647c759 −69, 00d2b08d −74, c5eedaea −87 (borrowed from
00d2b08d), 13fecaf5 +33, bf6c72e9 +34 (borrowed), 0d5f4d92 +34,
4ffe5d4b +27, 7fda9b3c +50. Synthfam map: +33/34 on its Synth Bass voices.
sc55/synthfam/fusion variants and bass/pads stems re-rendered.

DONE (2026-07-10): the f0-analysis core is ported to C
(`tools/pitch_analyze.c`, built by `hd_music_pipeline.sh build` into
`hdmusic_work/bin/pitch_analyze`); `validate_pitch.py` stays the driver and
batches all windows of a render into one process call (analysis ~97× faster
per render; a full run is now fluidsynth-render-bound, ~8 min total).
Golden-referenced against the pure-python path (kept behind `--pure-python`):
3 diverse voices × 80 windows, identical f0/gating to <0.02 st. TEST B's
expected frequency was verified correct as-is: `opl_hz × 2^(transpose/12)`,
deliberately EXCLUDING the cents column (cents steers the render toward
opl_hz; folding it into the reference too would cancel the correction out of
the measurement). First full post-cents-correction run confirms the fix: all
corrected voices now read |median| ≤ 0.08 st vs OPL truth on all 3 fonts
(bf6c72e9 remains detector-unreliable `?`, MAD 0.46; c5eedaea has no usable
solo windows, n/a).

- **Fusion (`fusion`)** — user idea: sum paths 2+3+4 into one wide modern mix.
  Hybrid stays center bed; SC-55 `extrastereo=m=2.0` ×0.5 and all-synth
  `extrastereo=m=2.6` ×0.45 fill the sides; ×0.7 trim + limiter →
  `variants/hdmusic_30.fusion.ogg` (mean −19.3 dB). Layering works because
  every render plays the identical performance off the same tick clock.

## Direction locked: FluidR3 (2026-07-10, user)

User verdict after the pitch fix: FluidR3 is fine for 0-30s (the gate-floor
prototype turned out unnecessary for the stabs — the earlier "wrong keys"
impression came from hybrid/fusion layering, now retired along with SC-55 and
all-synth). **FluidR3 per-voice mapping is the chosen path.** Page is down to
Classic + FluidR3 v6 stems. Current focus: V14 (4b52fb40, 2nd melody).

V14 profile (from .notes, per loop): 304 notes, monophonic, on the melody's
channels 0/1, range 45..81, gates 86 ms..7.1 s (median 230 ms), enters 31.4 s,
spans 31-46 / 57-61 / 64-66 / 83-101 s; 136/304 notes overlap the main brass
melody (it's a duet counter-melody, not a hand-off). Needs: indefinite
sustain for the 7 s holds, fast articulation for the 86 ms runs, and timbral
contrast against Brass Section without leaving the ensemble's world.
Candidate strip on the page (0:30-1:01 in-context clips, only V14 swapped):
overdriven gt (current) / clean gt / distortion gt / trombone / synth brass 2
/ saw lead. Instrument history: trumpet (velocity-layer distortion), french
horn (washed), brass section (muffled), piano (no), overdriven gt (muffled).

## V14 resolved: Trumpet, dry, gate=85, echo=75 (2026-07-10, user-approved)

Round-2 findings that generalize beyond V14:

- **fluidsynth renders were never dry**: `-ni` leaves the reverb+chorus units
  ON (global). CC91/CC93 sends do nothing audible with FluidR3 (preset-level
  sends dominate) — the working lever is `fluidsynth -R 0 -C 0`. The "echo-y
  vs original" complaint against Synth Brass 2 / Saw Lead was mostly this.
- **The LDS data fakes delay-echo by doubling notes**: every V14 note is
  re-struck with identical pitch/length on a sibling channel exactly +6 ticks
  (86 ms) later (ch1 lead, ch0 echo; 120 doubles/loop). On OPL that's the
  classic no-reverb echo trick; with samples it flams ("sawing"). New map
  option `echo=NN` scales a detected double's velocity (same fingerprint +
  pitch, still keyed, within 10 ticks). 75% kept the impact w/o the flam.
- Other new per-voice map options from this round: `gate=NN` (emitted-gate
  percent — 85% restored OPL-style note separation), `rev=`/`cho=` (CC91/93,
  kept for CC-honoring fonts, no-op on FluidR3).
- Winning V14 line: `4b52fb40 56 0 127 0 gate=85 echo=75` — Trumpet at max
  CC7 (user wanted +10%, ceiling allowed +6%), trumpet history: plain=
  distorted (velocity layer), horn/brass=washed; dry+short+echo-tamed works.

## V10 + V13 restored (2026-07-10, user request)

After hearing the full dry v8 context, the user asked for the two removed
rhythm voices back: efa39919 (V10, fixed-note pulse) -> Timpani vol 80, and
8251725a (V13, 4751-note 37 ms arpeggio) -> Marimba vol 60 (quiet; mallets
are the only acoustic voice that articulates 37 ms repeats — same lesson as
V19). Full variant re-rendered as `fluidr3v8` (mean -18.4 dB, max -1.9 dB,
no clipping) plus stems 6/7; page stamp "FluidR3 v8".

## V13 resolved: rewritten as drum-kit percussion (2026-07-10, user-approved)

Isolating V13 (8251725a) against the classic chip (new `LDS_SOLO_FPS` env in
render_music — masks key-ons of unlisted voice fingerprints, unfiltered
output byte-identical) showed the "4751-note 37 ms arpeggio" is really
bursts: a low note (<55) + a 13-note high cascade, which the OPL fuses into
one composite hit ("small drum with a small brass hat", per the user). No GM
program articulates that — six percussion candidates all failed ("I hear one
drum hit"), and mallets machine-gun the cascade.

Fix: **rewrite the part, don't transcode it.** `tools/v13_percussion.py`
reads the .notes dump and emits drum-channel MIDI: hi-mid tom (48) + closed
hat (42) on EVERY low note (not per burst — a 91 s continuous section has 42
drum hits; grouping by silence gaps missed 0:46-2:17), quiet open-hat (46)
tail per cascade segment, pedal-hat (44) choke before real silence. Same
tick clock as lds_to_midi, so the render is sample-aligned and amixed in by
the pipeline's new `PERC_MIDI`/`PERC_GAIN` hook (+2.4 dB w/ FLUID_GAIN
0.48). 8251725a is vol 0 in the map. Full render: `fluidr3v9` (mean −19.4,
max −2.1 dB). An OPL-chip-layer fallback also prototyped and rejected in
favor of the rewrite.

## Mix balance vs classic (2026-07-10, measured, NOT yet applied)

`tools/compare_mix_balance.py` (+ `tools/stem_groups_30.txt`) renders each
group solo through both chains and compares group-relative loudness. First
60 s: melody −4.1 dB vs classic balance (too quiet), ticks −3.5, brassriff
−2.3, choir +1.7 hot, figuration +1.6 hot, bass ~neutral except 13fecaf5
~+1 dB hot broadband. Suggested CC7 set is in the session log; user has not
yet picked which moves to apply.

## V1 intro drone -> real OPL chip; V6 cello -4.3 dB (2026-07-10, user)

The intro drone (c5eedaea) is an octave-doubled pair (38+26, i.e. 73+36.5
Hz) with independent ~5 Hz +-40c software vibrato per layer. The vibrato IS
captured (pitch bends) and rendered, but 36.5 Hz on any sampled preset is
inaudible rumble, so the beating double-layer collapses to one voice.
Brighter presets and a fold-up-to-unison prototype were rejected by ear:
"I don't think a string instrument can replicate it." Resolution: the voice
is vol 0 in the map and rendered by the REAL OPL chip via the pipeline's new
`CHIP_FPS`/`CHIP_GAIN` hook (render_music LDS_SOLO_FPS solo, amixed at
+2.6 dB = measured GM-minus-classic full-mix gap). This is the second
"chip idiom" resolution after V13 (rewrite) — chip-layer is the fallback
when rewriting isn't faithful either.

V6 (4ffe5d4b, Cello): measured +2.1 dB hot vs classic balance per-voice
(the pads GROUP average hid it); user chose the stronger -4.3 dB cut
(CC7 78) since it's accompaniment. Full render: `fluidr3v10` (mean -19.6,
max -2.1 dB) with PERC_MIDI + CHIP_FPS both active.
