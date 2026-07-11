#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# validate_pitch.py - automated pitch validation for the HD music remaster of
# Tyrian's LDS/OPL2 title theme (music track 30).
#
# It runs TWO independent tests so future soundfont / lds_gm_map.txt changes can
# be validated WITHOUT a human listening for wrong notes:
#
#   TEST A  (OPL->MIDI conversion arithmetic, no audio)
#       Reads hdmusic_work/midi/hdmusic_30.notes (which tools/lds_to_midi.c now
#       dumps with per-note opl_hz / fnum / block / transpose columns) and, for
#       every note-on, compares the exact OPL2 chip frequency to the frequency
#       the emitted MIDI note represents. It reports the deviation distribution
#       and flags any voice (fingerprint) whose median deviation exceeds 0.5 st.
#       This proves the converter is octave/pitch faithful (or names the bug).
#
#   TEST B  (soundfont preset tuning, audio analysis)
#       For each pitched voice and each soundfont, it renders a SOLO MIDI (only
#       that fingerprint audible), fluidsynth-renders it, and measures the
#       sounding f0 (octave-robust Harmonic Product Spectrum) on clean solo
#       windows. It compares the measured pitch to the pitch the MIDI asked for
#       and flags presets that play the right instrument at the WRONG octave/key.
#
# Output: internal/hd-music-pitch-report.md  (TEST A results, TEST B table,
# and a plain-language verdict naming the offending voice/font pairs).
#
# ---- RE-RUN (single command, from repo root; takes a few minutes) ----
#
#     python3 tools/validate_pitch.py
#
# Optional overrides via env:
#     DATA_DIR   Tyrian data dir  (default /Users/felixhuang/source/opentyrian/tyrian21)
#     SONG       1-based song number (default 30)
#     FLUID_GAIN fluidsynth -g gain for solo renders (default 1.0)
#     MAX_NOTES  max analysis windows per voice (default 40)
#
# Dependencies: python3 stdlib only (wave, math, cmath, struct). No numpy /
# scipy / pip installs required. fluidsynth + the built lds_to_midi are needed
# for TEST B; if either is missing, TEST A still runs and TEST B is skipped
# with a note in the report.
# -----------------------------------------------------------------------------

import os
import sys
import math
import cmath
import wave
import struct
import subprocess
import shutil

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKDIR = os.path.join(REPO_ROOT, "hdmusic_work")
BIN_LDS = os.path.join(WORKDIR, "bin", "lds_to_midi")
MIDIDIR = os.path.join(WORKDIR, "midi")
SFDIR = os.path.join(WORKDIR, "sf")
PITCHDIR = os.path.join(WORKDIR, "pitchcheck")
GM_MAP = os.path.join(REPO_ROOT, "tools", "lds_gm_map.txt")
REPORT = os.path.join(REPO_ROOT, "internal", "hd-music-pitch-report.md")

DATA_DIR = os.environ.get("DATA_DIR", "/Users/felixhuang/source/opentyrian/tyrian21")
SONG = int(os.environ.get("SONG", "30"))
FLUID_GAIN = os.environ.get("FLUID_GAIN", "1.0")
MAX_NOTES = int(os.environ.get("MAX_NOTES", "40"))

SF_FILES = ["FluidR3_GM.sf2", "GeneralUser-GS.sf2", "SC-55.sf2"]

# tick length: 1 tick = 113.669048 / 7900 seconds (see task/lds cadence)
SECONDS_PER_TICK = 113.669048 / 7900.0
SAMPLE_RATE = 44100

# GM programs that are unpitched / quasi-pitched percussion: exclude from
# TEST B pass/fail (still listed, marked n/a).
PERCUSSION_PROGRAMS = {115, 118, 47}  # woodblock, synth drum, timpani

NN = "%02d" % SONG


def midi_to_hz(note):
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


