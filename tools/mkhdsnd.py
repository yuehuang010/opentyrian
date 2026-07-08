#!/usr/bin/env python3
"""
Phase S2 (SFX & voice remaster) -- bank packager.

Bakes the HD (16-bit / 44100 Hz) WAVs produced by hd_upsample_snd.py's
"best" method (DC-offset removal + soxr VHQ resample + TPDF dither -- see
tools/HDSFX_EXPERIMENT.md) into three HD sound banks consumed by the engine's
loadSndFile() in src/nortsong.c:

  hdsnd_sfx.dat      29 SFX samples, same order as tyrian.snd
  hdsnd_voices.dat    9 voice samples, same order as voices.snd
  hdsnd_voicesc.dat   9 Christmas voice samples, same order as voicesc.snd

Bank binary format, all little-endian:
  u32 magic              literal ASCII bytes 'H','S','N','D' (not byte-swapped
                          as an integer -- written as the 4 bytes in that order)
  u16 count
  u32 sampleRate          (= 44100)
  (count+1) x u32         absolute byte offsets into this file; offset[i] is
                          where sample i's PCM data starts, offset[count] is
                          EOF (total file size). Sample i's PCM byte length is
                          offset[i+1] - offset[i].
  ...                     contiguous Sint16 LE PCM data for all samples,
                          back to back, starting right after the offset table.

stdlib-only (wave + struct) -- no numpy/scipy/soxr needed for this packaging
step, since it only repackages PCM bytes that were already resampled offline.
"""
import argparse
import glob
import os
import re
import struct
import sys
import wave

DST_RATE = 44100
MAGIC = b"HSND"

SFX_COUNT = 29
VOICE_COUNT = 9


def read_wav_i16_mono(path):
    """Read a 16-bit mono WAV, return (raw_pcm_bytes, framerate)."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            raise ValueError("%s: expected 16-bit mono, got sampwidth=%d nchannels=%d"
                              % (path, w.getsampwidth(), w.getnchannels()))
        raw = w.readframes(w.getnframes())
        rate = w.getframerate()
    return raw, rate


def collect_ordered(method_dir, pattern, count):
    """Find WAVs in method_dir matching pattern, ordered by leading index."""
    files = glob.glob(os.path.join(method_dir, pattern))
    indexed = []
    for f in files:
        m = re.match(r"^(\d+)_", os.path.basename(f))
        if not m:
            continue
        indexed.append((int(m.group(1)), f))
    indexed.sort(key=lambda t: t[0])
    if len(indexed) != count:
        raise ValueError("%s: found %d files matching %r, expected %d"
                          % (method_dir, len(indexed), pattern, count))
    return [f for _, f in indexed]


def collect_ordered_xmas(method_dir, count):
    files = glob.glob(os.path.join(method_dir, "xmas_*_voice_*.wav"))
    indexed = []
    for f in files:
        m = re.match(r"^xmas_(\d+)_", os.path.basename(f))
        if not m:
            continue
        indexed.append((int(m.group(1)), f))
    indexed.sort(key=lambda t: t[0])
    if len(indexed) != count:
        raise ValueError("%s: found %d xmas voice files, expected %d"
                          % (method_dir, len(indexed), count))
    return [f for _, f in indexed]


def write_bank(out_path, wav_paths, sample_rate):
    payloads = []
    for p in wav_paths:
        raw, rate = read_wav_i16_mono(p)
        if rate != sample_rate:
            raise ValueError("%s: sample rate %d != expected %d" % (p, rate, sample_rate))
        payloads.append(raw)

    count = len(payloads)
    header_len = 4 + 2 + 4 + 4 * (count + 1)  # magic + count + rate + offsets
    offsets = []
    pos = header_len
    for pl in payloads:
        offsets.append(pos)
        pos += len(pl)
    offsets.append(pos)  # EOF marker

    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<H", count))
        f.write(struct.pack("<I", sample_rate))
        for off in offsets:
            f.write(struct.pack("<I", off))
        for pl in payloads:
            f.write(pl)

    total = pos
    print("wrote %s: %d samples, %d bytes" % (out_path, count, total))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("data_dir", nargs="?",
                    default="/Users/felixhuang/source/opentyrian/tyrian21",
                    help="output data dir for hdsnd_*.dat (default: tyrian21)")
    ap.add_argument("--previews", default=os.path.join(here, "hdsfx_previews"),
                    help="dir containing the 'best' method's upsampled WAVs "
                         "(hd_upsample_snd.py --out); default: tools/hdsfx_previews")
    ap.add_argument("--method", default="best",
                    help="method subdir to bake from (default: best, the chosen "
                         "S2 method -- see tools/HDSFX_EXPERIMENT.md)")
    args = ap.parse_args()

    method_dir = os.path.join(args.previews, args.method)
    if not os.path.isdir(method_dir):
        print("error: %s does not exist (run hd_extract_snd.py + hd_upsample_snd.py first)"
              % method_dir, file=sys.stderr)
        return 1

    os.makedirs(args.data_dir, exist_ok=True)

    sfx_wavs = collect_ordered(method_dir, "??_sfx_*.wav", SFX_COUNT)
    voice_wavs = collect_ordered(method_dir, "??_voice_*.wav", VOICE_COUNT)
    voicesc_wavs = collect_ordered_xmas(method_dir, VOICE_COUNT)

    write_bank(os.path.join(args.data_dir, "hdsnd_sfx.dat"), sfx_wavs, DST_RATE)
    write_bank(os.path.join(args.data_dir, "hdsnd_voices.dat"), voice_wavs, DST_RATE)
    write_bank(os.path.join(args.data_dir, "hdsnd_voicesc.dat"), voicesc_wavs, DST_RATE)

    return 0


if __name__ == "__main__":
    sys.exit(main())
