#!/usr/bin/env python3
"""
hd_extract_font.py -- Offline HD asset extraction tool for OpenTyrian's font
glyph tables (FONT_SHAPES, SMALL_FONT_SHAPES, TINY_FONT), standalone from
(and safe to run concurrently with) tools/hd_extract.py -- it only reads
tyrian.shp and writes brand-new tyrian21/hdfont_*.dat files plus its own
manifest, never touching any existing hdcomp_*/hdcompb_*/hdpic*/hdplanet*
asset.

WHY A BRIGHTNESS MAP, NOT A BAKED COLOR (see the module docstring section
below for the full investigation): font glyphs in Tyrian are NOT drawn with
a fixed baked color. Every draw call in src/fonthand.c (JE_dString(),
JE_outText(), JE_outTextModify(), JE_outTextAdjust(), JE_outTextAndDarken())
takes a caller-supplied `colorbank`/`filter`/`hue` (a 0..15 palette *band*)
and a `brightness`/`value` (a signed shade delta), and those pass straight
through to src/sprite.c's blit_sprite_hv_unsafe() / blit_sprite_hv() /
blit_sprite_hv_blend(), which recolor per-call:

    hue <<= 4;
    ...
    *pixels = hue | ((*data & 0x0f) + value);   // blit_sprite_hv_unsafe
    // blit_sprite_hv/_blend do the same, just with clamping/averaging of
    // the low nibble before OR-ing in `hue`.

So the *raw* glyph byte's low nibble (`*data & 0x0f`) is a per-pixel
brightness/coverage level (0..15); its high nibble is discarded by every one
of these draw paths (masked out by `& 0x0f` before use) and is therefore
irrelevant noise, not a second baked color. Confirmed empirically against
the actual tyrian21/tyrian.shp data: after masking to the low nibble, every
non-transparent pixel across all three font tables falls in exactly the
0..15 range (table 0/1: 3..15, table 2/TINY_FONT: 2..10) -- i.e. these
tables genuinely only carry a brightness value per pixel, never a fixed
color. The caller's hue/colorbank always wins.

Given that, this tool extracts each glyph as a BRIGHTNESS MAP: a grayscale
RGBA image where R=G=B = (low-nibble brightness / 15) * 255 and alpha =
0/255 coverage (opaque vs. transparent pixel, per the RLE's 255/253
transparent-pixel ops), xBRZ 4x upscaled (alpha-aware edge-directed
scaling, so glyph edges get smooth anti-aliased coverage instead of hard
pixel steps), written as tyrian21/hdfont_<table>_NN.dat (HDPX-with-alpha).
This lets the engine recolor each glyph per draw call exactly like the
original 8bpp path does (map brightness -> whatever palette band/shade the
caller wants), rather than baking one arbitrary color choice that would be
wrong for every other call site.

Tables extracted (tyrian.shp, table indices per src/sprite.h):
  - FONT_SHAPES       (0) -> hdfont_font_NN.dat
  - SMALL_FONT_SHAPES (1) -> hdfont_small_NN.dat
  - TINY_FONT         (2) -> hdfont_tiny_NN.dat

Manifest: tyrian21/hd_font_manifest.json (table -> glyph count -> per-glyph
{index, file, src WxH, hd WxH}), plus a top-level "format"/"format_note"
recording the brightness-map decision and why.

A few PNG previews (grayscale, alpha shown as a black background matte) of
a handful of FONT_SHAPES glyphs land in tools/hdfont_previews/ for eyeballing
(skip with --no-preview).

Standard library only (no Pillow/numpy). Tested against python3.9.

Usage:
  hd_extract_font.py [--no-preview]

Format references:
  - src/fonthand.c: JE_dString(), JE_outText(), JE_outTextModify(),
    JE_outTextAdjust(), JE_outTextAndDarken(), JE_textShade() -- every font
    draw call site and how colorbank/filter/hue + brightness/value are
    supplied by the caller.
  - src/sprite.c: blit_sprite_hv_unsafe() / blit_sprite_hv() /
    blit_sprite_hv_blend() / blit_sprite_dark() -- the actual per-pixel
    recolor math (`hue | ((*data & 0x0f) + value)` and friends) confirming
    the low nibble is brightness and the high nibble is discarded.
  - src/sprite.h: FONT_SHAPES / SMALL_FONT_SHAPES / TINY_FONT table indices.
  - src/sprite.c load_sprites(), JE_loadMainShapeTables() (tyrian.shp
    layout and its RLE sprite encoding) -- table/sprite decode below is
    copied (not imported) from tools/hd_extract.py's implementation of the
    same, so this tool has no dependency on it and is parallel-safe.
  - doc/files.txt -- background on the original Tyrian data file formats.
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
SHP_PATH = os.path.join(DATA_DIR, "tyrian.shp")
PREVIEW_DIR = os.path.join(REPO_ROOT, "tools", "hdfont_previews")
FONT_MANIFEST_PATH = os.path.join(DATA_DIR, "hd_font_manifest.json")

SCALE = 4

# tyrian.shp table indices (see src/sprite.h) and the SHP_NUM used by
# JE_loadMainShapeTables() in src/sprite.c for the leading offset table.
SHP_NUM = 12
FONT_SHAPES = 0
SMALL_FONT_SHAPES = 1
TINY_FONT = 2

# (table index, output prefix, manifest table name)
FONT_TABLES = [
    (FONT_SHAPES, "hdfont_font", "FONT_SHAPES"),
    (SMALL_FONT_SHAPES, "hdfont_small", "SMALL_FONT_SHAPES"),
    (TINY_FONT, "hdfont_tiny", "TINY_FONT"),
]

FORMAT_NOTE = (
    "Font glyphs are runtime-recolored, not fixed-color: every draw call in "
    "src/fonthand.c (JE_dString/JE_outText/JE_outTextModify/JE_outTextAdjust/"
    "JE_outTextAndDarken) passes a caller-chosen hue/colorbank band plus a "
    "brightness/value shade into src/sprite.c's blit_sprite_hv_unsafe()/"
    "blit_sprite_hv()/blit_sprite_hv_blend(), which compute "
    "`hue<<4 | ((glyph_byte & 0x0f) + value)` per pixel -- i.e. only the raw "
    "glyph byte's LOW NIBBLE (a 0..15 brightness/coverage level) is used; the "
    "high nibble is masked away and is not a second baked color. Verified "
    "empirically against tyrian21/tyrian.shp: every non-transparent pixel's "
    "low nibble across all three tables falls in 0..15 (table 0/1 use 3..15, "
    "table 2/TINY_FONT uses 2..10). Because of this, each glyph is extracted "
    "here as a grayscale brightness map (R=G=B=brightness scaled to 0..255, "
    "alpha=0/255 coverage) rather than a baked RGBA color, so the engine can "
    "recolor it per draw call exactly like the original 8bpp path."
)


# ---------------------------------------------------------------------------
# STEP 1: tyrian.shp table/sprite decode
# (vendored/copied from tools/hd_extract.py -- NOT imported, so this tool has
# no dependency on it and stays safe to run concurrently on different files)
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
    (raw_byte_or_None) -- None marks a transparent pixel -- mirroring the
    pointer arithmetic of blit_sprite() in src/sprite.c exactly (a flat
    "pixels" cursor plus an in-row "x_offset", with no explicit row
    variable; pitch == width since we have no surface stride padding):
      255       -> next byte is a count of transparent pixels
      254       -> advance the cursor to the start of the next row
      253       -> a single transparent pixel
      0..252    -> a direct opaque raw byte (font tables: brightness in the
                   low nibble; see FORMAT_NOTE above for why the high nibble
                   is ignored)
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
# STEP 2: raw glyph bytes -> flat RGBA brightness-map bytearray
# (R=G=B=brightness*17 (0..15 -> 0..255), alpha=0/255 coverage)
# ---------------------------------------------------------------------------

def brightness_map_rgba(indices):
    """
    Like colorize_rgba() in hd_extract.py, but for font glyphs: each raw
    byte's LOW NIBBLE (0..15) is the brightness level (see FORMAT_NOTE),
    scaled to 0..255 and written to all three of R/G/B (a neutral grayscale
    brightness map the engine can recolor at draw time); alpha is 0 for
    transparent pixels (None) and 255 for opaque ones.
    """
    rgba = bytearray(len(indices) * 4)
    for idx, raw in enumerate(indices):
        o = idx * 4
        if raw is None:
            rgba[o] = 0
            rgba[o + 1] = 0
            rgba[o + 2] = 0
            rgba[o + 3] = 0
        else:
            level = (raw & 0x0f) * 17  # 0..15 -> 0..255
            rgba[o] = level
            rgba[o + 1] = level
            rgba[o + 2] = level
            rgba[o + 3] = 255
    return rgba


# ---------------------------------------------------------------------------
# STEP 3: xBRZ 4x edge-directed upscaling
#
# A pure-Python, stdlib-only port of Zenju's xBRZ ("edge-directed" pixel-art
# scaler; https://sourceforge.net/projects/xbrz), ARGB color format, fixed
# to scale factor 4. Vendored/copied verbatim (not imported) from
# tools/hd_extract.py's port of xbrz.cpp/xbrz.h/config.h (GPLv3, Copyright
# (C) Zenju) -- see that file's "STEP 4b" block comment for the full
# per-function attribution; duplicated here only so this tool has zero
# import dependency on hd_extract.py and stays parallel-safe.
# ---------------------------------------------------------------------------

_XBRZ_EQUAL_COLOR_TOLERANCE = 30.0
_XBRZ_DOMINANT_DIRECTION_THRESHOLD = 3.6
_XBRZ_STEEP_DIRECTION_THRESHOLD = 2.2

_XBRZ_K_B = 0.0593
_XBRZ_K_R = 0.2627
_XBRZ_K_G = 1.0 - _XBRZ_K_B - _XBRZ_K_R
_XBRZ_SCALE_B = 0.5 / (1.0 - _XBRZ_K_B)
_XBRZ_SCALE_R = 0.5 / (1.0 - _XBRZ_K_R)

_BLEND_NONE = 0
_BLEND_NORMAL = 1
_BLEND_DOMINANT = 2


def _dist_argb(p1, p2):
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
    if rot == 0:
        return b
    elif rot == 1:
        return ((b << 2) | (b >> 6)) & 0xFF
    elif rot == 2:
        return ((b << 4) | (b >> 4)) & 0xFF
    else:
        return ((b << 6) | (b >> 2)) & 0xFF


_XBRZ_ROT_IDX = {
    0: (0, 1, 2, 3, 4, 5, 6, 7, 8),
    1: (6, 3, 0, 7, 4, 1, 8, 5, 2),
    2: (8, 7, 6, 5, 4, 3, 2, 1, 0),
    3: (2, 5, 8, 1, 4, 7, 0, 3, 6),
}


def _xbrz_out_index(bi, bj, rot, n=4):
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
    xBRZ-scale an RGBA buffer (as produced by brightness_map_rgba()) by 4x,
    returning a new RGBA bytearray of size (src_w*4) * (src_h*4) * 4.
    """
    w, h = src_w, src_h
    total = w * h

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
# STEP 4a: write the engine asset (HDPX raw RGBA format)
# ---------------------------------------------------------------------------

