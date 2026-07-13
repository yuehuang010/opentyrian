#!/usr/bin/env python3
"""
hd_extract_comp.py -- Offline HD asset extraction tool for OpenTyrian's
in-flight COMPILED sprite sheets (the "Sprite2" format: Sprite2_array +
JE_loadCompShapesB() in src/sprite.c), as opposed to the plain Sprite
format handled by hd_extract.py.

STANDALONE / PARALLEL-SAFE: this file is intentionally self-contained. It
vendors (copies) several helpers from hd_extract.py -- the xBRZ 4x scaler,
the HDPX writer, the palette loader, and the minimal PNG writer -- rather
than importing them, so this tool can run concurrently with another agent
editing hd_extract.py without an import race or partial-file import error.
Any duplication with hd_extract.py is deliberate; see NOTE comments below
at each vendored block.

--------------------------------------------------------------------------
THE Sprite2 / "compiled shapes" FORMAT (reverse-engineered from
src/sprite.c: JE_loadCompShapesB(), blit_sprite2(), JE_loadMainShapeTables(),
and src/sprite.h's Sprite2_array/blit_sprite2x2* declarations; cross-checked
byte-for-byte against every sheet listed below -- zero decode anomalies):

  A Sprite2_array is just a flat byte blob (Sprite2_array.data / .size);
  JE_loadCompShapesB() does nothing but slurp `size` bytes into memory --
  ALL of the structure lives inside the blob itself:

    offset 0            : Uint16 LE offsets[frame_count], one per frame,
                           1-based (`data + offsets[index-1]` in
                           blit_sprite2()). The table is
                           self-describing: offsets[0] always equals
                           frame_count*2 (the byte offset of the first
                           frame's RLE stream, i.e. right after the last
                           table entry), so frame_count = offsets[0] // 2.
                           Verified empirically: every sheet checked here
                           has exactly 304 frames this way, with the last
                           frame's (offset + decoded length) landing
                           exactly on EOF.
    offsets[i]..         : per-frame RLE stream, byte-run-length encoded
                           against a FIXED 12 (wide) x 14 (tall) pixel
                           canvas (see the "- 12" literals throughout
                           blit_sprite2()/_clip/_blend/_darken/_filter and
                           the index+1 / index+20 / index+21 tile-grid math
                           in blit_sprite2x2(), which glues four 12x14
                           tiles into a 24x28 "big" sprite). Opcode byte:
                             - byte == 0x0F (whole byte, i.e. hi-nibble 0
                               AND lo-nibble 0xF) is the frame terminator
                               (no operand).
                             - otherwise: lo-nibble = transparent-pixel
                               skip count (0-14) advancing the column
                               cursor; hi-nibble = opaque run length
                               (0-15). If the opaque run is 0, the byte is
                               a "next row" marker (column resets to 0, row
                               += 1) -- empirically these always carry
                               skip==0 (byte 0x00) in every real sheet.
                               Otherwise, `count` raw palette-index bytes
                               follow the opcode byte and are written
                               starting at the current cursor.
                           A "blank"/unused sprite slot is just a
                           terminator with nothing before it (decodes to
                           an all-transparent 12x14 tile).

  This directly mirrors blit_sprite2() in src/sprite.c:
    const Uint8 *data = sprite2s.data + SDL_SwapLE16(((Uint16*)sprite2s.data)[index - 1]);
    for (; *data != 0x0f; ++data) {
        pixels += *data & 0x0f;                    // skip nibble
        unsigned int count = (*data & 0xf0) >> 4;  // opaque nibble
        if (count == 0) pixels += VGAScreen->pitch - 12;  // next row
        else while (count--) { ++data; *pixels++ = *data; }
    }

--------------------------------------------------------------------------
SHEETS EXTRACTED (see internal/plan/REMASTER_ASSETS.md sec.3; source call sites are
src/sprite.c JE_loadMainShapeTables()/JE_loadCompShapes(), src/tyrian2.c,
src/destruct.c, src/game_menu.c):

  - sheet8..sheet12  : spriteSheet8..spriteSheet12, five back-to-back
                        Sprite2 blocks inside tyrian.shp (player shots,
                        ships, power-ups, coins/datacubes, more shots).
                        tyrian.shp's own header is the SAME layout as
                        JE_loadMainShapeTables() reads: Uint16 shp_num
                        (12), then shp_num Int32 LE block offsets; blocks
                        0-6 are plain Sprite tables (unrelated -- handled
                        by hd_extract.py), blocks 7-11 (0-based) are the
                        five Sprite2 comp sheets extracted here, each
                        spanning [shp_pos[i], shp_pos[i+1]) (EOF for the
                        last one).
  - shop             : shopSpriteSheet, whole-file newsh1.shp (shop icons,
                        mouse pointer/arrows).
  - explosion        : explosionSpriteSheet, whole-file newsh6.shp.
  - destruct         : destructSpriteSheet, whole-file newsh~.shp
                        (Destruct minigame units).
  - enemy_<id>       : enemySpriteSheets[0..3] are NOT static per-file
                        sheets -- src/tyrian2.c loads them dynamically at
                        runtime, one of 34 possible newsh?.shp files
                        (indexed via shapeFile[] in src/lvlmast.c) into
                        whichever of the 4 *runtime bank slots* a given
                        level's event data asks for. There is no fixed
                        "bank 0 = file X" mapping to preserve, and the
                        five shapes?.dat files this task's brief named
                        (shapes).dat/shapesw.dat/shapesx.dat/shapesy.dat/
                        shapesz.dat) are a DIFFERENT, unrelated format --
                        per-level tileset graphics (doc/files.txt:
                        "shapes?.dat: Level tileset graphics"), read by a
                        wholly different loader (fixed 24x28 planar blocks
                        in src/tyrian2.c's level loader), NOT Sprite2/RLE.
                        Feeding them through this decoder would silently
                        produce garbage, so instead every *actual* enemy
                        Sprite2 file present in the data dir (every
                        newsh?.shp not already claimed by shop/explosion/
                        destruct above) is extracted individually and
                        named by its filename suffix character, e.g.
                        enemy_0 (newsh0.shp), enemy_a (newsha.shp),
                        enemy_hash (newsh#.shp), enemy_caret (newsh^.shp).
                        This is a deliberate, documented deviation from
                        the literal "4 banks from 5 named files" ask --
                        see the final report for details.

--------------------------------------------------------------------------
OUTPUT (all colorized with palettes[0], the main game palette -- recoloring
by ship hue / enemy "value" happens at DRAW time in the engine and is
explicitly out of scope here):

  - tyrian21/hdcomp_<sheet>_NN.dat  : one HDPX-with-alpha asset per frame
                                       (NN = 2-digit zero-padded frame
                                       index), 4x xBRZ-upscaled from the
                                       12x14 source tile to 48x56, palette
                                       index 0 forced to alpha 0.
  - tyrian21/hd_comp_manifest.json  : per-sheet source file, frame count,
                                       and a per-frame array of
                                       {index, file, src_width, src_height,
                                       hd_width, hd_height}.
  - tools/hdcomp_previews/*.png     : a couple of PNG previews (one enemy
                                       frame, one ship frame) composited
                                       over mid-grey, for eyeballing.

Standard library only (no Pillow/numpy). Offline tooling; does not touch
the engine, only reads from ./tyrian21 and writes new files there (plus a
couple of preview PNGs under tools/).

Usage:
  python3 tools/hd_extract_comp.py [--jobs N]

PARALLELIZATION: per-frame decode (cheap RLE unpacking) stays serial in the
main process; the expensive part -- xBRZ 4x upscale + HDPX write -- is farmed
out to a ProcessPoolExecutor, ONE TASK PER SHEET (39 tasks, each covering all
of its sheet's 304 frames), not one task per frame.

Per-frame dispatch was the original design, on the theory that sheets differ
enough in frame complexity to need dynamic load balancing. That rationale no
longer holds: with the C xBRZ kernel (tools/hdkernels.c) one frame costs only
tens of microseconds, so ~11,856 round trips of pickling/IPC dwarfed the
actual work. Per-sheet tasks are naturally balanced anyway -- every sheet has
exactly 304 frames (see the format notes above; that count is structural, not
incidental) -- so each of the 39 tasks carries the same frame budget, and the
only residual variation is per-frame opaque-pixel count, which is noise at C-
kernel speeds. Under the pure-Python fallback (HD_KERNELS=0) a sheet is a much
bigger slug of serial work per task, but the 304-frames-per-task uniformity
still keeps the pool balanced.

Workers write their own .dat files and return only an aggregated per-sheet
result, so nothing travels back per frame except the single preview buffer a
sheet may own. The palette (the only data constant across every frame in the
whole run) is shipped to each worker process once via ProcessPoolExecutor's
initializer/initargs, not re-sent per task.
"""

