# Phase S2 — SFX & Voice Remaster: Upsampling Experiment

Experiment for the "SFX & voice remaster" phase of the standalone plan. Goal:
take Tyrian's tiny 8-bit / 11025 Hz sound banks and produce clean 16-bit /
44.1 kHz assets **without changing playback timing or the game's feel**, then
pick a default resampling method for S2.

This is tooling + audio experimentation only. No engine C code was modified.

## Source data & format

Three banks (read-only, from the freeware Tyrian 2.1 data):

| file        | count | notes                          |
|-------------|-------|--------------------------------|
| tyrian.snd  | 29    | SFX (`SFX_COUNT`)              |
| voices.snd  | 9     | voices (`VOICE_COUNT`)         |
| voicesc.snd | 9     | Christmas voices               |

Format (verified against `src/nortsong.c` `loadSndFile` and `src/sndmast.c`):

- Header: `Uint16` LE count, then `count` × `Uint32` LE offsets. Sample *i*
  spans `offset[i]`..`offset[i+1]`; the last runs to EOF.
- Raw **signed** 8-bit mono PCM at **11025 Hz**, each ≤ 64 KiB.
- Engine quirk replicated in extraction: the **last 100 bytes of every voice
  sample** (voices.snd / voicesc.snd only) are stripped as bad data.
- Names from `soundTitle[]`.

Extractor header/count checks all pass: `sfx=29, voice=9, voicesc=9`. The
100-byte voice strip is confirmed (`raw_bytes − kept_samples == 100` for every
voice). SFX are stored untouched.

## Tools (in `tools/`)

- **`hd_extract_snd.py`** — stdlib-only. Splits the three banks into pristine
  per-sample WAVs under `hdsfx_previews/orig/`, stored as 8-bit *unsigned* PCM
  (the WAV spec's only 8-bit form; a lossless `+128` bijection of the signed
  source). This is the source-of-truth reference set. Takes the data dir as an
  arg (default `/Users/felixhuang/source/opentyrian/tyrian21`).
- **`hd_upsample_snd.py`** — needs numpy / scipy / soxr. Reads the reference
  WAVs and writes 16-bit / 44.1 kHz candidates under
  `hdsfx_previews/<method>/`, then prints the duration-verification table and
  metrics, and writes `hdsfx_previews/metrics.csv` (per-sample).

## Resampling stacks available on this host

- Python 3.9 with a venv holding **numpy 2.0.2, scipy 1.13.1, soxr 1.1.0**
  (installed for the experiment — system python had none).
- **ffmpeg** and **afconvert** present on the macOS host (not needed once the
  numpy/scipy/soxr stack was available; kept as fallback options).

The full high-quality stack was available, so no fallback to ffmpeg/afconvert
or a hand-rolled sinc kernel was necessary.

## Methods

| method   | pipeline |
|----------|----------|
| `linear` | naive linear interpolation — the **control**, ≈ what `SDL_ConvertAudio` does in `loadSndFile` today |
| `poly`   | `scipy.signal.resample_poly(4, 1)` — Kaiser-windowed-sinc polyphase FIR |
| `soxr`   | libsoxr VHQ variable-rate resampler |
| `clean`  | poly + DC removal + gentle 5.2 kHz Butterworth low-pass + TPDF dither |
| `best`   | **RECOMMENDED**: DC removal + soxr VHQ + TPDF dither, no extra low-pass |

Resample ratio is exactly `44100 / 11025 = 4`, so every candidate must be
exactly `4 × N` samples. All methods enforce this; soxr's near-exact output is
padded/trimmed to the exact target (it was already exact in practice).

## Duration preservation (HARD constraint)

**PASS.** All 235 method-outputs (47 samples × 5 methods) satisfy
`n_out == n_src × 4` exactly — 0 failures. Gameplay timing (derived from sample
length) is therefore unchanged. See the full table from
`hd_upsample_snd.py`; every row reads `OK`.

## Metrics summary (mean over all 47 samples)

| method  | rms_dB | peak_dB | hf>5.5k % | clips (total) |
|---------|--------|---------|-----------|---------------|
| linear  | −0.136 |  0.000  | **0.1750** | 34900 |
| poly    | −0.009 |  0.030  | 0.0194    | 28058 |
| soxr    | −0.017 |  0.031  | **0.0020** | 24591 |
| clean   | −0.299 |  0.047  | 0.0036    | 25672 |
| best    | −0.290 |  0.051  | 0.0025    | 26050 |

- **rms_dB / peak_dB**: candidate-vs-original loudness delta (0 = identical).
- **hf>5.5k %**: share of spectral energy **above the 5512.5 Hz source
  Nyquist**. The source cannot contain anything up there, so this is purely
  imaging/artifact — **lower is better**.
- **clips**: 16-bit full-scale samples. The source routinely hits ±full-scale
  (8-bit banks are hot), so some clipping is inherent, not a method defect; the
  count mainly tracks how much a method's ringing adds new full-scale samples.

