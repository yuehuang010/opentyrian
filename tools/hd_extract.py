#!/usr/bin/env python3
"""
hd_extract.py -- Offline HD asset extraction tool for OpenTyrian.

Extracts the Tyrian title-screen backdrop (image #4 in tyrian.pic, using
palette #8 from palette.dat), upscales it 4x (320x200 -> 1280x800) using a
separable Lanczos-3 resampler, and writes:

  1. tyrian21/hdtitle.dat  -- a simple raw RGBA asset for the game engine
     to load (magic "HDPX", width, height, then RGBA8888 pixel data).
  2. tools/title_preview.png -- a PNG so a human can visually inspect the
     upscaled result.

Standard library only (no Pillow/numpy/ImageMagick). Tested against
python3.9.

Format references:
  - src/palette.c JE_loadPals() (palette.dat layout, 6-bit -> 8-bit scaling)
  - src/picload.c JE_loadPic() (tyrian.pic layout, PCX-style RLE decoding)
"""

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
PALETTE_PATH = os.path.join(DATA_DIR, "palette.dat")
PIC_PATH = os.path.join(DATA_DIR, "tyrian.pic")
OUT_ASSET_PATH = os.path.join(DATA_DIR, "hdtitle.dat")
OUT_PREVIEW_PATH = os.path.join(REPO_ROOT, "tools", "title_preview.png")

SRC_W, SRC_H = 320, 200
SCALE = 4
DST_W, DST_H = SRC_W * SCALE, SRC_H * SCALE

PALETTE_INDEX = 8       # 0-based palette index to use
PIC_NUMBER_1BASED = 4   # 1-based image number (matches JE_loadPic's PCXnumber)
PCX_NUM = 13            # number of images in tyrian.pic (see pcxmast.h)


# ---------------------------------------------------------------------------
# STEP 1: palette.dat -> palette8[256] of (r, g, b) 8-bit tuples
# ---------------------------------------------------------------------------

def load_palette(path, palette_index):
    with open(path, "rb") as f:
        f.seek(palette_index * 256 * 3)
        raw = f.read(256 * 3)
    if len(raw) != 256 * 3:
        raise ValueError(
            "palette.dat: expected %d bytes for palette %d, got %d"
            % (256 * 3, palette_index, len(raw))
        )

    def expand6to8(v6):
        # Same trick as JE_loadPals: (v << 2) | (v >> 4), using the top 2
        # bits of the original 6-bit value to fill in the low 2 bits so the
        # max value maps to 255 instead of 252.
        return (v6 << 2) | (v6 >> 4)

    palette = []
    for i in range(256):
        r6, g6, b6 = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
        palette.append((expand6to8(r6), expand6to8(g6), expand6to8(b6)))
    return palette


# ---------------------------------------------------------------------------
# STEP 2: tyrian.pic -> decoded 320x200 index buffer for image #4
# ---------------------------------------------------------------------------

def load_pic_indices(path, pic_number_1based, pcx_num=PCX_NUM):
    pcx_index = pic_number_1based - 1  # mirrors PCXnumber-- in JE_loadPic

    with open(path, "rb") as f:
        data = f.read()

    # bytes 0-1: Uint16 LE, skipped
    offset = 2

    # PCX_NUM signed Int32 LE offsets
    pcxpos = list(struct.unpack_from("<%di" % pcx_num, data, offset))
    offset += 4 * pcx_num

    filesize = len(data)
    pcxpos.append(filesize)  # pcxpos[PCX_NUM] = ftell_eof(f)

    if not (0 <= pcx_index < pcx_num):
        raise ValueError("pic_number out of range: %d" % pic_number_1based)

    start = pcxpos[pcx_index]
    end = pcxpos[pcx_index + 1]
    if start < 0 or end < start or end > filesize:
        raise ValueError(
            "invalid pcxpos range for image %d: start=%d end=%d filesize=%d"
            % (pic_number_1based, start, end, filesize)
        )

    chunk = data[start:end]

    # PCX-style RLE decode, per picload.c:60-78, into a flat 320*200 buffer
    # (no row-stride padding, so it's a straightforward linear append).
    out = bytearray(SRC_W * SRC_H)
    i = 0    # pixels written
    p = 0    # read cursor into chunk
    n = len(chunk)
    while i < SRC_W * SRC_H:
        if p >= n:
            raise ValueError("unexpected end of compressed data for image %d" % pic_number_1based)
        b = chunk[p]
        if (b & 0xC0) == 0xC0:
            count = b & 0x3F
            color = chunk[p + 1]
            p += 2
            # Clamp writes to the buffer bound, matching the effect of the
            # engine's row-based screen pitch handling for our flat buffer.
            remaining = SRC_W * SRC_H - i
            take = min(count, remaining)
            out[i:i + take] = bytes([color]) * take
            i += count
        else:
            out[i] = b
            i += 1
            p += 1

    return bytes(out)


# ---------------------------------------------------------------------------
# STEP 3: colorize indices -> flat RGB bytearray (row-major, 3 bytes/pixel)
# ---------------------------------------------------------------------------

def colorize(indices, palette):
    rgb = bytearray(len(indices) * 3)
    for idx, pixel_index in enumerate(indices):
        r, g, b = palette[pixel_index]
        o = idx * 3
        rgb[o] = r
        rgb[o + 1] = g
        rgb[o + 2] = b
    return rgb