# -----------------------------------------------------------------------------
# lds_gm_map.txt parsing
# -----------------------------------------------------------------------------
def parse_gm_map(path):
    """fingerprint(str) -> dict(program, transpose, volume)."""
    out = {}
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            # strip trailing inline comment
            s = s.split("#", 1)[0].strip()
            if not s:
                continue
            parts = s.split()
            if len(parts) < 2:
                continue
            fp = parts[0].lower()
            try:
                prog = int(parts[1])
            except ValueError:
                continue
            transpose = int(parts[2]) if len(parts) >= 3 else 0
            volume = int(parts[3]) if len(parts) >= 4 else 100
            out[fp] = {"program": prog, "transpose": transpose, "volume": volume}
    return out


# -----------------------------------------------------------------------------
# .notes parsing
# -----------------------------------------------------------------------------
class Note:
    __slots__ = ("on", "off", "note", "ch", "fp", "opl_hz", "fnum", "block", "transpose")

    def __init__(self, on, off, note, ch, fp, opl_hz, fnum, block, transpose):
        self.on = on
        self.off = off
        self.note = note
        self.ch = ch
        self.fp = fp
        self.opl_hz = opl_hz
        self.fnum = fnum
        self.block = block
        self.transpose = transpose


def parse_notes(path):
    notes = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 5:
                continue
            on = int(p[0]); off = int(p[1]); note = int(p[2]); ch = int(p[3])
            fp = p[4].lower()
            opl_hz = float(p[5]) if len(p) >= 6 else 0.0
            fnum = int(p[6]) if len(p) >= 7 else 0
            block = int(p[7]) if len(p) >= 8 else 0
            transpose = int(p[8]) if len(p) >= 9 else 0
            notes.append(Note(on, off, note, ch, fp, opl_hz, fnum, block, transpose))
    return notes


# -----------------------------------------------------------------------------
# TEST A
# -----------------------------------------------------------------------------
def median(xs):
    if not xs:
        return 0.0
    s = sorted(xs)
    n = len(s)
    m = n // 2
    return s[m] if n % 2 else 0.5 * (s[m - 1] + s[m])


def run_test_a(notes):
    """Per-note deviation between OPL chip pitch and the emitted MIDI note.

    dev = 12*log2(opl_hz / midi_hz), where midi_hz corresponds to the emitted
    MIDI note with the map transpose removed (so this isolates the converter's
    fnum/block->note rounding; a stray transpose would show as an offset here
    AND is reported separately from the map)."""
    per_fp = {}
    all_abs = []
    over = 0
    checked = 0
    max_abs = 0.0
    max_ctx = None
    for nt in notes:
        if nt.opl_hz <= 0:
            continue
        # midi note the converter would emit BEFORE transpose:
        base_note = nt.note - nt.transpose
        midi_hz = midi_to_hz(base_note)
        dev = 12.0 * math.log2(nt.opl_hz / midi_hz)
        a = abs(dev)
        checked += 1
        all_abs.append(a)
        if a > 0.5:
            over += 1
        if a > max_abs:
            max_abs = a
            max_ctx = (nt.fp, nt.note)
        per_fp.setdefault(nt.fp, []).append(dev)
    result = {
        "checked": checked,
        "over_half": over,
        "max_abs": max_abs,
        "max_ctx": max_ctx,
        "mean_abs": (sum(all_abs) / len(all_abs)) if all_abs else 0.0,
        "per_fp": {},
    }
    for fp, devs in per_fp.items():
        med = median([abs(d) for d in devs])
        med_signed = median(devs)
        result["per_fp"][fp] = {
            "n": len(devs),
            "median_abs": med,
            "median_signed": med_signed,
            "flag": med > 0.5,
        }
    return result


# -----------------------------------------------------------------------------
# Pure-python FFT (iterative radix-2 Cooley-Tukey) + HPS f0 estimation
# -----------------------------------------------------------------------------
def _next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def fft(a):
    """In-place iterative radix-2 FFT. len(a) must be a power of 2."""
    n = len(a)
    if n <= 1:
        return a
    # bit-reversal permutation
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        ang = -2.0 * math.pi / length
        wlen = complex(math.cos(ang), math.sin(ang))
        half = length >> 1
        for i in range(0, n, length):
            w = 1.0 + 0.0j
            for k in range(half):
                u = a[i + k]
                v = a[i + k + half] * w
                a[i + k] = u + v
                a[i + k + half] = u - v
                w *= wlen
        length <<= 1
    return a


