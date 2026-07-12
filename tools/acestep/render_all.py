#!/usr/bin/env python3
"""Render ACE-Step covers of the FULL soundtrack with the locked recipe.

Locked recipe (user, 2026-07-11): task_type="cover", torch/MPS DiT,
cover_noise_strength 0.80, seed 4242, generic orchestral caption.

Per song 1..41:
  1. classic OPL render via tools/hd_music_pipeline.sh classic NN (skipped if
     the wav already exists)
  2. cover source = full track + the looped region appended (so the model
     hears the loop repeat; for loopstart=0 this is the whole track twice)
  3. ACE-Step cover (model loaded once for the whole run)
  4. post: cut back to one full pass, resample 44.1 kHz stereo, gain-match to
     the classic render's mean volume, encode ogg with the classic LOOPSTART/
     LOOPLENGTH tags -> hdmusic_work/variants/hdmusic_NN.acestep.ogg
  5. log an envelope-correlation score vs classic

Run from the ACE-Step repo root:
    uv run python render_all.py
"""
import math
import os
import re
import struct
import subprocess
import sys
import time

ACESTEP_ROOT = os.environ.get("ACESTEP_ROOT",
                              os.path.expanduser("~/source/ACE-Step-1.5"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(REPO, "hdmusic_work")
CLASSIC = os.path.join(WORK, "classic")
OUT = os.path.join(WORK, "acestep", "out")
VARIANTS = os.path.join(WORK, "variants")
MUSIC_NUM = 41
SEED = 4242
NOISE = 0.80
MAX_SRC_SECONDS = 590.0  # ACE-Step duration cap is 600

CAPTION = (
    "Epic orchestral video game theme, heroic and driving; "
    "brass section lead melody, string ensemble, timpani and orchestral "
    "percussion; tight dry 90s studio recording, clear mix, instrumental"
)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ffprobe_duration(path):
    p = run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", path])
    return float(p.stdout.strip())


def mean_volume(path):
    p = run(["ffmpeg", "-i", path, "-af", "volumedetect", "-f", "null", "-"])
    m = re.search(r"mean_volume: (-?[\d.]+) dB", p.stderr)
    return float(m.group(1)) if m else None


def read_loop(nn):
    # Non-looping jingles (e.g. 10 "End of Level", 11 "Game Over Solo") write
    # no .loop file.
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

    os.makedirs(OUT, exist_ok=True)
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

    for nn in range(1, MUSIC_NUM + 1):
        try:
            wav = os.path.join(CLASSIC, f"hdmusic_{nn:02d}.wav")
            if not os.path.exists(wav):
                p = run(["bash", os.path.join(REPO, "tools", "hd_music_pipeline.sh"),
                         "classic", str(nn)], cwd=REPO)
                if p.returncode != 0:
                    print(f"[error] song {nn}: classic render failed: {p.stderr.strip()[-200:]}", flush=True)
                    continue

            dur = ffprobe_duration(wav)
            loop_start, loop_length = read_loop(nn)
            loops = loop_start is not None and loop_length is not None
            loop_start_s = (loop_start / 44100.0) if loops else 0.0

            # source = full track + looped region appended (single pass for
            # non-looping jingles)
            src = os.path.join(CLASSIC, f"hdmusic_{nn:02d}_cover_src.wav")
            src_dur = dur + (dur - loop_start_s) if loops else dur
            if not loops or src_dur > MAX_SRC_SECONDS:
                if src_dur > MAX_SRC_SECONDS:
                    print(f"[warn] song {nn}: source {src_dur:.0f}s > cap, using single pass", flush=True)
                subprocess.run(["cp", wav, src], check=True)
            else:
                p = run(["ffmpeg", "-y", "-v", "error", "-i", wav, "-filter_complex",
                         f"[0:a]atrim=start={loop_start_s}[l];[0:a][l]concat=n=2:v=0:a=1",
                         src])
                if p.returncode != 0:
                    print(f"[error] song {nn}: concat failed: {p.stderr.strip()[-200:]}", flush=True)
                    continue

            flac = os.path.join(OUT, f"t{nn:02d}_n080.flac")
            if not os.path.exists(flac):
                params = GenerationParams(
                    task_type="cover",
                    src_audio=src,
                    caption=CAPTION,
                    lyrics="[Instrumental]",
                    audio_cover_strength=1.0,
                    cover_noise_strength=NOISE,
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
                    print(f"[error] song {nn}: generation failed: {result.error}", flush=True)
                    continue
                os.replace(result.audios[0]["path"], flac)
                gen_s = time.time() - t1
            else:
                gen_s = 0.0

            # post-process to a game-ready variant ogg
            classic_mean = mean_volume(wav)
            cut = os.path.join(OUT, f"t{nn:02d}_cut.wav")
            p = run(["ffmpeg", "-y", "-v", "error", "-i", flac,
                     "-af", f"atrim=0:{dur}", "-ar", "44100", "-ac", "2", cut])
            if p.returncode != 0:
                print(f"[error] song {nn}: cut failed: {p.stderr.strip()[-200:]}", flush=True)
                continue
            cover_mean = mean_volume(cut)
            gain = 0.0
            if classic_mean is not None and cover_mean is not None:
                gain = classic_mean - cover_mean
            ogg = os.path.join(VARIANTS, f"hdmusic_{nn:02d}.acestep.ogg")
            meta = ["-metadata", f"LOOPSTART={loop_start}",
                    "-metadata", f"LOOPLENGTH={loop_length}"] if loops else []
            p = run(["ffmpeg", "-y", "-v", "error", "-i", cut,
                     "-af", f"volume={gain:.2f}dB",
                     "-c:a", "libvorbis", "-qscale:a", "6",
                     *meta, ogg])
            if p.returncode != 0:
                print(f"[error] song {nn}: encode failed: {p.stderr.strip()[-200:]}", flush=True)
                continue
            corr = envelope_corr(wav, cut)
            os.remove(cut)
            os.remove(src)
            print(f"[done] song {nn:02d}: gen {gen_s:.0f}s corr {corr:.3f} "
                  f"gain {gain:+.1f}dB -> {os.path.basename(ogg)}", flush=True)
        except Exception as exc:  # keep the batch going on any per-song failure
            print(f"[error] song {nn}: {type(exc).__name__}: {exc}", flush=True)

    print("[all-done]", flush=True)


if __name__ == "__main__":
    main()
