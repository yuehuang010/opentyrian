#!/usr/bin/env python3
"""
Phase S2 (SFX & voice remaster) EXPERIMENT -- upsampling bench.

Reads the pristine 8-bit / 11025 Hz reference WAVs produced by
hd_extract_snd.py (tools/hdsfx_previews/orig/) and produces 16-bit / 44100 Hz
mono candidates under tools/hdsfx_previews/<method>/ using several resampling
stacks, then emits objective metrics and a HARD duration-preservation check.

Resampling ratio is exactly 44100 / 11025 = 4, so every candidate MUST have
exactly 4 * N samples (N = source sample count). This is a hard constraint:
gameplay timing is derived from sample length.

Methods
  linear  Naive linear interpolation. The control -- approximates what
          SDL_ConvertAudio does in the engine today (loadSndFile). Cheap, but
          leaves spectral imaging above the 5.5 kHz source band.
  poly    scipy.signal.resample_poly(up=4, down=1): Kaiser-windowed-sinc
          polyphase FIR. Proper band-limited interpolation.
  soxr    soxr very-high-quality variable-rate resampler (libsoxr).
  clean   poly + DC-offset removal + gentle 5.2 kHz low-pass (to suppress
          8-bit quantization hiss riding above the musical band) + TPDF
          dithered requantization to 16-bit. Tuned to keep transients crisp.
  best    RECOMMENDED. DC-offset removal + soxr VHQ + TPDF dither, with NO
          extra low-pass (soxr's own anti-imaging filter already band-limits
          cleanly and the Butterworth LP in `clean` rings on sharp transients
          like explosions). Best artifact suppression with transients intact.

Requires numpy, scipy, soxr (install into a venv:
  python3 -m venv venv && ./venv/bin/pip install numpy scipy soxr
then run with ./venv/bin/python tools/hd_upsample_snd.py).
"""
import argparse
import glob
import os
import sys
import wave

import numpy as np
from scipy import signal
import soxr

SRC_RATE = 11025
DST_RATE = 44100
RATIO = DST_RATE // SRC_RATE  # 4
SRC_NYQ = SRC_RATE / 2.0      # 5512.5 Hz -- everything above this is invented
HF_EDGE = 5500.0             # "above the source band" threshold for metrics

METHODS = ["linear", "poly", "soxr", "clean", "best"]


def read_orig_wav(path):
    """Read an 8-bit unsigned mono WAV -> float64 signed in [-1, 1)."""
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 1 and w.getnchannels() == 1
        assert w.getframerate() == SRC_RATE
        raw = w.readframes(w.getnframes())
    u8 = np.frombuffer(raw, dtype=np.uint8).astype(np.float64)
    return (u8 - 128.0) / 128.0


def to_i16(x):
    """Clamp float [-1,1] and convert to int16 (no dither)."""
    return np.clip(np.round(x * 32768.0), -32768, 32767).astype(np.int16)


def to_i16_tpdf(x):
    """TPDF-dithered requantization to int16 (1 LSB triangular noise)."""
    lsb = 1.0 / 32768.0
    n = x.shape[0]
    tpdf = (np.random.random(n) - np.random.random(n)) * lsb
    return np.clip(np.round((x + tpdf) * 32768.0), -32768, 32767).astype(np.int16)


def write_wav_i16(path, samples_i16):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(DST_RATE)
        w.writeframes(samples_i16.tobytes())


def enforce_len(y, target):
    """Guarantee exactly `target` samples (pad with last / trim tail)."""
    if y.shape[0] > target:
        return y[:target]
    if y.shape[0] < target:
        pad = np.full(target - y.shape[0], y[-1] if y.shape[0] else 0.0)
        return np.concatenate([y, pad])
    return y


# --- resampling methods (all operate float->float, then caller quantizes) ---

def m_linear(x):
    n = x.shape[0]
    tgt = n * RATIO
    xp = np.arange(n, dtype=np.float64)
    xi = np.arange(tgt, dtype=np.float64) / RATIO
    return np.interp(xi, xp, x)  # clamps at edges -> exactly tgt


def m_poly(x):
    n = x.shape[0]
    y = signal.resample_poly(x, RATIO, 1)  # exact length n*RATIO
    return enforce_len(y, n * RATIO)


def m_soxr(x):
    n = x.shape[0]
    y = soxr.resample(x, SRC_RATE, DST_RATE, quality="VHQ")
    return enforce_len(np.asarray(y, dtype=np.float64), n * RATIO)


def m_clean(x):
    n = x.shape[0]
    # DC removal on the source (8-bit banks often carry a small DC bias).
    xd = x - np.mean(x)
    y = signal.resample_poly(xd, RATIO, 1)
    y = enforce_len(y, n * RATIO)
    # Gentle low-pass at 5.2 kHz: trims quantization hiss at the very top of
    # the band without dulling transients. 4th-order Butterworth, zero-phase.
    sos = signal.butter(4, 5200.0, btype="low", fs=DST_RATE, output="sos")
    y = signal.sosfiltfilt(sos, y)
    return y


def m_best(x):
    """RECOMMENDED: DC removal + soxr VHQ, no extra low-pass."""
    n = x.shape[0]
    xd = x - np.mean(x)
    y = soxr.resample(xd, SRC_RATE, DST_RATE, quality="VHQ")
    return enforce_len(np.asarray(y, dtype=np.float64), n * RATIO)


METHOD_FN = {"linear": m_linear, "poly": m_poly, "soxr": m_soxr,
             "clean": m_clean, "best": m_best}


