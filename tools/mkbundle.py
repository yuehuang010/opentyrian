#!/usr/bin/env python3
"""
mkbundle.py -- build the OpenTyrian standalone asset bundles.

Phase S0 of internal/plan/STANDALONE_PLAN.md packed the classic required
game-data / asset set into `tyrian.base`, the archive that the engine's
`dir_fopen` choke point falls back to when no loose file is found on disk.

Phase S5 (this revision) adds:

  * QOI compression for `HDPX` payloads (a 4-byte magic "HDPX" + u32 LE
    width + u32 LE height + raw RGBA32 pixel blob -- see src/video.c). QOI
    (https://qoiformat.org) is a simple, lossless, dependency-free image
    codec; the encoder here is a from-spec, pure-Python (optionally
    numpy-accelerated) reimplementation. The engine only ever needs to
    *decode*, via the vendored src/qoi.h.
  * A second, optional pak: `tyrian.hd` carries the `hd*` HD-remaster
    assets (excluded from `tyrian.base`), QOI-compressed where they are
    HDPX images. `tyrian.base` remains the byte-exact classic/required set,
    uncompressed (STORE), exactly as in S0.

Bundle format (all integers little-endian) -- unchanged from S0:

    magic        8 bytes   b"TYBUNDL1"
    u32          entry_count
    index[entry_count], each:
        u16      name_len
        bytes    name         (ASCII, stored lowercase, no NUL)
        u8       compression  (0 = store; 1 = QOI, added in S5)
        u32      uncompressed_size   (original file's byte length)
        u32      compressed_size     (stored blob's byte length)
        u32      offset        (absolute, from start of file)
    blobs        concatenated payloads

For a QOI entry (compression == 1), the *stored blob* is the original
12-byte HDPX header (magic + width + height) followed by the QOI chunk
stream -- so the reader (src/bundle.c) can reconstruct the exact original
HDPX byte stream (header + raw RGBA) without the engine's image loaders
(src/video.c) ever knowing compression happened.
"""

import argparse
import os
import re
import struct
import sys

MAGIC = b"TYBUNDL1"

COMPRESS_STORE = 0
COMPRESS_QOI = 1

# Files that live in a tyrian21-style dir but are NOT engine assets: DOS build
# tools, and the HD pipeline's own generated output/manifests/previews. Matched
# case-insensitively against the bare filename.
DEFAULT_EXCLUDE = [
    r"^hd.*",           # hd_*, hdanim_*, hdcomp_* generated HD assets/manifests
    r".*manifest.*",
    r".*previews.*",
    r"^setup.*",
    r"^file_id.*",
    r"^file\d+\.exe$",
    r"^dpmi16bi.*",
    r"^exitmsg\.bin$",
    r".*\.(exe|diz|doc|ovl|png|json|wav|ogg|md|txt|awe|tfp|ico|int)$",
]

# The HD tier: only the hd* generated assets, still dropping non-engine
# metadata (manifests/previews/json/png) that ships alongside them in a
# tyrian21-style working dir.
HD_INCLUDE = re.compile(r"^hd.*", re.IGNORECASE)
HD_EXCLUDE = [
    r".*manifest.*",
    r".*previews.*",
    r".*\.(json|png|md|txt)$",
]


# ---------------------------------------------------------------------------
# QOI encoder (from-spec, https://qoiformat.org/qoi-specification.pdf).
#
# Encoding is inherently sequential (the running "seen pixel" index and the
# run-length counter both depend on encode order), so this cannot be fully
# vectorized. Pixels are packed into a single uint32 per pixel (numpy, if
# available) so the hot loop only ever compares/hashes integers instead of
# 4-tuples, which is the dominant speedup available in pure Python.
# ---------------------------------------------------------------------------

try:
    import numpy as _np
except ImportError:  # pragma: no cover - numpy is expected in the tools venv
    _np = None

_OP_INDEX = 0x00
_OP_DIFF = 0x40
_OP_LUMA = 0x80
_OP_RUN = 0xc0
_OP_RGB = 0xfe
_OP_RGBA = 0xff


def _pack_pixels(rgba_bytes):
    """Return a sequence of per-pixel ints packed as 0xAABBGGRR (r|g<<8|b<<16|a<<24),
    i.e. byte order matches the on-disk RGBA bytes when read little-endian."""
    if _np is not None:
        arr = _np.frombuffer(rgba_bytes, dtype="<u4")
        return arr.tolist()  # a plain python int list indexes much faster in the hot loop
    else:
        n = len(rgba_bytes) // 4
        return list(struct.unpack("<%dI" % n, rgba_bytes))


