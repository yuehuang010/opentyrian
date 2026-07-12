#!/usr/bin/env python3
"""Build a self-contained A/B listening page for an ACE-Step cover batch.

Usage: build_ab_page.py <song>     (song key in SONGS below)
Encodes each input to opus 64k, base64-embeds them, writes ab_page_<song>.html.
"""
import base64
import json
import os
import subprocess
import sys

HERE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "hdmusic_work", "acestep")
CLASSICDIR = os.path.join(HERE, "..", "classic")
OUT = os.path.join(HERE, "out")
GENRES = os.path.join(HERE, "genres")
VARIANTSDIR = os.path.join(HERE, "..", "variants")
TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ab_template.html")

SONGS = {
    "genres": {
        "h1": 'Track 30 &ldquo;Tyrian: The Song&rdquo; &mdash; bass-drop rescue',
        "title": "Track 30 &mdash; bass-drop rescue A/B",
        "loop_seconds": 113.669048,
        # trim to one pass so the page stays under the 16 MB cap
        "trim_seconds": 113.669048,
        "sub": ("The classic slams into full bass at ~22.2&nbsp;s; the "
                "low-retention covers smear it. Rescue attempts: source "
                "pre-emphasis (duck &minus;6&nbsp;dB before the drop / boost "
                "+4&nbsp;dB after) and caption dynamics language. Listen "
                "around 18&ndash;26&nbsp;s. Seed 4242 throughout. One loop "
                "pass each; switching tracks keeps the playhead."),
        "tracks": [
            ("classic", "Classic OPL", os.path.join(CLASSICDIR, "hdmusic_30_x2.wav"),
             "render_music reference — the drop lands at 22.2 s"),
            ("filmscore_n020", "Film score &middot; 0.20 (winner, has the issue)", os.path.join(GENRES, "t30_filmscore_n020.flac"),
             "unmodified winner for comparison"),
            ("filmscore_n020_duck", "Film score &middot; duck before drop", os.path.join(GENRES, "t30_filmscore_n020_duck.flac"),
             "source dimmed -6 dB at 18-22.1 s so the slam reads louder"),
            ("filmscore_n020_boost", "Film score &middot; boost after drop", os.path.join(GENRES, "t30_filmscore_n020_boost.flac"),
             "source +4 dB at 22.1-24.5 s"),
            ("filmscore_n020_dropcap", "Film score &middot; drop in caption", os.path.join(GENRES, "t30_filmscore_n020_dropcap.flac"),
             "caption asks for heavy bass drop + dynamic contrast"),
            ("operasynth_n020", "Operatic synthony &middot; 0.20 (winner, has the issue)", os.path.join(GENRES, "t30_operasynth_n020.flac"),
             "unmodified winner for comparison"),
            ("operasynth_n020_duck", "Operatic synthony &middot; duck before drop", os.path.join(GENRES, "t30_operasynth_n020_duck.flac"),
             "source dimmed -6 dB at 18-22.1 s"),
            ("operasynth_n020_dropcap", "Operatic synthony &middot; drop in caption", os.path.join(GENRES, "t30_operasynth_n020_dropcap.flac"),
             "caption asks for heavy bass drop + dynamic contrast"),
        ],
    },
    "30": {
        "h1": 'Track 30 &ldquo;Tyrian: The Song&rdquo; &mdash; ACE-Step 1.5 covers',
        "title": "Track 30 &mdash; ACE-Step covers A/B",
        "loop_seconds": 113.669048,
        "sub": ("Source: classic OPL render, two loops (227.3&nbsp;s). Covers: "
                "acestep-v15-turbo audio-to-audio (torch/MPS), melody retention via "
                "cover_noise_strength. The &#9670; tick is the loop seam "
                "(113.67&nbsp;s). Switching tracks keeps the playhead."),
        "tracks": [
            ("classic", "Classic OPL", os.path.join(CLASSICDIR, "hdmusic_30_x2.wav"),
             "render_music reference, 2 loops"),
            ("orch_n020", "Orchestral · melody 0.20", os.path.join(OUT, "orch_n020.flac"),
             "seed 4242 · recommended retention range, most style freedom"),
            ("orch_n050", "Orchestral · melody 0.50", os.path.join(OUT, "orch_n050.flac"),
             "seed 4242 · balanced"),
            ("orch_n080", "Orchestral · melody 0.80", os.path.join(OUT, "orch_n080.flac"),
             "seed 4242 · strong retention, least style freedom"),
            ("nofsq_n050", "Orchestral · melody 0.50 · raw latents", os.path.join(OUT, "nofsq_n050.flac"),
             "seed 4242 · no-FSQ: conditions on source latents, not quantized codes"),
        ],
    },
    "18": {
        "h1": 'Track 18 &ldquo;Tyrian, The Level&rdquo; &mdash; ACE-Step 1.5 covers',
        "title": "Track 18 &mdash; ACE-Step covers A/B",
        "loop_seconds": 77.3093425,
        "sub": ("Source: classic OPL render, two loops (154.6&nbsp;s). Covers: "
                "acestep-v15-turbo audio-to-audio (torch/MPS), same recipe as "
                "tracks 30/28. The &#9670; tick is the loop seam (77.3&nbsp;s). "
                "Switching tracks keeps the playhead."),
        "tracks": [
            ("classic", "Classic OPL", os.path.join(CLASSICDIR, "hdmusic_18_x2.wav"),
             "render_music reference, 2 loops"),
            ("t18_n050", "Orchestral · melody 0.50", os.path.join(OUT, "t18_n050.flac"),
             "seed 4242 · balanced"),
            ("t18_n080", "Orchestral · melody 0.80", os.path.join(OUT, "t18_n080.flac"),
             "seed 4242 · strong retention (won on track 28)"),
        ],
    },
    "28": {
        "h1": 'Track 28 &ldquo;Torm - The Gathering&rdquo; &mdash; ACE-Step 1.5 covers',
        "title": "Track 28 &mdash; ACE-Step covers A/B",
        "loop_seconds": 90.201429,
        "sub": ("Source: classic OPL render, two loops (180.4&nbsp;s). Covers: "
                "acestep-v15-turbo audio-to-audio (torch/MPS), the two retention "
                "settings that won on track 30. The &#9670; tick is the loop seam "
                "(90.2&nbsp;s). Switching tracks keeps the playhead."),
        "tracks": [
            ("classic", "Classic OPL", os.path.join(CLASSICDIR, "hdmusic_28_x2.wav"),
             "render_music reference, 2 loops"),
            ("t28_n050", "Orchestral · melody 0.50", os.path.join(OUT, "t28_n050.flac"),
             "seed 4242 · balanced"),
            ("t28_n080", "Orchestral · melody 0.80", os.path.join(OUT, "t28_n080.flac"),
             "seed 4242 · strong retention, least style freedom"),
        ],
    },
}