# ---------------------------------------------------------------------------
# STEP 4: separable Lanczos-3 upscaling
# ---------------------------------------------------------------------------

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
        # Center of destination pixel maps back into source space.
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
            # Degenerate fallback (shouldn't happen for a>=1): nearest sample.
            nearest = min(max(int(round(src_center)), 0), src_n - 1)
            weighted = [(nearest, 1.0)]
            total = 1.0

        # Normalize so weights sum to 1.
        weighted = [(idx, w / total) for idx, w in weighted]
        taps.append(weighted)
    return taps


def resample_axis_horizontal(src, src_w, src_h, dst_w, taps):
    """Resample width src_w -> dst_w for an RGB buffer (3 bytes/pixel)."""
    dst = bytearray(dst_w * src_h * 3)
    for y in range(src_h):
        row_off = y * src_w * 3
        out_off = y * dst_w * 3
        for x in range(dst_w):
            r_acc = g_acc = b_acc = 0.0
            for src_x, w in taps[x]:
                o = row_off + src_x * 3
                r_acc += src[o] * w
                g_acc += src[o + 1] * w
                b_acc += src[o + 2] * w
            o2 = out_off + x * 3
            dst[o2] = clamp_byte(r_acc)
            dst[o2 + 1] = clamp_byte(g_acc)
            dst[o2 + 2] = clamp_byte(b_acc)
    return dst


def resample_axis_vertical(src, src_w, src_h, dst_h, taps):
    """Resample height src_h -> dst_h for an RGB buffer (3 bytes/pixel)."""
    dst = bytearray(src_w * dst_h * 3)
    # Precompute column base offsets once.
    for y in range(dst_h):
        out_row_off = y * src_w * 3
        col_taps = taps[y]
        for x in range(src_w):
            r_acc = g_acc = b_acc = 0.0
            xo = x * 3
            for src_y, w in col_taps:
                o = src_y * src_w * 3 + xo
                r_acc += src[o] * w
                g_acc += src[o + 1] * w
                b_acc += src[o + 2] * w
            o2 = out_row_off + xo
            dst[o2] = clamp_byte(r_acc)
            dst[o2 + 1] = clamp_byte(g_acc)
            dst[o2 + 2] = clamp_byte(b_acc)
    return dst


def clamp_byte(v):
    iv = int(round(v))
    if iv < 0:
        return 0
    if iv > 255:
        return 255
    return iv


def lanczos_upscale(rgb, src_w, src_h, dst_w, dst_h, a=3):
    h_taps = build_taps(src_w, dst_w, a)
    v_taps = build_taps(src_h, dst_h, a)

    # Horizontal pass first (src_w -> dst_w), producing dst_w x src_h.
    stage1 = resample_axis_horizontal(rgb, src_w, src_h, dst_w, h_taps)
    # Vertical pass second (src_h -> dst_h), producing dst_w x dst_h.
    stage2 = resample_axis_vertical(stage1, dst_w, src_h, dst_h, v_taps)
    return stage2


# ---------------------------------------------------------------------------
# STEP 5a: write the engine asset (HDPX raw RGBA format)
# ---------------------------------------------------------------------------

def write_hdpx_asset(path, rgb, width, height):
    with open(path, "wb") as f:
        f.write(b"HDPX")
        f.write(struct.pack("<I", width))
        f.write(struct.pack("<I", height))
        # Build RGBA in bulk: interleave rgb triplets with a constant alpha.
        out = bytearray(width * height * 4)
        for i in range(width * height):
            so = i * 3
            do = i * 4
            out[do] = rgb[so]
            out[do + 1] = rgb[so + 1]
            out[do + 2] = rgb[so + 2]
            out[do + 3] = 255
        f.write(out)


# ---------------------------------------------------------------------------
# STEP 5b: minimal stdlib-only PNG writer (8-bit RGB, color type 2)
# ---------------------------------------------------------------------------

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

    # Raw scanlines, each prefixed with filter-type byte 0 (None).
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


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    if not os.path.isfile(PALETTE_PATH):
        print("error: palette.dat not found at %s" % PALETTE_PATH, file=sys.stderr)
        return 1
    if not os.path.isfile(PIC_PATH):
        print("error: tyrian.pic not found at %s" % PIC_PATH, file=sys.stderr)
        return 1

    print("Loading palette #%d from %s ..." % (PALETTE_INDEX, PALETTE_PATH))
    palette = load_palette(PALETTE_PATH, PALETTE_INDEX)

    print("Decoding image #%d from %s ..." % (PIC_NUMBER_1BASED, PIC_PATH))
    indices = load_pic_indices(PIC_PATH, PIC_NUMBER_1BASED)

    print("Colorizing %dx%d indexed image ..." % (SRC_W, SRC_H))
    rgb = colorize(indices, palette)

    print("Upscaling %dx%d -> %dx%d with separable Lanczos-3 (this may take a bit) ..."
          % (SRC_W, SRC_H, DST_W, DST_H))
    upscaled = lanczos_upscale(rgb, SRC_W, SRC_H, DST_W, DST_H, a=3)

    os.makedirs(os.path.dirname(OUT_ASSET_PATH), exist_ok=True)
    os.makedirs(os.path.dirname(OUT_PREVIEW_PATH), exist_ok=True)

    print("Writing engine asset to %s ..." % OUT_ASSET_PATH)
    write_hdpx_asset(OUT_ASSET_PATH, upscaled, DST_W, DST_H)

    print("Writing PNG preview to %s ..." % OUT_PREVIEW_PATH)
    write_png(OUT_PREVIEW_PATH, upscaled, DST_W, DST_H)

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
