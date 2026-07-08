#!/usr/bin/env python3
"""
hd_extract_tiles.py -- Offline HD asset extraction tool for OpenTyrian's
level-tileset graphics (Phase S3 of internal/plan/STANDALONE_PLAN.md).

Extracts the five 24x28 8-bit indexed tileset banks (`shapes).dat`,
`shapesw.dat`, `shapesx.dat`, `shapesy.dat`, `shapesz.dat`) that back
in-flight level backgrounds (see src/backgrnd.c, src/tyrian2.c around line
3110), colorizes each tile with every one of the 23 palette.dat slots (or a
requested subset), 4x-upscales, and packs all 600 tiles of a given
(bank, palette) pair into a single HDPX "grid atlas" asset -- 20 columns x
30 rows of 96x112 cells (1920x3360 RGBA) -- for the engine's HD tile
compositor (src/video.c, ~line 530) to slice up at draw time.

--------------------------------------------------------------------------
INPUT FORMAT (verified against src/tyrian2.c:3110-3178; do not re-derive --
this header is the source of truth for this tool):

  Each bank file holds exactly 600 tiles, read sequentially from offset 0
  (520 trailing bytes after tile 600 are ignored). Per tile:
    - 1 byte blank flag. Nonzero -> the tile is blank (24x28, fully
      transparent, no further bytes). Zero -> followed by 672 bytes of raw
      8-bit palette indices, row-major, 24 wide x 28 tall. Index 0 is the
      transparency key.

  Verified non-blank/blank tile counts per bank (asserted at decode time):
    shapes).dat: 175 non-blank / 425 blank
    shapesw.dat: 255 non-blank / 345 blank
    shapesx.dat: 322 non-blank / 278 blank
    shapesy.dat: 226 non-blank / 374 blank
    shapesz.dat: 361 non-blank / 239 blank

--------------------------------------------------------------------------
PALETTE (tyrian21/palette.dat): 17664 bytes = 23 palettes x 256 colors x 3
bytes (r, g, b), each channel a 6-bit VGA value (0..63). Expanded to 8-bit
exactly as src/palette.c:60 does: c8 = (c << 2) | (c >> 4) (NOT c*255/63 or
plain c << 2).

--------------------------------------------------------------------------
OUTPUT: one HDPX grid atlas per (bank, palette) processed.

  - Cell size after 4x upscale: 96x112 (24*4 x 28*4). Grid: 20 columns x 30
    rows = 600 cells; atlas image = 1920x3360 RGBA32. Tile z lands at
    column z%20, row z//20 -> pixel origin ((z%20)*96, (z//20)*112). A
    blank tile's cell is fully transparent (0,0,0,0) across the whole
    96x112 block.
  - File layout (matches the engine's HD loader, src/video.c ~line 530):
    4 bytes ASCII magic "HDPX", u32 LE width, u32 LE height, then
    width*height*4 bytes of RGBA8888 pixels, row-major top-to-bottom.
    Header is exactly 4 + 4 + 4 = 12 bytes (NOT 16 -- there is no 4th
    header field). Total file size for a full grid atlas is therefore
    12 + 1920*3360*4 = 25,804,812 bytes.
  - Filename: hdtile_%c_p%02d.dat, where %c is the bank's shape character
    (lowercased exactly as tyrian2.c:3107's tolower(char_shapeFile)) --
    ')', 'w', 'x', 'y', 'z' for the five banks respectively -- and %02d is
    the zero-based palette index (00-22). The close-paren is a perfectly
    valid filename character on macOS/Linux/Windows; it is used here
    literally, unescaped (shell callers just need to quote it).
  - Written into the data dir (tyrian21/ by default) next to the other hd*
    assets, unless --out overrides.

--------------------------------------------------------------------------
DEPENDENCIES: numpy is REQUIRED unconditionally (used for all the per-pixel
palette/upscale math; a pure stdlib nested-loop implementation over
1920x3360x23-palettes of pixels would be prohibitively slow for this tool's
purpose). This is a deliberate deviation from the stdlib-only convention of
tools/hd_extract.py / tools/hd_extract_comp.py, in the same spirit as
tools/hd_upsample_snd.py's numpy/scipy dependency for its own domain.
Pillow is OPTIONAL and imported lazily -- only inside the lanczos upscaler
and the --preview PNG writer -- so `--method nearest` without `--preview`
runs with numpy alone.

Install (matches the tools/.venv_hdtiles convention used for this tool):
  python3 -m venv tools/.venv_hdtiles
  tools/.venv_hdtiles/bin/pip install numpy pillow

--------------------------------------------------------------------------
Usage:
  hd_extract_tiles.py [--data DIR] [--out DIR] [--method {nearest,lanczos}]
                       [--palettes all|N[,N...]] [--banks all|<chars>]
                       [--preview] [--selftest]

  --selftest performs a lossless round-trip check (nearest method only):
  for 3 non-blank tiles per bank, it re-reads the freshly-written atlas
  from disk, downsamples each tile's 96x112 cell back to 24x28 by taking
  the top-left pixel of every 4x4 block, maps each resulting RGBA pixel
  back to a palette index, and asserts the reconstructed 24x28 index array
  is COLOR-exact against the original 672-byte source tile (i.e. every
  reconstructed index resolves to the same (r,g,b) as the original index --
  see build_reverse_palette()'s note on why raw index equality isn't the
  right bar when a palette has duplicate colors). This proves the
  extract -> colorize -> atlas pipeline is lossless under nearest-neighbor
  upscaling.
"""

