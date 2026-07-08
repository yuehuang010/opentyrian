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

Additionally extracts four static sprite tables from tyrian.shp (see
src/sprite.c load_sprites()/JE_loadMainShapeTables() and doc/files.txt):
PLANET_SHAPES (table 3, 151 frames -- frame 146 is the big Tyrian title
logo), FACE_SHAPES (table 4, 12 frames), OPTION_SHAPES (table 5, 45
frames -- menu/HUD/help icons), and WEAPON_SHAPES (table 6, 22 frames --
weapon icons). It also extracts EXTRA_SHAPES (9 frames), which is loaded
from the *separate* file estsc.shp, not tyrian.shp (see
src/mainint.c JE_playCredits() / load_sprites_file(EXTRA_SHAPES,
"estsc.shp")) -- table index 7 inside tyrian.shp itself is unrelated and
is not used for EXTRA_SHAPES by the engine.

Each frame is decoded from its RLE-compressed indexed pixels, keyed to
RGBA using a palette.dat slot, 4x xBRZ upscaled (edge-directed pixel-art
scaling; alpha channel included as part of the edge-detection so
transparency gets smooth anti-aliased cutout edges rather than
blocky/blurry ones), and written as tyrian21/hd<prefix>_NN.dat (NN = frame
index zero-padded to 2 digits) alongside a tyrian21/hd_sprite_manifest.json
manifest. Palette choice per table:
  - PLANET_SHAPES: palettes[8] (the title-screen palette; see
    SPRITE_TABLE_PAL_TITLE below).
  - FACE_SHAPES: NOT a single fixed palette -- the engine recolors each
    portrait per-frame via facepal[face_sprite] (src/pcxmast.c, wired up
    in src/game_menu.c around the MENU_DATA_CUBES case), so each frame is
    baked with its own palettes[facepal[frame_index]] slot (see FACEPAL
    below, mirroring src/pcxmast.c's `facepal` table exactly).
  - OPTION_SHAPES / WEAPON_SHAPES: these are blitted across many screens
    (shop/menu screens in game_menu.c, in-flight HUD icons in mainint.c/
    shots.c/varz.c) under whatever palette happens to be active there, so
    there's no single universally-correct answer. Defaulting to
    palette.dat slot 0, which is at least the palette active on the shop's
    own background screen (JE_itemScreen() loads pic 1 -> PCXPAL[0] == 0)
    -- flagged here for review, not a confident answer.

Also extracts 8 large full-screen 320x200 8bpp PCX images that are
*not* part of tyrian.pic (tshp2.pcx, shipedit.pcx, tyrian.pcx-adjacent
tyrset.pcx, and the netXXX.pcx multiplayer-tool screens). Unlike
tyrian.pic's images (which carry no embedded palette and are colorized
via the pcxpal-indexed palette.dat table), every one of these files ships
its own standard-PCX 769-byte VGA palette trailer (marker byte 0x0C + 768
raw 8-bit RGB bytes -- see src/pcxload.c JE_loadPCX(), the only one of
these actually loaded by the engine, which reads that trailer into
`colors` directly with no 6-bit->8-bit expansion, unlike palette.dat).
So each is decoded with its own embedded palette, not a palette.dat slot.
These are photographic/dithered full-screen art like the tyrian.pic
backdrops, so they use the same Lanczos 4x RGB (opaque) path and are
written as tyrian21/hdpcx_<name>.dat.

The full-screen PCX/PIC backdrops are photographic-ish dithered art, so
they intentionally stay on the Lanczos path -- xBRZ is a pixel-art scaler
and would misbehave on them. Everything sprite-shaped (icons, portraits,
logos) goes through xBRZ instead.

Standard library only (no Pillow/numpy/ImageMagick). Tested against
python3.9.

Usage:
  hd_extract.py [--pics N[,N...]] [--no-preview]

  --pics N[,N...]  1-based image numbers to process (default: all 1..13).
  --no-preview     skip writing PNG previews (faster).

Format references:
  - src/palette.c JE_loadPals() (palette.dat layout, 6-bit -> 8-bit scaling)
  - src/picload.c JE_loadPic() (tyrian.pic layout, PCX-style RLE decoding)
  - src/pcxmast.c (pcxpal table: which palette index goes with which image;
    facepal table: which palette index goes with which FACE_SHAPES frame)
  - src/pcxload.c JE_loadPCX() (standalone .pcx layout: 128-byte header,
    RLE body, trailing 769-byte VGA palette)
  - src/sprite.c load_sprites(), JE_loadMainShapeTables(),
    blit_sprite() (tyrian.shp layout and its RLE sprite encoding)
  - src/sprite.h (PLANET_SHAPES / FACE_SHAPES / OPTION_SHAPES /
    WEAPON_SHAPES / EXTRA_SHAPES table indices)
  - src/mainint.c JE_playCredits() (estsc.shp / EXTRA_SHAPES loading and
    its palette)
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
EXTRA_SHP_PATH = os.path.join(DATA_DIR, "estsc.shp")
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
OPTION_SHAPES = 5   # menu/HUD/help icons (src/sprite.h: "Also contains help shapes")
WEAPON_SHAPES = 6   # weapon icons