### Reading the numbers

- **linear (control) is measurably dirty**: 0.175 % HF energy — ~9× the poly
  imaging and ~90× soxr. This is the aliased-image "fizz" above the musical
  band that today's engine playback carries. It's the thing to beat.
- **poly and soxr both essentially eliminate imaging** with negligible loudness
  change. soxr's steeper anti-imaging filter wins on HF (0.002 %).
- **clean/best show ~−0.3 dB RMS** — that drop is *DC-offset removal*, not
  dulling. It's desirable (see below).

## Spot-listening notes (reasoned from waveforms/spectra)

- **DC offsets are large in the voices.** `VOICE8` sits at **0.36** normalized
  DC; `VOICE1/2/9/3` are 0.13–0.19; median |DC| across all samples is 0.028.
  Un-removed DC wastes headroom and produces a click at sample start/stop. DC
  removal is genuinely worth doing, and it's why `best`/`clean` read −0.29 dB
  RMS — that's DC energy leaving, not signal.
- **Transient integrity (EXPLHUG explosion).** Original peak is at src sample
  277 → 44 kHz sample 1108. `linear` and `soxr` both land the full-scale peak
  at 1107–1108 — dead on. The `clean` method's 5.2 kHz Butterworth **rings**:
  its full-scale peak migrates to a *different* sample (1028, opposite sign),
  i.e. filter overshoot manufactured a new transient. That's exactly the
  artifact we don't want on zaps/explosions, and it's why the recommendation
  drops the extra low-pass. soxr's own anti-imaging filter band-limits cleanly
  without that ringing.

## Recommendation for the S2 default: **`best`** (DC removal + soxr VHQ + TPDF dither)

Rationale:

1. **Cleanest of the transient-safe options.** soxr VHQ gives the lowest
   imaging (0.002 %) of any non-ringing method — ~90× cleaner than the current
   linear-style playback — while landing transients on the exact sample.
2. **DC removal fixes a real defect** in the voice bank (up to 0.36 DC) that
   causes start/stop clicks and eats headroom. Cheap, always safe.
3. **No aggressive low-pass.** The content is already band-limited at 5.5 kHz,
   so an extra 5.2 kHz LP only dulls and (via filtfilt overshoot) rings on
   explosions. Skipping it keeps zaps/explosions crisp. If a future pass wants
   to attack 8-bit quantization hiss, do it as a *very* mild, transient-aware
   step, not a blanket Butterworth.
4. **TPDF dither** on the 16-bit requantization decorrelates truncation noise —
   free quality on quiet tails.

`poly` (scipy) is the recommended **fallback** if soxr can't be a build/tooling
dependency: it's nearly as clean (0.019 % HF) and dependency-lighter.

## Proposed packaged format for the engine

Bake the chosen candidates offline (this is an authoring step, not runtime):

- Package per-sample as **16-bit signed mono PCM at 44.1 kHz**, one entry per
  sound slot, keyed by index (`hdsfx_00`..`hdsfx_37`, plus the 9 xmas voice
  variants). A simple concatenated bank with a `Uint16 count` + `Uint32`
  offset table (mirroring the existing `.snd` layout, but 16-bit/44.1k and no
  100-byte voice tail — already stripped at authoring time) keeps the loader
  change tiny.
- In `nortsong.c` `loadSndFile`: if the HD bank is present and
  `audioSampleRate == 44100`, load it directly into `soundSamples[]` /
  `soundSampleCount[]` and **skip the `SDL_BuildAudioCVT`/`SDL_ConvertAudio`
  path** for those slots. **Per-sample fallback**: any slot missing from the HD
  bank falls back to the current extract-from-`.snd` + `SDL_ConvertAudio` path,
  so a partial HD bank still works and the vanilla data path is untouched.
- Because HD sample counts are exactly `4 × N`, and the engine already targets
  44.1 kHz, no playback-rate or timing code changes are needed.

## Open questions

- **Runtime sample rate isn't always 44.1 kHz.** `audioSampleRate` is
  configurable; the baked bank is 44.1k. Either (a) gate HD on
  `audioSampleRate == 44100`, or (b) still run `SDL_ConvertAudio` from the HD
  16-bit source for other rates (better source, cheap). Recommend (b).
- **Should the xmas voices ship as a separate HD bank** (they load into the
  same slots as `voices.snd` when `xmas=true`)? Mirroring the existing
  `voices/voicesc` split is cleanest.
- **8-bit quantization hiss** is left in place by `best`. Is it audible in-game
  over music/mix at these levels, or is chasing it not worth the transient risk?
  Needs a listening test on real hardware/output, which this headless
  environment can't do.
- **Loudness normalization** across the bank was deliberately *not* applied
  (would change relative SFX balance the original tuned). Confirm that's the
  intent for S2.
- **Bank size**: 16-bit/44.1k is ~8× the byte size of the originals (still only
  a few MB total). Fine for a standalone bundle; noted for the pak/VFS phase.