import argparse
import os
import struct
import sys

import numpy as np

# ---------------------------------------------------------------------------
# Paths / constants
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DATA_DIR = os.path.join(REPO_ROOT, "tyrian21")
PREVIEW_DIR = os.path.join(REPO_ROOT, "tools", "hdtile_previews")

TILES_PER_BANK = 600
TILE_W, TILE_H = 24, 28
TILE_BYTES = TILE_W * TILE_H  # 672
SCALE = 4
CELL_W, CELL_H = TILE_W * SCALE, TILE_H * SCALE  # 96, 112
GRID_COLS, GRID_ROWS = 20, 30  # 20*30 == 600
ATLAS_W, ATLAS_H = GRID_COLS * CELL_W, GRID_ROWS * CELL_H  # 1920, 3360

HDPX_HEADER_BYTES = 4 + 4 + 4  # magic(4) + width u32(4) + height u32(4) == 12
ATLAS_FILE_BYTES = HDPX_HEADER_BYTES + ATLAS_W * ATLAS_H * 4

NUM_PALETTES = 23
PALETTE_ENTRY_BYTES = 256 * 3
PALETTE_TOTAL_BYTES = NUM_PALETTES * PALETTE_ENTRY_BYTES  # 17664

# Ordered list of bank shape-chars and their filenames (tyrian2.c:3107 /
# 3110-3178). Order is fixed so --banks/--palettes output is deterministic.
BANKS_ORDER = [")", "w", "x", "y", "z"]
BANK_FILENAME = {
    ")": "shapes).dat",
    "w": "shapesw.dat",
    "x": "shapesx.dat",
    "y": "shapesy.dat",
    "z": "shapesz.dat",
}

# Ground-truth non-blank tile counts per bank, asserted at decode time (see
# module docstring).
GROUND_TRUTH_NONBLANK = {")": 175, "w": 255, "x": 322, "y": 226, "z": 361}


# ---------------------------------------------------------------------------
# STEP 1: palette.dat -> (256, 3) uint8 array of 8-bit (r, g, b)
# ---------------------------------------------------------------------------

def load_palette(path, palette_index):
    """
    Load one 256-color palette slot from palette.dat, expanding each 6-bit
    VGA channel to 8-bit exactly as JE_loadPals (src/palette.c:60) does:
    c8 = (c << 2) | (c >> 4).
    """
    with open(path, "rb") as f:
        f.seek(palette_index * PALETTE_ENTRY_BYTES)
        raw = f.read(PALETTE_ENTRY_BYTES)
    if len(raw) != PALETTE_ENTRY_BYTES:
        raise ValueError(
            "palette.dat: expected %d bytes for palette %d, got %d"
            % (PALETTE_ENTRY_BYTES, palette_index, len(raw)))

    raw6 = np.frombuffer(raw, dtype=np.uint8).reshape(256, 3)
    # (c << 2) | (c >> 4), vectorized; raw6 values are 0..63 so this stays
    # within uint8 range (max (63<<2)|(63>>4) = 252|3 = 255).
    pal8 = ((raw6.astype(np.uint16) << 2) | (raw6.astype(np.uint16) >> 4)).astype(np.uint8)
    return pal8