import argparse
import concurrent.futures
import glob
import json
import math
import os
import struct
import sys
import zlib

import hdkernels

# The C xBRZ kernel (tools/hdkernels.c, built by CMake) when it's available,
# else None and the pure-Python scaler below runs instead -- byte-identical
# output either way. See tools/hdkernels.py.
_KERNELS = hdkernels.load()

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(REPO_ROOT, "tyrian21")
PALETTE_PATH = os.path.join(DATA_DIR, "palette.dat")
SHP_PATH = os.path.join(DATA_DIR, "tyrian.shp")
MANIFEST_PATH = os.path.join(DATA_DIR, "hd_comp_manifest.json")
PREVIEW_DIR = os.path.join(REPO_ROOT, "tools", "hdcomp_previews")

SCALE = 4
TILE_W, TILE_H = 12, 14  # fixed Sprite2 tile canvas size (see format notes above)

MAIN_PALETTE_INDEX = 0  # palettes[0], the main in-flight game palette

# tyrian.shp header, same layout JE_loadMainShapeTables() reads.
SHP_NUM = 12
# 0-based block indices 7..11 (of 0..11) are the five Sprite2 comp sheets;
# see JE_loadMainShapeTables() in src/sprite.c.
SHP_COMP_BLOCKS = [
    (7, "sheet8"),   # spriteSheet8:  player shot sprites
    (8, "sheet9"),   # spriteSheet9:  player ship sprites
    (9, "sheet10"),  # spriteSheet10: power-up sprites
    (10, "sheet11"), # spriteSheet11: coins, datacubes, etc.
    (11, "sheet12"), # spriteSheet12: more player shot sprites
]