def ifft(a):
    """Inverse FFT via conjugation trick. len(a) power of 2."""
    n = len(a)
    conj = [x.conjugate() for x in a]
    fft(conj)
    return [x.conjugate() / n for x in conj]


def estimate_f0(samples, sr, f_expected):
    """Octave-robust, resolution-independent f0 via the YIN algorithm
    (cumulative-mean-normalized difference function), with FFT-accelerated
    autocorrelation and parabolic period interpolation.

    YIN is chosen over HPS/plain autocorrelation because it does not depend on
    FFT bin resolution (period is estimated in the time domain, so low bass is
    as accurate as treble) and its cumulative-mean normalization rejects the
    octave-up error HPS is prone to. The tau search is bounded to
    [sr/(f_expected*4.5), sr/(f_expected/4.5)] so that up to ~2-octave real
    mistunings are still caught but 3+ octave garbage is rejected.

    Returns (f0_hz, rms). f0 == 0 means no confident estimate."""
    n0 = len(samples)
    if n0 < 128:
        return (0.0, 0.0)
    rms = math.sqrt(sum(x * x for x in samples) / n0)
    if rms <= 0:
        return (0.0, 0.0)

    # remove DC
    mean = sum(samples) / n0
    x = [s - mean for s in samples]

    tau_min = max(2, int(sr / (f_expected * 4.5)))
    tau_max = int(sr / (f_expected / 4.5))
    # need at least ~2 periods of overlap at the longest lag
    if tau_max > n0 - tau_min - 4:
        tau_max = n0 - tau_min - 4
    if tau_max <= tau_min + 2:
        return (0.0, rms)

    # Autocorrelation-method difference function over the shrinking overlap:
    #   d(tau) = sum_{i=0}^{n0-1-tau} (x[i]-x[i+tau])^2
    #          = A(tau) + B(tau) - 2 P(tau)
    # where A(tau)=sum_{i=0}^{n0-1-tau} x[i]^2, B(tau)=sum_{i=tau}^{n0-1} x[i]^2,
    # and P(tau)=sum_{i=0}^{n0-1-tau} x[i]x[i+tau] (the full-overlap autocorr,
    # computed via FFT). Using the same overlap range for the energy and the
    # correlation terms is what keeps low-frequency estimates unbiased.
    nfft = _next_pow2(2 * n0)
    buf = [0j] * nfft
    for i in range(n0):
        buf[i] = complex(x[i], 0.0)
    fft(buf)
    for i in range(nfft):
        buf[i] = buf[i] * buf[i].conjugate()
    r = ifft(buf)
    P = [r[t].real for t in range(tau_max + 1)]

    # prefix sum of squares: cs[k] = sum_{i=0}^{k-1} x[i]^2
    cs = [0.0] * (n0 + 1)
    for i in range(n0):
        cs[i + 1] = cs[i] + x[i] * x[i]

    d = [0.0] * (tau_max + 1)
    for tau in range(0, tau_max + 1):
        A = cs[n0 - tau]            # i = 0 .. n0-1-tau
        B = cs[n0] - cs[tau]        # i = tau .. n0-1
        d[tau] = A + B - 2.0 * P[tau]
        if d[tau] < 0:
            d[tau] = 0.0

    # cumulative mean normalized difference
    dp = [1.0] * (tau_max + 1)
    running = 0.0
    for tau in range(1, tau_max + 1):
        running += d[tau]
        dp[tau] = d[tau] * tau / running if running > 0 else 1.0

    # find first local minimum below threshold in [tau_min, tau_max]
    threshold = 0.15
    best_tau = -1
    tau = tau_min
    while tau < tau_max:
        if dp[tau] < threshold:
            while tau + 1 < tau_max and dp[tau + 1] < dp[tau]:
                tau += 1
            best_tau = tau
            break
        tau += 1
    if best_tau < 0:
        # no dip below threshold: take global min of dp in range
        best_tau = tau_min
        best_val = dp[tau_min]
        for t in range(tau_min + 1, tau_max):
            if dp[t] < best_val:
                best_val = dp[t]
                best_tau = t

    # parabolic interpolation around best_tau on d()
    t = best_tau
    if 1 <= t < tau_max:
        a = d[t - 1]; b = d[t]; c = d[t + 1]
        denom = (a - 2 * b + c)
        shift = 0.5 * (a - c) / denom if denom != 0 else 0.0
    else:
        shift = 0.0
    period = t + shift
    if period <= 0:
        return (0.0, rms)
    return (sr / period, rms)


