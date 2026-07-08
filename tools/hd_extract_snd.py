#!/usr/bin/env python3
"""
Phase S2 (SFX & voice remaster) EXPERIMENT tool.

Extract every sample from Tyrian's three sound banks (tyrian.snd, voices.snd,
voicesc.snd) into per-sample WAV files, preserving the original 8-bit PCM data
losslessly as the source-of-truth for later upsampling experiments.

Format (verified against src/nortsong.c loadSndFile + src/sndmast.c):
  - Header: Uint16 LE count, then count x Uint32 LE offsets into the file.
    Sample i runs from offset[i] to offset[i+1]; the last sample runs to EOF.
  - Samples are raw *signed* 8-bit mono PCM at 11025 Hz, each <= 64 KiB.
  - Engine quirk: the last 100 bytes of every VOICE sample (voices.snd /
    voicesc.snd only) are stripped as bad data. We replicate that here.
  - Effect names come from soundTitle[] in src/sndmast.c.

WAV is written as 8-bit *unsigned* PCM (the WAV spec's only 8-bit encoding),
which is a lossless bijection of the signed source (add 128). No resampling or
requantization happens here -- this is the pristine reference set.

stdlib-only (struct + wave); no numpy needed.
"""
import argparse
import os
import struct
import sys
import wave

SRC_RATE = 11025
VOICE_TAIL_STRIP = 100  # bytes stripped from the end of every voice sample

# soundTitle[] from src/sndmast.c -- the 29 SFX names, in order.
SFX_NAMES = [
    "SCALEDN2", "F2", "TEMP10", "EXPLSM", "PASS3", "TEMP2", "BYPASS1",
    "EXP1RT", "EXPLLOW", "TEMP13", "EXPRETAP", "MT2BOOM", "TEMP3", "LAZB",
    "LAZGUN2", "SPRING", "WARNING", "ITEM", "HIT2", "MACHNGUN", "HYPERD2",
    "EXPLHUG", "CLINK1", "CLICK", "SCALEDN1", "TEMP11", "TEMP16", "SMALL1",
    "POWERUP",
]
VOICE_NAMES = ["VOICE%d" % i for i in range(1, 10)]  # VOICE1..VOICE9

SFX_COUNT = 29
VOICE_COUNT = 9


def read_bank(path, expect_count):
    """Return list of raw signed-8-bit byte payloads for each sample."""
    with open(path, "rb") as f:
        data = f.read()
    (count,) = struct.unpack_from("<H", data, 0)
    if count != expect_count:
        raise ValueError("%s: header count %d != expected %d"
                         % (path, count, expect_count))
    offs = list(struct.unpack_from("<%dI" % count, data, 2))
    offs.append(len(data))
    return [data[offs[i]:offs[i + 1]] for i in range(count)]


def write_wav_u8(path, signed_bytes):
    """Write raw signed-8-bit PCM as 8-bit *unsigned* mono WAV (lossless)."""
    # signed [-128,127] -> unsigned [0,255] by +128 (bijective).
    ub = bytes((b + 128) & 0xFF for b in signed_bytes)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(SRC_RATE)
        w.writeframes(ub)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_dir", nargs="?",
                    default="/Users/felixhuang/source/opentyrian/tyrian21",
                    help="directory containing tyrian.snd / voices.snd / voicesc.snd")
    ap.add_argument("--out", default=None,
                    help="output dir (default: <tools>/hdsfx_previews/orig)")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    out = args.out or os.path.join(here, "hdsfx_previews", "orig")
    os.makedirs(out, exist_ok=True)

    manifest = []  # (index, bank, name, orig_bytes, kept_samples)
    idx = 0

    # --- tyrian.snd : 29 SFX, no tail strip ---
    sfx = read_bank(os.path.join(args.data_dir, "tyrian.snd"), SFX_COUNT)
    for i, payload in enumerate(sfx):
        name = SFX_NAMES[i]
        fn = "%02d_sfx_%s.wav" % (idx, name)
        write_wav_u8(os.path.join(out, fn), payload)
        manifest.append((idx, "sfx", name, len(payload), len(payload)))
        idx += 1

    # --- voices.snd : 9 voices, strip trailing 100 bytes ---
    voi = read_bank(os.path.join(args.data_dir, "voices.snd"), VOICE_COUNT)
    for i, payload in enumerate(voi):
        kept = payload[:-VOICE_TAIL_STRIP] if len(payload) >= VOICE_TAIL_STRIP else b""
        name = VOICE_NAMES[i]
        fn = "%02d_voice_%s.wav" % (idx, name)
        write_wav_u8(os.path.join(out, fn), kept)
        manifest.append((idx, "voice", name, len(payload), len(kept)))
        idx += 1

    # --- voicesc.snd : 9 xmas voices, strip trailing 100 bytes ---
    # These share the same engine slots as voices.snd (loaded when xmas=true);
    # extracted separately so the experiment can cover them too.
    voic = read_bank(os.path.join(args.data_dir, "voicesc.snd"), VOICE_COUNT)
    for i, payload in enumerate(voic):
        kept = payload[:-VOICE_TAIL_STRIP] if len(payload) >= VOICE_TAIL_STRIP else b""
        name = VOICE_NAMES[i]
        fn = "xmas_%02d_voice_%s.wav" % (i, name)
        write_wav_u8(os.path.join(out, fn), kept)
        manifest.append(("x%d" % i, "voicesc", name, len(payload), len(kept)))

    # --- report ---
    print("Extracted to: %s" % out)
    print("%-6s %-9s %-10s %10s %10s %10s" %
          ("idx", "bank", "name", "raw_bytes", "kept_samp", "dur_ms"))
    for idx_, bank, name, raw, kept in manifest:
        print("%-6s %-9s %-10s %10d %10d %10.1f" %
              (str(idx_), bank, name, raw, kept, 1000.0 * kept / SRC_RATE))

    n_sfx = sum(1 for m in manifest if m[1] == "sfx")
    n_voi = sum(1 for m in manifest if m[1] == "voice")
    n_voic = sum(1 for m in manifest if m[1] == "voicesc")
    print()
    print("counts: sfx=%d (expect %d), voice=%d (expect %d), voicesc=%d (expect %d)"
          % (n_sfx, SFX_COUNT, n_voi, VOICE_COUNT, n_voic, VOICE_COUNT))
    ok = (n_sfx == SFX_COUNT and n_voi == VOICE_COUNT and n_voic == VOICE_COUNT)
    print("COUNT CHECK: %s" % ("OK" if ok else "MISMATCH"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
