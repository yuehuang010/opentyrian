#!/usr/bin/env python3
"""
hd_extract_filter.py -- Offline HD asset extraction tool that emits
"brightness-map" assets for OpenTyrian's in-flight `_filter`-tinted Sprite2
sheets (the Phase-D HD recolor crux).

STANDALONE / PARALLEL-SAFE: this file is intentionally self-contained and
touches ONLY itself. It vendors (copies -- does not import) the HDPX writer,
palette loader, xBRZ 4x scaler, minimal PNG writer, and the Sprite2 decoder
from tools/hd_extract_comp.py (which itself vendors from tools/hd_extract.py)
so this tool can run concurrently with another agent editing those files
without any import race or partial-file import error. It writes ONLY NEW
output files named `hdcompb_<sheet>_NN.dat` (the trailing `b` = brightness)
and NEVER touches any existing `hdcomp_*.dat` asset written by
hd_extract_comp.py -- those are colorized-at-palette-0 RGBA assets for a
DIFFERENT (non-filtered) draw path and a concurrent agent may be reading/
writing them right now.

--------------------------------------------------------------------------
WHY THIS EXISTS -- how `_filter` recoloring works at runtime
(src/sprite.c blit_sprite2_filter(), confirmed by direct read):

    *pixels = filter | (*data & 0x0f);

Each opaque Sprite2 RLE byte's LOW nibble is not a real palette index at
all -- it's a 0-15 "brightness" level baked into the original 12x14 sprite
art. The HIGH nibble is thrown away and replaced at blit time with the
`filter` byte (a caller-supplied "color band" select, e.g. enemy[i].filter).
The final displayed color is `palette[filter | brightness]`, i.e. Tyrian's
256-color palette is laid out as 16 bands of 16 shades, and `_filter` lets
one 12x14 sprite be re-shaded to any of those 16 bands (iced/frozen status,
boss-bar flash, Magneto RePulse, etc.) without needing separate art per
band -- just by swapping which band's 16-shade ramp the brightness index
looks up into.

At native 8-bit VGA resolution this is a trivial reinterpret of the same
byte. At HD/truecolor resolution it is NOT reproducible by upscaling the
already-palette-0-colorized `hdcomp_*` RGBA assets (those assets have baked
in band 0's colors -- there is no way to recover "which of the 16 shades
was this pixel" from an RGBA pixel that's already been through one specific
band's ramp, especially post-xBRZ blending which mixes neighboring colors).
The fix: extract the RAW BRIGHTNESS (low nibble, 0-15) as its own per-pixel
field, encode it as a smooth grayscale image, xBRZ-upscale THAT (xBRZ only
ever blends existing input values together, so upscaling brightness first
and mapping through a band ramp after is equivalent -- to first order -- to
mapping-then-upscaling, since adjacent brightness levels in the same band
are colinear samples along that band's ramp). Alpha carries the normal
opaque/transparent cutout, exactly like hdcomp_*. At runtme the engine can
then do, per HD pixel: `output_rgb = live_palette[band | round(v * 15/255)]`
(or interpolate between the two nearest band shades for extra smoothness)
using the CURRENT live palette, so palette fades/flashes still work.

--------------------------------------------------------------------------
WHICH SHEETS ACTUALLY USE `_filter` (evidence, confirmed by direct grep +
read of the call graph -- there is exactly ONE call site tree):

  - src/sprite.c:788  blit_sprite2_filter()      -- the primitive itself.
  - src/sprite.c:824  blit_sprite2_filter_clip()  -- clipped variant, same
                       nibble-swap logic; no call sites found in src/ at
                       all (grep for the symbol turns up only its own
                       definition and the .h declaration) -- effectively
                       dead code today, but decoded identically so nothing
                       extra is needed if it's ever wired up.
  - src/interp.c:306,308  INTERP_SPRITE2_FILTER(_CLIP) dispatch -- this is
                       just the high-fps interpolation replay of whatever
                       blit_sprite2_filter() already recorded; not an
                       independent draw path.
  - src/tyrian2.c:190  blit_enemy() -- THE only real caller:
                           if (enemy[i].filter != 0)
                               blit_sprite2_filter(surface, x, y,
                                   *enemy[i].sprite2s, index, enemy[i].filter);
                           else
                               blit_sprite2(surface, x, y, *enemy[i].sprite2s, index);
                       i.e. enemies draw via `_filter` whenever a nonzero
                       band has been set on them, and via the plain
                       (already-covered-by-hd_extract_comp.py) path
                       otherwise. `enemy[i].sprite2s` is always one of the
                       34 dynamically-loaded `newsh?.shp` banks (see
                       src/lvlmast.c shapeFile[34]), i.e. exactly the
                       "enemy_<id>" sheet set hd_extract_comp.py already
                       enumerates (every newsh*.shp not claimed by
                       shop/newsh1, explosion/newsh6, destruct/newsh~ --
                       none of shapeFile[]'s 34 letters is '1', '6', or
                       '~', so there is no overlap to worry about).

  grep across src/*.c for other blit_sprite2_filter call sites (shots.c,
  destruct.c, game_menu.c, menus.c) found NONE -- player shots, ships,
  power-ups, shop icons, explosions, and Destruct units are ALL drawn via
  plain blit_sprite2()/blit_sprite2x2*() (no filter), so hd_extract_comp.py's
  existing hdcomp_sheet8..12 / shop / explosion / destruct assets are
  already final for those and need no brightness-map counterpart.

  CONCLUSION: brightness maps are needed for, and only for, the enemy_<id>
  sheets (every newsh*.shp except newsh1/newsh6/newsh~). This is NOT a
  rarely-used path -- iced enemies (filter=0x09, src/tyrian2.c:375),
  Magneto RePulse (filter=0x70, src/tyrian2.c:440), boss-bar-linked
  ground-enemy tinting (src/tyrian2.c:1548,1558, value from a local temp2),
  and the "Enemy Global AccelRev" level-scripting event (event type 27,
  src/tyrian2.c:4714, `eventdat3` in 1..16 straight from level data) all
  drive it, so ordinary levels exercise this regularly. This phase IS worth
  wiring up.

  CAVEAT on enumerating every band value used: 0x09 and 0x70 are literal
  constants found by grep; the event-27 path can supply any value in
  1..16 from arbitrary level data (`eventdat3`), and the boss-bar path
  (`temp2`) is itself a caller-supplied parameter threaded through several
  functions -- fully enumerating every band any level ever uses would
  require parsing all `levels?.dat` event streams (out of scope for this
  tool). This is fine: the manifest just records the literal constants as
  evidence, and notes that the engine should synthesize `palette[band |
  brightness]` for whatever band value it encounters at runtime rather than
  needing a precomputed list.

--------------------------------------------------------------------------
BRIGHTNESS ENCODING + xBRZ CAVEAT

Per opaque pixel: `brightness = sprite_byte & 0x0F` (0-15, the same nibble
blit_sprite2_filter() ORs the runtime band into). Encoded as grayscale RGBA
`(v, v, v, 255)` with `v = round(brightness * 255 / 15)` (so 0->0, 15->255,
exactly reversible by `round(v * 15 / 255)`); transparent source pixels
(raw index 0, matching hd_extract_comp.py's convention) -> RGBA all-zero
(alpha 0). This buffer is then xBRZ 4x upscaled exactly like the color
assets, using the SAME vendored scaler -- xBRZ only mixes/blends existing
neighboring input pixel values (never invents new ones out of palette
range), so smooth-brightness-then-band-lookup at runtime stays a a valid
approximation of what band-lookup-then-smooth-color would have produced.

The one real caveat: mapping a smoothly-interpolated brightness value
through a DISCRETE 16-shade band ramp (`palette[band | brightness]`) can
reintroduce banding/aliasing at HD resolution if the engine just rounds to
the nearest integer shade, since xBRZ produces a continuum of v in [0,255]
but the ramp only has 16 real palette entries to sample. The recommended
runtime mitigation (left for the engine-side wiring, not implemented here):
linearly interpolate between the two nearest shades,
`lerp(palette[band|floor], palette[band|ceil], frac)`, rather than a hard
round -- this keeps xBRZ's smooth edges smooth in the final recolored
output instead of re-quantizing them back down to 16 flat steps.

--------------------------------------------------------------------------
OUTPUT

  - tyrian21/hdcompb_<sheet>_NN.dat   : one HDPX RGBA brightness-map asset
                                         per frame (NN = 2-digit zero-padded
                                         frame index) for every enemy_<id>
                                         sheet, 4x xBRZ-upscaled from the
                                         12x14 source tile to 48x56. R==G==B
                                         carries the smooth brightness;
                                         alpha carries the cutout. NEVER
                                         overwrites any hdcomp_<sheet>_NN.dat
                                         (different filename, different
                                         directory entries entirely).
  - tyrian21/hd_filter_manifest.json  : which sheets got brightness maps,
                                         per-sheet frame counts, and the
                                         statically-discoverable band/filter
                                         literal values (see caveat above).
  - tools/hdcompb_previews/*.png      : 1-2 preview PNGs -- one enemy frame's
                                         brightness map reconstructed under
                                         a couple of representative bands
                                         (0x10, 0x60) via palette.dat slot 0,
                                         composited over mid-grey.

Standard library only (no Pillow/numpy). Offline tooling; reads from
./tyrian21 and writes only NEW files there (plus new preview PNGs under
tools/).

Usage:
  python3 tools/hd_extract_filter.py
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
MANIFEST_PATH = os.path.join(DATA_DIR, "hd_filter_manifest.json")
PREVIEW_DIR = os.path.join(REPO_ROOT, "tools", "hdcompb_previews")

SCALE = 4
TILE_W, TILE_H = 12, 14  # fixed Sprite2 tile canvas size

MAIN_PALETTE_INDEX = 0  # palette.dat slot 0, per the task brief

# Whole-file Sprite2 sheets that are NOT enemy banks (claimed by other
# draw paths that never call blit_sprite2_filter() -- see report above).
_CLAIMED_NEWSH = {"newsh1.shp", "newsh6.shp", "newsh~.shp"}

_ENEMY_ID_OVERRIDES = {
    "#": "hash",
    "^": "caret",
    "~": "tilde",
}

# Statically-discoverable `filter`/band literal values (see big comment
# above for source lines); event-27 / temp2-driven values are data-driven
# and not enumerable here -- see manifest "band_values_note".
KNOWN_BAND_VALUES = {
    "0x09": "iced/frozen enemy tint (src/tyrian2.c:375, enemy[i].iced path)",
    "0x70": "Magneto RePulse tint (src/tyrian2.c:440, difficulty > EASY, j==3)",
    "0x01..0x10": ("event type 27 'Enemy Global AccelRev', eventdat3 in 1..16 "
                   "(src/tyrian2.c:4714) -- exact value is level-data-driven"),
}


def _enemy_sheet_name(basename):
    # basename like "newsh0.shp", "newsha.shp", "newsh#.shp"
    suffix = basename[len("newsh"):-len(".shp")]
    safe = _ENEMY_ID_OVERRIDES.get(suffix, suffix)
    return "enemy_%s" % safe


# ---------------------------------------------------------------------------
# VENDORED from tools/hd_extract_comp.py / tools/hd_extract.py (kept
# standalone/parallel-safe -- see module docstring). Do not diverge from
# upstream behavior when editing; if the source files' versions change,
# re-sync manually.
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
        return (v6 << 2) | (v6 >> 4)

    palette = []
    for i in range(256):
        r6, g6, b6 = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
        palette.append((expand6to8(r6), expand6to8(g6), expand6to8(b6)))
    return palette


# --- xBRZ 4x scaler (ARGB), ported from the xBRZ reference implementation
# (Zenju's xbrz.cpp) -- vendored verbatim from tools/hd_extract_comp.py /
# tools/hd_extract.py.

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
    xBRZ-scale an RGBA buffer by 4x, returning a new RGBA bytearray of size
    (src_w*4) * (src_h*4) * 4. Works identically whether the buffer holds
    real colors or (as here) a grayscale brightness field -- xBRZ only ever
    mixes/blends existing input pixel values.
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
    height, then width*height RGBA8888 pixels.
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
    """Composite an RGBA buffer over a flat mid-grey background -> RGB."""
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
# VENDORED Sprite2 ("compiled shapes") decode -- see tools/hd_extract_comp.py
# for the full derivation of this format from src/sprite.c.
# ---------------------------------------------------------------------------

class DecodeError(Exception):
    pass


def sprite2_frame_count(data):
    """frame_count = offsets[0] // 2 (see format notes in hd_extract_comp.py)."""
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
    transparent). Returns (indices, bytes_consumed_including_terminator).
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
# NEW: brightness-field encoding (this tool's actual purpose)
# ---------------------------------------------------------------------------