def hf_fraction(y, fs):
    """Fraction of total spectral energy above HF_EDGE Hz."""
    if y.shape[0] < 8:
        return 0.0
    w = np.hanning(y.shape[0])
    spec = np.abs(np.fft.rfft(y * w)) ** 2
    freqs = np.fft.rfftfreq(y.shape[0], 1.0 / fs)
    tot = spec.sum()
    if tot <= 0:
        return 0.0
    return float(spec[freqs > HF_EDGE].sum() / tot)


def db(a, b):
    if b <= 0 or a <= 0:
        return float("nan")
    return 20.0 * np.log10(a / b)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--orig", default=os.path.join(here, "hdsfx_previews", "orig"))
    ap.add_argument("--out", default=os.path.join(here, "hdsfx_previews"))
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()

    np.random.seed(args.seed)

    files = sorted(glob.glob(os.path.join(args.orig, "*.wav")))
    if not files:
        print("no orig WAVs found in %s (run hd_extract_snd.py first)" % args.orig)
        return 1

    for m in METHODS:
        os.makedirs(os.path.join(args.out, m), exist_ok=True)

    rows = []            # metrics rows
    dur_fail = 0
    for f in files:
        base = os.path.basename(f)
        x = read_orig_wav(f)
        n = x.shape[0]
        tgt = n * RATIO
        rms_o = float(np.sqrt(np.mean(x ** 2))) if n else 0.0
        peak_o = float(np.max(np.abs(x))) if n else 0.0

        for m in METHODS:
            y = METHOD_FN[m](x)
            # duration hard-check BEFORE quantization
            if y.shape[0] != tgt:
                dur_fail += 1
            yi = to_i16_tpdf(y) if m in ("clean", "best") else to_i16(y)
            write_wav_i16(os.path.join(args.out, m, base), yi)

            yf = yi.astype(np.float64) / 32768.0
            rms_c = float(np.sqrt(np.mean(yf ** 2))) if yi.shape[0] else 0.0
            peak_c = float(np.max(np.abs(yf))) if yi.shape[0] else 0.0
            clip = int(np.sum((yi == 32767) | (yi == -32768)))
            hf = hf_fraction(yf, DST_RATE)
            rows.append({
                "file": base, "method": m, "n_src": n,
                "n_out": int(yi.shape[0]), "n_expected": tgt,
                "dur_ok": yi.shape[0] == tgt,
                "rms_dB": db(rms_c, rms_o), "peak_dB": db(peak_c, peak_o),
                "hf_frac": hf, "clip": clip,
            })

    # --- duration verification table ---
    print("=" * 78)
    print("DURATION VERIFICATION  (n_out must == n_src * 4)")
    print("=" * 78)
    print("%-34s %8s %8s %8s  %s" %
          ("file", "n_src", "n_out", "n_exp*4", "methods"))
    # collapse: duration is identical across methods for a file, so show once
    seen = {}
    for r in rows:
        k = r["file"]
        seen.setdefault(k, r)
    all_ok = True
    for k in sorted(seen):
        r = seen[k]
        ok = all(rr["dur_ok"] for rr in rows if rr["file"] == k)
        all_ok = all_ok and ok
        print("%-34s %8d %8d %8d  %s" %
              (k, r["n_src"], r["n_out"], r["n_expected"], "OK" if ok else "FAIL"))
    print("-" * 78)
    print("DURATION CHECK: %s   (%d method-outputs, %d failures)"
          % ("ALL OK" if all_ok and dur_fail == 0 else "FAIL",
             len(rows), dur_fail))

    # --- metrics summary (mean per method) + full CSV ---
    print()
    print("=" * 78)
    print("METRICS SUMMARY  (mean over all %d samples, per method)" % len(files))
    print("=" * 78)
    print("%-8s %10s %10s %12s %10s" %
          ("method", "rms_dB", "peak_dB", "hf>5.5k%", "clips"))
    for m in METHODS:
        mr = [r for r in rows if r["method"] == m]
        rms = np.nanmean([r["rms_dB"] for r in mr])
        peak = np.nanmean([r["peak_dB"] for r in mr])
        hf = 100.0 * np.mean([r["hf_frac"] for r in mr])
        clip = sum(r["clip"] for r in mr)
        print("%-8s %10.3f %10.3f %12.4f %10d" % (m, rms, peak, hf, clip))
    print()
    print("Note: rms_dB / peak_dB are candidate-vs-original level deltas (0 = "
          "identical loudness).\n"
          "hf>5.5k%% = share of spectral energy above the 5512.5 Hz source "
          "Nyquist -- this is\nentirely imaging/artifact; lower is better. "
          "Original content cannot live there.")

    # full per-sample CSV for the report
    csv_path = os.path.join(args.out, "metrics.csv")
    with open(csv_path, "w") as fh:
        fh.write("file,method,n_src,n_out,n_expected,dur_ok,rms_dB,peak_dB,hf_frac,clip\n")
        for r in rows:
            fh.write("%s,%s,%d,%d,%d,%d,%.4f,%.4f,%.6f,%d\n" %
                     (r["file"], r["method"], r["n_src"], r["n_out"],
                      r["n_expected"], int(r["dur_ok"]), r["rms_dB"],
                      r["peak_dB"], r["hf_frac"], r["clip"]))
    print("\nPer-sample metrics CSV: %s" % csv_path)
    return 0 if (all_ok and dur_fail == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
