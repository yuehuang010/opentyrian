#!/usr/bin/env python3
"""
mkbundle.py -- build the OpenTyrian standalone asset bundle (`tyrian.base`).

Phase S0 of doc/STANDALONE_PLAN.md: pack the classic required game-data /
asset set into a single archive that the engine's `dir_fopen` choke point
falls back to when no loose file is found on disk. This lets a checkout with
no `--data` dir still boot.

Format (all integers little-endian):

    magic        8 bytes   b"TYBUNDL1"
    u32          entry_count
    index[entry_count], each:
        u16      name_len
        bytes    name         (ASCII, stored lowercase, no NUL)
        u8       compression  (0 = store; other values reserved for S5)
        u32      uncompressed_size
        u32      compressed_size
        u32      offset        (absolute, from start of file)
    blobs        concatenated payloads

S0 ships everything STORE (compression 0). The per-entry compression byte and
the separate compressed/uncompressed sizes exist so Phase S5 can drop in
zstd/deflate blobs without a format break.
"""

import argparse
import os
import re
import struct
import sys

MAGIC = b"TYBUNDL1"

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


def is_excluded(name, patterns):
    lower = name.lower()
    return any(re.match(p, lower) for p in patterns)


def gather(src, manifest, exclude):
    if manifest:
        with open(manifest) as f:
            names = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    else:
        names = []
        for entry in sorted(os.listdir(src)):
            path = os.path.join(src, entry)
            if not os.path.isfile(path):
                continue
            if is_excluded(entry, exclude):
                continue
            names.append(entry)
    return names


def build(src, out, names):
    # Read every payload first (STORE mode: compressed == uncompressed).
    blobs = []
    for name in names:
        path = os.path.join(src, name)
        with open(path, "rb") as f:
            data = f.read()
        blobs.append((name.lower(), data))

    # Header size is fixed once we know the names, so offsets can be resolved.
    header_len = len(MAGIC) + 4
    for name, _ in blobs:
        header_len += 2 + len(name.encode("ascii")) + 1 + 4 + 4 + 4

    index = bytearray()
    index += MAGIC
    index += struct.pack("<I", len(blobs))

    offset = header_len
    payload = bytearray()
    for name, data in blobs:
        nb = name.encode("ascii")
        index += struct.pack("<H", len(nb))
        index += nb
        index += struct.pack("<B", 0)               # compression: store
        index += struct.pack("<I", len(data))       # uncompressed_size
        index += struct.pack("<I", len(data))       # compressed_size (== store)
        index += struct.pack("<I", offset)          # offset
        payload += data
        offset += len(data)

    assert len(index) == header_len, (len(index), header_len)

    with open(out, "wb") as f:
        f.write(index)
        f.write(payload)

    return len(blobs), header_len + len(payload)


def main():
    ap = argparse.ArgumentParser(description="Build the OpenTyrian asset bundle.")
    ap.add_argument("--src", required=True, help="source data directory (e.g. tyrian21)")
    ap.add_argument("--out", default="tyrian.base", help="output bundle path")
    ap.add_argument("--manifest", help="optional file listing names to pack (one per line)")
    ap.add_argument("--no-default-exclude", action="store_true",
                    help="pack every regular file in --src (skip the DOS/HD exclude list)")
    args = ap.parse_args()

    exclude = [] if args.no_default_exclude else DEFAULT_EXCLUDE
    names = gather(args.src, args.manifest, exclude)
    if not names:
        print("error: no files to pack", file=sys.stderr)
        return 1

    count, total = build(args.src, args.out, names)
    print(f"wrote {args.out}: {count} files, {total} bytes ({total / 1024 / 1024:.1f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
