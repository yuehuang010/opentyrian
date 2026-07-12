#!/usr/bin/env python3
"""
hd_extract_anim.py -- Offline HD asset extraction tool for OpenTyrian's ending
cutscene, tyrian21/tyrend.anm.

STANDALONE: this file is deliberately self-contained (see the "VENDORED
HELPERS" section below) so it can run concurrently with other extractor
tools/agents editing tools/hd_extract.py without either file importing the
other. Touches no tracked file besides itself.

Format background (tyrend.anm is NOT a DOS Autodesk FLI/FLC file, despite the
".anm" extension and superficial resemblance -- it's OpenTyrian's own simpler
"page-based delta" animation container). Mirrored 1:1 from the engine's own
decoder/player:
  - src/animlib.c: readFileHeader(), readPalette(), readPageDescriptors(),
    decodeRunSkipDump() -- the record delta-opcode decoder.
  - src/tyrian2.c ~2506: playAnim("tyrend.anm", 0, 7) -- confirms the file
    used, the starting record (0), and (importantly) that the palette used
    on screen is the one embedded in the .anm file itself (readPalette()),
    NOT palette.dat.

File layout (see animlib.c for the authoritative field-by-field decode):
  1. FileHeader (256 bytes): 6 bytes skip, u16 LE pageCount, u32 LE
     recordCount (total logical frames + 1, see below), 244 bytes skip.
  2. Palette (1024 bytes): 256 entries of (b, g, r, pad) raw bytes -- note
     the on-disk channel order is B,G,R, and unlike palette.dat (which
     stores 6-bit VGA values needing the "(v<<2)|(v>>4)" expansion in
     JE_loadPals()), this animation's embedded palette already stores full
     8-bit values (confirmed by inspection: byte values up to 252 appear,
     which is impossible for a 6-bit-only source) -- so no bit expansion is
     applied here, only the raw (r, g, b) triples are used as-is.
  3. PageDescriptors (256 * 6 bytes): per page, u16 LE firstRecord, u16 LE
     recordCount, u16 LE recordsSize. Only the first `pageCount` entries are
     meaningful. Pages are NOT stored in firstRecord order on disk -- each
     page i's on-disk location is purely a function of its *index* in this
     table: file offset 0xB00 + (i << 16), one fixed-size (64K-ish) slot per
     page, regardless of how much of that slot the page's data occupies.
  4. Each page slot: 8 bytes skip (page header, "duplicated but not to be
     relied upon" per animlib.c's comment), then recordCount u16 LE record
     sizes, then recordsSize bytes of concatenated record bodies (record
     sizes index directly into this concatenated blob, in order).
  5. Each record body: 4-byte header (u8 bitmap id 'B', u8 flags, u16 LE
     body type) then a "run/skip/dump" delta-opcode stream (see
     decodeRunSkipDump() below) that is applied CUMULATIVELY on top of a
     persistent 320x200 framebuffer -- i.e. each record's decode starts from
     wherever the framebuffer was left after the previous record, and a
     "skip" opcode means "leave these bytes unchanged from last frame" (this
     is the actual inter-frame delta compression).

  playAnim() iterates record numbers 0 .. fileHeader.recordCount-2 inclusive
  (loop condition is `record < recordCount - 1`), producing
  fileHeader.recordCount - 1 decoded/displayed frames total. We mirror this
  exactly to get bit-identical frame content, then take every decoded
  framebuffer (not just what's visible on screen) as one HD-remaster frame.

Run/skip/dump opcode stream (decodeRunSkipDump() in animlib.c), applied to a
flat running write-cursor over the 320*200 framebuffer:
  0x00              : u8 size, u8 value  -> fill `size` bytes with `value`
  0x01..0x7F         : (size = opcode)    -> dump: copy `size` raw bytes from
                        the stream into the framebuffer at the cursor
  0x81..0xFF         : (size = opcode-0x80) -> skip `size` bytes (cursor
                        advances, framebuffer bytes there are UNCHANGED)
  0x80 + u16 LE word :
    0x0000                    -> stop (end of this record's stream)
    0x0001..0x7FFF (size=word)-> long skip
    0x8000..0xBFFF (size=word-0x8000) -> long dump
    0xC000..0xFFFF (size=word-0xC000, then u8 value) -> long fill/run

Verified against tools/hd_extract.py's HDPX-writer / PNG-writer / Lanczos
upscaler conventions but all of that code is COPIED (vendored) below, not
imported, per the parallel-safety requirement -- tools/hd_extract.py may be
concurrently edited by another agent.

Output:
  - tyrian21/hdanim_tyrend_NNNN.dat: one HDPX raw-RGBA asset per decoded
    frame (NNNN = zero-padded frame index), Lanczos-3 4x upscaled
    (320x200 -> 1280x800), colorized with the animation's own embedded
    palette, fully opaque.
  - tyrian21/hd_anim_manifest.json: frame count, source/HD dimensions, and
    the ordered list of output files.
  - tyrian21/hd_anim_previews/hdanim_tyrend_first.png /
    _mid.png / _last.png: PNG previews of the first, middle, and last
    decoded frame, for human eyeballing. (Placed under tyrian21/, which is
    already wholly gitignored, rather than under tools/, to avoid adding a
    new untracked path outside of gitignored output directories.)

Standard library only (no Pillow/numpy/ImageMagick). Data directory is
assumed to be ./tyrian21 relative to the repo root (gitignored, populated by
unzipping the freeware Tyrian 2.1 data files).

Usage:
  tools/hd_extract_anim.py [--no-preview]
"""