# Whole-file Sprite2 sheets (see src/destruct.c, src/game_menu.c/menus.c,
# src/tyrian2.c call sites for JE_loadCompShapes()).
WHOLE_FILE_SHEETS = [
    ("newsh1.shp", "shop"),       # shopSpriteSheet
    ("newsh6.shp", "explosion"),  # explosionSpriteSheet
    ("newsh~.shp", "destruct"),   # destructSpriteSheet
]

# Filenames already claimed by WHOLE_FILE_SHEETS above; every other
# newsh*.shp present in the data dir is an enemy Sprite2 sheet (loaded
# dynamically into enemySpriteSheets[0..3] at runtime via shapeFile[] in
# src/lvlmast.c -- see the big format comment above for why this tool
# extracts them individually rather than as 4 fixed banks).
_CLAIMED_NEWSH = {name for name, _ in WHOLE_FILE_SHEETS}

_ENEMY_ID_OVERRIDES = {
    "#": "hash",
    "^": "caret",
    "~": "tilde",
}


def _enemy_sheet_name(basename):
    # basename like "newsh0.shp", "newsha.shp", "newsh#.shp"
    suffix = basename[len("newsh"):-len(".shp")]
    safe = _ENEMY_ID_OVERRIDES.get(suffix, suffix)
    return "enemy_%s" % safe


# ---------------------------------------------------------------------------
# VENDORED from tools/hd_extract.py (kept standalone/parallel-safe -- see
# module docstring). Do not diverge from upstream behavior when editing;
# if hd_extract.py's version changes, re-sync manually.
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


def colorize_rgba(indices, palette):
    """
    indices: flat sequence of palette index ints (0..255), length w*h.
    Index 0 is treated as the transparency key (alpha 0), per this tool's
    brief -- matching the convention used elsewhere in the engine's other
    sprite formats.
    """
    rgba = bytearray(len(indices) * 4)
    for idx, pixel_index in enumerate(indices):
        o = idx * 4
        if pixel_index == 0:
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