def brightness_rgba(indices):
    """
    indices: flat sequence of raw Sprite2 palette-index ints (0..255),
    length w*h -- the RAW decoded bytes, i.e. NOT run through a palette.
    Per the `_filter` blit's `filter | (byte & 0x0f)`, only the low nibble
    is real "brightness" (0-15); index 0 is treated as transparent (same
    convention as hd_extract_comp.py's colorize_rgba()).
    Returns a grayscale RGBA bytearray, v = round(brightness * 255 / 15).
    """
    rgba = bytearray(len(indices) * 4)
    for idx, raw in enumerate(indices):
        o = idx * 4
        if raw == 0:
            rgba[o] = 0
            rgba[o + 1] = 0
            rgba[o + 2] = 0
            rgba[o + 3] = 0
        else:
            brightness = raw & 0x0F
            v = round(brightness * 255 / 15)
            rgba[o] = v
            rgba[o + 1] = v
            rgba[o + 2] = v
            rgba[o + 3] = 255
    return rgba


def reconstruct_band_rgb(indices, palette, band, width, height):
    """
    Debug/preview helper: reconstruct what blit_sprite2_filter() would have
    actually displayed for this frame under a given `band` (filter) byte,
    i.e. palette[band | (raw & 0x0f)] per opaque pixel, composited over
    mid-grey. Used only to sanity-check the brightness-map approach in the
    preview PNGs; NOT part of the shipped asset pipeline.
    """
    out = bytearray(width * height * 3)
    for idx, raw in enumerate(indices):
        do = idx * 3
        if raw == 0:
            out[do] = out[do + 1] = out[do + 2] = 128
        else:
            brightness = raw & 0x0F
            r, g, b = palette[(band | brightness) & 0xFF]
            out[do] = r
            out[do + 1] = g
            out[do + 2] = b
    return out


