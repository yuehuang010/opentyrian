#!/usr/bin/env python3
"""
hd_vectorize_font.py -- regenerate the HD font glyph assets (tyrian21/hdfont_*.dat)
by VECTOR-TRACING the original Tyrian bitmap glyphs, replacing the earlier
xBRZ-upscale approach in tools/hd_extract_font.py.

WHY (the crux):
  The three font tables in tyrian.shp are tiny -- TINY_FONT glyphs are 1..6 x 2..8
  px. xBRZ 4x can only smooth a 6px source; it can't add detail, so upscaled body
  text (menus/save-load) reads as a soft blob. No external typeface matches these
  glyphs (they are custom pixel art by Daniel Cook; the Tyrian fan community never
  produced a vector version), so the faithful HD path is to VECTORIZE the originals.

APPROACH:
  Each source glyph carries a per-pixel SHADE INDEX in the low nibble (0..15) --
  a lookup into a 16-entry color bank, NOT anti-aliasing coverage. The original
  renders to an 8-bit paletted surface with no alpha, so every present pixel is
  fully opaque; nibbles encode real interior shading (e.g. "K" has a body shade,
  a lit highlight row, and darkened shadow diagonals), not a soft edge falloff.
  So the trace is split into two independent fields:
    - PRESENCE (nibble > 0, i.e. the pixel exists in the RLE at all) is what gets
      padded, bilinearly upsampled at supersample resolution, thresholded at the
      0.5 iso-surface, and box-downsampled back down -- this yields the crisp,
      anti-aliased glyph SILHOUETTE in the ALPHA channel.
    - The raw per-pixel SHADE NIBBLE is edge-extended (empty cells outside the
      presence mask are filled by iteratively dilating in the nearest present
      values) so upsampling doesn't drag edge shades toward 0, then bilinearly
      upsampled directly to the output resolution and encoded as R=G=B=nibble*17.
      This preserves the original highlight/body/shadow shading as a smooth
      per-pixel gradient instead of collapsing it to one uniform peak value.
  Result:
    - alpha  = true sub-pixel coverage of the traced PRESENCE outline (crisp AA edges)
    - R=G=B  = per-pixel source shade nibble * 17, edge-extended before upsampling
               (preserves original interior shading; smooth since the engine lerps
               between adjacent shade-bank entries anyway)
  The engine (src/video.c synth_hd_font_glyph) already consumes exactly this: it
  recolors R->palette shade (bilinearly interpolating between the two nearest
  shade-bank entries) and blends by alpha, so this is a drop-in asset swap with
  NO engine change. It also obsoletes the old flat-glyph corner-softening hack
  (real AA now comes from the supersampled presence fill for every glyph).

OUTPUT: tyrian21/hdfont_{font,small,tiny}_NN.dat (HDPX: 'HDPX' + u32 LE w + u32 LE h
  + w*h*4 RGBA), dims = src_w*SCALE x src_h*SCALE (SCALE=4, same as before so the
  engine's logical footprint gw/4 is unchanged), plus hd_font_manifest.json.

Run:  ./.venv-fonts/bin/python tools/hd_vectorize_font.py --data <dir>
Deps: numpy, Pillow (see .venv-fonts).
"""
import argparse
import json
import os
import struct

import numpy as np

SHP_NUM = 12
SCALE = 4          # keep == engine's HD footprint divisor (video.c hd_font_queue_glyph)
SS = 4             # anti-alias supersampling factor for the fill

# tyrian.shp table indices (src/sprite.h) -> (output prefix, manifest name)
FONT_TABLES = [
    (0, "hdfont_font", "FONT_SHAPES"),
    (1, "hdfont_small", "SMALL_FONT_SHAPES"),
    (2, "hdfont_tiny", "TINY_FONT"),
]

# ---------------------------------------------------------------------------
# tyrian.shp decode (mirrors JE_loadMainShapeTables / load_sprites / blit_sprite;
# vendored from hd_extract_font.py so this tool stands alone)
# ---------------------------------------------------------------------------
def load_shp_table_offsets(path):
    data = open(path, "rb").read()
    n = struct.unpack_from("<H", data, 0)[0]
    assert n == SHP_NUM, "tyrian.shp: expected %d tables, header says %d" % (SHP_NUM, n)
    pos = list(struct.unpack_from("<%di" % SHP_NUM, data, 2))
    pos.append(len(data))
    return data, pos