# facepal[12] (src/pcxmast.c): the palette.dat slot the engine recolors each
# FACE_SHAPES portrait with (see the MENU_DATA_CUBES case in game_menu.c,
# `temp2 = facepal[face_sprite]; ... colors[temp] = palettes[temp2][temp]`).
# These are used as direct 0-based palette.dat slot indices, same as PCXPAL.
FACEPAL = [1, 2, 3, 4, 6, 9, 11, 12, 16, 13, 14, 15]

# Sprite tables to extract as HD overlays: (table index, output prefix,
# manifest table name, palette-or-None). When palette is None, the table is
# recolored per-frame instead of with one fixed palette (see FACEPAL, used
# for FACE_SHAPES).
#
# The palette is the one *active on screen where the sprite is drawn*, not
# palette 0: the engine blits these against whatever palette the current
# screen loaded. The title logo (PLANET_SHAPES frame 146) is drawn on the
# title screen, which loads pic 4 -> palettes[PCXPAL[4-1]] = palettes[8]
# (gold); baking it with palette 0 turns it silver. FACE_SHAPES portraits are
# recolored per-face via facepal[] at draw time (game_menu.c) -- see FACEPAL
# above. OPTION_SHAPES/WEAPON_SHAPES icons are blitted across many different
# screens (shop menus in game_menu.c, in-flight HUD in mainint.c/shots.c/
# varz.c) each with a different active palette, so there is no single
# correct answer; palette 0 is used as a documented default (see the module
# docstring) -- NOTE: flagged for review, not a confident answer.
SPRITE_TABLE_PAL_TITLE = PCXPAL[4 - 1]  # palettes[8], the title-screen palette
SPRITE_TABLES = [
    (PLANET_SHAPES, "hdplanet", "PLANET_SHAPES", SPRITE_TABLE_PAL_TITLE),
    (FACE_SHAPES, "hdface", "FACE_SHAPES", None),
    (OPTION_SHAPES, "hdoption", "OPTION_SHAPES", 0),
    (WEAPON_SHAPES, "hdweapon", "WEAPON_SHAPES", 0),
]

# EXTRA_SHAPES (src/sprite.h table index 7, "Used for Ending pics") is loaded
# by the engine from a *separate* file, estsc.shp, not tyrian.shp (see
# src/mainint.c JE_playCredits(): load_sprites_file(EXTRA_SHAPES,
# "estsc.shp")) -- tyrian.shp's own table index 7 is unrelated. It's blitted
# during the end-of-game credits scroll against `memcpy(colors,
# palettes[6-1], ...)`, i.e. palette.dat slot 5 (0-based) -- the same slot as
# PCXPAL[2] (image 3).
EXTRA_SHAPES_PAL = 5  # palettes[6-1], per JE_playCredits() in src/mainint.c


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
# STEP 2a: standalone full-screen .pcx files (own embedded VGA palette)
#
# These are 320x200 8bpp single-plane PCX files that are NOT part of
# tyrian.pic and are not colorized via the pcxpal/palette.dat mechanism.
# tshp2.pcx is the only one actually loaded by the current engine
# (src/pcxload.c JE_loadPCX(), called from src/tyrian2.c for the 'P0'
# script command); the rest (shipedit.pcx, tyrset.pcx, netXXX.pcx) belong
# to the original DOS-era companion tools (shipedit.exe, netarena.exe,
# etc.) and were never wired into OpenTyrian, but are ordinary PCX files
# with the same 128-byte-header + RLE-body + 769-byte-VGA-palette-trailer
# layout, so the same decode applies.
# ---------------------------------------------------------------------------

# (source filename, output prefix) -- output asset is hdpcx_<prefix>.dat.
STANDALONE_PCX = [
    ("tshp2.pcx", "tshp2"),
    ("shipedit.pcx", "shipedit"),
    ("tyrset.pcx", "tyrset"),
    ("netarena.pcx", "netarena"),
    ("netset.pcx", "netset"),
    ("netmega.pcx", "netmega"),
    ("netfont1.pcx", "netfont1"),
    ("netfont2.pcx", "netfont2"),
]


def load_standalone_pcx(path):
    """
    Decode a standalone 8bpp single-plane PCX file: the 128-byte header (for
    width/height), the PCX-RLE pixel body (same 0xC0-run-length scheme as
    load_pic_indices()/JE_loadPCX()), and the trailing 769-byte VGA palette
    (marker byte 0x0C followed by 768 raw 8-bit RGB triples -- no 6-bit ->
    8-bit expansion, matching JE_loadPCX()'s direct `colors[i].r = rgb[0]`).

    Returns (indices: bytes, palette: list of (r,g,b), width, height).
    """
    with open(path, "rb") as f:
        data = f.read()

    header = data[:128]
    if len(header) < 128:
        raise ValueError("%s: file too short for a PCX header" % path)

    manufacturer = header[0]
    bpp = header[3]
    xmin, ymin, xmax, ymax = struct.unpack_from("<HHHH", header, 4)
    nplanes = header[65]
    width = xmax - xmin + 1
    height = ymax - ymin + 1

    if manufacturer != 10:
        raise ValueError("%s: not a PCX file (manufacturer=%d)" % (path, manufacturer))
    if bpp != 8 or nplanes != 1:
        raise ValueError(
            "%s: unsupported PCX format (bpp=%d nplanes=%d), expected 8bpp/1-plane"
            % (path, bpp, nplanes))
    if width <= 0 or height <= 0:
        raise ValueError("%s: invalid PCX dimensions %dx%d" % (path, width, height))

    if len(data) < 128 + 769:
        raise ValueError("%s: file too short to hold a VGA palette trailer" % path)

    marker = data[-769]
    if marker != 0x0C:
        raise ValueError(
            "%s: missing 769-byte VGA palette trailer (marker byte=%d, expected 12)"
            % (path, marker))

    pal_raw = data[-768:]
    palette = [(pal_raw[i * 3], pal_raw[i * 3 + 1], pal_raw[i * 3 + 2]) for i in range(256)]

    body_end = len(data) - 769
    chunk = data[128:body_end]

    total = width * height
    out = bytearray(total)
    i = 0    # pixels written
    p = 0    # read cursor into chunk
    n = len(chunk)
    while i < total:
        if p >= n:
            raise ValueError("%s: unexpected end of compressed pixel data" % path)
        b = chunk[p]
        if (b & 0xC0) == 0xC0:
            count = b & 0x3F
            color = chunk[p + 1]
            p += 2
            remaining = total - i
            take = min(count, remaining)
            out[i:i + take] = bytes([color]) * take
            i += count
        else:
            out[i] = b
            i += 1
            p += 1

    return bytes(out), palette, width, height