# ---------------------------------------------------------------------------
# STEP 2: shapesX.dat -> list[600] of (bytes of length 672) | None (blank)
# ---------------------------------------------------------------------------

def read_bank(path):
    with open(path, "rb") as f:
        data = f.read()

    tiles = []
    pos = 0
    for z in range(TILES_PER_BANK):
        if pos >= len(data):
            raise ValueError("%s: truncated before tile %d (blank-flag byte)" % (path, z))
        flag = data[pos]
        pos += 1
        if flag != 0:
            tiles.append(None)
            continue
        payload = data[pos:pos + TILE_BYTES]
        if len(payload) != TILE_BYTES:
            raise ValueError(
                "%s: truncated tile %d (wanted %d payload bytes, got %d)"
                % (path, z, TILE_BYTES, len(payload)))
        tiles.append(bytes(payload))
        pos += TILE_BYTES

    return tiles


# ---------------------------------------------------------------------------
# STEP 3: one tile (672 raw index bytes) -> (28, 24, 4) RGBA uint8
# ---------------------------------------------------------------------------

def decode_tile_rgba(tile_bytes, pal8):
    idx = np.frombuffer(tile_bytes, dtype=np.uint8).reshape(TILE_H, TILE_W)
    rgba = np.zeros((TILE_H, TILE_W, 4), dtype=np.uint8)
    mask = idx != 0
    rgba[..., 0:3][mask] = pal8[idx[mask]]
    rgba[..., 3][mask] = 255
    # index 0 -> (0, 0, 0, 0), already satisfied by the zero-initialized array.
    return rgba


# ---------------------------------------------------------------------------
# STEP 4: upscaling dispatch (nearest / lanczos), a future xbr/AI method is
# just another entry in UPSCALERS calling a new upscale_<name>(img) function.
# ---------------------------------------------------------------------------

def upscale_nearest(img):
    """Pure 4x nearest-neighbor: each source pixel becomes a 4x4 block of
    identical pixels. Pixel-exact -- required for --selftest round-tripping."""
    return np.repeat(np.repeat(img, SCALE, axis=0), SCALE, axis=1)


def upscale_lanczos(img):
    """
    4x Lanczos resample via Pillow, RGBA.

    To avoid transparent-edge color bleed, this uses approach (a) from the
    tool brief: premultiply alpha into RGB before resampling, then
    un-premultiply after. Rationale: PIL's RGBA resize treats each channel
    independently, so a fully-transparent source pixel's RGB (which we've
    already zeroed for index 0, but which Lanczos's negative/overshoot taps
    can still pull *toward* from a neighboring transparent pixel) would
    otherwise contribute equally-weighted color to nearby opaque output
    pixels regardless of its own coverage. Premultiplying makes a
    transparent source pixel's RGB contribution exactly zero, so resampled
    edges fade based on real coverage, not on the mostly-black filler in
    fully-transparent texels.

    After resampling, alpha is thresholded (< 16 -> 0) to snap
    Lanczos-ringing near-zero alpha back to true transparency, since index-0
    (fully transparent) source regions should stay fully transparent in the
    output rather than pick up faint ringing halos.
    """
    from PIL import Image

    h, w = img.shape[:2]
    dst_w, dst_h = w * SCALE, h * SCALE

    rgb = img[..., 0:3].astype(np.float32)
    alpha = img[..., 3].astype(np.float32) / 255.0

    premult = rgb * alpha[..., None]
    premult_img = Image.fromarray(np.clip(premult, 0, 255).astype(np.uint8))
    alpha_img = Image.fromarray(np.clip(alpha * 255.0, 0, 255).astype(np.uint8))

    premult_resized = premult_img.resize((dst_w, dst_h), Image.LANCZOS)
    alpha_resized = alpha_img.resize((dst_w, dst_h), Image.LANCZOS)

    premult_arr = np.asarray(premult_resized).astype(np.float32)
    alpha_arr = np.asarray(alpha_resized).astype(np.float32)  # 0..255

    alpha_norm = alpha_arr / 255.0
    have_coverage = alpha_arr >= 16.0  # threshold: snap near-zero alpha to fully transparent
    safe_alpha = np.where(have_coverage, alpha_norm, 1.0)

    out_rgb = premult_arr / safe_alpha[..., None]
    out_rgb = np.clip(out_rgb, 0, 255)

    out_alpha = np.where(have_coverage, alpha_arr, 0.0)

    out = np.empty((dst_h, dst_w, 4), dtype=np.uint8)
    out[..., 0:3] = np.clip(np.round(out_rgb), 0, 255).astype(np.uint8)
    out[..., 3] = np.clip(np.round(out_alpha), 0, 255).astype(np.uint8)
    return out


