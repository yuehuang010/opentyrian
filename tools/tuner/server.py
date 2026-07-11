#!/usr/bin/env python3
"""tools/tuner/server.py - local ear-tuning web app for the HD (soundfont)
remaster of an OpenTyrian LDS music track.

Zero external dependencies: Python 3 stdlib only (http.server, json,
subprocess, hashlib, threading, argparse). Shells out to the already-built
hdmusic_work/bin/{lds_to_midi,render_music} tools (see tools/hd_music_pipeline.sh)
plus ffmpeg/ffprobe/fluidsynth on PATH, exactly like tools/compare_mix_balance.py
does for its per-group solo renders.

Usage:
    python3 tools/tuner/server.py --song 30 [--port 8765]
        [--data /path/to/tyrian21] [--sf2 hdmusic_work/sf/FluidR3_GM.sf2]
        [--map tools/lds_gm_map.txt] [--trim 113.669048]

Then open http://localhost:8765 in a browser. No AI is involved in the
loop -- this is a human ear-tuning tool: change GM instruments, per-voice
volume/transpose/cents/gate/echo, remix, and A/B against the classic OPL
render in real time; "Save to map" rewrites tools/lds_gm_map.txt losslessly
(only the numeric fields/options of the fingerprints you actually changed;
every other line, including trailing comments, is preserved byte-for-byte).

Cache dir: hdmusic_work/tuner/<song>/ (never touches hdmusic_work/midi/ or
other pipeline caches -- the tuner always renders through its own temp maps
and out-dirs).
"""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
HDWORK = REPO / "hdmusic_work"
BINDIR = HDWORK / "bin"

DEFAULT_DATA = "/Users/felixhuang/source/opentyrian/tyrian21"
DEFAULT_SF2 = str(HDWORK / "sf" / "FluidR3_GM.sf2")
DEFAULT_MAP = str(TOOLS / "lds_gm_map.txt")

FLUID_ARGS = ["-ni", "-R", "0", "-C", "0", "-g", "0.48", "-r", "44100"]

# Per-song trim override (loop length in seconds). Only song 30 is hardcoded
# per the ear-tuning sessions to date (internal/hd-music-track30.md); other
# songs render full-length and untrimmed unless --trim is passed.
TRIM_BY_SONG = {30: 113.669048}

# Per-song special-voice config: fingerprints whose map volume is pinned at 0
# because they're rendered by a different path than "GM program on this
# fingerprint" (see internal/hd-music-opl-idioms.md idioms #1-3). The tuner
# renders/caches these stems too (so they can be muted/soloed/A-B'd) but only
# exposes a gain control in the UI -- no program/transpose/cents.
SPECIAL_BY_SONG = {
    30: {
        "c5eedaea": {"kind": "chip", "gain_db": 2.6, "label": "OPL chip layer"},
        "8251725a": {"kind": "perc", "gain_db": 2.4, "label": "Percussion rewrite"},
    },
}

MAX_WORKERS = 4


def run(cmd, env=None, cwd=None, check=True):
    r = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(
            f"command failed ({r.returncode}): {' '.join(str(c) for c in cmd)}\n"
            f"--- stdout ---\n{r.stdout}\n--- stderr ---\n{r.stderr}"
        )
    return r


def ensure_binaries():
    need = [BINDIR / "render_music", BINDIR / "lds_to_midi"]
    if all(p.exists() and os.access(p, os.X_OK) for p in need):
        return
    print("tuner: binaries missing, running hd_music_pipeline.sh build ...", file=sys.stderr)
    run(["bash", str(TOOLS / "hd_music_pipeline.sh"), "build"])
    for p in need:
        if not p.exists():
            sys.exit(f"error: expected {p} after build, not found")


# -------------------------------------------------------------------------
# lds_gm_map.txt: lossless parse + rewrite
# -------------------------------------------------------------------------
#
# A line is either:
#   - a "raw" passthrough line (blank, or first non-space char is '#') --
#     kept byte-for-byte on save, always.
#   - a "voice" line: <fp> <gm> [transpose] [volume] [cents] [key=val ...] [# comment]
#     Positional fields are consumed left-to-right as long as the next token
#     is a plain (possibly signed) integer and fewer than 3 positional
#     fields (transpose, volume, cents) have been consumed yet; the first
#     token containing '=' (or '#') ends the positional run and starts the
#     key=value options. Kept byte-for-byte on save UNLESS the in-memory
#     settings for that fingerprint differ from what was parsed, in which
#     case only the numeric/option fields are regenerated -- the trailing
#     comment (verbatim, including the '#') is always preserved.