def load_sprite_table(data, off):
    count = struct.unpack_from("<H", data, off)[0]
    p = off + 2
    out = []
    for _ in range(count):
        populated = data[p]
        p += 1
        if not populated:
            out.append(None)
            continue
        w, h, size = struct.unpack_from("<HHH", data, p)
        p += 6
        out.append({"width": w, "height": h, "rle": data[p:p + size]})
        p += size
    return out


def decode_sprite_rle(sp):
    w, h = sp["width"], sp["height"]
    rle = sp["rle"]
    total = w * h
    out = [None] * total
    pos = 0
    xo = 0
    i = 0
    n = len(rle)

    def wr(idx, v):
        if 0 <= idx < total:
            out[idx] = v

    while i < n:
        b = rle[i]
        if b == 255:
            run = rle[i + 1]
            i += 2
            pos += run
            xo += run
        elif b == 254:
            pos += w - xo
            xo = w
            i += 1
        elif b == 253:
            wr(pos, None)
            pos += 1
            xo += 1
            i += 1
        else:
            wr(pos, b)
            pos += 1
            xo += 1
            i += 1
        if xo >= w:
            pos += w - xo
            xo = 0
    return out, w, h


def source_nibbles(sp):
    """HxW int array of low-nibble brightness (0 where transparent)."""
    flat, w, h = decode_sprite_rle(sp)
    a = np.zeros((h, w), np.int32)
    for i, v in enumerate(flat):
        if v is not None:
            a[i // w, i % w] = v & 0x0f
    return a, w, h


# ---------------------------------------------------------------------------
# vector trace + AA bake
# ---------------------------------------------------------------------------
def _bilinear_up(g, fh, fw):
    H, W = g.shape
    ys = np.linspace(0, H - 1, fh)
    xs = np.linspace(0, W - 1, fw)
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, H - 1)
    x1 = np.minimum(x0 + 1, W - 1)
    wy = (ys - y0)[:, None]
    wx = (xs - x0)[None, :]
    return (g[np.ix_(y0, x0)] * (1 - wy) * (1 - wx) + g[np.ix_(y0, x1)] * (1 - wy) * wx +
            g[np.ix_(y1, x0)] * wy * (1 - wx) + g[np.ix_(y1, x1)] * wy * wx)


def _edge_extend(field, present):
    """Fill `field` outside `present` by iteratively dilating in the nearest
    present values (8-connected mean of already-filled neighbors), so that
    upsampling the field later doesn't drag values at the glyph edge toward
    zero from the surrounding (semantically meaningless) transparent nibbles."""
    H, W = field.shape
    val = field.astype(np.float32).copy()
    filled = present.copy()
    val[~filled] = 0.0
    while not filled.all():
        new_val = val.copy()
        new_filled = filled.copy()
        progressed = False
        for y in range(H):
            for x in range(W):
                if filled[y, x]:
                    continue
                s = 0.0
                c = 0
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dy == 0 and dx == 0:
                            continue
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < H and 0 <= nx < W and filled[ny, nx]:
                            s += val[ny, nx]
                            c += 1
                if c > 0:
                    new_val[y, x] = s / c
                    new_filled[y, x] = True
                    progressed = True
        val, filled = new_val, new_filled
        if not progressed:
            break
    return val