def process_standalone_pcx(filename, prefix, want_preview):
    """
    Process one standalone full-screen PCX (see STANDALONE_PCX): decode with
    its own embedded palette, Lanczos-upscale 4x (photographic/dithered art,
    same reasoning as the tyrian.pic backdrops), and write
    tyrian21/hdpcx_<prefix>.dat (+ optional PNG preview). Returns True on
    success. If the source file is missing, prints a notice and returns
    False without raising, so the caller can continue with other assets.
    """
    src_path = os.path.join(DATA_DIR, filename)
    if not os.path.isfile(src_path):
        print("skip: %s not found at %s (not present in this data dir)" % (filename, src_path),
              file=sys.stderr)
        return False

    try:
        out_path = os.path.join(DATA_DIR, "hdpcx_%s.dat" % prefix)
        print("PCX %s: own embedded palette -> %s ..." % (filename, out_path))

        indices, palette, src_w, src_h = load_standalone_pcx(src_path)
        rgb = colorize(indices, palette)
        dst_w, dst_h = src_w * SCALE, src_h * SCALE
        upscaled = lanczos_upscale(rgb, src_w, src_h, dst_w, dst_h, a=3)

        os.makedirs(DATA_DIR, exist_ok=True)
        write_hdpx_asset(out_path, upscaled, dst_w, dst_h)

        if want_preview:
            os.makedirs(PREVIEW_DIR, exist_ok=True)
            preview_path = os.path.join(PREVIEW_DIR, "hdpcx_%s.png" % prefix)
            write_png(preview_path, upscaled, dst_w, dst_h)

        return True
    except Exception as e:
        print("error: failed to process %s: %s" % (filename, e), file=sys.stderr)
        return False


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
# STEP 4b: xBRZ 4x edge-directed upscaling (SPRITE path only)
#
# A pure-Python, stdlib-only port of Zenju's xBRZ ("edge-directed" pixel-art
# scaler; https://sourceforge.net/projects/xbrz), ARGB color format, fixed
# to scale factor 4 (all this tool needs). Ported from xbrz.cpp / xbrz.h /
# config.h (GPLv3, Copyright (C) Zenju), specifically:
#   - distYCbCr()/ColorDistanceARGB::dist() -> _dist_argb(): the ITU-R
#     BT.2020-weighted YCbCr color-distance metric, alpha-aware per
#     ColorDistanceARGB (distance is scaled by the *lesser* of the two
#     pixels' alpha, plus a term proportional to |a1 - a2| * 255 -- so a
#     transparent/opaque boundary always registers as a strong "edge",
#     which is exactly what gives the logo a clean anti-aliased cutout
#     instead of a blurred or hard-stepped one).
#   - gradientARGB()/ColorGradientARGB::alphaGrad() -> _gradient_argb():
#     the alpha-weighted color+alpha blend used for every softened pixel.
#     Because each source color is weighted by *its own* alpha before
#     blending, a fully-transparent neighbor (garbage RGB or not) never
#     contributes any color -- only coverage -- so there is no dark/color
#     halo at cutout edges.
#   - preProcessCorners() -> _preprocess_corners(): the per-corner
#     "which diagonal is the real edge" detection over a 4x4-pixel
#     neighborhood (the "5x5-ish" neighborhood the corner tests reach
#     into), producing BLEND_NONE/BLEND_NORMAL/BLEND_DOMINANT per corner.
#   - blendPixel() -> _blend_pixel_rot(): the 3x3-neighborhood, per-corner
#     rule set (shallow/steep/steep+shallow/diagonal line blends, or a
#     rounded corner blend) evaluated once per 90-degree rotation of the
#     kernel (4 rotations cover all four corners of the output block).
#   - Scaler4x<ColorGradientARGB>'s blendLineShallow/Steep/
#     SteepAndShallow/Diagonal/Corner -> _blend_line_*()/_blend_corner():
#     the exact scale-4 sub-pixel weight tables (as M/N alpha-blend
#     fractions) that shape the rounded/diagonal edges.
#   - The OutputMatrix<N, rotDeg> compile-time rotation is reproduced by
#     _out_index(), a closed-form solution of the same recursive
#     definition (verified against all 4 rotations transcribed from the
#     DEF_GETTER macros in xbrz.cpp).
#
# Deviations from upstream (all deliberate, and all preserve the visual
# algorithm -- nothing here changes *which* pixels get blended or by how
# much):
#   - Only ColorFormat::ARGB at scale factor 4 is implemented (2x/3x/5x/6x
#     and the plain-RGB scaler are omitted; this tool never needs them).
#   - distYCbCr()'s "lumaWeight" parameter is fixed at 1.0. Upstream's
#     ColorDistanceARGB::dist() always routes through DistYCbCrBuffer
#     (a precomputed 256^3 LUT keyed only on the raw R/G/B diffs), which
#     structurally ignores cfg.luminanceWeight -- and the library's own
#     default ScalerCfg::luminanceWeight is 1.0 anyway, so this matches
#     upstream's actual runtime behavior exactly, not just its defaults.
#   - No 256^3 precomputed distance LUT (DistYCbCrBuffer): a 64 MB cache
#     buys ~30% perf in C++ but is pure overhead to build in Python for a
#     few hundred small sprite frames, so distances are computed directly.
#   - No multithreaded row-striping (yFirst/yLast slicing) -- processes
#     the whole image in one pass; irrelevant to the output.
#   - Guard against transparent-pixel RGB bleed: source pixels with
#     alpha == 0 have their R/G/B forced to 0 before scaling starts, so
#     any stray/garbage color baked into a fully-transparent source texel
#     can never leak into a blend (on top of the alpha-weighting in
#     _gradient_argb() already suppressing it structurally).
# ---------------------------------------------------------------------------