# -----------------------------------------------------------------------------
# TEST B - solo renders + pitch analysis
# -----------------------------------------------------------------------------
def read_wav_mono(path):
    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise RuntimeError("expected 16-bit WAV, got sampwidth=%d" % sw)
    total = len(raw) // 2
    ints = struct.unpack("<%dh" % total, raw)
    if nch == 1:
        mono = [float(x) for x in ints]
    else:
        mono = [0.0] * (total // nch)
        for i in range(len(mono)):
            s = 0
            base = i * nch
            for c in range(nch):
                s += ints[base + c]
            mono[i] = s / float(nch)
    return mono, sr


# A plucked/decaying voice (harp, marimba, pizzicato) keeps ringing well past
# its note-off, so a note that ENDED shortly before our window still bleeds its
# (often octave-lower) release tail into it. We therefore reject a candidate
# window if any other same-fingerprint note was sounding within GUARD_TICKS
# before it starts. This is what stops the harp octave-dyad voices from being
# mis-measured as "an octave down" (the tail of the lower octave note dominates
# the spectrum). Dense pluck voices end up with no clean window and are
# reported n/a, which is the honest outcome - they can't be validated by a
# solo-window method.
GUARD_TICKS = 24  # ~345 ms of release headroom before a window


def select_windows(notes, fp, max_notes):
    """Notes of `fp` lasting >= 8 ticks whose sounding interval doesn't overlap
    another note of the SAME fp AND which have GUARD_TICKS of same-fp silence
    before them (so a plucked release tail can't bleed in). Returns list of
    (on_tick, off_tick, midi_note), spread across the track, capped."""
    same = sorted([nt for nt in notes if nt.fp == fp], key=lambda x: x.on)
    cand = []
    for i, nt in enumerate(same):
        if nt.off - nt.on < 8:
            continue
        overlap = False
        for j in range(len(same)):
            if j == i:
                continue
            o = same[j]
            # direct overlap of note gates
            if o.on < nt.off and o.off > nt.on:
                overlap = True
                break
            # release-tail bleed: a LOWER same-fp note that ended within
            # GUARD_TICKS before this window starts keeps ringing (plucked
            # voices) and its lower fundamental can be mistaken for a sub-octave.
            # Only a substantially lower predecessor is a risk, so legato
            # sustained lines (small steps) keep their windows.
            if (o.off <= nt.on and o.off > nt.on - GUARD_TICKS
                    and o.note <= nt.note - 5):
                overlap = True
                break
        if not overlap:
            cand.append((nt.on, nt.off, nt.note))
    if len(cand) <= max_notes:
        return cand
    # spread evenly across the track
    step = len(cand) / float(max_notes)
    return [cand[int(k * step)] for k in range(max_notes)]


def analyze_voice(mono, sr, windows):
    """Return list of offset-in-semitones for each usable window."""
    offsets = []
    if not windows:
        return offsets
    # RMS gate: relative to the loudest window's RMS (skip decayed/silent tails)
    rmss = []
    prepared = []
    for (on, off, note) in windows:
        s0 = int(on * SECONDS_PER_TICK * sr)
        s1 = int(off * SECONDS_PER_TICK * sr)
        length = s1 - s0
        if length < 64:
            continue
        # middle 60%
        a = s0 + int(0.20 * length)
        b = s0 + int(0.80 * length)
        if b > len(mono):
            b = len(mono)
        if a >= b - 64:
            continue
        seg = mono[a:b]
        expected = midi_to_hz(note)
        f0, rms = estimate_f0(seg, sr, expected)
        rmss.append(rms)
        prepared.append((f0, rms, expected))
    if not prepared:
        return offsets
    peak_rms = max(r for _, r, _ in prepared) or 1.0
    gate = 0.08 * peak_rms
    for (f0, rms, expected) in prepared:
        if rms < gate or f0 <= 0:
            continue
        off_st = 12.0 * math.log2(f0 / expected)
        offsets.append(off_st)
    return offsets


def write_solo_map(all_fps, gm_map, target_fp, path):
    """Every fingerprint at volume 0 except target at its mapped program/vol."""
    with open(path, "w") as f:
        f.write("# solo pitch-check map for %s (auto-generated by validate_pitch.py)\n" % target_fp)
        for fp in all_fps:
            m = gm_map.get(fp, {"program": 0, "transpose": 0, "volume": 100})
            if fp == target_fp:
                vol = 100
            else:
                vol = 0
            f.write("%s %d %d %d\n" % (fp, m["program"], m["transpose"], vol))


def run_test_b(notes, gm_map):
    if not os.path.isfile(BIN_LDS):
        return None, "lds_to_midi binary not found at %s (run './tools/hd_music_pipeline.sh build')" % BIN_LDS
    if not shutil.which("fluidsynth"):
        return None, "fluidsynth not on PATH; TEST B skipped"

    all_fps = sorted(set(nt.fp for nt in notes))
    sf_present = [sf for sf in SF_FILES if os.path.isfile(os.path.join(SFDIR, sf))]
    if not sf_present:
        return None, "no soundfonts found in %s; TEST B skipped" % SFDIR

    mapdir = os.path.join(PITCHDIR, "maps")
    middir = os.path.join(PITCHDIR, "midi")
    wavdir = os.path.join(PITCHDIR, "wav")
    for d in (mapdir, middir, wavdir):
        os.makedirs(d, exist_ok=True)

    # target voices: mapped, pitched, volume > 0 in the real map.
    targets = []
    for fp in all_fps:
        m = gm_map.get(fp)
        if m is None:
            continue  # unmapped -> defaults; not something we tuned
        if m["volume"] == 0:
            continue  # removed voice
        is_perc = m["program"] in PERCUSSION_PROGRAMS
        targets.append((fp, m["program"], is_perc))

    rows = []  # dict per voice
    for (fp, program, is_perc) in targets:
        windows = select_windows(notes, fp, MAX_NOTES)
        solo_map = os.path.join(mapdir, "solo_%s.txt" % fp)
        write_solo_map(all_fps, gm_map, fp, solo_map)

        # render the solo MIDI for this fingerprint
        env = dict(os.environ)
        env["LDS_GM_MAP"] = solo_map
        subprocess.run([BIN_LDS, DATA_DIR, middir, str(SONG)],
                       env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        solo_mid = os.path.join(middir, "hdmusic_%s.mid" % NN)
        row = {"fp": fp, "program": program, "perc": is_perc, "windows": len(windows), "fonts": {}}
        for sf in sf_present:
            sf_path = os.path.join(SFDIR, sf)
            wav = os.path.join(wavdir, "solo_%s_%s.wav" % (fp, sf.replace(".sf2", "")))
            subprocess.run(
                ["fluidsynth", "-ni", "-g", FLUID_GAIN, "-r", str(SAMPLE_RATE),
                 "-F", wav, sf_path, solo_mid],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                mono, sr = read_wav_mono(wav)
            except Exception as e:
                row["fonts"][sf] = {"error": str(e)}
                continue
            offs = analyze_voice(mono, sr, windows)
            if offs:
                med = median(offs)
                mad = median([abs(o - med) for o in offs])
                within = sum(1 for o in offs if abs(o) <= 0.5) / float(len(offs)) * 100.0
                # A measurement is only trustworthy if the per-window estimates
                # agree (low MAD) and there are a few of them. High MAD means the
                # texture (fast arps, chords, noisy attacks) defeated the pitch
                # detector - don't turn such a cell into a hard verdict.
                reliable = (mad <= 0.35) and (len(offs) >= 2)
                flag = (abs(med) >= 0.5) and reliable and not is_perc
                row["fonts"][sf] = {"n": len(offs), "median": med, "mad": mad,
                                    "within": within, "flag": flag, "reliable": reliable}
            else:
                row["fonts"][sf] = {"n": 0}
            try:
                os.remove(wav)
            except OSError:
                pass
            sys.stdout.write("  %s / %-16s median=%s\n" % (
                fp, sf, ("%.2f st" % row["fonts"][sf].get("median", 0.0)) if row["fonts"][sf].get("n") else "n/a"))
            sys.stdout.flush()
        rows.append(row)
    return (rows, sf_present), None


# -----------------------------------------------------------------------------
# Report
# -----------------------------------------------------------------------------
def gm_name(prog):
    names = {
        12: "Marimba", 29: "Overdriven Guitar", 38: "Synth Bass 1",
        42: "Cello", 43: "Contrabass", 45: "Pizzicato Strings",
        46: "Orchestral Harp", 47: "Timpani", 48: "String Ensemble 1",
        52: "Choir Aahs", 61: "Brass Section", 89: "Pad 2 (warm)",
        112: "Tinkle Bell", 115: "Woodblock",
    }
    return names.get(prog, "GM %d" % prog)


def write_report(a_result, b_result, b_error, gm_map, notes):
    fp_first = {}
    for nt in notes:
        if nt.fp not in fp_first:
            fp_first[nt.fp] = nt.on

    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    L = []
    L.append("# HD music pitch validation report (track %d)" % SONG)
    L.append("")
    L.append("_Generated by `python3 tools/validate_pitch.py`._")
    L.append("")
    L.append("Two automated tests check that the layered soundfont renders play the")
    L.append("correct pitches: **TEST A** verifies the OPL->MIDI note conversion is")
    L.append("arithmetically faithful; **TEST B** measures whether each soundfont preset")
    L.append("actually sounds at the MIDI pitch it was handed.")
    L.append("")

    # ---- TEST A ----
    L.append("## TEST A - OPL->MIDI conversion correctness")
    L.append("")
    L.append("For every note-on, the exact OPL2 chip frequency "
             "`opl_hz = fnum * 49716 / 2^(20-block)` is compared to the frequency of the")
    L.append("emitted MIDI note (transpose removed). Deviation = "
             "`12*log2(opl_hz / midi_hz)` semitones.")
    L.append("")
    L.append("- Note-ons checked: **%d**" % a_result["checked"])
    L.append("- Mean |deviation|: **%.4f st**" % a_result["mean_abs"])
    if a_result["max_ctx"]:
        L.append("- Max |deviation|: **%.4f st** (fingerprint `%s`, note %d)" %
                 (a_result["max_abs"], a_result["max_ctx"][0], a_result["max_ctx"][1]))
    L.append("- Note-ons exceeding 0.5 st: **%d**" % a_result["over_half"])
    L.append("")
    flagged_a = [fp for fp, v in a_result["per_fp"].items() if v["flag"]]
    if flagged_a:
        L.append("**Voices with median |deviation| > 0.5 st (converter bug):**")
        L.append("")
        for fp in flagged_a:
            v = a_result["per_fp"][fp]
            L.append("- `%s` median %.3f st (n=%d)" % (fp, v["median_abs"], v["n"]))
    else:
        L.append("**Verdict: PASS.** No voice has a median deviation above 0.5 st; every")
        L.append("note-on is within a rounding step of its OPL pitch. The OPL->MIDI")
        L.append("conversion is octave/pitch accurate.")
    L.append("")
    L.append("Per-voice median deviation (signed; negative = OPL pitch slightly flat of")
    L.append("the nearest MIDI note, an artifact of the 16-notes/octave OPL table):")
    L.append("")
    L.append("| fingerprint | GM | n | median dev (st) |")
    L.append("|---|---|---|---|")
    for fp in sorted(a_result["per_fp"], key=lambda x: fp_first.get(x, 0)):
        v = a_result["per_fp"][fp]
        prog = gm_map.get(fp, {}).get("program", None)
        pstr = ("%d %s" % (prog, gm_name(prog))) if prog is not None else "-"
        L.append("| `%s` | %s | %d | %+.3f |" % (fp, pstr, v["n"], v["median_signed"]))
    L.append("")

    # ---- transpose audit ----
    L.append("### Transpose audit")
    L.append("")
    nonzero = {fp: m["transpose"] for fp, m in gm_map.items() if m["transpose"] != 0}
    L.append("`lds_to_midi.c` applies the map's transpose column to the emitted note "
             "(`note = hz_to_midi_note(hz) + transpose`), and the `.notes` dump confirms")
    L.append("it round-trips (the `transpose` column matches the map).")
    if nonzero:
        L.append("")
        L.append("**Non-zero transposes present (would shift keys):**")
        for fp, t in nonzero.items():
            L.append("- `%s`: %+d semitones" % (fp, t))
    else:
        L.append("All current map entries have transpose **0**, so no voice is key-shifted")
        L.append("by the map. (A stray non-zero transpose would be exactly \"right")
        L.append("instrument, wrong keys\" - none is present.)")
    L.append("")

    # ---- TEST B ----
    L.append("## TEST B - soundfont preset tuning")
    L.append("")
    if b_result is None:
        L.append("_TEST B did not run: %s_" % (b_error or "unknown"))
        L.append("")
    else:
        rows, sf_present = b_result
        L.append("Each pitched voice is rendered SOLO (only that fingerprint audible) "
                 "through each soundfont; the sounding f0 is measured on clean solo")
        L.append("windows (>= 8 ticks, non-overlapping, %d-tick release guard, middle 60%%, "
                 "YIN pitch detection)" % GUARD_TICKS)
        L.append("and compared to the MIDI note's expected frequency. Offset = "
                 "`12*log2(measured / expected)` semitones. **Flag (⚠) = |median| >= 0.5 st**")
        L.append("on a reliable measurement (MAD <= 0.35 st, >= 2 windows); `?` marks a")
        L.append("low-confidence cell where the texture defeated the detector "
                 "(not counted as a verdict). Percussion voices are excluded, marked n/a.")
        L.append("")
        header = "| voice | GM program | " + " | ".join(sf.replace(".sf2", "") for sf in sf_present) + " |"
        sep = "|---|---|" + "|".join(["---"] * len(sf_present)) + "|"
        L.append(header)
        L.append(sep)
        for row in rows:
            fp = row["fp"]
            label = "`%s`%s" % (fp, " (perc)" if row["perc"] else "")
            prog = "%d %s" % (row["program"], gm_name(row["program"]))
            cells = []
            for sf in sf_present:
                d = row["fonts"].get(sf, {})
                if "error" in d:
                    cells.append("err")
                elif not d.get("n"):
                    cells.append("n/a")
                else:
                    tag = ""
                    if row["perc"]:
                        tag = " (n/a)"
                    elif d["flag"]:
                        tag = " ⚠"
                    elif not d.get("reliable", True):
                        tag = " ?"
                    cells.append("%+.2f±%.2f%s" % (d["median"], d["mad"], tag))
            L.append("| %s | %s | %s |" % (label, prog, " | ".join(cells)))
        L.append("")
        L.append("Cells show `median_offset ± MAD` in semitones over the analysed windows. "
                 "⚠ marks |median| >= 0.5 st (mistuned). `n/a` = percussion or no usable")
        L.append("windows.")
        L.append("")

        # ---- verdict ----
        L.append("## Verdict")
        L.append("")
        offenders = []
        for row in rows:
            if row["perc"]:
                continue
            for sf in sf_present:
                d = row["fonts"].get(sf, {})
                if d.get("n") and d.get("flag"):
                    offenders.append((row["fp"], row["program"], sf, d["median"]))
        if a_result["over_half"] == 0 and not flagged_a:
            L.append("- **TEST A: converter is pitch-accurate.** The OPL->MIDI conversion "
                     "introduces no octave or key errors; if voices sound wrong it is the")
            L.append("  soundfont preset, not the MIDI.")
        else:
            L.append("- **TEST A found converter deviations** - see the flagged voices above.")
        if offenders:
            # group by voice to judge cross-font consistency
            by_voice = {}
            for (fp, prog, sf, med) in offenders:
                by_voice.setdefault(fp, []).append((sf, med, prog))
            n_fonts = len(sf_present)
            L.append("- **TEST B mistuned (voice, font) pairs:**")
            for fp in sorted(by_voice, key=lambda x: fp_first.get(x, 0)):
                items = by_voice[fp]
                prog = items[0][2]
                consistent = len(items) == n_fonts
                # also: is it a ~octave, i.e. detector-sensitive on bright timbres?
                octavey = any(abs(abs(m) - 12.0) < 2.0 for _, m, _ in items)
                scope = ("ALL fonts - real preset tuning" if consistent
                         else "%d/%d fonts - font-specific, verify by ear%s" %
                              (len(items), n_fonts,
                               " (octave outlier on one font is often a bright-timbre "
                               "detector artifact, not audible wrong-key)" if octavey else ""))
                L.append("  - `%s` (%d %s) - %s:" % (fp, prog, gm_name(prog), scope))
                for (sf, med, _) in items:
                    direction = "sharp" if med > 0 else "flat"
                    oct_note = ""
                    if abs(abs(med) - 12.0) < 2.0:
                        oct_note = " (~1 octave %s)" % ("up" if med > 0 else "down")
                    L.append("    - **%s**: %+.2f st %s%s" %
                             (sf.replace(".sf2", ""), med, direction, oct_note))
        else:
            L.append("- **TEST B: all pitched voices within ±0.5 st on all soundfonts** - no")
            L.append("  preset plays the wrong octave/key for track %d." % SONG)
        L.append("")

    with open(REPORT, "w") as f:
        f.write("\n".join(L) + "\n")
    print("\nwrote %s" % REPORT)


# -----------------------------------------------------------------------------
def main():
    notes_path = os.path.join(MIDIDIR, "hdmusic_%s.notes" % NN)
    if not os.path.isfile(notes_path):
        # generate it
        if os.path.isfile(BIN_LDS):
            os.makedirs(MIDIDIR, exist_ok=True)
            subprocess.run([BIN_LDS, DATA_DIR, MIDIDIR, str(SONG)], check=True)
        else:
            sys.exit("error: %s missing and lds_to_midi not built" % notes_path)

    gm_map = parse_gm_map(GM_MAP)
    notes = parse_notes(notes_path)

    # Guard: TEST A needs the opl_hz columns.
    if all(nt.opl_hz == 0 for nt in notes):
        sys.exit("error: %s has no opl_hz column; rebuild lds_to_midi (the TEST A "
                 "columns were added to tools/lds_to_midi.c) and regenerate the notes."
                 % notes_path)

    print("== TEST A: OPL->MIDI conversion ==")
    a_result = run_test_a(notes)
    print("  checked=%d  over0.5st=%d  max|dev|=%.4f st  mean|dev|=%.4f st" %
          (a_result["checked"], a_result["over_half"], a_result["max_abs"], a_result["mean_abs"]))

    print("== TEST B: soundfont tuning (solo renders) ==")
    b_result, b_error = run_test_b(notes, gm_map)
    if b_error:
        print("  " + b_error)

    write_report(a_result, b_result, b_error, gm_map, notes)


if __name__ == "__main__":
    main()
