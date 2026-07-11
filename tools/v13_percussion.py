#!/usr/bin/env python3
"""v13_percussion.py - re-author track 30's V13 (8251725a) as drum-kit MIDI.

The OPL voice is bursts: a low note (<55) followed by a 13-note 37 ms high
cascade. The chip fuses each burst into one composite hit ("small drum with
a small brass hat"); per-note sample playback machine-guns it. So instead of
mapping the voice to a GM program (it's vol 0 in lds_gm_map.txt), this
script rewrites the part from the .notes ground-truth dump:

  - every LOW note  -> hi-mid tom (48) + closed hi-hat (42) struck together
  - each cascade segment between low notes -> quiet open hi-hat (46) tail,
    choked by a pedal hat (44) where real silence (>8 ticks) follows

Output is an SMF-0 file on the percussion channel, same tick clock as
lds_to_midi (1 LDS tick = 1 MIDI tick @ 96 TPQN, tempo meta matches), so a
fluidsynth render of it is sample-aligned with the main render and can be
amixed straight in (see hd_music_pipeline.sh PERC_MIDI/PERC_GAIN).

Usage: v13_percussion.py <hdmusic_30.notes> <out.mid> [fingerprint]
"""

import struct
import sys

FP_DEFAULT = "8251725a"
LOW_SPLIT = 55        # below: drum note; at/above: cascade
GAP_TICKS = 8         # silence gap that ends a cascade segment
TOM, CLOSED_HAT, OPEN_HAT, PEDAL_HAT = 48, 42, 46, 44
TPQN = 96
TEMPO = int(round(113.669048e6 / 7900.0 * TPQN))  # usec/qn: 1 tick = 1 LDS tick


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    notes_path, out_path = sys.argv[1], sys.argv[2]
    fp = (sys.argv[3] if len(sys.argv) > 3 else FP_DEFAULT).lower()

    notes = []
    with open(notes_path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) >= 5 and p[4].lower() == fp:
                notes.append((int(p[0]), int(p[2])))
    notes.sort()
    if not notes:
        sys.exit(f"error: no notes for fingerprint {fp} in {notes_path}")

    lows = [i for i, (t, n) in enumerate(notes) if n < LOW_SPLIT]
    all_on = [t for t, _ in notes]
    events = []  # (tick, drum_note, velocity, duration_ticks)
    for k, i in enumerate(lows):
        t0 = notes[i][0]
        limit = notes[lows[k + 1]][0] if k + 1 < len(lows) else 1 << 62
        seg = []
        prev = t0
        for t, n in notes[i + 1:]:
            if t >= limit or t - prev > GAP_TICKS:
                break
            if n >= LOW_SPLIT:
                seg.append(t)
            prev = t
        events.append((t0, TOM, 108, 8))
        events.append((t0, CLOSED_HAT, 95, 4))
        if seg:
            tend = seg[-1]
            events.append((t0 + 2, OPEN_HAT, 68, max(4, tend - t0 - 2)))
            nxt = [t for t in all_on if t > tend]
            if not nxt or nxt[0] - tend > GAP_TICKS:
                events.append((tend + 2, PEDAL_HAT, 60, 4))

    track = bytearray()
    track += b"\x00\xff\x51\x03" + struct.pack(">I", TEMPO)[1:]
    timeline = []
    for t, note, vel, dur in events:
        timeline.append((t, 1, note, vel))
        timeline.append((t + dur, 0, note, 0))
    timeline.sort()
    last = 0
    for t, on, note, vel in timeline:
        track += vlq(t - last)
        last = t
        track += bytes([0x99 if on else 0x89, note, vel if on else 0])
    track += b"\x00\xff\x2f\x00"

    with open(out_path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQN))
        f.write(b"MTrk" + struct.pack(">I", len(track)) + bytes(track))
    print(f"{out_path}: {len(lows)} drum hits, {len(events)} events "
          f"(from {len(notes)} source notes)")


if __name__ == "__main__":
    main()
