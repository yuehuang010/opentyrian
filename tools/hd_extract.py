#!/usr/bin/env python3
"""
hd_extract.py -- Offline HD asset extraction tool for OpenTyrian.

Extracts full-screen backdrops from tyrian.pic (any/all of the PCX_NUM=13
1-based images), colorizes each with its matching palette from palette.dat
(per the pcxpal table in src/pcxmast.c), upscales 4x (320x200 -> 1280x800)
using a separable Lanczos-3 resampler [PLACEHOLDER for a real AI upscaler --
see NOTE below], and writes:

  1. tyrian21/hdpicNN.dat -- a simple raw RGBA asset for the game engine to
     load (magic "HDPX", width, height, then RGBA8888 pixel data), one per
     processed image, NN = 1-based image number zero-padded to 2 digits.
  2. tools/hdpic_previews/hdpicNN.png -- a PNG per processed image so a
     human can visually inspect the upscaled result (skip with
     --no-preview).

For backward compatibility with the current engine build, image #4 (the
title-screen backdrop) is ALSO written to tyrian21/hdtitle.dat, identical
bytes to hdpic04.dat.

NOTE: the Lanczos-3 resample is a PLACEHOLDER for a real AI upscaler; it
just cleanly enlarges the original pixel art without hallucinating detail.

Additionally extracts two static sprite tables from tyrian.shp (see
src/sprite.c load_sprites()/JE_loadMainShapeTables() and doc/files.txt):
PLANET_SHAPES (table 3, 151 frames -- frame 146 is the big Tyrian title
logo) and FACE_SHAPES (table 4, 12 frames). Each frame is decoded from its
RLE-compressed indexed pixels, keyed to RGBA using palette.dat slot 0 (the
main game palette -- these tables are blitted against palettes[0] by the
engine, unlike the per-image pcxpal-indexed backdrops), 4x Lanczos
upscaled (alpha channel included, producing soft edges), and written as
tyrian21/hdplanet_NN.dat / hdface_NN.dat (NN = frame index zero-padded to
2 digits) alongside a tyrian21/hd_sprite_manifest.json manifest.

Standard library only (no Pillow/numpy/ImageMagick). Tested against
python3.9.

Usage:
  hd_extract.py [--pics N[,N...]] [--no-preview]

  --pics N[,N...]  1-based image numbers to process (default: all 1..13).
  --no-preview     skip writing PNG previews (faster).

Format references:
  - src/palette.c JE_loadPals() (palette.dat layout, 6-bit -> 8-bit scaling)
  - src/picload.c JE_loadPic() (tyrian.pic layout, PCX-style RLE decoding)
  - src/pcxmast.c (pcxpal table: which palette index goes with which image)
  - src/sprite.c load_sprites(), JE_loadMainShapeTables(),
    blit_sprite() (tyrian.shp layout and its RLE sprite encoding)
  - src/sprite.h (PLANET_SHAPES / FACE_SHAPES table indices)
"""

import argparse
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
PALETTE_PATH = os.path.join(DATA_DIR, "palette.dat")
PIC_PATH = os.path.join(DATA_DIR, "tyrian.pic")
SHP_PATH = os.path.join(DATA_DIR, "tyrian.shp")
OUT_TITLE_ASSET_PATH = os.path.join(DATA_DIR, "hdtitle.dat")
PREVIEW_DIR = os.path.join(REPO_ROOT, "tools", "hdpic_previews")
SPRITE_MANIFEST_PATH = os.path.join(DATA_DIR, "hd_sprite_manifest.json")

SRC_W, SRC_H = 320, 200
SCALE = 4
DST_W, DST_H = SRC_W * SCALE, SRC_H * SCALE

PIC_NUMBER_1BASED = 4   # legacy title-screen image number (for hdtitle.dat)
PCX_NUM = 13            # number of images in tyrian.pic (see pcxmast.h)

# 0-based index into this list = image number - 1; value = palette index to
# use for that image (see src/pcxmast.c).
PCXPAL = [0, 7, 5, 8, 10, 5, 18, 19, 19, 20, 21, 22, 5]