# ---------------------------------------------------------------------------
# Extraction driver
#
# Parallelization unit: one Sprite2 FRAME (across ALL sheets), not one sheet.
# Frame counts and per-frame opaque-pixel counts vary a lot between enemy
# sheets, and decode/xBRZ cost scales with opaque pixel count, so chunking by
# sheet would repeat mkbundle.py's load-imbalance trap (one worker stuck with
# a disproportionately expensive sheet while the rest idle). Instead every
# frame from every sheet is queued as its own task and handed to
# executor.map() with chunksize=1 so the pool dynamically self-balances.
#
# Each worker writes its own hdcompb_*.dat file directly and returns only
# small metadata, EXCEPT for frames belonging to the first enemy sheet, which
# also return their raw decoded indices (need_raw=True) -- the serial code's
# preview-target selection is "first sheet, first frame with any nonzero raw
# index", and that can't be known ahead of decode, so the cheapest way to
# preserve exact selection semantics is to have all of that one sheet's
# (small, 12x14) frames ship their raw indices back and let the parent pick.
# ---------------------------------------------------------------------------

# Per-worker cache of sheet file bytes, keyed by path. chunksize=1 means a
# worker's tasks are not guaranteed to be contiguous within a sheet, but a
# worker typically still processes several frames from the same sheet over
# its lifetime, so caching avoids redundant re-reads of the same small file.
_worker_sheet_cache = {}


