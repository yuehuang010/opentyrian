# HD music: OPL idioms that don't survive sample playback (and the fixes)

Distilled from the track-30 ear-tuning sessions (2026-07-10). These will
recur on every other LDS track — check for them BEFORE hunting soundfont
presets, because no GM program fixes a composition-level idiom. Full track-30
narrative: [hd-music-track30.md](hd-music-track30.md).

## The principle: rewrite the part, don't transcode it

LDS songs are composed FOR the OPL2: instant attack, zero reverb, and
several tricks that exploit how FM voices fuse. A 1:1 note-for-note MIDI
conversion is pitch-exact (proven, see hd-music-pitch-report.md) and still
sounds wrong wherever the music relies on such a trick. The winning fixes
re-author the *part* (fewer/different events with the right samples) rather
than searching for a magic preset.

## Idioms found so far in the data

1. **Delay-echo doubling** (track 30 V14): every note re-struck with the
   same pitch/length on a sibling channel a fixed +6 ticks later — the
   classic no-reverb echo. Samples flam ("sawing"). Fix: detect the double
   (same fingerprint + pitch, still keyed, ≤10 ticks) and scale its velocity
   — `echo=NN` map option in lds_to_midi; 75% kept impact without flam.
2. **Burst fusion** (track 30 V13): a low note + a 13-note 37 ms cascade
   every ~0.9 s. The chip fuses each burst into ONE composite hit ("small
   drum with a small brass hat"); samples machine-gun 13 strikes. Fix:
   `tools/v13_percussion.py` rewrites the part as drum-channel percussion
   (tom + closed hat per LOW note, open-hat tail per cascade, choke before
   silence), mixed in sample-aligned via the pipeline `PERC_MIDI`/`PERC_GAIN`
   hook. Gotcha inside the gotcha: emit per LOW NOTE, not per
   silence-delimited burst — one continuous 91 s stretch holds 42 drum hits.
3. **Sub-grid detuning as color**: some voices are intentionally played
   ~+0.4 st off the equal-tempered grid. Fidelity target is the OPL chip
   frequency (`opl_hz` in .notes), not the nominal note — `cents` map column
   (MIDI RPN 1).
4. **37 ms same-note repeats** (V19): only mallet presets articulate them;
   everything else mushes. (Marimba worked for V19's repeats; it did NOT
   work for V13's bursts — repeats and bursts are different idioms.)

## Diagnosis workflow that found these

1. **Isolate the voice on BOTH synths.** `LDS_SOLO_FPS=<fp,fp,...>`
   (render_music env) plays only the listed voice fingerprints through the
   real OPL — the reference for what the voice actually contributes.
   Unfiltered output stays byte-identical. GM side: derived solo map
   (others vol 0) via `LDS_GM_MAP`.
2. **Read the .notes dump before proposing instruments.** V13's "arpeggio"
   label hid the burst structure; one awk over the dump exposed it (onset
   clustering, pitch histogram). The user's ear description ("one drum
   hit") matched the data, not the .patches summary.
3. **Loudness-match every A/B row** (candidates to a common mean dB) so the
   user judges character, not level.
4. **Balance is measurable**: `tools/compare_mix_balance.py` +
   `tools/stem_groups_30.txt` — solo each group through both chains,
   compare group-vs-own-full-mix loudness; the delta is the CC7 correction.
   Caveat: EBU LUFS is K-weighted and under-counts bass — cross-check
   low-frequency questions with broadband/lowpassed RMS (the "bass too
   loud" complaint measured ~neutral in LUFS, +1 dB broadband).

## Rendering facts worth not re-deriving

- fluidsynth `-ni` leaves reverb+chorus ON; CC91/93 are inaudible with
  FluidR3. Dry = `-R 0 -C 0` (pipeline `FLUID_EXTRA`).
- All renders share the LDS tick clock (1 tick = 113.669048/7900 s), so
  separately rendered stems/percussion amix sample-aligned by construction.
- The listening-page artifact caps at 16 MB — retire stale candidate cards
  when adding rounds; keep a visible build stamp so audio-only changes are
  provable to the listener.