# --- xBRZ 4x scaler (ARGB), ported from the xBRZ reference implementation
# (Zenju's xbrz.cpp) -- vendored verbatim from tools/hd_extract.py. See
# that file's "STEP 4b" block comment for the full deviations list; the
# short version: only ARGB @ 4x is implemented, no precomputed distance
# LUT, no multithreading -- none of that changes the algorithm's output.

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
    xBRZ-scale an RGBA buffer (as produced by colorize_rgba()) by 4x,
    returning a new RGBA bytearray of size (src_w*4) * (src_h*4) * 4.
    """
    if _KERNELS is not None:
        return _KERNELS.xbrz_scale_4x(rgba, src_w, src_h)

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


def write_hdpx_asset(path, pixels, width, height, channels=4):
    """
    Write the HDPX raw-asset format: ASCII "HDPX", u32 LE width, u32 LE
    height, then width*height RGBA8888 pixels. `channels` is 4 for all
    sheets extracted here (alpha carries real transparency).
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


def rgba_over_grey(rgba, width, height, grey=128):
    """Composite an RGBA buffer over a flat mid-grey background -> RGB, for
    PNG previews (so transparency is visible instead of showing as black)."""
    out = bytearray(width * height * 3)
    for i in range(width * height):
        so = i * 4
        do = i * 3
        r, g, b, a = rgba[so], rgba[so + 1], rgba[so + 2], rgba[so + 3]
        if a == 255:
            out[do] = r
            out[do + 1] = g
            out[do + 2] = b
        elif a == 0:
            out[do] = out[do + 1] = out[do + 2] = grey
        else:
            out[do] = (r * a + grey * (255 - a)) // 255
            out[do + 1] = (g * a + grey * (255 - a)) // 255
            out[do + 2] = (b * a + grey * (255 - a)) // 255
    return out


# ---------------------------------------------------------------------------
# NEW: Sprite2 ("compiled shapes") decode -- see the big format comment at
# the top of this file for the derivation.
# ---------------------------------------------------------------------------

class DecodeError(Exception):
    pass


def sprite2_frame_count(data):
    """frame_count = offsets[0] // 2 (see format notes)."""
    if len(data) < 2:
        raise DecodeError("sheet too small to contain even one offset entry")
    off0 = struct.unpack_from("<H", data, 0)[0]
    if off0 == 0 or off0 % 2 != 0:
        raise DecodeError("offsets[0]=%d is not a sane frame-table byte length" % off0)
    return off0 // 2


def decode_comp_frame(data, offset, width=TILE_W, height=TILE_H):
    """
    Decode one Sprite2 RLE frame starting at byte `offset` in `data` into a
    flat width*height list of raw palette-index ints (0 = background/
    transparent, whether via an explicit skip or a written index-0 byte).
    Returns (indices, bytes_consumed_including_terminator).
    Raises DecodeError on any structural anomaly (overrun, row/col
    overflow) rather than silently emitting garbage.
    """
    n = len(data)
    buf = [0] * (width * height)
    i = offset
    row = 0
    col = 0

    while True:
        if i >= n:
            raise DecodeError("overrun while reading opcode at %d" % i)
        b = data[i]
        i += 1
        if b == 0x0F:
            break

        skip = b & 0x0F
        count = (b >> 4) & 0x0F
        col += skip

        if count == 0:
            # next-row marker
            row += 1
            col = 0
            if row > height:
                raise DecodeError("row overflow (row=%d > %d) at opcode offset %d" % (row, height, i - 1))
            continue

        if col + count > width:
            raise DecodeError(
                "col overflow (col=%d + count=%d > %d) at opcode offset %d" % (col, count, width, i - 1))
        if i + count > n:
            raise DecodeError("overrun while reading %d run bytes at %d" % (count, i))

        for _ in range(count):
            val = data[i]
            i += 1
            buf[row * width + col] = val
            col += 1

    return buf, i - offset


def load_sprite2_sheet(data):
    """Given a whole Sprite2_array blob, return (frame_count, offsets)."""
    frame_count = sprite2_frame_count(data)
    offsets = struct.unpack_from("<%dH" % frame_count, data, 0)
    return frame_count, offsets