import argparse
import concurrent.futures
import json
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(REPO_ROOT, "tyrian21")
ANM_PATH = os.path.join(DATA_DIR, "tyrend.anm")
MANIFEST_PATH = os.path.join(DATA_DIR, "hd_anim_manifest.json")
PREVIEW_DIR = os.path.join(DATA_DIR, "hd_anim_previews")

SRC_W, SRC_H = 320, 200
IMAGE_SIZE = SRC_W * SRC_H
SCALE = 4
DST_W, DST_H = SRC_W * SCALE, SRC_H * SCALE

PAGE_TABLE_SIZE = 256  # fixed-size descriptor table on disk
PAGE_BASE_OFFSET = 0xB00
PAGE_SLOT_SIZE = 1 << 16
PAGE_HEADER_SIZE = 8


# ===========================================================================
# VENDORED HELPERS (copied from tools/hd_extract.py for parallel-safe
# standalone use -- NOT imported, since that file may be concurrently edited
# by another extractor agent). Keep these in sync manually if the upstream
# implementations change; do not import tools/hd_extract.py from here.
# ===========================================================================

# --- HDPX raw-asset writer -------------------------------------------------

def write_hdpx_asset(path, pixels, width, height, channels=3):
    """
    Write the HDPX raw-asset format: ASCII "HDPX", u32 LE width, u32 LE
    height, then width*height RGBA8888 pixels.

    `pixels` holds `channels` bytes/pixel already (3 = RGB, opaque assumed;
    4 = RGBA, alpha taken as-is).
    """
    with open(path, "wb") as f:
        f.write(b"HDPX")
        f.write(struct.pack("<I", width))
        f.write(struct.pack("<I", height))
        if channels == 4:
            f.write(bytes(pixels))
        else:
            out = bytearray(width * height * 4)
            for i in range(width * height):
                so = i * 3
                do = i * 4
                out[do] = pixels[so]
                out[do + 1] = pixels[so + 1]
                out[do + 2] = pixels[so + 2]
                out[do + 3] = 255
            f.write(out)


# --- minimal stdlib-only PNG writer (8-bit RGB, color type 2) -------------

def write_png(path, rgb, width, height):
    def chunk(tag, data):
        out = struct.pack(">I", len(data))
        out += tag
        out += data
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        out += struct.pack(">I", crc)
        return out

    sig = b"\x89PNG\r\n\x1a\n"

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)

    raw = bytearray((width * 3 + 1) * height)
    stride = width * 3
    for y in range(height):
        src_off = y * stride
        dst_off = y * (stride + 1)
        raw[dst_off] = 0  # filter type: None
        raw[dst_off + 1:dst_off + 1 + stride] = rgb[src_off:src_off + stride]

    idat = zlib.compress(bytes(raw), 9)

    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


# --- colorize indices -> flat RGB bytearray (row-major, 3 bytes/pixel) ----

def colorize(indices, palette):
    rgb = bytearray(len(indices) * 3)
    for idx, pixel_index in enumerate(indices):
        r, g, b = palette[pixel_index]
        o = idx * 3
        rgb[o] = r
        rgb[o + 1] = g
        rgb[o + 2] = b
    return rgb


# --- separable Lanczos-3 upscaling ----------------------------------------

def sinc(x):
    if x == 0.0:
        return 1.0
    px = math.pi * x
    return math.sin(px) / px