INT_RE = re.compile(r"^-?\d+$")
FP_LINE_RE = re.compile(r"^(?P<lead>\s*)(?P<fp>[0-9a-fA-F]{8})(?P<rest>.*)$")


class MapFile:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.lines = []       # list of ("raw", text) | ("voice", fp)
        self.voices = {}      # fp(lower) -> dict of parsed fields
        self.order = []       # fp order as first seen
        self._load()

    def _load(self):
        text = self.path.read_text()
        for raw in text.splitlines():
            stripped = raw.lstrip()
            if not stripped or stripped.startswith("#"):
                self.lines.append(("raw", raw))
                continue
            m = FP_LINE_RE.match(raw)
            if not m:
                # doesn't look like a voice line after all; keep verbatim
                self.lines.append(("raw", raw))
                continue
            fp = m.group("fp").lower()
            rest = m.group("rest")
            comment = None
            body = rest
            if "#" in rest:
                body, comment = rest.split("#", 1)
                comment = "#" + comment
            tokens = body.split()
            gm = int(tokens[0]) if tokens else 0
            transpose, volume, cents = 0, 100, 0
            positional_slots = [("transpose", 0), ("volume", 100), ("cents", 0)]
            idx = 1
            consumed = 0
            values = {}
            while idx < len(tokens) and consumed < 3 and INT_RE.match(tokens[idx]):
                name, _default = positional_slots[consumed]
                values[name] = int(tokens[idx])
                consumed += 1
                idx += 1
            transpose = values.get("transpose", 0)
            volume = values.get("volume", 100)
            cents = values.get("cents", 0)
            rev = cho = None
            gate = 100
            echo = 100
            while idx < len(tokens):
                t = tokens[idx]
                if "=" in t:
                    key, _, val = t.partition("=")
                    if val.lstrip("-").isdigit():
                        val = int(val)
                        if key == "rev":
                            rev = val
                        elif key == "cho":
                            cho = val
                        elif key == "gate":
                            gate = val
                        elif key == "echo":
                            echo = val
                idx += 1

            entry = {
                "fp": fp,
                "gm": gm,
                "transpose": transpose,
                "volume": volume,
                "cents": cents,
                "gate": gate,
                "echo": echo,
                "rev": rev,
                "cho": cho,
                "comment": comment,
            }
            if fp not in self.voices:
                self.order.append(fp)
            self.voices[fp] = entry
            self.lines.append(("voice", fp))

    def comment_label(self, fp):
        c = self.voices[fp].get("comment")
        if not c:
            return ""
        return c.lstrip("#").strip()

    @staticmethod
    def _format_voice_line(entry):
        parts = [entry["fp"], str(entry["gm"]), str(entry["transpose"]),
                 str(entry["volume"]), str(entry["cents"])]
        line = "  ".join(parts)
        opts = []
        if entry.get("rev") is not None:
            opts.append(f"rev={entry['rev']}")
        if entry.get("cho") is not None:
            opts.append(f"cho={entry['cho']}")
        if entry.get("gate", 100) != 100:
            opts.append(f"gate={entry['gate']}")
        if entry.get("echo", 100) != 100:
            opts.append(f"echo={entry['echo']}")
        if opts:
            line += "  " + " ".join(opts)
        if entry.get("comment"):
            line += "  " + entry["comment"]
        return line

    def render(self, overrides):
        """overrides: fp(lower) -> dict of NEW field values (subset of
        gm/transpose/volume/cents/gate/echo). Returns the full file text.
        Lines for fingerprints not in `overrides` (or whose values are
        unchanged) are emitted byte-identical to the source file."""
        out = []
        for kind, payload in self.lines:
            if kind == "raw":
                out.append(payload)
                continue
            fp = payload
            entry = dict(self.voices[fp])
            changed = False
            if fp in overrides:
                for k, v in overrides[fp].items():
                    if entry.get(k) != v:
                        entry[k] = v
                        changed = True
            if changed:
                out.append(self._format_voice_line(entry))
            else:
                # find original raw text for this line -- reconstruct by
                # re-walking is wasteful; instead we kept only ("voice", fp)
                # so recover verbatim text from a parallel raw-text list.
                out.append(self._raw_text_for(fp))
        text = "\n".join(out)
        if not text.endswith("\n"):
            text += "\n"
        return text

    def _raw_text_for(self, fp):
        return self._raw_lines[self._voice_line_index[fp]]

    # populated by _load2 below (kept separate to not disturb the parse
    # logic above); see _load_raw_text().
    _raw_lines = None
    _voice_line_index = None


