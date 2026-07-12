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
FILMSYNTH = (
    "Epic hybrid film score, live symphony orchestra on a scoring stage "
    "layered with a deep analog synthesizer pulse; realistic violins, "
    "cellos, French horns and timpani carry the heroic theme over "
    "electronic bass and subtle synthetic percussion, dynamic crescendos, "
    "warm hall reverb, modern trailer production, instrumental")

FILMSCORE_DROP = FILMSCORE.replace(
    ", instrumental",
    "; dramatic buildup into a heavy bass drop, hard-hitting low end, "
    "strong dynamic contrast between sections, instrumental")
OPERASYNTH_DROP = (OPERASYNTH +
    "; dramatic buildup into a heavy bass drop, hard-hitting low end, "
    "strong dynamic contrast between sections")

# User bug report: the classic slams into full bass at ~22.2s (the "drop");
# all low-retention covers smear it. Countermeasure: pre-emphasize the
# event in the cover source so the exaggerated dynamics survive the
# regeneration. Times cover both loop passes (second = t + 113.669048).
DUCK_PRE_DROP = ("volume=volume=0.5:enable="
                 "'between(t,18.0,22.12)+between(t,131.67,135.79)'")
BOOST_DROP = ("volume=volume=1.6:enable="
              "'between(t,22.12,24.5)+between(t,135.79,138.17)'")

# (key, cover_noise_strength, audio_cover_strength, caption, seed, src_af)
# src_af: optional ffmpeg audio filter applied to the cover source.
VARIANTS = [
    # winners (kept; skip-idempotent)
    ("filmscore_n020", 0.20, 1.0, FILMSCORE, SEED, None),
    ("operasynth_n020", 0.20, 1.0, OPERASYNTH, SEED, None),
    # drop rescue: source pre-emphasis
    ("filmscore_n020_duck", 0.20, 1.0, FILMSCORE, SEED, DUCK_PRE_DROP),
    ("operasynth_n020_duck", 0.20, 1.0, OPERASYNTH, SEED, DUCK_PRE_DROP),
    ("filmscore_n020_boost", 0.20, 1.0, FILMSCORE, SEED, BOOST_DROP),
    # drop rescue: caption dynamics language, plain source
    ("filmscore_n020_dropcap", 0.20, 1.0, FILMSCORE_DROP, SEED, None),
    ("operasynth_n020_dropcap", 0.20, 1.0, OPERASYNTH_DROP, SEED, None),
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

    for key, noise, cover_strength, caption, seed, src_af in VARIANTS:
        flac = os.path.join(OUT, f"t{nn:02d}_{key}.flac")
        if os.path.exists(flac):
            print(f"[skip] {key}: exists", flush=True)
            continue
        variant_src = src
        if src_af:
            variant_src = os.path.join(CLASSIC, f"hdmusic_{nn:02d}_x2_{key}.wav")
            if not os.path.exists(variant_src):
                p = run(["ffmpeg", "-y", "-v", "error", "-i", src,
                         "-af", src_af, variant_src])
                if p.returncode != 0:
                    print(f"[error] {key}: src filter failed: {p.stderr.strip()[-200:]}", flush=True)
                    continue
        params = GenerationParams(
            task_type="cover",
            src_audio=variant_src,
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