def write_hdpx_asset(path, pixels, width, height):
    """
    Write the HDPX raw-asset format: ASCII "HDPX", u32 LE width, u32 LE
    height, then width*height RGBA8888 pixels (alpha taken as-is).
    """
    with open(path, "wb") as f:
        f.write(b"HDPX")
        f.write(struct.pack("<I", width))
        f.write(struct.pack("<I", height))
        f.write(bytes(pixels))


# ---------------------------------------------------------------------------
# STEP 4b: minimal stdlib-only PNG writer (8-bit RGB, color type 2), with a
# flat matte color composited under alpha so transparent glyph background
# shows as black rather than undefined garbage.
# ---------------------------------------------------------------------------

def write_png(path, rgba, width, height):
    def chunk(tag, data):
        out = struct.pack(">I", len(data))
        out += tag
        out += data
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        out += struct.pack(">I", crc)
        return out

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)

    stride = width * 3
    raw = bytearray((stride + 1) * height)
    for y in range(height):
        dst_off = y * (stride + 1)
        raw[dst_off] = 0  # filter type: None
        for x in range(width):
            so = (y * width + x) * 4
            r, g, b, a = rgba[so], rgba[so + 1], rgba[so + 2], rgba[so + 3]
            do = dst_off + 1 + x * 3
            # Matte over black so alpha=0 pixels render as black, not garbage.
            raw[do] = (r * a) // 255
            raw[do + 1] = (g * a) // 255
            raw[do + 2] = (b * a) // 255

    idat = zlib.compress(bytes(raw), 9)

    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