# ScalerCfg defaults, from config.h (Copyright (C) Zenju):
_XBRZ_EQUAL_COLOR_TOLERANCE = 30.0
_XBRZ_DOMINANT_DIRECTION_THRESHOLD = 3.6
_XBRZ_STEEP_DIRECTION_THRESHOLD = 2.2

# ITU-R BT.2020 YCbCr conversion weights (distYCbCr() in xbrz.cpp).
_XBRZ_K_B = 0.0593
_XBRZ_K_R = 0.2627
_XBRZ_K_G = 1.0 - _XBRZ_K_B - _XBRZ_K_R
_XBRZ_SCALE_B = 0.5 / (1.0 - _XBRZ_K_B)
_XBRZ_SCALE_R = 0.5 / (1.0 - _XBRZ_K_R)

_BLEND_NONE = 0
_BLEND_NORMAL = 1
_BLEND_DOMINANT = 2


def _dist_argb(p1, p2):
    """
    Port of ColorDistanceARGB::dist() (xbrz.cpp): an alpha-aware YCbCr color
    distance. `p1`/`p2` are packed 0xAARRGGBB pixels. lumaWeight is fixed at
    1.0 (see the deviations note above).
    """
    a1 = (p1 >> 24) & 0xFF
    a2 = (p2 >> 24) & 0xFF
    r_diff = ((p1 >> 16) & 0xFF) - ((p2 >> 16) & 0xFF)
    g_diff = ((p1 >> 8) & 0xFF) - ((p2 >> 8) & 0xFF)
    b_diff = (p1 & 0xFF) - (p2 & 0xFF)

    y = _XBRZ_K_R * r_diff + _XBRZ_K_G * g_diff + _XBRZ_K_B * b_diff
    c_b = _XBRZ_SCALE_B * (b_diff - y)
    c_r = _XBRZ_SCALE_R * (r_diff - y)
    d = math.sqrt(y * y + c_b * c_b + c_r * c_r)

    fa1 = a1 / 255.0
    fa2 = a2 / 255.0
    if fa1 < fa2:
        return fa1 * d + 255.0 * (fa2 - fa1)
    else:
        return fa2 * d + 255.0 * (fa1 - fa2)


def _gradient_argb(pix_front, pix_back, m, n):
    """
    Port of gradientARGB<M,N>() (xbrz.cpp): blend `pix_front` over
    `pix_back` with nominal opacity M/N, weighting each side's contribution
    by its own alpha (so a fully-transparent side contributes zero color,
    only its absence of coverage) -- NOT regular "over" alpha compositing.
    Returns a new packed 0xAARRGGBB pixel.
    """
    weight_front = ((pix_front >> 24) & 0xFF) * m
    weight_back = ((pix_back >> 24) & 0xFF) * (n - m)
    weight_sum = weight_front + weight_back
    if weight_sum == 0:
        return 0

    def calc(c_front, c_back):
        return (c_front * weight_front + c_back * weight_back) // weight_sum

    r = calc((pix_front >> 16) & 0xFF, (pix_back >> 16) & 0xFF)
    g = calc((pix_front >> 8) & 0xFF, (pix_back >> 8) & 0xFF)
    b = calc(pix_front & 0xFF, pix_back & 0xFF)
    a = weight_sum // n
    return (a << 24) | (r << 16) | (g << 8) | b