def lanczos(x, a=3):
    if x <= -a or x >= a:
        return 0.0
    return sinc(x) * sinc(x / a)


def build_taps(src_n, dst_n, a=3):
    """
    Precompute, for each destination sample, the list of (src_index, weight)
    taps needed for a separable Lanczos-a resample from src_n to dst_n
    samples, using the standard "align centers of the sampling grid" mapping.
    Weights are normalized to sum to 1 and clamped to valid source indices
    (edge samples are effectively clamped/replicated).
    """
    scale = src_n / float(dst_n)
    taps = []
    for dst_i in range(dst_n):
        src_center = (dst_i + 0.5) * scale - 0.5
        lo = int(math.floor(src_center)) - a + 1
        hi = int(math.floor(src_center)) + a

        weighted = []
        total = 0.0
        for src_i in range(lo, hi + 1):
            w = lanczos(src_center - src_i, a)
            if w == 0.0:
                continue
            clamped = min(max(src_i, 0), src_n - 1)
            weighted.append((clamped, w))
            total += w

        if total == 0.0:
            nearest = min(max(int(round(src_center)), 0), src_n - 1)
            weighted = [(nearest, 1.0)]
            total = 1.0

        weighted = [(idx, w / total) for idx, w in weighted]
        taps.append(weighted)
    return taps


def clamp_byte(v):
    iv = int(round(v))
    if iv < 0:
        return 0
    if iv > 255:
        return 255
    return iv


def resample_axis_horizontal(src, src_w, src_h, dst_w, taps, channels=3):
    dst = bytearray(dst_w * src_h * channels)
    for y in range(src_h):
        row_off = y * src_w * channels
        out_off = y * dst_w * channels
        for x in range(dst_w):
            acc = [0.0] * channels
            for src_x, w in taps[x]:
                o = row_off + src_x * channels
                for c in range(channels):
                    acc[c] += src[o + c] * w
            o2 = out_off + x * channels
            for c in range(channels):
                dst[o2 + c] = clamp_byte(acc[c])
    return dst


def resample_axis_vertical(src, src_w, src_h, dst_h, taps, channels=3):
    dst = bytearray(src_w * dst_h * channels)
    for y in range(dst_h):
        out_row_off = y * src_w * channels
        col_taps = taps[y]
        for x in range(src_w):
            acc = [0.0] * channels
            xo = x * channels
            for src_y, w in col_taps:
                o = src_y * src_w * channels + xo
                for c in range(channels):
                    acc[c] += src[o + c] * w
            o2 = out_row_off + xo
            for c in range(channels):
                dst[o2 + c] = clamp_byte(acc[c])
    return dst


def lanczos_upscale(rgb, src_w, src_h, dst_w, dst_h, a=3, channels=3, h_taps=None, v_taps=None):
    """
    `h_taps`/`v_taps` may be passed in precomputed (they only depend on
    src/dst dimensions, not on any per-frame pixel data) to skip the
    redundant build_taps() recomputation -- used by the parallel frame
    workers below, which compute the taps once per worker process instead
    of once per frame.
    """
    if h_taps is None:
        h_taps = build_taps(src_w, dst_w, a)
    if v_taps is None:
        v_taps = build_taps(src_h, dst_h, a)

    stage1 = resample_axis_horizontal(rgb, src_w, src_h, dst_w, h_taps, channels)
    stage2 = resample_axis_vertical(stage1, dst_w, src_h, dst_h, v_taps, channels)
    return stage2


# ===========================================================================
# ANM decoding (mirrors src/animlib.c + src/tyrian2.c's playAnim(); see the
# module docstring above for the format writeup)
# ===========================================================================

class AnmError(Exception):
    pass


def read_file_header(data):
    """Mirrors readFileHeader(): 256 bytes total."""
    if len(data) < 256:
        raise AnmError("truncated file header")
    page_count = struct.unpack_from("<H", data, 6)[0]
    record_count = struct.unpack_from("<I", data, 8)[0]
    return page_count, record_count


def read_palette(data):
    """
    Mirrors readPalette(): 256 entries of (b, g, r, pad) raw bytes, no 6->8
    bit expansion (see module docstring for why: the embedded palette is
    already full 8-bit).
    """
    if len(data) < 4 * 256:
        raise AnmError("truncated palette")
    palette = []
    for i in range(256):
        b, g, r, _pad = data[4 * i:4 * i + 4]
        palette.append((r, g, b))
    return palette