UPSCALERS = {
    "nearest": upscale_nearest,
    "lanczos": upscale_lanczos,
}


# ---------------------------------------------------------------------------
# STEP 5: assemble the 20x30 grid atlas for one (bank, palette) pair
# ---------------------------------------------------------------------------

def tile_cell_origin(z):
    col = z % GRID_COLS
    row = z // GRID_COLS
    return col * CELL_W, row * CELL_H


def build_atlas(tiles, pal8, method):
    upscale_fn = UPSCALERS[method]
    atlas = np.zeros((ATLAS_H, ATLAS_W, 4), dtype=np.uint8)
    for z, tile in enumerate(tiles):
        if tile is None:
            continue  # blank cell stays (0, 0, 0, 0)
        x0, y0 = tile_cell_origin(z)
        rgba = decode_tile_rgba(tile, pal8)
        up = upscale_fn(rgba)
        atlas[y0:y0 + CELL_H, x0:x0 + CELL_W] = up
    return atlas


# ---------------------------------------------------------------------------
# STEP 6: HDPX asset read/write
# ---------------------------------------------------------------------------

def write_hdpx_asset(path, atlas):
    h, w = atlas.shape[:2]
    with open(path, "wb") as f:
        f.write(b"HDPX")
        f.write(struct.pack("<I", w))
        f.write(struct.pack("<I", h))
        f.write(np.ascontiguousarray(atlas, dtype=np.uint8).tobytes())


def read_hdpx_asset(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"HDPX":
            raise ValueError("%s: bad magic %r (expected b'HDPX')" % (path, magic))
        w = struct.unpack("<I", f.read(4))[0]
        h = struct.unpack("<I", f.read(4))[0]
        data = f.read(w * h * 4)
    if len(data) != w * h * 4:
        raise ValueError(
            "%s: truncated pixel data (wanted %d bytes, got %d)" % (path, w * h * 4, len(data)))
    arr = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 4)
    return arr, w, h


# ---------------------------------------------------------------------------
# STEP 7: minimal stdlib PNG writer (reused convention from hd_extract.py)
# ---------------------------------------------------------------------------

def write_png(path, rgb, width, height):
    import zlib

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


def write_preview(preview_dir, char, pal_idx, tiles, atlas):
    """
    Write a small PNG contact sheet of every non-blank tile in `bank`
    (composited over mid-grey so transparency is visible), packed into a
    tight 20-column grid -- smaller than the full sparse atlas since blank
    cells are skipped entirely. Requires Pillow (lazily imported here).
    """
    from PIL import Image

    non_blank = [z for z, t in enumerate(tiles) if t is not None]
    if not non_blank:
        return None

    cols = GRID_COLS
    rows = (len(non_blank) + cols - 1) // cols
    canvas = np.zeros((rows * CELL_H, cols * CELL_W, 4), dtype=np.uint8)

    for i, z in enumerate(non_blank):
        x0, y0 = tile_cell_origin(z)
        cell = atlas[y0:y0 + CELL_H, x0:x0 + CELL_W]
        pi, pr = i % cols, i // cols
        canvas[pr * CELL_H:(pr + 1) * CELL_H, pi * CELL_W:(pi + 1) * CELL_W] = cell

    grey = 128.0
    rgb = canvas[..., 0:3].astype(np.float32)
    alpha = canvas[..., 3:4].astype(np.float32) / 255.0
    composited = (rgb * alpha + grey * (1.0 - alpha)).astype(np.uint8)

    os.makedirs(preview_dir, exist_ok=True)
    out_path = os.path.join(preview_dir, "hdtile_%s_p%02d.png" % (_safe_char(char), pal_idx))
    Image.fromarray(composited).save(out_path)
    return out_path