def _get_sheet_data(sheet_path):
    data = _worker_sheet_cache.get(sheet_path)
    if data is None:
        with open(sheet_path, "rb") as f:
            data = f.read()
        _worker_sheet_cache[sheet_path] = data
    return data


def _process_frame_worker(sheet_name, sheet_path, index, offset, need_raw):
    """
    Runs in a worker process. Decodes one Sprite2 frame, builds its
    brightness map, xBRZ-upscales it, and writes the hdcompb_*.dat asset
    directly (minimizing IPC -- only metadata crosses back to the parent).
    Returns a 9-tuple:
      (sheet_name, index, out_name, frame_record, error_msg,
       raw_indices, upscaled, hd_w, hd_h)
    On decode failure, out_name/frame_record are None and error_msg is set.
    raw_indices/upscaled/hd_w/hd_h are only populated when `need_raw` (the
    first enemy sheet), for preview-target selection in the parent.
    """
    data = _get_sheet_data(sheet_path)
    try:
        indices, _consumed = decode_comp_frame(data, offset)
    except DecodeError as e:
        return sheet_name, index, None, None, str(e), None, None, None, None

    rgba = brightness_rgba(indices)
    upscaled = xbrz_scale_4x(rgba, TILE_W, TILE_H)
    hd_w, hd_h = TILE_W * SCALE, TILE_H * SCALE

    out_name = "hdcompb_%s_%02d.dat" % (sheet_name, index)
    out_path = os.path.join(DATA_DIR, out_name)
    # HARD CONSTRAINT: never overwrite an existing hdcomp_* asset. Our
    # filenames use the distinct "hdcompb_" prefix so there is no
    # collision by construction, but assert it defensively anyway.
    assert out_name.startswith("hdcompb_"), "refusing to write non-hdcompb_ filename"
    write_hdpx_asset(out_path, upscaled, hd_w, hd_h, channels=4)

    frame_record = {
        "index": index,
        "file": out_name,
        "src_width": TILE_W,
        "src_height": TILE_H,
        "hd_width": hd_w,
        "hd_height": hd_h,
    }

    if need_raw:
        return sheet_name, index, out_name, frame_record, None, indices, upscaled, hd_w, hd_h
    return sheet_name, index, out_name, frame_record, None, None, None, None, None