# ---------------------------------------------------------------------------
# tyrian.shp block-table parsing (mirrors JE_loadMainShapeTables() in
# src/sprite.c; NOT the same table layout as the Sprite2 offset table above
# -- this is the outer container that groups several Sprite/Sprite2 tables
# into one file).
# ---------------------------------------------------------------------------

def load_shp_block_offsets(path, shp_num=SHP_NUM):
    with open(path, "rb") as f:
        data = f.read()

    shp_numb = struct.unpack_from("<H", data, 0)[0]
    if shp_numb != shp_num:
        raise DecodeError("tyrian.shp: expected %d block offsets, header says %d" % (shp_num, shp_numb))

    shp_pos = list(struct.unpack_from("<%di" % shp_num, data, 2))
    shp_pos.append(len(data))
    return data, shp_pos


# ---------------------------------------------------------------------------
# Extraction driver
# ---------------------------------------------------------------------------

def decode_sheet_frames(sheet_name, data, want_preview=False):
    """
    Decode every frame of a Sprite2 sheet into flat palette-index buffers.
    This is pure RLE unpacking -- cheap relative to the xBRZ upscale -- so it
    stays serial in the main process; only the colorize+upscale+write of the
    sheet's frames (see _process_sheet_worker below) is farmed out to the pool.

    `data`: the Sprite2_array's raw bytes (already sliced to just this
    sheet's span, if it came from a multi-block container like tyrian.shp).

    Returns None on a structural decode failure serious enough to abort just
    this sheet. Otherwise returns (frame_count, bad_frames, frame_records,
    sheet_tasks):
      - frame_records: manifest entries in index order, for every frame that
        decoded successfully. These are fully determined by (sheet_name,
        index) -- file name and HD dimensions don't depend on the pool's
        output -- so they're built here rather than after processing.
      - sheet_tasks: (index, indices, need_preview) tuples, one per
        successfully-decoded frame; the whole list is handed to the process
        pool as ONE task (see _process_sheet_worker). need_preview is True for
        at most one frame per sheet: the first non-blank (any nonzero index)
        decoded frame, when want_preview is set -- matching the original serial
        code's "grab the first one as this sheet's preview" behavior.
    """
    try:
        frame_count, offsets = load_sprite2_sheet(data)
    except DecodeError as e:
        print("error: %s: failed to read frame table: %s" % (sheet_name, e), file=sys.stderr)
        return None

    bad_frames = 0
    decoded = []  # (index, indices) for successfully-decoded frames
    for index, off in enumerate(offsets):
        try:
            indices, _consumed = decode_comp_frame(data, off)
        except DecodeError as e:
            print("  warning: %s frame %d: decode error: %s" % (sheet_name, index, e), file=sys.stderr)
            bad_frames += 1
            continue
        decoded.append((index, indices))

    preview_index = None
    if want_preview:
        for index, indices in decoded:
            if any(v != 0 for v in indices):
                preview_index = index
                break

    hd_w, hd_h = TILE_W * SCALE, TILE_H * SCALE
    frame_records = []
    sheet_tasks = []
    for index, indices in decoded:
        out_name = "hdcomp_%s_%02d.dat" % (sheet_name, index)
        frame_records.append({
            "index": index,
            "file": out_name,
            "src_width": TILE_W,
            "src_height": TILE_H,
            "hd_width": hd_w,
            "hd_height": hd_h,
        })
        sheet_tasks.append((index, indices, index == preview_index))

    return frame_count, bad_frames, frame_records, sheet_tasks


# ---------------------------------------------------------------------------
# Parallel per-sheet processing
#
# Colorizing + xBRZ-upscaling + HDPX-writing one frame is independent of every
# other frame, but a frame is far too small to be a useful unit of dispatch
# (~10-20us of work with the C kernel), so the pool's unit is a whole SHEET:
# all 304 of its frames, done back to back inside one worker call. See the
# module docstring's PARALLELIZATION section for why per-sheet is balanced.
#
# The palette is the only data constant across every task in the whole run
# (there's no per-sheet resampling table here, unlike hd_extract_anim.py's
# Lanczos taps -- xBRZ needs no precomputed table), so it's the only thing
# shipped via initializer/initargs rather than re-pickled per task. Windows
# has no fork(), so the pool uses "spawn": worker processes start fresh and
# re-import this module, so all per-worker state must arrive via initargs or
# be a plain module-level constant.
# ---------------------------------------------------------------------------