def _safe_char(char):
    # ')' is a valid filename char but let's keep preview names shell-friendly
    # for humans browsing tools/hdtile_previews/; map it to a word like the
    # sibling hd_extract_comp.py does for its own odd-char sheet names.
    return {")": "paren"}.get(char, char)


# ---------------------------------------------------------------------------
# STEP 8: --selftest round-trip verification (nearest method only)
# ---------------------------------------------------------------------------

def build_reverse_palette(pal8):
    """
    Map (r, g, b) -> first palette index i in 1..255 whose color matches.
    Index 0 is handled separately (via alpha) by the caller.

    NOTE: some palettes (e.g. slot 0) contain duplicate (r, g, b) triples at
    different indices (verified: 41 aliased pairs in palette 0 alone, e.g.
    index 97 == index 49 == (0, 3, 5)). The HDPX atlas stores colors, not
    indices, so reconstructing "the" original index from a color alone is
    inherently ambiguous whenever the palette aliases colors -- this is a
    property of the source data, not data loss in the atlas. selftest_bank()
    below treats a reconstructed index as a pass if its color matches the
    original index's color, even when the index itself differs from the
    first-match lookup here.
    """
    rev = {}
    for i in range(1, 256):
        key = (int(pal8[i, 0]), int(pal8[i, 1]), int(pal8[i, 2]))
        if key not in rev:
            rev[key] = i
    return rev


def selftest_bank(char, tiles, pal8, atlas_path, sample_count=3):
    """
    Round-trip check for one (bank, palette) atlas already written to disk:
    re-read the atlas, and for the first `sample_count` non-blank tiles,
    downsample each 96x112 cell back to 24x28 by taking the top-left pixel
    of every 4x4 block, map RGBA back to palette indices, and assert the
    result exactly equals the original 672-byte tile. Returns (tested, passed,
    failures) where failures is a list of (tile_index, reason) for mismatches.
    """
    atlas, w, h = read_hdpx_asset(atlas_path)
    if (w, h) != (ATLAS_W, ATLAS_H):
        raise ValueError("%s: unexpected atlas dims %dx%d (want %dx%d)" %
                          (atlas_path, w, h, ATLAS_W, ATLAS_H))

    rev = build_reverse_palette(pal8)

    non_blank = [z for z, t in enumerate(tiles) if t is not None][:sample_count]
    tested = 0
    passed = 0
    failures = []

    for z in non_blank:
        tested += 1
        x0, y0 = tile_cell_origin(z)
        cell = atlas[y0:y0 + CELL_H, x0:x0 + CELL_W]
        down = cell[::SCALE, ::SCALE]  # (28, 24, 4), top-left pixel of each 4x4 block

        original = np.frombuffer(tiles[z], dtype=np.uint8).reshape(TILE_H, TILE_W)
        recon = np.zeros((TILE_H, TILE_W), dtype=np.uint8)

        alpha = down[..., 3]
        transparent = alpha == 0
        # index 0 wherever transparent; default 0 already covers that.

        ok = True
        ys, xs = np.nonzero(~transparent)
        for yy, xx in zip(ys.tolist(), xs.tolist()):
            key = (int(down[yy, xx, 0]), int(down[yy, xx, 1]), int(down[yy, xx, 2]))
            idx = rev.get(key)
            if idx is None:
                ok = False
                failures.append((z, "pixel (%d,%d): rgb %r not found in palette" % (yy, xx, key)))
                break
            recon[yy, xx] = idx

        if ok:
            mismatch = recon != original
            if np.any(mismatch):
                # A raw index mismatch is only a real failure if the two
                # indices resolve to different colors (i.e. it isn't just a
                # palette color-alias -- see build_reverse_palette's note).
                my, mx = np.nonzero(mismatch)
                orig_colors = pal8[original[my, mx].astype(np.int64)]
                recon_colors = pal8[recon[my, mx].astype(np.int64)]
                color_mismatch = ~np.all(orig_colors == recon_colors, axis=1)
                if np.any(color_mismatch):
                    ok = False
                    diff = int(np.count_nonzero(color_mismatch))
                    failures.append((z, "%d/%d TRUE color mismatches after reconstruction "
                                         "(%d additional index-only aliasing diffs ignored)"
                                         % (diff, TILE_BYTES, int(np.count_nonzero(mismatch)) - diff)))
                else:
                    passed += 1
            else:
                passed += 1

    return tested, passed, failures


