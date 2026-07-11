#!/usr/bin/env python3
"""compare_mix_balance.py - compare per-voice-group mix balance between the
classic OPL render and a soundfont (FluidR3) render of an LDS track.

For each group in a groups file (see tools/stem_groups_30.txt) this renders
the group SOLO through both synth chains:

  classic:  hdmusic_work/bin/render_music with LDS_SOLO_FPS=<group fps>
            (authentic OPL chip output, all other voices' key-ons masked)
  gm:       hdmusic_work/bin/lds_to_midi with a derived solo map (group
            voices keep their lds_gm_map.txt settings, all others vol 0)
            -> fluidsynth, dry (-R 0 -C 0), FLUID_GAIN 0.48

then measures EBU R128 integrated loudness (LUFS) of each solo plus both
FULL mixes, and reports each group's level relative to its own full mix:

  rel_classic = LUFS(classic solo) - LUFS(classic full)
  rel_gm      = LUFS(gm solo)      - LUFS(gm full)
  delta       = rel_gm - rel_classic     # +ve: group sits HOTTER than the
                                         # classic balance; -ve: quieter

A suggested CC7 correction is printed using the GM volume law
L(dB) = 40*log10(cc/127)  (approximate for fluidsynth, iterate by ear).

Renders are cached in hdmusic_work/balance/<song>/ and reused if present;
delete that directory (or a single .wav) after editing the map to re-render.

Usage:
  python3 tools/compare_mix_balance.py [--song 30] [--groups tools/stem_groups_30.txt]
                                       [--map tools/lds_gm_map.txt]
                                       [--sf2 hdmusic_work/sf/FluidR3_GM.sf2]
                                       [--data tyrian21] [--trim 113.669048]
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def run(cmd, **kw):
    r = subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)
    return r.stdout + r.stderr


def lufs(path):
    """EBU R128 integrated loudness of an audio file, in LUFS."""
    out = run(["ffmpeg", "-nostats", "-i", str(path), "-map", "a",
               "-af", "ebur128=framelog=quiet", "-f", "null", "-"])
    m = re.findall(r"I:\s+(-?[\d.]+)\s+LUFS", out)
    if not m:
        sys.exit(f"error: no ebur128 integrated loudness in ffmpeg output for {path}")
    return float(m[-1])


def parse_groups(path):
    groups = []
    for line in pathlib.Path(path).read_text().splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        name, _, fps = line.partition(":")
        fps = fps.split()
        if not fps:
            sys.exit(f"error: bad groups line: {line!r}")
        groups.append((name.strip(), fps))
    return groups


def parse_map(path):
    """map file -> list of (fingerprint, rest-of-line) preserving settings."""
    entries = []
    for line in pathlib.Path(path).read_text().splitlines():
        s = line.split("#")[0].strip()
        if not s:
            continue
        parts = s.split()
        entries.append((parts[0].lower(), parts))
    return entries


def solo_map_text(entries, keep_fps):
    keep = {fp.lower() for fp in keep_fps}
    out = []
    for fp, parts in entries:
        if fp in keep:
            out.append(" ".join(parts))
        else:
            out.append(f"{parts[0]}  {parts[1]}  0  0")
    return "\n".join(out) + "\n"


def render_classic(bin_dir, data, out_wav, song, trim, solo_fps=None):
    tmp = out_wav.parent / f".tmp_classic_{out_wav.stem}"
    tmp.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.pop("LDS_SOLO_FPS", None)
    if solo_fps:
        env["LDS_SOLO_FPS"] = ",".join(solo_fps)
    run([str(bin_dir / "render_music"), str(data), str(tmp), str(song)], env=env)
    raw = tmp / f"hdmusic_{song:02d}.wav"
    run(["ffmpeg", "-y", "-i", str(raw), "-t", str(trim), str(out_wav)])
    shutil.rmtree(tmp)


def render_gm(bin_dir, data, sf2, map_text, out_wav, song, trim):
    tmp = out_wav.parent / f".tmp_gm_{out_wav.stem}"
    tmp.mkdir(parents=True, exist_ok=True)
    map_path = tmp / "solo_map.txt"
    map_path.write_text(map_text)
    env = dict(os.environ)
    env["LDS_GM_MAP"] = str(map_path)
    run([str(bin_dir / "lds_to_midi"), str(data), str(tmp), str(song)],
        env=env, cwd=str(REPO))
    mid = tmp / f"hdmusic_{song:02d}.mid"
    raw = tmp / "render.wav"
    run(["fluidsynth", "-ni", "-R", "0", "-C", "0", "-g", "0.48",
         "-r", "44100", "-F", str(raw), str(sf2), str(mid)])
    run(["ffmpeg", "-y", "-i", str(raw), "-t", str(trim), str(out_wav)])
    shutil.rmtree(tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--song", type=int, default=30)
    ap.add_argument("--groups", default=str(REPO / "tools/stem_groups_30.txt"))
    ap.add_argument("--map", default=str(REPO / "tools/lds_gm_map.txt"))
    ap.add_argument("--sf2", default=str(REPO / "hdmusic_work/sf/FluidR3_GM.sf2"))
    ap.add_argument("--data", default="/Users/felixhuang/source/opentyrian/tyrian21")
    ap.add_argument("--trim", type=float, default=113.669048)
    args = ap.parse_args()

    bin_dir = REPO / "hdmusic_work/bin"
    cache = REPO / "hdmusic_work/balance" / f"{args.song:02d}"
    cache.mkdir(parents=True, exist_ok=True)

    groups = parse_groups(args.groups)
    entries = parse_map(args.map)
    map_vol = {}  # fp -> current CC7 volume from the map (default 100)
    for fp, parts in entries:
        map_vol[fp] = int(parts[3]) if len(parts) > 3 and parts[3].lstrip("-").isdigit() else 100

    jobs = [("FULL", None)] + groups
    results = {}
    for name, fps in jobs:
        cwav = cache / f"classic_{name}.wav"
        gwav = cache / f"gm_{name}.wav"
        if not cwav.exists():
            print(f"render classic {name}...", file=sys.stderr)
            render_classic(bin_dir, args.data, cwav, args.song, args.trim,
                           solo_fps=fps)
        if not gwav.exists():
            print(f"render gm      {name}...", file=sys.stderr)
            keep = fps if fps else [fp for fp, _ in entries]
            render_gm(bin_dir, args.data, args.sf2,
                      solo_map_text(entries, keep), gwav, args.song, args.trim)
        results[name] = (lufs(cwav), lufs(gwav))

    c_full, g_full = results["FULL"]
    print(f"\nfull mix: classic {c_full:.1f} LUFS, gm {g_full:.1f} LUFS")
    print(f"{'group':<12} {'classic':>9} {'gm':>9} {'rel_cls':>8} {'rel_gm':>8} "
          f"{'delta':>7}  suggestion")
    for name, fps in groups:
        c, g = results[name]
        rel_c, rel_g = c - c_full, g - g_full
        delta = rel_g - rel_c
        # GM volume law: dB = 40*log10(cc/127); scale current group CC7s down/up
        scale = 10 ** (-delta / 40)
        vols = sorted({map_vol.get(fp.lower(), 100) for fp in fps if map_vol.get(fp.lower(), 100) > 0})
        sug = ", ".join(f"{v}->{min(127, max(1, round(v * scale)))}" for v in vols) or "(all vol 0)"
        flag = " <-- hot" if delta > 1.5 else (" <-- quiet" if delta < -1.5 else "")
        print(f"{name:<12} {c:>9.1f} {g:>9.1f} {rel_c:>8.1f} {rel_g:>8.1f} "
              f"{delta:>+7.1f}  CC7 {sug}{flag}")
    print("\ndelta > 0: group sits hotter in the GM mix than in classic; "
          "suggested CC7 uses L=40*log10(cc/127) (approximate - iterate by ear).")


if __name__ == "__main__":
    main()