# How many of FONT_SHAPES's first glyphs to write PNG previews for.
PREVIEW_GLYPH_COUNT = 4


def process_font_tables(want_preview):
    """
    Extract FONT_SHAPES, SMALL_FONT_SHAPES, and TINY_FONT (FONT_TABLES) from
    tyrian.shp: decode each populated glyph's RLE bytes, build a grayscale
    brightness-map RGBA buffer (brightness_map_rgba()), xBRZ-upscale 4x, and
    write one hdfont_<prefix>_NN.dat HDPX-with-alpha asset per glyph. Returns
    (glyph_count_by_table, total_written).
    """
    if not os.path.isfile(SHP_PATH):
        print("error: tyrian.shp not found at %s" % SHP_PATH, file=sys.stderr)
        return {}, 0

    data, shp_pos = load_shp_table_offsets(SHP_PATH)

    manifest_tables = {}
    total_written = 0

    for table_index, prefix, manifest_name in FONT_TABLES:
        glyphs = load_sprite_table(data, shp_pos[table_index])
        print("Table %d (%s): %d glyph slots -> %s_NN.dat ..." %
              (table_index, manifest_name, len(glyphs), prefix))

        glyph_entries = []
        written = 0

        for glyph_index, glyph in enumerate(glyphs):
            if glyph is None:
                continue

            indices, src_w, src_h = decode_sprite_rle(glyph)
            if src_w == 0 or src_h == 0:
                continue

            rgba = brightness_map_rgba(indices)
            upscaled = xbrz_scale_4x(rgba, src_w, src_h)

            hd_w, hd_h = src_w * SCALE, src_h * SCALE
            out_name = "%s_%02d.dat" % (prefix, glyph_index)
            out_path = os.path.join(DATA_DIR, out_name)
            write_hdpx_asset(out_path, upscaled, hd_w, hd_h)

            glyph_entries.append({
                "index": glyph_index,
                "file": out_name,
                "src_width": src_w,
                "src_height": src_h,
                "hd_width": hd_w,
                "hd_height": hd_h,
            })
            written += 1

            if want_preview and manifest_name == "FONT_SHAPES" and written <= PREVIEW_GLYPH_COUNT:
                os.makedirs(PREVIEW_DIR, exist_ok=True)
                preview_path = os.path.join(PREVIEW_DIR, "%s_%02d.png" % (prefix, glyph_index))
                write_png(preview_path, upscaled, hd_w, hd_h)

        manifest_tables[manifest_name] = {
            "table_index": table_index,
            "prefix": prefix,
            "glyph_count": written,
            "glyphs": glyph_entries,
        }
        total_written += written

    return manifest_tables, total_written


def main():
    parser = argparse.ArgumentParser(
        description="Extract HD brightness-map assets for OpenTyrian's font glyph tables.")
    parser.add_argument(
        "--no-preview", action="store_true",
        help="skip writing PNG previews (faster)")
    args = parser.parse_args()

    if not os.path.isfile(SHP_PATH):
        print("error: tyrian.shp not found at %s" % SHP_PATH, file=sys.stderr)
        return 1

    os.makedirs(DATA_DIR, exist_ok=True)

    manifest_tables, total_written = process_font_tables(want_preview=not args.no_preview)
    if total_written == 0:
        print("error: no glyphs written", file=sys.stderr)
        return 1

    manifest = {
        "scale": SCALE,
        "format": "brightness_map",
        "format_note": FORMAT_NOTE,
        "tables": manifest_tables,
    }
    with open(FONT_MANIFEST_PATH, "w") as f:
        json.dump(manifest, f, indent=2)

    print("Wrote %d glyphs total, manifest -> %s" % (total_written, FONT_MANIFEST_PATH))
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