# ---------------------------------------------------------------------------
# CLI argument parsing
# ---------------------------------------------------------------------------

def parse_banks_arg(value):
    value = value.strip()
    if value.lower() == "all":
        return list(BANKS_ORDER)

    if "," in value:
        parts = [p.strip() for p in value.split(",") if p.strip()]
    else:
        parts = list(value)

    seen = []
    for p in parts:
        if p not in BANK_FILENAME:
            raise argparse.ArgumentTypeError(
                "unknown bank char %r (valid: %s, or 'all')" % (p, "".join(BANKS_ORDER)))
        if p not in seen:
            seen.append(p)
    if not seen:
        raise argparse.ArgumentTypeError("no banks given")

    return [c for c in BANKS_ORDER if c in seen]


def parse_palettes_arg(value):
    value = value.strip()
    if value.lower() == "all":
        return list(range(NUM_PALETTES))

    result = []
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        n = int(part)
        if not (0 <= n < NUM_PALETTES):
            raise argparse.ArgumentTypeError(
                "palette index %d out of range (0..%d)" % (n, NUM_PALETTES - 1))
        if n not in result:
            result.append(n)
    if not result:
        raise argparse.ArgumentTypeError("no palette indices given")
    return result


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Extract HD level-tileset atlases (Phase S3) from Tyrian shapes?.dat banks.")
    parser.add_argument(
        "--data", default=DEFAULT_DATA_DIR,
        help="data dir containing shapes*.dat and palette.dat (default: %s)" % DEFAULT_DATA_DIR)
    parser.add_argument(
        "--out", default=None,
        help="output dir for hdtile_*.dat atlases (default: same as --data)")
    parser.add_argument(
        "--method", choices=sorted(UPSCALERS.keys()), default="nearest",
        help="upscale method (default: nearest)")
    parser.add_argument(
        "--palettes", type=parse_palettes_arg, default=list(range(NUM_PALETTES)),
        help="'all' (default) or a comma-separated list of 0-based palette indices (0..22)")
    parser.add_argument(
        "--banks", type=parse_banks_arg, default=list(BANKS_ORDER),
        help="'all' (default), a string of bank chars (e.g. \")wxyz\"), or a comma-separated "
             "list of bank chars (e.g. \"),w,x\"). Valid chars: %s" % "".join(BANKS_ORDER))
    parser.add_argument(
        "--preview", action="store_true",
        help="also write a PNG contact sheet per generated atlas to tools/hdtile_previews/")
    parser.add_argument(
        "--selftest", action="store_true",
        help="after writing each atlas, round-trip-verify 3 non-blank tiles per bank against "
             "the freshly written file (nearest method only; a no-op warning under lanczos)")
    return parser


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    args = build_arg_parser().parse_args()

    data_dir = os.path.abspath(args.data)
    out_dir = os.path.abspath(args.out) if args.out else data_dir
    palette_path = os.path.join(data_dir, "palette.dat")

    if not os.path.isfile(palette_path):
        print("error: palette.dat not found at %s" % palette_path, file=sys.stderr)
        return 1

    for char in args.banks:
        bank_path = os.path.join(data_dir, BANK_FILENAME[char])
        if not os.path.isfile(bank_path):
            print("error: %s not found at %s" % (BANK_FILENAME[char], bank_path), file=sys.stderr)
            return 1

    os.makedirs(out_dir, exist_ok=True)

    # --- load + verify every requested bank once (independent of palette) ---
    bank_tiles = {}
    bank_counts = {}
    for char in args.banks:
        bank_path = os.path.join(data_dir, BANK_FILENAME[char])
        tiles = read_bank(bank_path)
        non_blank = sum(1 for t in tiles if t is not None)
        blank = TILES_PER_BANK - non_blank
        expected = GROUND_TRUTH_NONBLANK[char]
        assert non_blank == expected, (
            "%s: decoded %d non-blank tiles, expected %d (ground truth)"
            % (bank_path, non_blank, expected))
        assert non_blank + blank == TILES_PER_BANK
        bank_tiles[char] = tiles
        bank_counts[char] = (blank, non_blank)
        print("Bank %s (%s): %d blank, %d non-blank (of %d)" %
              (char, BANK_FILENAME[char], blank, non_blank, TILES_PER_BANK))

    palette_cache = {}

    total_atlases = 0
    total_bytes = 0
    selftest_totals = {}  # char -> [tested, passed]
    selftest_failures = []
    warned_selftest_method = False

    for pal_idx in args.palettes:
        pal8 = palette_cache.get(pal_idx)
        if pal8 is None:
            pal8 = load_palette(palette_path, pal_idx)
            palette_cache[pal_idx] = pal8

        for char in args.banks:
            tiles = bank_tiles[char]
            atlas = build_atlas(tiles, pal8, args.method)

            out_name = "hdtile_%s_p%02d.dat" % (char, pal_idx)
            out_path = os.path.join(out_dir, out_name)
            write_hdpx_asset(out_path, atlas)

            size = os.path.getsize(out_path)
            assert size == ATLAS_FILE_BYTES, (
                "%s: wrote %d bytes, expected %d (header %d + %dx%dx4 pixels)"
                % (out_path, size, ATLAS_FILE_BYTES, HDPX_HEADER_BYTES, ATLAS_W, ATLAS_H))

            total_atlases += 1
            total_bytes += size
            print("  wrote %s (%d bytes)" % (out_path, size))

            if args.preview:
                preview_path = write_preview(PREVIEW_DIR, char, pal_idx, tiles, atlas)
                if preview_path:
                    print("    preview -> %s" % preview_path)

            if args.selftest:
                if args.method != "nearest":
                    if not warned_selftest_method:
                        print("warning: --selftest round-trip only supports --method nearest; "
                              "skipping verification under --method %s" % args.method,
                              file=sys.stderr)
                        warned_selftest_method = True
                else:
                    tested, passed, failures = selftest_bank(char, tiles, pal8, out_path)
                    tot = selftest_totals.setdefault(char, [0, 0])
                    tot[0] += tested
                    tot[1] += passed
                    for z, reason in failures:
                        selftest_failures.append((char, pal_idx, z, reason))
                        print("  SELFTEST FAIL bank=%s palette=%d tile=%d: %s" %
                              (char, pal_idx, z, reason), file=sys.stderr)

    # --- summary ---
    print()
    print("=== Summary ===")
    for char in args.banks:
        blank, non_blank = bank_counts[char]
        print("  bank %s: %d blank, %d non-blank" % (char, blank, non_blank))
    print("Total atlases written: %d" % total_atlases)
    print("Total bytes written: %d" % total_bytes)

    overall_ok = True
    if args.selftest:
        print()
        print("=== Selftest (round-trip, nearest only) ===")
        grand_tested = grand_passed = 0
        for char in args.banks:
            tested, passed = selftest_totals.get(char, [0, 0])
            grand_tested += tested
            grand_passed += passed
            status = "PASS" if tested and passed == tested else ("SKIPPED" if not tested else "FAIL")
            print("  bank %s: %d/%d passed [%s]" % (char, passed, tested, status))
        overall_status = "PASS" if grand_tested and grand_passed == grand_tested else "FAIL"
        print("Overall: %d/%d passed [%s]" % (grand_passed, grand_tested, overall_status))
        if grand_tested == 0 or grand_passed != grand_tested:
            overall_ok = grand_tested > 0 and grand_passed == grand_tested
            if grand_tested == 0:
                overall_ok = True  # nothing ran (e.g. all lanczos) -- not a failure of the tool
            else:
                overall_ok = False

    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