# tyrian.shp table indices (see src/sprite.h) and the SHP_NUM used by
# JE_loadMainShapeTables() in src/sprite.c for the leading offset table.
SHP_NUM = 12
PLANET_SHAPES = 3
FACE_SHAPES = 4

# Sprite tables to extract as HD overlays: (table index, output prefix).
SPRITE_TABLES = [
    (PLANET_SHAPES, "hdplanet"),
    (FACE_SHAPES, "hdface"),
]


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
# STEP 2b: tyrian.shp -> decoded sprite frames (indexed pixels + alpha mask)
# ---------------------------------------------------------------------------

def load_shp_table_offsets(path, shp_num=SHP_NUM):
    """
    Mirrors JE_loadMainShapeTables(): u16 table count, then shp_num
    little-endian Int32 table offsets, with the file length appended as the
    trailing sentinel (used by other tables in the engine, unused here).
    """
    with open(path, "rb") as f:
        data = f.read()

    shp_numb = struct.unpack_from("<H", data, 0)[0]
    assert shp_numb == shp_num, (
        "tyrian.shp: expected %d table offsets, header says %d" % (shp_num, shp_numb))

    offset = 2
    shp_pos = list(struct.unpack_from("<%di" % shp_num, data, offset))
    shp_pos.append(len(data))
    return data, shp_pos


def load_sprite_table(data, start_offset):
    """
    Mirrors load_sprites() in src/sprite.c: u16 sprite count, then per
    sprite a populated byte, and if populated: u16 width, u16 height,
    u16 size, then `size` bytes of RLE-compressed indexed pixel data.

    Returns a list of length count, each entry either None (unpopulated
    slot) or a dict with width/height/rle (raw compressed bytes).
    """
    count = struct.unpack_from("<H", data, start_offset)[0]
    p = start_offset + 2

    sprites = []
    for _ in range(count):
        populated = data[p]
        p += 1
        if not populated:
            sprites.append(None)
            continue

        width, height, size = struct.unpack_from("<HHH", data, p)
        p += 6
        rle = data[p:p + size]
        p += size
        sprites.append({"width": width, "height": height, "rle": rle})

    return sprites


def decode_sprite_rle(sprite):
    """
    Decode one sprite's RLE pixel data into a flat width*height buffer of
    (palette_index_or_None) -- None marks a transparent pixel -- mirroring
    the pointer arithmetic of blit_sprite() in src/sprite.c exactly (a flat
    "pixels" cursor plus an in-row "x_offset", with no explicit row
    variable; pitch == width since we have no surface stride padding):
      255       -> next byte is a count of transparent pixels
      254       -> advance the cursor to the start of the next row
      253       -> a single transparent pixel
      0..252    -> a direct opaque palette index
    After every op, if x_offset has reached (or passed) width, the row is
    considered finished and x_offset resets to 0 (matching the unconditional
    post-switch wrap check in blit_sprite()).
    """
    width, height = sprite["width"], sprite["height"]
    rle = sprite["rle"]
    total = width * height

    out = [None] * total
    pos = 0        # flat cursor into out[], mirrors the `pixels` pointer
    x_offset = 0
    i = 0
    n = len(rle)

    def write(idx, value):
        if 0 <= idx < total:
            out[idx] = value

    while i < n:
        b = rle[i]
        if b == 255:
            run = rle[i + 1]
            i += 2
            # Transparent run: out[] entries are already None, just advance.
            pos += run
            x_offset += run
        elif b == 254:
            pos += width - x_offset
            x_offset = width
            i += 1
        elif b == 253:
            write(pos, None)
            pos += 1
            x_offset += 1
            i += 1
        else:
            write(pos, b)
            pos += 1
            x_offset += 1
            i += 1

        if x_offset >= width:
            pos += width - x_offset
            x_offset = 0

    return out, width, height


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