def _xbrz_preprocess_corners(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p):
    """
    Port of preProcessCorners() (xbrz.cpp): given a 4x4 pixel neighborhood
    (row-major a..p, with f/g/j/k the 2x2 block whose shared corner is
    being classified), decide which diagonal (f-k or g-j) is the "real"
    edge and return (blend_f, blend_g, blend_j, blend_k) blend-strength
    codes (0=none, 1=normal, 2=dominant) for that corner as seen from each
    of the four surrounding pixels.
    """
    if (f == g and j == k) or (f == j and g == k):
        return 0, 0, 0, 0

    weight = 4
    jg = (_dist_argb(i, f) + _dist_argb(f, c) + _dist_argb(n, k) + _dist_argb(k, h)
          + weight * _dist_argb(j, g))
    fk = (_dist_argb(e, j) + _dist_argb(j, o) + _dist_argb(b, g) + _dist_argb(g, l)
          + weight * _dist_argb(f, k))

    blend_f = blend_g = blend_j = blend_k = _BLEND_NONE
    if jg < fk:
        bt = _BLEND_DOMINANT if _XBRZ_DOMINANT_DIRECTION_THRESHOLD * jg < fk else _BLEND_NORMAL
        if f != g and f != j:
            blend_f = bt
        if k != j and k != g:
            blend_k = bt
    elif fk < jg:
        bt = _BLEND_DOMINANT if _XBRZ_DOMINANT_DIRECTION_THRESHOLD * fk < jg else _BLEND_NORMAL
        if j != f and j != k:
            blend_j = bt
        if g != f and g != k:
            blend_g = bt
    return blend_f, blend_g, blend_j, blend_k


def _xbrz_rotate_blend_info(b, rot):
    """Port of rotateBlendInfo<rotDeg>(): rotate the 4x 2-bit corner fields."""
    if rot == 0:
        return b
    elif rot == 1:
        return ((b << 2) | (b >> 6)) & 0xFF
    elif rot == 2:
        return ((b << 4) | (b >> 4)) & 0xFF
    else:
        return ((b << 6) | (b >> 2)) & 0xFF


# Index permutations reproducing the get_a<rotDeg>..get_i<rotDeg> DEF_GETTER
# macros in xbrz.cpp, for a Kernel_3x3 flattened as (a, b, c, d, e, f, g, h, i).
_XBRZ_ROT_IDX = {
    0: (0, 1, 2, 3, 4, 5, 6, 7, 8),
    1: (6, 3, 0, 7, 4, 1, 8, 5, 2),
    2: (8, 7, 6, 5, 4, 3, 2, 1, 0),
    3: (2, 5, 8, 1, 4, 7, 0, 3, 6),
}


def _xbrz_out_index(bi, bj, rot, n=4):
    """
    Closed-form port of the recursive OutputMatrix<N, rotDeg>/MatrixRotation
    template in xbrz.cpp (verified by unrolling the recursion for all 4
    rotations): maps a rotation-relative (I, J) sub-block coordinate to the
    actual (row, col) in the unrotated NxN output block, returned as a flat
    row-major index.
    """
    if rot == 0:
        io, jo = bi, bj
    elif rot == 1:
        io, jo = n - 1 - bj, bi
    elif rot == 2:
        io, jo = n - 1 - bi, n - 1 - bj
    else:
        io, jo = bj, n - 1 - bi
    return io * n + jo


def _xbrz_ag(block, bi, bj, rot, col, m, n):
    idx = _xbrz_out_index(bi, bj, rot)
    block[idx] = _gradient_argb(col, block[idx], m, n)


# The following five functions are Scaler4x<ColorGradientARGB>'s
# blendLineShallow/Steep/SteepAndShallow/Diagonal/blendCorner (xbrz.cpp),
# transcribed 1:1 (scale = 4 baked in).

def _xbrz_blend_line_shallow(block, col, rot):
    _xbrz_ag(block, 3, 0, rot, col, 1, 4)
    _xbrz_ag(block, 2, 2, rot, col, 1, 4)
    _xbrz_ag(block, 3, 1, rot, col, 3, 4)
    _xbrz_ag(block, 2, 3, rot, col, 3, 4)
    block[_xbrz_out_index(3, 2, rot)] = col
    block[_xbrz_out_index(3, 3, rot)] = col


def _xbrz_blend_line_steep(block, col, rot):
    _xbrz_ag(block, 0, 3, rot, col, 1, 4)
    _xbrz_ag(block, 2, 2, rot, col, 1, 4)
    _xbrz_ag(block, 1, 3, rot, col, 3, 4)
    _xbrz_ag(block, 3, 2, rot, col, 3, 4)
    block[_xbrz_out_index(2, 3, rot)] = col
    block[_xbrz_out_index(3, 3, rot)] = col


def _xbrz_blend_line_steep_and_shallow(block, col, rot):
    _xbrz_ag(block, 3, 1, rot, col, 3, 4)
    _xbrz_ag(block, 1, 3, rot, col, 3, 4)
    _xbrz_ag(block, 3, 0, rot, col, 1, 4)
    _xbrz_ag(block, 0, 3, rot, col, 1, 4)
    _xbrz_ag(block, 2, 2, rot, col, 1, 3)
    block[_xbrz_out_index(3, 3, rot)] = col
    block[_xbrz_out_index(3, 2, rot)] = col
    block[_xbrz_out_index(2, 3, rot)] = col