_worker_palette = None


def _init_worker(palette):
    global _worker_palette
    _worker_palette = palette


def _process_sheet_worker(sheet_name, sheet_tasks):
    """
    Runs in a worker process. For every frame of one sheet -- sheet_tasks is
    the (index, indices, need_preview) list built by decode_sheet_frames() --
    colorizes + xBRZ-upscales + writes the .dat file, using the palette
    stashed by _init_worker().

    Returns (sheet_name, preview), where preview is (index, upscaled) for the
    one frame that had need_preview set, else None. Nothing else travels back:
    the parent already knows each frame's out_name/dimensions (deterministic
    from sheet_name/index, recorded in decode_sheet_frames()), so the only
    payload worth shipping is that single (small, 48x56 RGBA = ~10KB) buffer.
    """
    hd_w, hd_h = TILE_W * SCALE, TILE_H * SCALE
    preview = None

    for index, indices, need_preview in sheet_tasks:
        rgba = colorize_rgba(indices, _worker_palette)
        upscaled = xbrz_scale_4x(rgba, TILE_W, TILE_H)

        out_name = "hdcomp_%s_%02d.dat" % (sheet_name, index)
        out_path = os.path.join(DATA_DIR, out_name)
        write_hdpx_asset(out_path, upscaled, hd_w, hd_h, channels=4)

        if need_preview:
            preview = (index, upscaled)

    return sheet_name, preview