def bake_glyph(sp):
    """Return (rgba HxWx4 uint8, w, h) HDPX payload for one glyph."""
    nib, w, h = source_nibbles(sp)
    maxn = int(nib.max())
    if maxn == 0:
        # fully transparent/empty glyph slot: emit a blank cell of the src size
        return np.zeros((h * SCALE, w * SCALE, 4), np.uint8), w * SCALE, h * SCALE

    present = (nib > 0)                          # RLE presence, NOT AA coverage
    pad = 1
    g = np.zeros((h + 2 * pad, w + 2 * pad), np.float32)
    g[pad:pad + h, pad:pad + w] = present.astype(np.float32)

    outw, outh = w * SCALE, h * SCALE
    fh = (h + 2 * pad) * SCALE * SS
    fw = (w + 2 * pad) * SCALE * SS
    y0 = pad * SCALE * SS
    x0 = pad * SCALE * SS

    # --- alpha: AA-trace the PRESENCE silhouette (supersampled 0.5 iso-surface) ---
    fine = _bilinear_up(g, fh, fw)
    mask = (fine >= 0.5).astype(np.float32)     # traced silhouette (counters preserved)
    mask = mask[y0:y0 + outh * SS, x0:x0 + outw * SS]
    alpha = mask.reshape(outh, SS, outw, SS).mean(axis=(1, 3))   # box-downsample -> AA
    a8 = np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)

    # --- shade: edge-extend the raw nibble field so upsampling doesn't drag edge
    # pixels toward 0, then resample through the *identical* padded grid geometry
    # as the alpha path (same shape, fh/fw, crop, box-downsample) so the two
    # channels stay pixel-registered by construction, not by coincidence. ---
    nib_pad = np.zeros((h + 2 * pad, w + 2 * pad), np.float32)
    nib_pad[pad:pad + h, pad:pad + w] = nib.astype(np.float32)
    present_pad = np.zeros((h + 2 * pad, w + 2 * pad), bool)
    present_pad[pad:pad + h, pad:pad + w] = present
    nib_ext = _edge_extend(nib_pad, present_pad)            # fills the pad ring too
    shade_fine = _bilinear_up(nib_ext, fh, fw)             # same fine grid as alpha
    shade_fine = shade_fine[y0:y0 + outh * SS, x0:x0 + outw * SS]
    shade_up = shade_fine.reshape(outh, SS, outw, SS).mean(axis=(1, 3))  # 0..15, smooth
    bright = np.clip(np.round(shade_up * 17.0), 0, 255).astype(np.uint8)

    rgba = np.zeros((outh, outw, 4), np.uint8)
    rgba[:, :, 0] = bright
    rgba[:, :, 1] = bright
    rgba[:, :, 2] = bright
    rgba[:, :, 3] = a8
    rgba[a8 == 0, :3] = 0                        # keep transparent pixels fully zero
    return rgba, outw, outh


def write_hdpx(path, rgba, w, h):
    with open(path, "wb") as f:
        f.write(b"HDPX")
        f.write(struct.pack("<II", w, h))
        f.write(rgba.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default="tyrian21", help="data dir with tyrian.shp (output too)")
    args = ap.parse_args()

    shp = os.path.join(args.data, "tyrian.shp")
    data, pos = load_shp_table_offsets(shp)

    manifest = {"scale": SCALE, "format": "shade_map_presence_trace",
                "supersample": SS,
                "note": "alpha=AA coverage of the traced RLE-presence silhouette; "
                        "R=G=B=per-pixel source shade nibble*17 (edge-extended into "
                        "the transparent surround before upsampling), preserving the "
                        "original's highlight/body/shadow interior shading. "
                        "Generated by tools/hd_vectorize_font.py.",
                "tables": {}}

    total = 0
    for tbl_idx, prefix, tname in FONT_TABLES:
        table = load_sprite_table(data, pos[tbl_idx])
        glyphs = []
        for idx, sp in enumerate(table):
            if sp is None:
                continue
            rgba, w, h = bake_glyph(sp)
            fname = "%s_%02d.dat" % (prefix, idx)
            write_hdpx(os.path.join(args.data, fname), rgba, w, h)
            glyphs.append({"index": idx, "file": fname,
                           "src_width": sp["width"], "src_height": sp["height"],
                           "hd_width": w, "hd_height": h})
            total += 1
        manifest["tables"][tname] = {"table_index": tbl_idx, "prefix": prefix,
                                     "glyph_count": len(glyphs), "glyphs": glyphs}
        print("%-18s %3d glyphs -> %s_NN.dat" % (tname, len(glyphs), prefix))

    with open(os.path.join(args.data, "hd_font_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("total %d glyphs; manifest -> hd_font_manifest.json" % total)


if __name__ == "__main__":
    main()