def colorize_rgba(indices, palette):
    """
    Like colorize(), but for a sequence that may contain None entries
    (transparent pixels, per decode_sprite_rle()): produces an RGBA buffer
    (4 bytes/pixel) with alpha 0 for transparent pixels and 255 for opaque
    ones, so it can be Lanczos-upscaled as a single 4-channel image.
    """
    rgba = bytearray(len(indices) * 4)
    for idx, pixel_index in enumerate(indices):
        o = idx * 4
        if pixel_index is None:
            rgba[o] = 0
            rgba[o + 1] = 0
            rgba[o + 2] = 0
            rgba[o + 3] = 0
        else:
            r, g, b = palette[pixel_index]
            rgba[o] = r
            rgba[o + 1] = g
            rgba[o + 2] = b
            rgba[o + 3] = 255
    return rgba


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


def resample_axis_horizontal(src, src_w, src_h, dst_w, taps, channels=3):
    """Resample width src_w -> dst_w for an interleaved pixel buffer."""
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
    """Resample height src_h -> dst_h for an interleaved pixel buffer."""
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


def clamp_byte(v):
    iv = int(round(v))
    if iv < 0:
        return 0
    if iv > 255:
        return 255
    return iv


def lanczos_upscale(rgb, src_w, src_h, dst_w, dst_h, a=3, channels=3):
    h_taps = build_taps(src_w, dst_w, a)
    v_taps = build_taps(src_h, dst_h, a)

    # Horizontal pass first (src_w -> dst_w), producing dst_w x src_h.
    stage1 = resample_axis_horizontal(rgb, src_w, src_h, dst_w, h_taps, channels)
    # Vertical pass second (src_h -> dst_h), producing dst_w x dst_h.
    stage2 = resample_axis_vertical(stage1, dst_w, src_h, dst_h, v_taps, channels)
    return stage2


# ---------------------------------------------------------------------------
# STEP 5a: write the engine asset (HDPX raw RGBA format)
# ---------------------------------------------------------------------------

def write_hdpx_asset(path, pixels, width, height, channels=3):
    """
    Write the HDPX raw-asset format: ASCII "HDPX", u32 LE width, u32 LE
    height, then width*height RGBA8888 pixels.

    `pixels` holds `channels` bytes/pixel already (3 = RGB, opaque assumed;
    4 = RGBA, alpha taken as-is -- e.g. a Lanczos-upscaled sprite overlay).
    """
    with open(path, "wb") as f:
        f.write(b"HDPX")
        f.write(struct.pack("<I", width))
        f.write(struct.pack("<I", height))
        if channels == 4:
            f.write(bytes(pixels))
        else:
            # Build RGBA in bulk: interleave triplets with a constant alpha.
            out = bytearray(width * height * 4)
            for i in range(width * height):
                so = i * 3
                do = i * 4
                out[do] = pixels[so]
                out[do + 1] = pixels[so + 1]
                out[do + 2] = pixels[so + 2]
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

def process_pic(n, palette_cache, want_preview):
    """
    Process one 1-based image number n: decode, colorize, upscale, and write
    the engine asset (plus optional PNG preview and, for image #4, the
    legacy hdtitle.dat). Returns True on success, False on failure (errors
    are printed but not raised, so callers can continue with other images).
    """
    try:
        pal_index = PCXPAL[n - 1]

        out_asset_path = os.path.join(DATA_DIR, "hdpic%02d.dat" % n)
        print("Image %d/%d: palette %d -> %s ..." % (n, PCX_NUM, pal_index, out_asset_path))

        palette = palette_cache.get(pal_index)
        if palette is None:
            palette = load_palette(PALETTE_PATH, pal_index)
            palette_cache[pal_index] = palette

        indices = load_pic_indices(PIC_PATH, n)
        rgb = colorize(indices, palette)
        upscaled = lanczos_upscale(rgb, SRC_W, SRC_H, DST_W, DST_H, a=3)

        os.makedirs(DATA_DIR, exist_ok=True)
        write_hdpx_asset(out_asset_path, upscaled, DST_W, DST_H)

        if n == PIC_NUMBER_1BASED:
            write_hdpx_asset(OUT_TITLE_ASSET_PATH, upscaled, DST_W, DST_H)
            print("  also wrote legacy asset %s" % OUT_TITLE_ASSET_PATH)

        if want_preview:
            os.makedirs(PREVIEW_DIR, exist_ok=True)
            preview_path = os.path.join(PREVIEW_DIR, "hdpic%02d.png" % n)
            write_png(preview_path, upscaled, DST_W, DST_H)

        return True
    except Exception as e:
        print("error: failed to process image %d: %s" % (n, e), file=sys.stderr)
        return False


