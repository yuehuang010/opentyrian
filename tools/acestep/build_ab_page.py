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
TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ab_template.html")

SONGS = {
    "genres": {
        "h1": 'Track 30 &ldquo;Tyrian: The Song&rdquo; &mdash; genre remakes',
        "title": "Track 30 &mdash; genre remakes A/B",
        "loop_seconds": 113.669048,
        # 7 tracks: trim to one pass so the page stays under the 16 MB cap
        "trim_seconds": 113.669048,
        "sub": ("Locked cover recipe (seed 4242, melody 0.80 unless noted), "
                "caption varied per genre. One loop pass each (113.7&nbsp;s). "
                "Switching tracks keeps the playhead."),
        "tracks": [
            ("classic", "Classic OPL", os.path.join(CLASSICDIR, "hdmusic_30_x2.wav"),
             "render_music reference"),
            ("synthony", "Synthony &middot; orchestra + EDM", os.path.join(GENRES, "t30_synthony.flac"),
             "symphony orchestra fused with driving synths, four-on-the-floor"),
            ("synthony_n050", "Synthony &middot; melody 0.50", os.path.join(GENRES, "t30_synthony_n050.flac"),
             "same caption, more style freedom (less melody retention)"),
            ("synthwave", "Synthwave / retrowave", os.path.join(GENRES, "t30_synthwave.flac"),
             "80s analog polysynths, arpeggiated bass, gated reverb drums"),
            ("metal", "Symphonic power metal", os.path.join(GENRES, "t30_metal.flac"),
             "distorted guitars + double kick under orchestra"),
            ("trance", "Uplifting trance", os.path.join(GENRES, "t30_trance.flac"),
             "supersaw lead, rolling bassline, club production"),
            ("funk", "Jazz-funk fusion", os.path.join(GENRES, "t30_funk.flac"),
             "electric piano, slap bass, horn section"),
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


def encode_opus(path: str, trim=None) -> bytes:
    cmd = ["ffmpeg", "-v", "error", "-i", path]
    if trim:
        cmd += ["-t", f"{trim:.6f}"]
    cmd += ["-c:a", "libopus", "-b:a", "64k", "-f", "ogg", "-"]
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
        data = base64.b64encode(encode_opus(path, cfg.get("trim_seconds"))).decode("ascii")
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