def qoi_encode(rgba_bytes, width, height, channels=4):
    """Encode raw RGBA8888 bytes (row-major, top-to-bottom) into a QOI chunk
    stream (no container header -- OpenTyrian keeps width/height in the HDPX
    header already). Returns bytes."""
    px_count = width * height
    assert len(rgba_bytes) == px_count * 4, (len(rgba_bytes), px_count)

    pixels = _pack_pixels(rgba_bytes)

    index = [0] * 64          # packed-pixel value per hash slot, init to 0x00000000 (r=g=b=0,a=0)
    out = bytearray()
    prev = 0x000000ff         # r=0,g=0,b=0,a=255 packed little-endian (a is high byte)
    run = 0

    append = out.append

    for i in range(px_count):
        px = pixels[i]

        if px == prev:
            run += 1
            if run == 62 or i == px_count - 1:
                append(_OP_RUN | (run - 1))
                run = 0
            continue

        if run > 0:
            append(_OP_RUN | (run - 1))
            run = 0

        r = px & 0xff
        g = (px >> 8) & 0xff
        b = (px >> 16) & 0xff
        a = (px >> 24) & 0xff

        hash_idx = (r * 3 + g * 5 + b * 7 + a * 11) % 64

        if index[hash_idx] == px:
            append(_OP_INDEX | hash_idx)
        else:
            index[hash_idx] = px

            pr = prev & 0xff
            pg = (prev >> 8) & 0xff
            pb = (prev >> 16) & 0xff
            pa = (prev >> 24) & 0xff

            if a == pa:
                dr = (r - pr + 128) % 256 - 128
                dg = (g - pg + 128) % 256 - 128
                db = (b - pb + 128) % 256 - 128

                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    append(_OP_DIFF | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2))
                else:
                    dg_r = dr - dg
                    dg_b = db - dg
                    if -32 <= dg <= 31 and -8 <= dg_r <= 7 and -8 <= dg_b <= 7:
                        append(_OP_LUMA | (dg + 32))
                        append(((dg_r + 8) << 4) | (dg_b + 8))
                    else:
                        append(_OP_RGB)
                        append(r); append(g); append(b)
            else:
                append(_OP_RGBA)
                append(r); append(g); append(b); append(a)

        prev = px

    return bytes(out)


def is_hdpx(data):
    return len(data) >= 12 and data[:4] == b"HDPX"


def encode_hdpx_entry(data):
    """Given full HDPX file bytes, return (compression, blob) -- QOI-compressed
    (header + QOI stream) if it round-trips byte-exact, else STORE as a
    fallback (never emit a lossy or unverified blob)."""
    width, height = struct.unpack_from("<II", data, 4)
    pixel_off = 12
    rgba = data[pixel_off:pixel_off + width * height * 4]
    if len(rgba) != width * height * 4 or width == 0 or height == 0:
        return COMPRESS_STORE, data

    stream = qoi_encode(rgba, width, height)
    blob = data[:12] + stream

    return COMPRESS_QOI, blob


def is_excluded(name, patterns):
    lower = name.lower()
    return any(re.match(p, lower) for p in patterns)


def gather(src, manifest, include, exclude):
    if manifest:
        with open(manifest) as f:
            names = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    else:
        names = []
        for entry in sorted(os.listdir(src)):
            path = os.path.join(src, entry)
            if not os.path.isfile(path):
                continue
            if include is not None and not include.match(entry):
                continue
            if is_excluded(entry, exclude):
                continue
            names.append(entry)
    return names


def build(src, out, names, compress_hdpx, verify, progress_every=200):
    # Read every payload, optionally QOI-compressing HDPX images.
    blobs = []  # (name.lower(), compression, uncompressed_size, stored_blob)
    qoi_before = 0
    qoi_after = 0
    for n, name in enumerate(names):
        path = os.path.join(src, name)
        with open(path, "rb") as f:
            data = f.read()

        compression = COMPRESS_STORE
        stored = data

        if compress_hdpx and is_hdpx(data):
            compression, stored = encode_hdpx_entry(data)
            if compression == COMPRESS_QOI:
                if verify:
                    roundtrip = decode_qoi_entry(stored)
                    if roundtrip != data:
                        print(f"error: QOI round-trip mismatch for {name}; falling back to STORE",
                              file=sys.stderr)
                        compression, stored = COMPRESS_STORE, data
                if compression == COMPRESS_QOI:
                    qoi_before += len(data)
                    qoi_after += len(stored)

        blobs.append((name.lower(), compression, len(data), stored))

        if progress_every and (n + 1) % progress_every == 0:
            print(f"  ...{n + 1}/{len(names)} files packed", file=sys.stderr)

    # Header size is fixed once we know the names, so offsets can be resolved.
    header_len = len(MAGIC) + 4
    for name, _, _, _ in blobs:
        header_len += 2 + len(name.encode("ascii")) + 1 + 4 + 4 + 4

    index = bytearray()
    index += MAGIC
    index += struct.pack("<I", len(blobs))

    offset = header_len
    payload = bytearray()
    for name, compression, uncompressed_size, stored in blobs:
        nb = name.encode("ascii")
        index += struct.pack("<H", len(nb))
        index += nb
        index += struct.pack("<B", compression)
        index += struct.pack("<I", uncompressed_size)
        index += struct.pack("<I", len(stored))
        index += struct.pack("<I", offset)
        payload += stored
        offset += len(stored)

    assert len(index) == header_len, (len(index), header_len)

    with open(out, "wb") as f:
        f.write(index)
        f.write(payload)

    return len(blobs), header_len + len(payload), qoi_before, qoi_after