def read_page_descriptors(data):
    """Mirrors readPageDescriptors(): 256 entries of 6 bytes each."""
    if len(data) < 6 * PAGE_TABLE_SIZE:
        raise AnmError("truncated page descriptor table")
    descriptors = []
    for i in range(PAGE_TABLE_SIZE):
        first_record, record_count, records_size = struct.unpack_from("<HHH", data, 6 * i)
        descriptors.append((first_record, record_count, records_size))
    return descriptors


def load_page_records(f, page_index, record_count, records_size):
    """
    Mirrors the page-loading half of playAnim()'s per-record loop: seek to
    the page's fixed-size slot, skip the (unreliable) page header, read the
    record-size table, then split the concatenated record-body blob per
    those sizes. Returns a list of `record_count` raw record-body byte
    strings, in order.
    """
    f.seek(PAGE_BASE_OFFSET + (page_index << 16), os.SEEK_SET)
    page_size = PAGE_HEADER_SIZE + 2 * record_count + records_size
    raw = f.read(page_size)
    if len(raw) < PAGE_HEADER_SIZE + 2 * record_count:
        raise AnmError(
            "page %d: truncated read (wanted %d bytes, got %d)" % (page_index, page_size, len(raw)))

    pos = PAGE_HEADER_SIZE
    sizes = list(struct.unpack_from("<%dH" % record_count, raw, pos))
    pos += 2 * record_count

    bodies = []
    for size in sizes:
        body = raw[pos:pos + size]
        if len(body) != size:
            raise AnmError("page %d: truncated record body (wanted %d, got %d)" % (page_index, size, len(body)))
        pos += size
        bodies.append(body)
    return bodies


def decode_run_skip_dump(image, write_pos, reader_data, reader_pos):
    """
    Mirrors decodeRunSkipDump(): applies one record's delta-opcode stream
    onto `image` (a bytearray of length IMAGE_SIZE, modified in place, and
    persisting cumulatively across records/frames -- this function does NOT
    clear it first). `write_pos` is the starting cursor into `image`;
    `reader_pos` the starting cursor into `reader_data`. Returns nothing;
    raises AnmError on any out-of-bounds access (there should be none for a
    well-formed file; treated as fatal so a torn decode is never silently
    emitted).
    """
    n = len(reader_data)
    image_size = len(image)

    def need(pos, size, what):
        if pos + size > n:
            raise AnmError("record stream ran out of data while reading %s" % what)

    while reader_pos < n:
        need(reader_pos, 1, "opcode")
        op_code = reader_data[reader_pos]
        reader_pos += 1

        if op_code == 0x00:  # Short run
            need(reader_pos, 2, "short run size/value")
            size = reader_data[reader_pos]
            value = reader_data[reader_pos + 1]
            reader_pos += 2
            if write_pos + size > image_size:
                raise AnmError("short run overflows framebuffer")
            image[write_pos:write_pos + size] = bytes([value]) * size
            write_pos += size
        elif op_code > 0x80:  # Short skip
            size = op_code - 0x80
            write_pos += size
            if write_pos > image_size:
                raise AnmError("short skip overflows framebuffer")
        elif op_code < 0x80:  # Short dump
            size = op_code
            need(reader_pos, size, "short dump payload")
            if write_pos + size > image_size:
                raise AnmError("short dump overflows framebuffer")
            image[write_pos:write_pos + size] = reader_data[reader_pos:reader_pos + size]
            reader_pos += size
            write_pos += size
        else:  # 0x80: long op
            need(reader_pos, 2, "long opcode word")
            long_op = struct.unpack_from("<H", reader_data, reader_pos)[0]
            reader_pos += 2

            if long_op == 0x0000:  # Stop
                return
            elif long_op >= 0xC000:  # Long run
                size = long_op - 0xC000
                need(reader_pos, 1, "long run value")
                value = reader_data[reader_pos]
                reader_pos += 1
                if write_pos + size > image_size:
                    raise AnmError("long run overflows framebuffer")
                image[write_pos:write_pos + size] = bytes([value]) * size
                write_pos += size
            elif long_op < 0x8000:  # Long skip
                size = long_op
                write_pos += size
                if write_pos > image_size:
                    raise AnmError("long skip overflows framebuffer")
            else:  # Long dump (0x8000..0xBFFF)
                size = long_op - 0x8000
                need(reader_pos, size, "long dump payload")
                if write_pos + size > image_size:
                    raise AnmError("long dump overflows framebuffer")
                image[write_pos:write_pos + size] = reader_data[reader_pos:reader_pos + size]
                reader_pos += size
                write_pos += size
    # Ran out of stream data without a Stop opcode -- treat as "implicit
    # stop", matching the C reader loop (`while (!reader->error)`), which
    # simply exits once the record's MemReader is exhausted.