def main():
    parser = argparse.ArgumentParser(
        description="Extract HD enemy brightness-map assets from tyrian21/newsh*.shp.")
    parser.add_argument("--jobs", type=int, default=(os.cpu_count() or 1),
                         help="number of worker processes for the per-frame decode/upscale "
                              "(default: os.cpu_count())")
    args = parser.parse_args()

    print(hdkernels.describe())

    os.makedirs(DATA_DIR, exist_ok=True)

    if not os.path.isfile(PALETTE_PATH):
        print("error: palette.dat not found at %s -- cannot proceed" % PALETTE_PATH, file=sys.stderr)
        return 1

    palette = load_palette(PALETTE_PATH, MAIN_PALETTE_INDEX)

    enemy_paths = sorted(
        p for p in glob.glob(os.path.join(DATA_DIR, "newsh*.shp"))
        if os.path.basename(p) not in _CLAIMED_NEWSH
    )
    if not enemy_paths:
        print("note: no enemy newsh*.shp files found (beyond shop/explosion/destruct)", file=sys.stderr)

    # First pass (cheap, serial): read each sheet just far enough to parse
    # its frame table (offsets), so we know every frame task up front. The
    # expensive per-frame decode/upscale/write happens in the worker pool
    # below, keyed by sheet_name so results can be regrouped in the original
    # enemy_paths order regardless of which worker processed which frame.
    sheet_infos = []  # (sheet_name, filename, frame_count) in enemy_paths order
    tasks = []  # (sheet_name, sheet_path, index, offset, need_raw)
    for enemy_i, path in enumerate(enemy_paths):
        filename = os.path.basename(path)
        sheet_name = _enemy_sheet_name(filename)
        with open(path, "rb") as f:
            data = f.read()
        print("Sheet %-14s %s (%d bytes) ..." % (sheet_name, filename, len(data)))
        try:
            frame_count, offsets = load_sprite2_sheet(data)
        except DecodeError as e:
            print("error: %s: failed to read frame table: %s" % (sheet_name, e), file=sys.stderr)
            continue

        sheet_infos.append((sheet_name, filename, frame_count))
        need_raw = (enemy_i == 0)  # preview target can only come from the first sheet
        for index, off in enumerate(offsets):
            tasks.append((sheet_name, path, index, off, need_raw))

    # Per-sheet accumulators, filled in as frame results stream back from the
    # pool (in submission order, per executor.map()'s ordering guarantee).
    sheet_written = {name: 0 for name, _f, _fc in sheet_infos}
    sheet_bad = {name: 0 for name, _f, _fc in sheet_infos}
    sheet_frames = {name: [] for name, _f, _fc in sheet_infos}
    preview_targets = {}  # sheet_name -> (index, raw_indices, upscaled_rgba, hd_w, hd_h)

    if tasks:
        jobs = max(1, args.jobs)
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as executor:
            # chunksize=1: frame decode cost is roughly proportional to
            # opaque pixel count and varies a lot across sheets/frames, so a
            # larger static chunksize risks the same load-imbalance trap
            # mkbundle.py hit (one worker stuck with a cluster of expensive
            # frames while others idle). Dynamic one-at-a-time dispatch lets
            # the pool self-balance instead.
            results = executor.map(_process_frame_worker, *zip(*tasks), chunksize=1)
            for sheet_name, index, out_name, frame_record, error_msg, raw_indices, upscaled, hd_w, hd_h in results:
                if error_msg is not None:
                    print("  warning: %s frame %d: decode error: %s" % (sheet_name, index, error_msg),
                          file=sys.stderr)
                    sheet_bad[sheet_name] += 1
                    continue

                sheet_written[sheet_name] += 1
                sheet_frames[sheet_name].append(frame_record)

                if (raw_indices is not None and sheet_name not in preview_targets
                        and any(v != 0 for v in raw_indices)):
                    preview_targets[sheet_name] = (index, raw_indices, upscaled, hd_w, hd_h)

    manifest_sheets = {}
    total_written = 0
    total_failed = 0
    sheet_summaries = []  # (name, filename, frame_count, written, failed)
    for sheet_name, filename, frame_count in sheet_infos:
        written = sheet_written[sheet_name]
        bad = sheet_bad[sheet_name]
        manifest_sheets[sheet_name] = {
            "frame_count": frame_count,
            "frames_written": written,
            "frames_failed": bad,
            "frames": sheet_frames[sheet_name],
        }
        total_written += written
        total_failed += bad
        sheet_summaries.append((sheet_name, filename, frame_count, written, bad))

    with open(MANIFEST_PATH, "w") as f:
        json.dump({
            "tile_width": TILE_W,
            "tile_height": TILE_H,
            "scale": SCALE,
            "palette_index_used_for_previews_only": MAIN_PALETTE_INDEX,
            "description": (
                "Brightness-map assets (hdcompb_<sheet>_NN.dat) for enemy "
                "Sprite2 sheets drawn via blit_sprite2_filter() (src/sprite.c, "
                "called only from blit_enemy() in src/tyrian2.c). R=G=B "
                "channel is the smooth (post-xBRZ) brightness in [0,255], "
                "recoverable brightness nibble = round(v * 15 / 255); alpha "
                "carries the opaque/transparent cutout. At runtime the "
                "engine should compute output_rgb = live_palette[band | "
                "brightness_nibble] per pixel for whatever `filter`/band "
                "byte the caller supplies (see band_values_known below for "
                "statically-discoverable examples; bands from level event "
                "data are NOT enumerable ahead of time and must be handled "
                "generically)."
            ),
            "band_values_known": KNOWN_BAND_VALUES,
            "sheets_without_filter_use": (
                "sheet8..sheet12 (player shots/ships/power-ups/coins), shop "
                "(newsh1), explosion (newsh6), destruct (newsh~) -- confirmed "
                "by grep: no call site other than blit_enemy() invokes "
                "blit_sprite2_filter()/_clip(). These already have final "
                "hdcomp_* assets from hd_extract_comp.py and need no "
                "brightness-map counterpart."
            ),
            "sheets": manifest_sheets,
        }, f, indent=2)
    print("Wrote manifest: %s" % MANIFEST_PATH)

    # --- previews: reconstruct one enemy frame under two representative bands ---
    if preview_targets:
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        for sheet_name, (index, raw_indices, upscaled, hd_w, hd_h) in preview_targets.items():
            # 1) the brightness map itself, visualized directly (grayscale).
            gray_rgb = rgba_over_grey(upscaled, hd_w, hd_h)
            gray_path = os.path.join(PREVIEW_DIR, "hdcompb_%s_%02d_brightness.png" % (sheet_name, index))
            write_png(gray_path, gray_rgb, hd_w, hd_h)
            print("Wrote preview: %s" % gray_path)

            # 2) reconstructed under two representative bands, at native
            #    12x14 res (reconstruction is per-raw-index, not upscaled --
            #    this is a correctness eyeball check on the band mapping,
            #    not a demo of the HD upscale itself).
            for band in (0x10, 0x60):
                recon_rgb = reconstruct_band_rgb(raw_indices, palette, band, TILE_W, TILE_H)
                recon_path = os.path.join(
                    PREVIEW_DIR, "hdcompb_%s_%02d_band_0x%02x.png" % (sheet_name, index, band))
                write_png(recon_path, recon_rgb, TILE_W, TILE_H)
                print("Wrote preview: %s" % recon_path)

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