def main():
    parser = argparse.ArgumentParser(
        description="Extract HD frame assets from tyrian21's compiled Sprite2 sheets.")
    parser.add_argument("--jobs", type=int, default=(os.cpu_count() or 1),
                         help="number of worker processes for the per-sheet xBRZ upscale (default: os.cpu_count())")
    args = parser.parse_args()

    print(hdkernels.describe())

    os.makedirs(DATA_DIR, exist_ok=True)

    if not os.path.isfile(PALETTE_PATH):
        print("error: palette.dat not found at %s -- cannot proceed" % PALETTE_PATH, file=sys.stderr)
        return 1

    palette = load_palette(PALETTE_PATH, MAIN_PALETTE_INDEX)

    manifest = {}
    all_tasks = []  # (sheet_name, sheet_tasks) -- ONE entry per sheet, see _process_sheet_worker
    total_written = 0
    total_failed = 0
    sheet_summaries = []  # (name, src, frame_count, written, failed) for the report

    def record_sheet(sheet_name, src, result):
        # Records manifest/summary data immediately from the (serial) decode
        # step -- these fields don't depend on the pool's output, see
        # decode_sheet_frames()'s docstring -- and queues the sheet's frame
        # list as a single pool task.
        nonlocal total_written, total_failed
        if result is None:
            return
        frame_count, bad_frames, frame_records, sheet_tasks = result
        manifest[sheet_name] = {
            "frame_count": frame_count,
            "frames_written": len(frame_records),
            "frames_failed": bad_frames,
            "frames": frame_records,
        }
        all_tasks.append((sheet_name, sheet_tasks))
        total_written += len(frame_records)
        total_failed += bad_frames
        sheet_summaries.append((sheet_name, src, frame_count, len(frame_records), bad_frames))

    # --- five Sprite2 blocks inside tyrian.shp ---
    if not os.path.isfile(SHP_PATH):
        print("note: %s not found, skipping sheet8..sheet12" % SHP_PATH, file=sys.stderr)
    else:
        try:
            data, shp_pos = load_shp_block_offsets(SHP_PATH)
        except DecodeError as e:
            print("error: %s: %s" % (SHP_PATH, e), file=sys.stderr)
            data, shp_pos = None, None

        if data is not None:
            for block_index, sheet_name in SHP_COMP_BLOCKS:
                start = shp_pos[block_index]
                end = shp_pos[block_index + 1]
                block = data[start:end]
                print("Sheet %-14s tyrian.shp[%d:%d] (%d bytes) ..." % (sheet_name, start, end, end - start))
                # sheet9 = player ship sprites; grab a preview frame from it.
                result = decode_sheet_frames(sheet_name, block, want_preview=(sheet_name == "sheet9"))
                record_sheet(sheet_name, "tyrian.shp", result)

    # --- whole-file shop/explosion/destruct sheets ---
    for filename, sheet_name in WHOLE_FILE_SHEETS:
        path = os.path.join(DATA_DIR, filename)
        if not os.path.isfile(path):
            print("note: %s not found, skipping %s" % (path, sheet_name), file=sys.stderr)
            continue
        with open(path, "rb") as f:
            data = f.read()
        print("Sheet %-14s %s (%d bytes) ..." % (sheet_name, filename, len(data)))
        result = decode_sheet_frames(sheet_name, data)
        record_sheet(sheet_name, filename, result)

    # --- every remaining newsh*.shp is an enemy Sprite2 sheet ---
    enemy_paths = sorted(
        p for p in glob.glob(os.path.join(DATA_DIR, "newsh*.shp"))
        if os.path.basename(p) not in _CLAIMED_NEWSH
    )
    if not enemy_paths:
        print("note: no enemy newsh*.shp files found (beyond shop/explosion/destruct)", file=sys.stderr)

    for enemy_i, path in enumerate(enemy_paths):
        filename = os.path.basename(path)
        sheet_name = _enemy_sheet_name(filename)
        with open(path, "rb") as f:
            data = f.read()
        print("Sheet %-14s %s (%d bytes) ..." % (sheet_name, filename, len(data)))
        # grab a preview frame from the first enemy sheet encountered
        result = decode_sheet_frames(sheet_name, data, want_preview=(enemy_i == 0))
        record_sheet(sheet_name, filename, result)

    # --- farm the sheets out to one process pool, one task per sheet (each
    # doing all 304 of its frames); see the "Parallel per-sheet processing"
    # block comment above. chunksize=1 keeps dispatch dynamic, which costs
    # nothing at 39 tasks and lets a worker that finishes early pick up the
    # next sheet instead of sitting on a pre-assigned batch. ---
    preview_targets = {}  # sheet_name -> (index, upscaled_rgba, hd_w, hd_h)
    if all_tasks:
        jobs = max(1, args.jobs)
        hd_w, hd_h = TILE_W * SCALE, TILE_H * SCALE
        with concurrent.futures.ProcessPoolExecutor(
                max_workers=jobs, initializer=_init_worker, initargs=(palette,)) as executor:
            # executor.map() preserves submission order in its result order
            # (regardless of which worker finishes first), so preview_targets
            # ends up populated in the same order the serial code produced it.
            for sheet_name, preview in executor.map(
                    _process_sheet_worker, *zip(*all_tasks), chunksize=1):
                if preview is not None:
                    index, upscaled = preview
                    preview_targets[sheet_name] = (index, upscaled, hd_w, hd_h)

    with open(MANIFEST_PATH, "w") as f:
        json.dump({
            "tile_width": TILE_W,
            "tile_height": TILE_H,
            "scale": SCALE,
            "palette_index": MAIN_PALETTE_INDEX,
            "sheets": manifest,
        }, f, indent=2)
    print("Wrote manifest: %s" % MANIFEST_PATH)

    # --- previews ---
    if preview_targets:
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        for sheet_name, (index, upscaled, hd_w, hd_h) in preview_targets.items():
            rgb = rgba_over_grey(upscaled, hd_w, hd_h)
            preview_path = os.path.join(PREVIEW_DIR, "hdcomp_%s_%02d.png" % (sheet_name, index))
            write_png(preview_path, rgb, hd_w, hd_h)
            print("Wrote preview: %s" % preview_path)

    # --- summary report ---
    print()
    print("=== Summary ===")
    for name, src, fc, w, bad in sheet_summaries:
        flag = "  <-- DECODE ISSUES" if bad else ""
        print("  %-14s src=%-14s frames=%-4d written=%-4d failed=%d%s" % (name, src, fc, w, bad, flag))
    print("Total frames written: %d, failed: %d" % (total_written, total_failed))

    return 0 if total_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