def process_sprite_tables(palette, manifest_frames):
    """
    Extract PLANET_SHAPES and FACE_SHAPES (SPRITE_TABLES) from tyrian.shp:
    decode each populated frame's RLE pixels, key transparency to alpha,
    Lanczos-upscale 4x (RGBA), and write one hdplanet_NN.dat / hdface_NN.dat
    HDPX-with-alpha asset per frame. Appends a manifest entry per frame to
    manifest_frames (in place). Returns the number of frames written.
    """
    if not os.path.isfile(SHP_PATH):
        print("error: tyrian.shp not found at %s" % SHP_PATH, file=sys.stderr)
        return 0

    data, shp_pos = load_shp_table_offsets(SHP_PATH)

    written = 0
    for table_index, prefix in SPRITE_TABLES:
        sprites = load_sprite_table(data, shp_pos[table_index])
        print("Table %d (%s): %d frames -> %s_NN.dat ..." %
              (table_index, prefix, len(sprites), prefix))

        for frame_index, sprite in enumerate(sprites):
            if sprite is None:
                continue

            indices, src_w, src_h = decode_sprite_rle(sprite)
            rgba = colorize_rgba(indices, palette)
            upscaled = lanczos_upscale(
                rgba, src_w, src_h, src_w * SCALE, src_h * SCALE, a=3, channels=4)

            out_name = "%s_%02d.dat" % (prefix, frame_index)
            out_path = os.path.join(DATA_DIR, out_name)
            write_hdpx_asset(out_path, upscaled, src_w * SCALE, src_h * SCALE, channels=4)

            manifest_frames.append({
                "table": "PLANET_SHAPES" if table_index == PLANET_SHAPES else "FACE_SHAPES",
                "frame_index": frame_index,
                "file": out_name,
                "src_width": src_w,
                "src_height": src_h,
                "hd_width": src_w * SCALE,
                "hd_height": src_h * SCALE,
            })
            written += 1

    return written


def parse_pics_arg(value):
    result = []
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        num = int(part)
        if not (1 <= num <= PCX_NUM):
            raise argparse.ArgumentTypeError(
                "image number %d out of range (1..%d)" % (num, PCX_NUM))
        result.append(num)
    if not result:
        raise argparse.ArgumentTypeError("no image numbers given")
    return result


def main():
    parser = argparse.ArgumentParser(description="Extract HD assets from Tyrian data files.")
    parser.add_argument(
        "--pics", type=parse_pics_arg, default=list(range(1, PCX_NUM + 1)),
        help="comma-separated 1-based image numbers to process (default: all 1..%d)" % PCX_NUM)
    parser.add_argument(
        "--no-preview", action="store_true",
        help="skip writing PNG previews (faster)")
    args = parser.parse_args()

    if not os.path.isfile(PALETTE_PATH):
        print("error: palette.dat not found at %s" % PALETTE_PATH, file=sys.stderr)
        return 1
    if not os.path.isfile(PIC_PATH):
        print("error: tyrian.pic not found at %s" % PIC_PATH, file=sys.stderr)
        return 1

    palette_cache = {}
    failed = []
    for n in args.pics:
        ok = process_pic(n, palette_cache, want_preview=not args.no_preview)
        if not ok:
            failed.append(n)

    os.makedirs(DATA_DIR, exist_ok=True)
    main_palette = load_palette(PALETTE_PATH, 0)
    manifest_frames = []
    sprite_count = process_sprite_tables(main_palette, manifest_frames)
    if sprite_count:
        with open(SPRITE_MANIFEST_PATH, "w") as f:
            json.dump({"scale": SCALE, "frames": manifest_frames}, f, indent=2)
        print("Wrote %d sprite frames, manifest -> %s" % (sprite_count, SPRITE_MANIFEST_PATH))

    if failed:
        print("Done with errors. Failed images: %s" % ", ".join(str(n) for n in failed),
              file=sys.stderr)
        return 1

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