def _xbrz_blend_line_diagonal(block, col, rot):
    _xbrz_ag(block, 3, 2, rot, col, 1, 2)
    _xbrz_ag(block, 2, 3, rot, col, 1, 2)
    block[_xbrz_out_index(3, 3, rot)] = col


def _xbrz_blend_corner(block, col, rot):
    _xbrz_ag(block, 3, 3, rot, col, 68, 100)
    _xbrz_ag(block, 3, 2, rot, col, 9, 100)
    _xbrz_ag(block, 2, 3, rot, col, 9, 100)


def _xbrz_blend_pixel_rot(block, k3, blend_byte, rot):
    """
    Port of blendPixel<Scaler4x, ColorDistanceARGB, rotDeg>() (xbrz.cpp) for
    one of the 4 rotations: decides (per this corner) whether to do a line
    blend (shallow/steep/both/diagonal) or a rounded corner blend, and
    applies it to `block` (a flat 16-entry row-major 4x4 output block).
    `k3` is the unrotated Kernel_3x3 as a 9-tuple (a, b, c, d, e, f, g, h, i).
    """
    br = _xbrz_rotate_blend_info(blend_byte, rot)
    bottom_r = (br >> 4) & 0x3
    if bottom_r == _BLEND_NONE:
        return

    idx = _XBRZ_ROT_IDX[rot]
    ra, rb, rc, rd, re, rf, rg, rh, ri = (k3[idx[0]], k3[idx[1]], k3[idx[2]], k3[idx[3]],
                                           k3[idx[4]], k3[idx[5]], k3[idx[6]], k3[idx[7]], k3[idx[8]])
    top_r = (br >> 2) & 0x3
    bottom_l = (br >> 6) & 0x3
    tol = _XBRZ_EQUAL_COLOR_TOLERANCE

    do_line_blend = True
    if bottom_r >= _BLEND_DOMINANT:
        do_line_blend = True
    elif top_r != _BLEND_NONE and _dist_argb(re, rg) >= tol:
        do_line_blend = False
    elif bottom_l != _BLEND_NONE and _dist_argb(re, rc) >= tol:
        do_line_blend = False
    elif (_dist_argb(re, ri) >= tol and _dist_argb(rg, rh) < tol and _dist_argb(rh, ri) < tol
          and _dist_argb(ri, rf) < tol and _dist_argb(rf, rc) < tol):
        do_line_blend = False

    px = rf if _dist_argb(re, rf) <= _dist_argb(re, rh) else rh

    if do_line_blend:
        fg = _dist_argb(rf, rg)
        hc = _dist_argb(rh, rc)
        have_shallow = _XBRZ_STEEP_DIRECTION_THRESHOLD * fg <= hc and re != rg and rd != rg
        have_steep = _XBRZ_STEEP_DIRECTION_THRESHOLD * hc <= fg and re != rc and rb != rc

        if have_shallow:
            if have_steep:
                _xbrz_blend_line_steep_and_shallow(block, px, rot)
            else:
                _xbrz_blend_line_shallow(block, px, rot)
        else:
            if have_steep:
                _xbrz_blend_line_steep(block, px, rot)
            else:
                _xbrz_blend_line_diagonal(block, px, rot)
    else:
        _xbrz_blend_corner(block, px, rot)