def encode_opus(path: str, trim=None, bitrate="64k") -> bytes:
    cmd = ["ffmpeg", "-v", "error", "-i", path]
    if trim:
        cmd += ["-t", f"{trim:.6f}"]
    cmd += ["-c:a", "libopus", "-b:a", bitrate, "-f", "ogg", "-"]
    p = subprocess.run(cmd, capture_output=True, check=True)
    return p.stdout


def loudness(path: str) -> str:
    p = subprocess.run(
        ["ffmpeg", "-i", path, "-af", "volumedetect", "-f", "null", "-"],
        capture_output=True, text=True)
    mean = maxv = "?"
    for line in p.stderr.splitlines():
        if "mean_volume" in line:
            mean = line.split("mean_volume:")[1].strip()
        if "max_volume" in line:
            maxv = line.split("max_volume:")[1].strip()
    return f"mean {mean} · max {maxv}"


def main() -> None:
    song = sys.argv[1] if len(sys.argv) > 1 else "30"
    cfg = SONGS[song]
    page = os.path.join(HERE, f"ab_page_{song}.html")

    tracks = []
    for tid, label, path, note in cfg["tracks"]:
        if not os.path.exists(path):
            print(f"skip (missing): {path}", file=sys.stderr)
            continue
        print(f"encoding {tid}...", flush=True)
        data = base64.b64encode(encode_opus(
            path, cfg.get("trim_seconds"), cfg.get("bitrate", "64k"))).decode("ascii")
        tracks.append({
            "id": tid, "label": label, "note": note,
            "loudness": loudness(path), "b64": data,
        })

    with open(TEMPLATE) as f:
        template = f.read()

    meta = [{k: t[k] for k in ("id", "label", "note", "loudness")} for t in tracks]
    audio_js = "\n".join(
        'AUDIO_B64["%s"] = "%s";' % (t["id"], t["b64"]) for t in tracks)
    html = (template
            .replace("__PAGE_TITLE__", cfg["title"])
            .replace("__H1__", cfg["h1"])
            .replace("__SUB__", cfg["sub"])
            .replace("/*__TRACKS_JSON__*/[]", json.dumps(meta))
            .replace("/*__LOOP_SECONDS__*/0", repr(cfg["loop_seconds"]))
            .replace("/*__AUDIO_B64__*/", audio_js))
    with open(page, "w") as f:
        f.write(html)
    print(f"wrote {page} ({os.path.getsize(page)/1e6:.1f} MB, {len(tracks)} tracks)")


if __name__ == "__main__":
    main()