def _map_load_raw_text(mf: MapFile):
    """Second pass: remember the exact original text of every voice line
    (by first occurrence) so unedited lines can be echoed byte-for-byte."""
    raw_lines = mf.path.read_text().splitlines()
    idx = 0
    voice_idx = {}
    li = 0
    for kind, payload in mf.lines:
        if kind == "voice" and payload not in voice_idx:
            voice_idx[payload] = li
        li += 1
    mf._raw_lines = raw_lines
    mf._voice_line_index = voice_idx


def load_map(path):
    mf = MapFile(path)
    _map_load_raw_text(mf)
    return mf


# -------------------------------------------------------------------------
# .patches parsing (note stats)
# -------------------------------------------------------------------------

PATCH_RE = re.compile(
    r"^(?P<fp>[0-9a-f]{8})\s+ch=(?P<ch>\S+)\s+tick=(?P<tick>\d+)\s+"
    r"notes=(?P<notes>\d+)\s+range=(?P<range>\S+)\s+gate=(?P<gate>[\d.]+)"
)


def parse_patches(path):
    out = {}
    p = pathlib.Path(path)
    if not p.exists():
        return out
    for line in p.read_text().splitlines():
        m = PATCH_RE.match(line)
        if not m:
            continue
        out[m.group("fp").lower()] = {
            "channels": m.group("ch"),
            "first_tick": int(m.group("tick")),
            "notes": int(m.group("notes")),
            "range": m.group("range"),
            "avg_gate_ticks": float(m.group("gate")),
        }
    return out


# -------------------------------------------------------------------------
# Rendering pipeline
# -------------------------------------------------------------------------