def xbrz_scale_4x(rgba, src_w, src_h):
    """
    xBRZ-scale an RGBA buffer (as produced by colorize_rgba()) by 4x,
    returning a new RGBA bytearray of size (src_w*4) * (src_h*4) * 4. See
    the "STEP 4b" block comment above for what this ports and how.
    """
    w, h = src_w, src_h
    total = w * h

    # Unpack to packed 0xAARRGGBB ints. Sanitize fully-transparent source
    # pixels to RGB=0 so no stray/garbage color can ever bleed into a blend.
    src = [0] * total
    for idx in range(total):
        o = idx * 4
        r = rgba[o]
        g = rgba[o + 1]
        b = rgba[o + 2]
        a = rgba[o + 3]
        if a == 0:
            r = g = b = 0
        src[idx] = (a << 24) | (r << 16) | (g << 8) | b

    # Pass 1: classify the blend strength of every interior corner
    # (equivalent to scaleImage()'s row-buffered preprocessing pass, just
    # computed directly per corner instead of carried in a rolling buffer).
    blend = bytearray(total)
    for y in range(h):
        ym1 = y - 1 if y > 0 else 0
        yp1 = y + 1 if y + 1 < h else h - 1
        yp2 = y + 2 if y + 2 < h else h - 1
        row_m1 = ym1 * w
        row_0 = y * w
        row_p1 = yp1 * w
        row_p2 = yp2 * w
        blend_row = y * w
        blend_row_p1 = (y + 1) * w if y + 1 < h else None

        for x in range(w):
            xm1 = x - 1 if x > 0 else 0
            xp1 = x + 1 if x + 1 < w else w - 1
            xp2 = x + 2 if x + 2 < w else w - 1

            a_ = src[row_m1 + xm1]
            b_ = src[row_m1 + x]
            c_ = src[row_m1 + xp1]
            d_ = src[row_m1 + xp2]
            e_ = src[row_0 + xm1]
            f_ = src[row_0 + x]
            g_ = src[row_0 + xp1]
            h_ = src[row_0 + xp2]
            i_ = src[row_p1 + xm1]
            j_ = src[row_p1 + x]
            k_ = src[row_p1 + xp1]
            l_ = src[row_p1 + xp2]
            m_ = src[row_p2 + xm1]
            n_ = src[row_p2 + x]
            o_ = src[row_p2 + xp1]
            p_ = src[row_p2 + xp2]

            bf, bg, bj, bk = _xbrz_preprocess_corners(
                a_, b_, c_, d_, e_, f_, g_, h_, i_, j_, k_, l_, m_, n_, o_, p_)

            if bf:
                blend[blend_row + x] |= (bf << 4)
            if bg and x + 1 < w:
                blend[blend_row + x + 1] |= (bg << 6)
            if bj and blend_row_p1 is not None:
                blend[blend_row_p1 + x] |= (bj << 2)
            if bk and blend_row_p1 is not None and x + 1 < w:
                blend[blend_row_p1 + x + 1] |= bk

    # Pass 2: fill each pixel's 4x4 output block with its own color, then
    # (if any corner needs it) blend all 4 rotations into that block.
    dst_w, dst_h = w * 4, h * 4
    dst = bytearray(dst_w * dst_h * 4)

    for y in range(h):
        ym1 = y - 1 if y > 0 else 0
        yp1 = y + 1 if y + 1 < h else h - 1
        row_m1 = ym1 * w
        row_0 = y * w
        row_p1 = yp1 * w
        blend_row = y * w
        dst_row_base = y * 4

        for x in range(w):
            center = src[row_0 + x]
            bl = blend[blend_row + x]

            if bl:
                xm1 = x - 1 if x > 0 else 0
                xp1 = x + 1 if x + 1 < w else w - 1
                k3 = (src[row_m1 + xm1], src[row_m1 + x], src[row_m1 + xp1],
                      src[row_0 + xm1], center, src[row_0 + xp1],
                      src[row_p1 + xm1], src[row_p1 + x], src[row_p1 + xp1])
                block = [center] * 16
                _xbrz_blend_pixel_rot(block, k3, bl, 0)
                _xbrz_blend_pixel_rot(block, k3, bl, 1)
                _xbrz_blend_pixel_rot(block, k3, bl, 2)
                _xbrz_blend_pixel_rot(block, k3, bl, 3)
            else:
                block = None

            dst_col_base = x * 4
            for li in range(4):
                row_off = ((dst_row_base + li) * dst_w + dst_col_base) * 4
                for lj in range(4):
                    px = center if block is None else block[li * 4 + lj]
                    o2 = row_off + lj * 4
                    dst[o2] = (px >> 16) & 0xFF
                    dst[o2 + 1] = (px >> 8) & 0xFF
                    dst[o2 + 2] = px & 0xFF
                    dst[o2 + 3] = (px >> 24) & 0xFF

    return dst


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


def _get_cached_palette(palette_cache, pal_index):
    palette = palette_cache.get(pal_index)
    if palette is None:
        palette = load_palette(PALETTE_PATH, pal_index)
        palette_cache[pal_index] = palette
    return palette


def process_sprite_tables(palette_cache, manifest_frames):
    """
    Extract PLANET_SHAPES, FACE_SHAPES, OPTION_SHAPES, and WEAPON_SHAPES
    (SPRITE_TABLES) from tyrian.shp: decode each populated frame's RLE
    pixels, key transparency to alpha, xBRZ-upscale 4x (RGBA; edge-directed
    pixel-art scaling, so the title logo gets smooth curved edges instead of
    blocky/blurry ones -- see the "STEP 4b" xBRZ port above), and write one
    hd<prefix>_NN.dat HDPX-with-alpha asset per frame. A table entry with
    palette None (FACE_SHAPES) is recolored per-frame via FACEPAL instead of
    one fixed palette. Appends a manifest entry per frame to manifest_frames
    (in place). Returns the number of frames written.
    """
    if not os.path.isfile(SHP_PATH):
        print("error: tyrian.shp not found at %s" % SHP_PATH, file=sys.stderr)
        return 0

    data, shp_pos = load_shp_table_offsets(SHP_PATH)

    written = 0
    for table_index, prefix, manifest_name, pal_index in SPRITE_TABLES:
        sprites = load_sprite_table(data, shp_pos[table_index])

        if pal_index is None:
            print("Table %d (%s): %d frames, per-frame FACEPAL palette -> %s_NN.dat ..." %
                  (table_index, prefix, len(sprites), prefix))
        else:
            print("Table %d (%s): %d frames, palette %d -> %s_NN.dat ..." %
                  (table_index, prefix, len(sprites), pal_index, prefix))

        for frame_index, sprite in enumerate(sprites):
            if sprite is None:
                continue

            if pal_index is None:
                if frame_index >= len(FACEPAL):
                    print("  warning: frame %d has no FACEPAL entry (table has %d frames, "
                          "FACEPAL has %d), falling back to palette 0" %
                          (frame_index, len(sprites), len(FACEPAL)), file=sys.stderr)
                    frame_pal_index = 0
                else:
                    frame_pal_index = FACEPAL[frame_index]
            else:
                frame_pal_index = pal_index

            palette = _get_cached_palette(palette_cache, frame_pal_index)

            indices, src_w, src_h = decode_sprite_rle(sprite)
            rgba = colorize_rgba(indices, palette)
            upscaled = xbrz_scale_4x(rgba, src_w, src_h)

            out_name = "%s_%02d.dat" % (prefix, frame_index)
            out_path = os.path.join(DATA_DIR, out_name)
            write_hdpx_asset(out_path, upscaled, src_w * SCALE, src_h * SCALE, channels=4)

            manifest_frames.append({
                "table": manifest_name,
                "frame_index": frame_index,
                "file": out_name,
                "palette": frame_pal_index,
                "src_width": src_w,
                "src_height": src_h,
                "hd_width": src_w * SCALE,
                "hd_height": src_h * SCALE,
            })
            written += 1

    return written


