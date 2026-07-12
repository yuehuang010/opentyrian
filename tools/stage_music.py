#!/usr/bin/env python3
"""
stage_music.py -- Stage the committed HD music OGGs (hdmusic/) into a data dir.

The 41 frozen HD music renders (see tools/MUSIC_REMASTER.md) are committed as
source under the top-level hdmusic/ directory via Git LFS -- they are no
longer produced by a local render/encode pipeline. The engine loads
hdmusic_NN.ogg out of the data dir at runtime (src/loudness.c,
hd_music_load_song(), via dir_fopen), and tools/mkbundle.py only packs files
that are physically present in its single --src directory (a flat
os.listdir(), see its DEFAULT_EXCLUDE/HD_INCLUDE). Neither can see straight
into hdmusic/, so this script's only job is copying the committed OGGs from
hdmusic/ into the data dir so both paths find them.

Usage:
  python3 tools/stage_music.py --list              # show what would be staged, no side effects
  python3 tools/stage_music.py --data ./tyrian21    # stage into ./tyrian21 (default)
  python3 tools/stage_music.py --data ./tyrian21 --force   # re-copy even if up-to-date

A destination file that already exists with the same size as the source is
considered up-to-date and skipped unless --force is given.

If hdmusic/ is missing, empty, or contains files that are suspiciously small
(< 1 KB -- almost certainly an un-materialized Git LFS pointer stub rather
than real audio), this script errors out with a `git lfs pull` hint instead
of staging a broken pointer file into the data dir.
"""

import argparse
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HDMUSIC_DIR = REPO_ROOT / "hdmusic"
DEFAULT_DATA_DIR = REPO_ROOT / "tyrian21"

# A real hdmusic_NN.ogg is hundreds of KB at least; a Git LFS pointer stub is
# ~130 bytes of text ("version https://git-lfs.github.com/spec/v1\n..."). 1 KB
# is a generous cutoff that can't mistake a genuine (if tiny) OGG for a stub.
MIN_REAL_OGG_BYTES = 1024

LFS_PULL_HINT = "  git lfs pull"


def _mb(num_bytes):
    return num_bytes / 1024 / 1024


def find_sources():
    """Return the sorted list of hdmusic/hdmusic_*.ogg source Paths."""
    if not HDMUSIC_DIR.is_dir():
        return []
    return sorted(HDMUSIC_DIR.glob("hdmusic_*.ogg"))


def check_sources(sources):
    """Return a list of (path, size) for valid sources, or raise SystemExit
    with an actionable message if hdmusic/ is missing/empty or any source
    file looks like an un-pulled Git LFS pointer stub."""
    if not sources:
        print("error: no hdmusic_*.ogg files found in %s" % HDMUSIC_DIR, file=sys.stderr)
        print("  This repo ships HD music as source under hdmusic/ via Git LFS.", file=sys.stderr)
        print("  If you just cloned (or cloned without LFS smudge), materialize it with:", file=sys.stderr)
        print(LFS_PULL_HINT, file=sys.stderr)
        raise SystemExit(1)

    stub_names = []
    sized = []
    for path in sources:
        size = path.stat().st_size
        if size < MIN_REAL_OGG_BYTES:
            stub_names.append((path.name, size))
        else:
            sized.append((path, size))

    if stub_names:
        print("error: %d file(s) in %s look like un-pulled Git LFS pointer stubs "
              "(< %d bytes, not real audio):" % (len(stub_names), HDMUSIC_DIR, MIN_REAL_OGG_BYTES),
              file=sys.stderr)
        for name, size in stub_names:
            print("  %s (%d bytes)" % (name, size), file=sys.stderr)
        print("  Materialize the real files with:", file=sys.stderr)
        print(LFS_PULL_HINT, file=sys.stderr)
        raise SystemExit(1)

    return sized


def do_list(sized_sources):
    total = 0
    for path, size in sized_sources:
        total += size
        print("  %-20s %6.1f MB" % (path.name, _mb(size)))
    print()
    print("%d file(s), total %.1f MB" % (len(sized_sources), _mb(total)))
    return 0


def stage(sized_sources, data_dir, force):
    if not data_dir.is_dir():
        print("error: data dir does not exist: %s" % data_dir, file=sys.stderr)
        print("  Base Tyrian 2.1 data must already be unpacked there -- see README.", file=sys.stderr)
        return 1

    copied = 0
    up_to_date = 0
    total = 0

    for path, size in sized_sources:
        total += size
        dest = data_dir / path.name

        if dest.is_file() and dest.stat().st_size == size and not force:
            print("  up-to-date  %s" % path.name)
            up_to_date += 1
            continue

        shutil.copy2(str(path), str(dest))
        print("  copied      %s (%.1f MB)" % (path.name, _mb(size)))
        copied += 1

    print()
    print("%d copied, %d up-to-date, total %.1f MB" % (copied, up_to_date, _mb(total)))
    return 0


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Stage the committed HD music OGGs (hdmusic/) into a data dir, so "
                     "both the engine (direct play) and tools/mkbundle.py (which only "
                     "reads files physically present in its --src dir) can find them.",
    )
    parser.add_argument(
        "--data", default=str(DEFAULT_DATA_DIR),
        help="destination data dir (default: %s)" % DEFAULT_DATA_DIR)
    parser.add_argument(
        "--force", action="store_true",
        help="re-copy every file even if the destination already exists with an "
             "identical size (default: skip up-to-date files)")
    parser.add_argument(
        "--list", action="store_true",
        help="print what would be copied and exit; no side effects")
    return parser


def main(argv=None):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    sources = find_sources()
    try:
        sized_sources = check_sources(sources)
    except SystemExit as e:
        return e.code

    if args.list:
        return do_list(sized_sources)

    data_dir = Path(args.data).resolve()
    return stage(sized_sources, data_dir, args.force)


if __name__ == "__main__":
    sys.exit(main())