def decode_anm(path):
    """
    Full decode of an .anm file: returns (palette, frames, meta) where
    palette is a list of 256 (r, g, b) tuples, frames is a list of
    bytearrays (each IMAGE_SIZE = 320*200 bytes, one full framebuffer
    snapshot per decoded record, in cumulative-delta order matching
    playAnim()'s on-screen frame sequence), and meta is a dict of header
    info for reporting.
    """
    with open(path, "rb") as f:
        header_bytes = f.read(256)
        page_count, record_count = read_file_header(header_bytes)

        palette_bytes = f.read(4 * 256)
        palette = read_palette(palette_bytes)

        descriptor_bytes = f.read(6 * PAGE_TABLE_SIZE)
        descriptors = read_page_descriptors(descriptor_bytes)

        # Build a global record-number -> raw body-bytes map by reading
        # every page slot once (pages are non-overlapping in record-number
        # space, so this is equivalent to -- but simpler than -- the
        # original's "reload only the page needed for the current record"
        # sequential-scan logic).
        record_bodies = {}
        for page_index in range(page_count):
            first_record, page_record_count, records_size = descriptors[page_index]
            if page_record_count == 0:
                continue
            bodies = load_page_records(f, page_index, page_record_count, records_size)
            for j, body in enumerate(bodies):
                record_bodies[first_record + j] = body

    # Mirrors playAnim()'s loop bound: `record < fileHeader.recordCount - 1`.
    frame_count = record_count - 1 if record_count > 0 else 0

    image = bytearray(IMAGE_SIZE)
    frames = []
    for record_num in range(frame_count):
        body = record_bodies.get(record_num)
        if body is None:
            raise AnmError("missing record %d (referenced by no page descriptor)" % record_num)
        if len(body) < 4:
            raise AnmError("record %d body too short for its 4-byte header" % record_num)
        # Record header: u8 bitmap id ('B'), u8 flags, u16 LE body type --
        # all skipped/assumed by the engine (see playAnim()'s comments).
        decode_run_skip_dump(image, 0, body, 4)
        frames.append(bytearray(image))  # snapshot: image persists/mutates in place

    meta = {
        "page_count": page_count,
        "record_count": record_count,
        "frame_count": frame_count,
    }
    return palette, frames, meta


# ===========================================================================
# main
# ===========================================================================

def process_frame(frame_index, indices, palette, h_taps=None, v_taps=None):
    rgb = colorize(indices, palette)
    upscaled = lanczos_upscale(rgb, SRC_W, SRC_H, DST_W, DST_H, a=3, h_taps=h_taps, v_taps=v_taps)
    out_name = "hdanim_tyrend_%04d.dat" % frame_index
    out_path = os.path.join(DATA_DIR, out_name)
    write_hdpx_asset(out_path, upscaled, DST_W, DST_H)
    return out_name, upscaled


# ---------------------------------------------------------------------------
# Parallel per-frame processing
#
# Each frame's palette lookup + Lanczos upscale + HDPX write is independent
# of every other frame (the animation's inter-frame delta compression is
# fully resolved by decode_anm() before any of this runs -- `frames` is
# already a list of complete, standalone framebuffer snapshots), so frames
# are farmed out to a process pool.
#
# The Lanczos taps (build_taps()) and the palette are constant across all
# 111 frames (same SRC_W/SRC_H/DST_W/DST_H every time), so they are computed
# ONCE in the parent and handed to each worker process a single time via
# ProcessPoolExecutor's `initializer`/`initargs`, rather than being
# re-pickled and re-sent on every one of the per-frame tasks. Windows has no
# fork(), so the pool uses the (default, cross-platform) "spawn" start
# method: worker processes start fresh and re-import this module, so all
# per-worker state must arrive either via initargs or be a plain
# module-level constant (DATA_DIR etc. qualify; they're computed at import
# time and don't depend on anything set up under `if __name__ == "__main__"`).
# ---------------------------------------------------------------------------

_worker_palette = None
_worker_h_taps = None
_worker_v_taps = None


