#!/usr/bin/env python3
"""Genre-experiment sweep: ACE-Step covers of ONE track across captions.

Same locked cover mechanics as render_all.py (task_type="cover", torch/MPS
DiT, seed 4242, audio_cover_strength=1.0); only the caption (and for one
variant the melody-retention knob) varies. Outputs raw flacs for A/B
listening — build the page with build_ab_page.py afterwards.

Run from the ACE-Step repo root:
    uv run python render_genres.py [song-number, default 30]
"""
import math
import os
import struct
import subprocess
import sys
import time

ACESTEP_ROOT = os.environ.get("ACESTEP_ROOT",
                              os.path.expanduser("~/source/ACE-Step-1.5"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(REPO, "hdmusic_work")
CLASSIC = os.path.join(WORK, "classic")
OUT = os.path.join(WORK, "acestep", "genres")
SEED = 4242
MAX_SRC_SECONDS = 590.0

SYNTHONY = (
    "Epic symphonic EDM hybrid, full symphony orchestra fused with driving "
    "analog synthesizers; soaring string ensemble and heroic brass over "
    "pulsing synth bass and four-on-the-floor electronic drums, festival "
    "main-stage energy, cinematic modern production, instrumental")
SYNTHWAVE = (
    "80s synthwave retrowave, analog polysynth lead melody, arpeggiated "
    "sequencer bassline, gated reverb drums, warm analog pads, neon "
    "nighttime driving energy, punchy retro production, instrumental")
METAL = (
    "Symphonic power metal, distorted electric rhythm guitars and double "
    "kick drums under orchestral strings and heroic brass, epic anthemic "
    "energy, tight modern metal production, instrumental")
TRANCE = (
    "Uplifting trance, euphoric supersaw lead melody, rolling bassline, "
    "four-on-the-floor kick, shimmering arpeggios, wide breakdowns, clean "
    "club production, instrumental")
FUNK = (
    "Jazz-funk fusion, electric piano and clavinet, slap bass groove, "
    "tight funky drums, punchy horn section stabs, groovy energetic "
    "instrumental")

# The turbo sampler's stock 8-step schedule ends [..., 0.5, 0.3]:
# cover_noise_strength=0.80 starts at t=0.3 (1 step -> caption is a no-op,
# round-1 finding: all genres byte-similar), 0.50 starts at t=0.5 (2 steps),
# 0.35/0.20 start at t=0.64/0.83 (3/6 steps -> caption gets room to act).
# A dense custom schedule covers all 20 valid timesteps, so after the
# cover-noise truncation more low-noise refinement steps remain.
DENSE_TIMESTEPS = [
    1.0, 0.9545454545454546, 0.9333333333333333, 0.9, 0.875,
    0.8571428571428571, 0.8333333333333334, 0.7692307692307693, 0.75,
    0.6666666666666666, 0.6428571428571429, 0.625, 0.5454545454545454,
    0.5, 0.4, 0.375, 0.3, 0.25, 0.2222222222222222, 0.125,
]

# (key, cover_noise_strength, audio_cover_strength, caption, timesteps-or-None)
# audio_cover_strength < 1.0 hands the last (1-strength) fraction of steps to
# text-only conditioning (the caption, no source FSQ codes) — the second
# style lever besides retention.
VARIANTS = [
    # round 1 (kept for skip-idempotency; all five 0.80s rendered identical)
    ("synthony", 0.80, 1.0, SYNTHONY, None),
    ("synthony_n050", 0.50, 1.0, SYNTHONY, None),
    ("synthwave", 0.80, 1.0, SYNTHWAVE, None),
    ("metal", 0.80, 1.0, METAL, None),
    ("trance", 0.80, 1.0, TRANCE, None),
    ("funk", 0.80, 1.0, FUNK, None),
    # round 2: retention low enough for the caption to bite
    # (finding: 0.35 genres differ only subtly, corr 0.94-0.96; 0.20 has
    # real latitude, corr 0.61 vs 0.35; dense schedule was a no-op, 0.998)
    ("synthony_n035", 0.35, 1.0, SYNTHONY, None),
    ("synthony_n020", 0.20, 1.0, SYNTHONY, None),
    ("synthwave_n035", 0.35, 1.0, SYNTHWAVE, None),
    ("metal_n035", 0.35, 1.0, METAL, None),
    ("trance_n035", 0.35, 1.0, TRANCE, None),
    ("synthony_n050_dense", 0.50, 1.0, SYNTHONY, DENSE_TIMESTEPS),
    # round 2b: genre spread at 0.20 + text-only late steps
    ("synthwave_n020", 0.20, 1.0, SYNTHWAVE, None),
    ("metal_n020", 0.20, 1.0, METAL, None),
    ("trance_n020", 0.20, 1.0, TRANCE, None),
    ("metal_n035_cs05", 0.35, 0.5, METAL, None),
    ("synthony_n050_cs05", 0.50, 0.5, SYNTHONY, None),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ffprobe_duration(path):
    p = run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", path])
    return float(p.stdout.strip())


def read_loop(nn):
    loop_path = os.path.join(CLASSIC, f"hdmusic_{nn:02d}.loop")
    if not os.path.exists(loop_path):
        return None, None
    start = length = None
    with open(loop_path) as f:
        for line in f:
            if line.startswith("LOOPSTART"):
                start = int(line.split()[1])
            elif line.startswith("LOOPLENGTH"):
                length = int(line.split()[1])
    return start, length


def envelope_corr(path_a, path_b):
    def env(path):
        p = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", "8000",
             "-f", "s16le", "-"], capture_output=True)
        raw = p.stdout
        n = 2000  # 0.25 s at 8 kHz
        vals = []
        for i in range(0, len(raw) - n * 2, n * 2):
            s = struct.unpack(f"<{n}h", raw[i:i + n * 2])
            vals.append(math.sqrt(sum(x * x for x in s) / n))
        return vals

    a, b = env(path_a), env(path_b)
    m = min(len(a), len(b))
    a, b = a[:m], b[:m]
    ma, mb = sum(a) / m, sum(b) / m
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = math.sqrt(sum((x - ma) ** 2 for x in a))
    db = math.sqrt(sum((y - mb) ** 2 for y in b))
    return num / (da * db) if da and db else float("nan")


def main():
    from acestep.handler import AceStepHandler
    from acestep.llm_inference import LLMHandler
    from acestep.inference import GenerationParams, GenerationConfig, generate_music

    nn = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    os.makedirs(OUT, exist_ok=True)

    # classic render + loop-aware cover source (kept for the A/B page)
    wav = os.path.join(CLASSIC, f"hdmusic_{nn:02d}.wav")
    if not os.path.exists(wav):
        p = run(["bash", os.path.join(REPO, "tools", "hd_music_pipeline.sh"),
                 "classic", str(nn)], cwd=REPO)
        if p.returncode != 0:
            print(f"classic render failed: {p.stderr.strip()[-300:]}")
            sys.exit(1)
    dur = ffprobe_duration(wav)
    loop_start, loop_length = read_loop(nn)
    loops = loop_start is not None and loop_length is not None
    loop_start_s = (loop_start / 44100.0) if loops else 0.0

    src = os.path.join(CLASSIC, f"hdmusic_{nn:02d}_x2.wav")
    if not os.path.exists(src):
        src_dur = dur + (dur - loop_start_s) if loops else dur
        if not loops or src_dur > MAX_SRC_SECONDS:
            subprocess.run(["cp", wav, src], check=True)
        else:
            p = run(["ffmpeg", "-y", "-v", "error", "-i", wav, "-filter_complex",
                     f"[0:a]atrim=start={loop_start_s}[l];[0:a][l]concat=n=2:v=0:a=1",
                     src])
            if p.returncode != 0:
                print(f"concat failed: {p.stderr.strip()[-300:]}")
                sys.exit(1)

    dit_handler = AceStepHandler()
    llm_handler = LLMHandler()
    t0 = time.time()
    msg, ok = dit_handler.initialize_service(
        project_root=ACESTEP_ROOT,
        config_path="acestep-v15-turbo",
        device="auto",
        use_mlx_dit=False,  # MLX DiT ignores cover_noise_strength
    )
    print(f"[init] ok={ok} ({time.time() - t0:.1f}s)", flush=True)
    if not ok:
        sys.exit(1)

    for key, noise, cover_strength, caption, timesteps in VARIANTS:
        flac = os.path.join(OUT, f"t{nn:02d}_{key}.flac")
        if os.path.exists(flac):
            print(f"[skip] {key}: exists", flush=True)
            continue
        params = GenerationParams(
            task_type="cover",
            src_audio=src,
            caption=caption,
            lyrics="[Instrumental]",
            audio_cover_strength=cover_strength,
            cover_noise_strength=noise,
            timesteps=timesteps,
            thinking=False,
        )
        config = GenerationConfig(
            batch_size=1,
            use_random_seed=False,
            seeds=[SEED],
            audio_format="flac",
        )
        t1 = time.time()
        result = generate_music(dit_handler, llm_handler, params, config, save_dir=OUT)
        if not result.success:
            print(f"[error] {key}: {result.error}", flush=True)
            continue
        os.replace(result.audios[0]["path"], flac)
        corr = envelope_corr(src, flac)
        print(f"[done] {key}: gen {time.time() - t1:.0f}s corr {corr:.3f}",
              flush=True)

    print("[all-done]", flush=True)


if __name__ == "__main__":
    main()