def decode_qoi_entry(blob):
    """Inverse of encode_hdpx_entry, for --verify. Reference decoder kept
    separate from src/qoi.h (which is C) -- this is the Python-side check
    that the *encoder* is self-consistent before it ever reaches the engine."""
    magic = blob[:4]
    width, height = struct.unpack_from("<II", blob, 4)
    stream = blob[12:]
    rgba = qoi_decode(stream, width, height)
    return magic + struct.pack("<II", width, height) + rgba


def qoi_decode(stream, width, height):
    px_count = width * height
    index = [0] * 64
    out = bytearray(px_count * 4)
    px = bytearray((0, 0, 0, 255))
    pos = 0
    run = 0
    n = len(stream)

    for i in range(px_count):
        if run > 0:
            run -= 1
        elif pos < n:
            b1 = stream[pos]; pos += 1
            if b1 == _OP_RGB:
                px[0] = stream[pos]; px[1] = stream[pos + 1]; px[2] = stream[pos + 2]
                pos += 3
            elif b1 == _OP_RGBA:
                px[0] = stream[pos]; px[1] = stream[pos + 1]
                px[2] = stream[pos + 2]; px[3] = stream[pos + 3]
                pos += 4
            elif (b1 & 0xc0) == _OP_INDEX:
                v = index[b1 & 0x3f]
                px[0] = v & 0xff; px[1] = (v >> 8) & 0xff
                px[2] = (v >> 16) & 0xff; px[3] = (v >> 24) & 0xff
            elif (b1 & 0xc0) == _OP_DIFF:
                px[0] = (px[0] + ((b1 >> 4) & 0x03) - 2) & 0xff
                px[1] = (px[1] + ((b1 >> 2) & 0x03) - 2) & 0xff
                px[2] = (px[2] + (b1 & 0x03) - 2) & 0xff
            elif (b1 & 0xc0) == _OP_LUMA:
                b2 = stream[pos]; pos += 1
                vg = (b1 & 0x3f) - 32
                px[0] = (px[0] + vg - 8 + ((b2 >> 4) & 0x0f)) & 0xff
                px[1] = (px[1] + vg) & 0xff
                px[2] = (px[2] + vg - 8 + (b2 & 0x0f)) & 0xff
            elif (b1 & 0xc0) == _OP_RUN:
                run = b1 & 0x3f

            index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] = (
                px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24)
            )

        o = i * 4
        out[o] = px[0]; out[o + 1] = px[1]; out[o + 2] = px[2]; out[o + 3] = px[3]

    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Build the OpenTyrian asset bundle(s).")
    ap.add_argument("--src", required=True, help="source data directory (e.g. tyrian21)")
    ap.add_argument("--out", default="tyrian.base", help="output bundle path (classic tier)")
    ap.add_argument("--hd-out", default=None,
                    help="also build the HD tier pak at this path (e.g. tyrian.hd)")
    ap.add_argument("--manifest", help="optional file listing names to pack (one per line)")
    ap.add_argument("--no-default-exclude", action="store_true",
                    help="pack every regular file in --src (skip the DOS/HD exclude list)")
    ap.add_argument("--no-qoi", action="store_true",
                    help="store HD tier HDPX assets uncompressed instead of QOI-encoding them")
    ap.add_argument("--verify", action="store_true",
                    help="round-trip every QOI-compressed entry during packing and abort "
                         "the QOI path (falling back to STORE) for any that don't reproduce "
                         "the original bytes exactly")
    args = ap.parse_args()

    exclude = [] if args.no_default_exclude else DEFAULT_EXCLUDE
    names = gather(args.src, args.manifest, None, exclude)
    if not names:
        print("error: no files to pack", file=sys.stderr)
        return 1

    count, total, _, _ = build(args.src, args.out, names, compress_hdpx=False, verify=False)
    print(f"wrote {args.out}: {count} files, {total} bytes ({total / 1024 / 1024:.1f} MiB)")

    if args.hd_out:
        hd_names = gather(args.src, None, HD_INCLUDE, HD_EXCLUDE)
        if not hd_names:
            print(f"warning: no HD assets found under {args.src}; not writing {args.hd_out}",
                  file=sys.stderr)
            return 0

        hd_count, hd_total, qoi_before, qoi_after = build(
            args.src, args.hd_out, hd_names,
            compress_hdpx=not args.no_qoi, verify=args.verify)

        print(f"wrote {args.hd_out}: {hd_count} files, {hd_total} bytes "
              f"({hd_total / 1024 / 1024:.1f} MiB)")
        if qoi_before:
            ratio = qoi_after / qoi_before
            print(f"  QOI: {qoi_before / 1024 / 1024:.1f} MiB raw -> "
                  f"{qoi_after / 1024 / 1024:.1f} MiB QOI "
                  f"({ratio * 100:.1f}% of original, {1 / ratio:.2f}x compression)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