class Tuner:
    def __init__(self, song, data_dir, sf2, map_path, trim, port):
        self.song = song
        self.song2 = f"{song:02d}"
        self.data_dir = data_dir
        self.sf2 = sf2
        self.map_path = map_path
        self.trim = trim
        self.port = port

        self.workdir = HDWORK / "tuner" / self.song2
        self.base_dir = self.workdir / "base"      # lds_to_midi base output (.notes/.patches)
        self.solo_dir = self.workdir / "solo"       # temp maps + midi/wav scratch
        self.stems_dir = self.workdir / "stems"     # cached opus stems
        self.classic_dir = self.workdir / "classic"
        for d in (self.base_dir, self.solo_dir, self.stems_dir, self.classic_dir):
            d.mkdir(parents=True, exist_ok=True)

        self.mapfile = load_map(map_path)
        self.specials = SPECIAL_BY_SONG.get(song, {})

        # current in-memory settings per fp, seeded from the map
        self.current = {}
        for fp in self.mapfile.order:
            e = self.mapfile.voices[fp]
            self.current[fp] = {
                "gm": e["gm"], "transpose": e["transpose"], "volume": e["volume"],
                "cents": e["cents"], "gate": e["gate"], "echo": e["echo"],
            }

        self.stem_url = {}       # fp -> "/stems/xxx.opus" (current cached url)
        self.classic_url = None
        self.fp_locks = {fp: threading.Lock() for fp in self.mapfile.order}
        self.executor = ThreadPoolExecutor(max_workers=MAX_WORKERS)
        self.state_lock = threading.Lock()
        self.prewarm_total = len(self.mapfile.order) + 1  # +1 classic reference
        self.prewarm_done = 0
        self.notes_path = self.base_dir / f"hdmusic_{self.song2}.notes"
        self.patches_path = self.base_dir / f"hdmusic_{self.song2}.patches"
        self.patch_stats = {}

    # -- bootstrap -------------------------------------------------------

    def bootstrap(self):
        ensure_binaries()
        # Run lds_to_midi once with the REAL map so .notes/.patches exist for
        # the note-stats panel and for v13_percussion.py's input.
        env = dict(os.environ)
        env["LDS_GM_MAP"] = self.map_path
        run([str(BINDIR / "lds_to_midi"), self.data_dir, str(self.base_dir), str(self.song)], env=env)
        self.patch_stats = parse_patches(self.patches_path)

    def start_prewarm(self):
        t = threading.Thread(target=self._prewarm_worker, daemon=True)
        t.start()

    def _prewarm_worker(self):
        futures = []
        futures.append(self.executor.submit(self._safe_render_classic))
        for fp in self.mapfile.order:
            settings = dict(self.current[fp])
            futures.append(self.executor.submit(self._safe_render_stem, fp, settings))
        for f in futures:
            f.result()

    def _safe_render_classic(self):
        try:
            self.render_classic()
        except Exception as e:
            print(f"tuner: classic reference render failed: {e}", file=sys.stderr)
        finally:
            with self.state_lock:
                self.prewarm_done += 1

    def _safe_render_stem(self, fp, settings):
        try:
            self.render_stem(fp, settings)
        except Exception as e:
            print(f"tuner: stem render failed for {fp}: {e}", file=sys.stderr)
        finally:
            with self.state_lock:
                self.prewarm_done += 1

    # -- hashing / cache ---------------------------------------------------

    def settings_hash(self, fp, settings, special_kind="none"):
        h = hashlib.sha1()
        parts = [
            fp, str(settings.get("gm", 0)), str(settings.get("transpose", 0)),
            str(settings.get("cents", 0)), str(settings.get("gate", 100)),
            str(settings.get("echo", 100)), str(self.song), special_kind,
        ]
        h.update("|".join(parts).encode("utf-8"))
        return h.hexdigest()[:8]

    def trim_args(self):
        return ["-t", str(self.trim)] if self.trim else []

    def _encode_opus(self, wav_path, opus_path):
        tmp = opus_path.with_suffix(".tmp.opus")
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav_path)]
        cmd += self.trim_args()
        cmd += ["-ac", "1", "-ar", "48000", "-c:a", "libopus", "-b:a", "64k", str(tmp)]
        run(cmd)
        tmp.replace(opus_path)

    # -- classic reference -------------------------------------------------

    def render_classic(self):
        if self.classic_url:
            return self.classic_url
        opus_path = self.classic_dir / f"hdmusic_{self.song2}_classic.opus"
        if not opus_path.exists():
            tmp = self.classic_dir / "raw"
            tmp.mkdir(exist_ok=True)
            env = dict(os.environ)
            env.pop("LDS_SOLO_FPS", None)
            run([str(BINDIR / "render_music"), self.data_dir, str(tmp), str(self.song)], env=env)
            wav = tmp / f"hdmusic_{self.song2}.wav"
            self._encode_opus(wav, opus_path)
            shutil.rmtree(tmp, ignore_errors=True)
        self.classic_url = f"/stems/classic/{opus_path.name}"
        return self.classic_url

    # -- special-voice stems (chip / perc) -----------------------------------

    def render_special(self, fp):
        spec = self.specials[fp]
        kind = spec["kind"]
        h = self.settings_hash(fp, {}, special_kind=kind)
        opus_path = self.stems_dir / f"{fp}_{h}.opus"
        with self.fp_locks[fp]:
            if opus_path.exists():
                self.stem_url[fp] = f"/stems/{opus_path.name}"
                return self.stem_url[fp]

            tmp = self.solo_dir / f"tmp_{fp}"
            tmp.mkdir(exist_ok=True)
            try:
                if kind == "chip":
                    env = dict(os.environ)
                    env["LDS_SOLO_FPS"] = fp
                    run([str(BINDIR / "render_music"), self.data_dir, str(tmp), str(self.song)], env=env)
                    wav = tmp / f"hdmusic_{self.song2}.wav"
                elif kind == "perc":
                    if not self.notes_path.exists():
                        raise RuntimeError(f"missing {self.notes_path}; bootstrap didn't run?")
                    perc_mid = tmp / "perc.mid"
                    run(["python3", str(TOOLS / "v13_percussion.py"),
                         str(self.notes_path), str(perc_mid), fp])
                    wav = tmp / "perc.wav"
                    run(["fluidsynth"] + FLUID_ARGS + ["-F", str(wav), self.sf2, str(perc_mid)])
                else:
                    raise RuntimeError(f"unknown special kind {kind}")
                self._encode_opus(wav, opus_path)
            finally:
                shutil.rmtree(tmp, ignore_errors=True)

            self.stem_url[fp] = f"/stems/{opus_path.name}"
            return self.stem_url[fp]

    # -- normal per-voice stems --------------------------------------------

    def _solo_map_text(self, fp, settings):
        lines = []
        for other in self.mapfile.order:
            if other == fp:
                gate = settings.get("gate", 100)
                echo = settings.get("echo", 100)
                extra = ""
                if gate != 100:
                    extra += f" gate={gate}"
                if echo != 100:
                    extra += f" echo={echo}"
                lines.append(
                    f"{fp}  {settings.get('gm', 0)}  {settings.get('transpose', 0)}  100  "
                    f"{settings.get('cents', 0)}{extra}"
                )
            else:
                other_gm = self.mapfile.voices[other]["gm"]
                lines.append(f"{other}  {other_gm}  0  0")
        return "\n".join(lines) + "\n"

    def render_stem(self, fp, settings):
        if fp in self.specials:
            return self.render_special(fp)

        h = self.settings_hash(fp, settings, special_kind="none")
        opus_path = self.stems_dir / f"{fp}_{h}.opus"
        with self.fp_locks[fp]:
            if opus_path.exists():
                self.stem_url[fp] = f"/stems/{opus_path.name}"
                self.current[fp].update(settings)
                return self.stem_url[fp]

            tmp = self.solo_dir / f"tmp_{fp}"
            tmp.mkdir(exist_ok=True)
            try:
                solo_map_path = tmp / "solo_map.txt"
                solo_map_path.write_text(self._solo_map_text(fp, settings))

                env = dict(os.environ)
                env["LDS_GM_MAP"] = str(solo_map_path)
                run([str(BINDIR / "lds_to_midi"), self.data_dir, str(tmp), str(self.song)], env=env)
                mid = tmp / f"hdmusic_{self.song2}.mid"
                if not mid.exists():
                    raise RuntimeError(f"lds_to_midi did not produce {mid}")

                wav = tmp / "render.wav"
                run(["fluidsynth"] + FLUID_ARGS + ["-F", str(wav), self.sf2, str(mid)])
                self._encode_opus(wav, opus_path)
            finally:
                shutil.rmtree(tmp, ignore_errors=True)

            self.stem_url[fp] = f"/stems/{opus_path.name}"
            self.current[fp].update(settings)
            return self.stem_url[fp]

    # -- state for the UI ----------------------------------------------------

    def voice_state(self, fp):
        e = self.mapfile.voices[fp]
        cur = self.current[fp]
        spec = self.specials.get(fp)
        stats = self.patch_stats.get(fp, {})
        return {
            "fp": fp,
            "label": self.mapfile.comment_label(fp),
            "special": spec["kind"] if spec else "none",
            "special_label": spec["label"] if spec else None,
            "default_gain_db": spec["gain_db"] if spec else 0.0,
            "gm": cur["gm"], "transpose": cur["transpose"], "volume": cur["volume"],
            "cents": cur["cents"], "gate": cur["gate"], "echo": cur["echo"],
            "stem_url": self.stem_url.get(fp),
            "stats": stats,
        }

    def state(self):
        with self.state_lock:
            done, total = self.prewarm_done, self.prewarm_total
        return {
            "song": self.song,
            "trim": self.trim,
            "port": self.port,
            "map_path": str(self.map_path),
            "classic_url": self.classic_url,
            "prewarm": {"done": done, "total": total},
            "voices": [self.voice_state(fp) for fp in self.mapfile.order],
        }

    # -- save ----------------------------------------------------------------

    def save(self, voices_payload):
        overrides = {}
        changed_fps = []
        for v in voices_payload:
            fp = str(v.get("fp", "")).lower()
            if fp not in self.mapfile.voices or fp in self.specials:
                continue  # special rows never touch the map (always vol 0)
            new_fields = {}
            for key, cast in (("gm", int), ("transpose", int), ("volume", int),
                              ("cents", int), ("gate", int), ("echo", int)):
                if key in v and v[key] is not None:
                    new_fields[key] = cast(v[key])
            if new_fields:
                overrides[fp] = new_fields
                orig = self.mapfile.voices[fp]
                if any(orig.get(k) != val for k, val in new_fields.items()):
                    changed_fps.append(fp)

        text = self.mapfile.render(overrides)

        backup = pathlib.Path(str(self.map_path) + ".bak")
        shutil.copyfile(self.map_path, backup)
        pathlib.Path(self.map_path).write_text(text)

        # reload so self.mapfile / self.current reflect the saved state
        self.mapfile = load_map(self.map_path)
        for fp in self.mapfile.order:
            e = self.mapfile.voices[fp]
            if fp not in self.current:
                self.current[fp] = {}
            self.current[fp].update({
                "gm": e["gm"], "transpose": e["transpose"], "volume": e["volume"],
                "cents": e["cents"], "gate": e["gate"], "echo": e["echo"],
            })
        return {"ok": True, "changed": changed_fps, "backup": str(backup)}