def process_extra_shapes(palette_cache, manifest_frames):
    """
    Extract EXTRA_SHAPES from estsc.shp (a separate file from tyrian.shp;
    see the EXTRA_SHAPES_PAL comment above), the same way
    process_sprite_tables() handles tyrian.shp's tables: decode each
    populated frame's RLE pixels, key transparency to alpha, xBRZ-upscale
    4x, and write one hdextra_NN.dat HDPX-with-alpha asset per frame.
    Appends a manifest entry per frame to manifest_frames (in place).
    Returns the number of frames written (0 if estsc.shp is missing).
    """
    if not os.path.isfile(EXTRA_SHP_PATH):
        print("skip: estsc.shp not found at %s (EXTRA_SHAPES not extracted)" % EXTRA_SHP_PATH,
              file=sys.stderr)
        return 0

    with open(EXTRA_SHP_PATH, "rb") as f:
        data = f.read()

    palette = _get_cached_palette(palette_cache, EXTRA_SHAPES_PAL)

    # estsc.shp has no leading table-offset header (unlike tyrian.shp) --
    # load_sprites_file() in src/sprite.c reads a single sprite table
    # straight from the start of the file, so start_offset is 0.
    sprites = load_sprite_table(data, 0)
    print("EXTRA_SHAPES (estsc.shp): %d frames, palette %d -> hdextra_NN.dat ..." %
          (len(sprites), EXTRA_SHAPES_PAL))

    written = 0
    for frame_index, sprite in enumerate(sprites):
        if sprite is None:
            continue

        indices, src_w, src_h = decode_sprite_rle(sprite)
        rgba = colorize_rgba(indices, palette)
        upscaled = xbrz_scale_4x(rgba, src_w, src_h)

        out_name = "hdextra_%02d.dat" % frame_index
        out_path = os.path.join(DATA_DIR, out_name)
        write_hdpx_asset(out_path, upscaled, src_w * SCALE, src_h * SCALE, channels=4)

        manifest_frames.append({
            "table": "EXTRA_SHAPES",
            "frame_index": frame_index,
            "file": out_name,
            "palette": EXTRA_SHAPES_PAL,
            "src_width": src_w,
            "src_height": src_h,
            "hd_width": src_w * SCALE,
            "hd_height": src_h * SCALE,
        })
        written += 1

    return written


def write_credits_backdrop_filler():
    """
    Write a solid-black dummy HD backdrop asset (hdpcx_credblack.dat) used
    only to *activate* HD compositing (hd_backdrop_active) for the credits
    screen (src/mainint.c JE_playCredits()), which has no full-screen picture
    of its own -- it's a scrolling black background with a small EXTRA_SHAPES
    corner picture. The hd_set_sprite() overlay queue is only drawn while an
    HD backdrop is active (see src/video.c scale_and_flip()), so credits
    needs *some* backdrop asset to unlock the per-frame hdextra_NN.dat corner
    sprite; a flat black filler is visually identical to the classic black
    background it sits behind. No source file required -- always (re)written.
    """
    pixels = bytes([0, 0, 0, 255]) * (8 * 8)
    out_path = os.path.join(DATA_DIR, "hdpcx_credblack.dat")
    write_hdpx_asset(out_path, pixels, 8, 8, channels=4)
    print("Wrote credits filler backdrop -> %s" % out_path)


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

    write_credits_backdrop_filler()

    skipped_pcx = []
    failed_pcx = []
    for filename, prefix in STANDALONE_PCX:
        src_path = os.path.join(DATA_DIR, filename)
        ok = process_standalone_pcx(filename, prefix, want_preview=not args.no_preview)
        if not ok:
            if os.path.isfile(src_path):
                failed_pcx.append(filename)
            else:
                skipped_pcx.append(filename)

    manifest_frames = []
    sprite_count = process_sprite_tables(palette_cache, manifest_frames)
    sprite_count += process_extra_shapes(palette_cache, manifest_frames)
    if sprite_count:
        with open(SPRITE_MANIFEST_PATH, "w") as f:
            json.dump({"scale": SCALE, "frames": manifest_frames}, f, indent=2)
        print("Wrote %d sprite frames, manifest -> %s" % (sprite_count, SPRITE_MANIFEST_PATH))

    if skipped_pcx:
        print("Skipped (source not present): %s" % ", ".join(skipped_pcx), file=sys.stderr)

    if failed or failed_pcx:
        if failed:
            print("Done with errors. Failed images: %s" % ", ".join(str(n) for n in failed),
                  file=sys.stderr)
        if failed_pcx:
            print("Done with errors. Failed PCX files: %s" % ", ".join(failed_pcx),
                  file=sys.stderr)
        return 1

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
