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

# Round-2 findings (see internal/hd-music-ai-regen.md gotcha #4): the turbo
# schedule leaves 1 denoise step at retention 0.80 (caption no-op), 3 steps
# at 0.35 (subtle), 6 at 0.20 (real genre separation). Dense custom
# timesteps and audio_cover_strength<1.0 are measured dead ends. So
# exploration runs at retention 0.10-0.20 where the caption has real power.

OPERA = (
    "Epic operatic symphony, live full symphony orchestra in a grand "
    "concert hall; dramatic wordless operatic choir and soaring soprano "
    "vocalise over lush legato strings, French horns and trumpets, timpani "
    "rolls and harp flourishes; expressive dynamics, lifelike acoustic "
    "recording, film-score grandeur")
FILMSCORE = (
    "Hollywood film score, live symphony orchestra recorded on a scoring "
    "stage; realistic acoustic instruments: violins, cellos, French horns, "
    "oboes, clarinets, timpani and orchestral percussion; heroic main theme "
    "with dynamic crescendos, warm natural hall reverb, pristine recording, "
    "instrumental")
OPERASYNTH = (
    "Operatic synthony, live symphony orchestra and dramatic wordless choir "
    "fused with a deep analog synthesizer pulse; realistic strings and "
    "brass carry the melody over an electronic bass heartbeat, cinematic "
    "hybrid production, epic and theatrical")

# (key, cover_noise_strength, audio_cover_strength, caption, seed)
VARIANTS = [
    ("opera_n020", 0.20, 1.0, OPERA, SEED),
    ("opera_n010", 0.10, 1.0, OPERA, SEED),
    ("opera_n020_alt", 0.20, 1.0, OPERA, 1337),
    ("filmscore_n020", 0.20, 1.0, FILMSCORE, SEED),
    ("filmscore_n010", 0.10, 1.0, FILMSCORE, SEED),
    ("operasynth_n020", 0.20, 1.0, OPERASYNTH, SEED),
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

    for key, noise, cover_strength, caption, seed in VARIANTS:
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
            thinking=False,
        )
        config = GenerationConfig(
            batch_size=1,
            use_random_seed=False,
            seeds=[seed],
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