# -------------------------------------------------------------------------
# HTTP server
# -------------------------------------------------------------------------

TUNER: Tuner = None
INDEX_HTML_PATH = pathlib.Path(__file__).resolve().parent / "index.html"

MIME_OPUS = "audio/ogg"


class Handler(BaseHTTPRequestHandler):
    server_version = "OpenTyrianTuner/1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_bytes(self, data, content_type, cache=False):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        if cache:
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        return json.loads(raw or b"{}")

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/" or path == "/index.html":
            data = INDEX_HTML_PATH.read_bytes()
            self._send_bytes(data, "text/html; charset=utf-8")
            return

        if path == "/api/state":
            self._send_json(TUNER.state())
            return

        if path.startswith("/stems/"):
            rel = path[len("/stems/"):]
            if rel.startswith("classic/"):
                fp = TUNER.classic_dir / rel[len("classic/"):]
            else:
                fp = TUNER.stems_dir / rel
            fp = fp.resolve()
            # basic containment check
            if TUNER.workdir.resolve() not in fp.parents and fp != TUNER.workdir.resolve():
                self.send_error(403)
                return
            if not fp.exists() or not fp.is_file():
                self.send_error(404)
                return
            self._send_bytes(fp.read_bytes(), MIME_OPUS, cache=True)
            return

        self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/api/stem":
            body = self._read_json()
            fp = str(body.get("fp", "")).lower()
            if fp not in TUNER.mapfile.voices:
                self._send_json({"error": f"unknown fingerprint {fp}"}, status=400)
                return
            if fp in TUNER.specials:
                url = TUNER.render_stem(fp, {})
                self._send_json({"url": url})
                return
            settings = {
                "gm": int(body.get("gm", TUNER.current[fp]["gm"])),
                "transpose": int(body.get("transpose", 0)),
                "cents": int(body.get("cents", 0)),
                "gate": int(body.get("gate", 100) or 100),
                "echo": int(body.get("echo", 100) or 100),
            }
            try:
                url = TUNER.render_stem(fp, settings)
            except Exception as e:
                self._send_json({"error": str(e)}, status=500)
                return
            self._send_json({"url": url})
            return

        if path == "/api/save":
            body = self._read_json()
            voices = body.get("voices", [])
            result = TUNER.save(voices)
            self._send_json(result)
            return

        self.send_error(404)


def main():
    global TUNER
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--song", type=int, default=30)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--data", default=DEFAULT_DATA)
    ap.add_argument("--sf2", default=DEFAULT_SF2)
    ap.add_argument("--map", default=DEFAULT_MAP)
    ap.add_argument("--trim", type=float, default=None,
                     help="override loop trim in seconds (default: hardcoded 113.669048 for song 30, none otherwise)")
    args = ap.parse_args()

    trim = args.trim if args.trim is not None else TRIM_BY_SONG.get(args.song)

    TUNER = Tuner(args.song, args.data, args.sf2, args.map, trim, args.port)
    print(f"tuner: bootstrapping song {args.song} ...", file=sys.stderr)
    TUNER.bootstrap()
    TUNER.start_prewarm()

    httpd = ThreadingHTTPServer(("localhost", args.port), Handler)
    print(f"http://localhost:{args.port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