def _init_worker(palette, h_taps, v_taps):
    """
    ProcessPoolExecutor initializer: stashes the (constant, per-pool-lifetime)
    palette and precomputed Lanczos taps into this worker process's globals,
    once, instead of resending them on every submitted task.
    """
    global _worker_palette, _worker_h_taps, _worker_v_taps
    _worker_palette = palette
    _worker_h_taps = h_taps
    _worker_v_taps = v_taps


def _process_frame_worker(frame_index, indices, need_preview):
    """
    Runs in a worker process. Colorizes + upscales + writes the .dat file for
    one frame, using the palette/taps stashed by _init_worker(). Only returns
    the (large, ~3MB) upscaled RGB buffer when `need_preview` is set (for the
    3 frames a PNG preview gets written for); otherwise returns None in its
    place to avoid shipping the buffer back across the process boundary for
    the ~108 frames that don't need it.
    """
    out_name, upscaled = process_frame(
        frame_index, indices, _worker_palette, _worker_h_taps, _worker_v_taps)
    return frame_index, out_name, (upscaled if need_preview else None)


def main():
    parser = argparse.ArgumentParser(
        description="Extract HD frame assets from tyrian21/tyrend.anm (OpenTyrian ending cutscene).")
    parser.add_argument("--no-preview", action="store_true", help="skip writing PNG previews (faster)")
    parser.add_argument("--jobs", type=int, default=(os.cpu_count() or 1),
                         help="number of worker processes for the per-frame upscale (default: os.cpu_count())")
    args = parser.parse_args()

    if not os.path.isfile(ANM_PATH):
        print("tyrend.anm not found at %s -- nothing to do (is the data dir populated?)" % ANM_PATH)
        return 0

    print("Decoding %s ..." % ANM_PATH)
    try:
        palette, frames, meta = decode_anm(ANM_PATH)
    except AnmError as e:
        print("error: failed to decode tyrend.anm: %s" % e, file=sys.stderr)
        return 1

    frame_count = meta["frame_count"]
    print("Header: pageCount=%d recordCount=%d -> %d decoded frames (320x200 -> %dx%d HD)" %
          (meta["page_count"], meta["record_count"], frame_count, DST_W, DST_H))

    if frame_count == 0:
        print("error: zero frames decoded", file=sys.stderr)
        return 1

    os.makedirs(DATA_DIR, exist_ok=True)

    manifest_files = [None] * frame_count
    preview_indices = {0: "first", frame_count // 2: "mid", frame_count - 1: "last"}
    preview_written = []

    # Lanczos taps depend only on SRC_W/SRC_H/DST_W/DST_H, which never vary
    # across frames -- compute them once here rather than once per frame (the
    # old serial code recomputed them on every process_frame() call via
    # lanczos_upscale()'s internal build_taps() calls).
    h_taps = build_taps(SRC_W, DST_W, a=3)
    v_taps = build_taps(SRC_H, DST_H, a=3)

    jobs = max(1, args.jobs)
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=jobs, initializer=_init_worker, initargs=(palette, h_taps, v_taps)) as executor:
        tasks = (
            (frame_index, indices, not args.no_preview and frame_index in preview_indices)
            for frame_index, indices in enumerate(frames)
        )
        # executor.map() preserves submission order in its result order
        # (regardless of which worker finishes first), so manifest_files
        # ends up in frame_index order 0..frame_count-1 for free.
        for frame_index, out_name, upscaled in executor.map(_process_frame_worker, *zip(*tasks)):
            manifest_files[frame_index] = out_name

            if frame_index % 10 == 0 or frame_index == frame_count - 1:
                print("  frame %d/%d -> %s" % (frame_index + 1, frame_count, out_name))

            if upscaled is not None:
                os.makedirs(PREVIEW_DIR, exist_ok=True)
                label = preview_indices[frame_index]
                preview_path = os.path.join(PREVIEW_DIR, "hdanim_tyrend_%s.png" % label)
                write_png(preview_path, upscaled, DST_W, DST_H)
                preview_written.append(preview_path)

    manifest = {
        "source": "tyrend.anm",
        "scale": SCALE,
        "src_width": SRC_W,
        "src_height": SRC_H,
        "hd_width": DST_W,
        "hd_height": DST_H,
        "frame_count": frame_count,
        "files": manifest_files,
    }
    with open(MANIFEST_PATH, "w") as f:
        json.dump(manifest, f, indent=2)
    print("Wrote %d frames, manifest -> %s" % (frame_count, MANIFEST_PATH))

    for p in preview_written:
        print("Preview -> %s" % p)

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
