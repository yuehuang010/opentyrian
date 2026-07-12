# `tools/hd_build.py` — HD asset pipeline orchestrator

One cross-platform command that runs the HD extractors in order and builds
the distributable paks (`tyrian.base` + `tyrian.hd`). Replaces what used to
be ~8 manual tool invocations with inconsistent CLIs.

Pure stdlib at the top level (no `numpy`/`PIL` imports at module scope), so
`--list` and `--help` work with zero third-party deps and zero base data
present — deps are checked lazily via `importlib.util.find_spec` right
before running, for only the steps actually selected.

## Prerequisites

- Base Tyrian 2.1 data files unpacked into `tyrian21/` at the repo root (or
  wherever `--data` points — see the hardcoded-tools caveat below):
  https://camanis.net/tyrian/tyrian21.zip. The preflight check requires at
  least `palette.dat`, `tyrian.pic`, `tyrian.shp`.
- Python deps, only for the steps that need them:
  - `numpy` — the vector font step (`hd_vectorize_font.py`) and the tiles
    step (`hd_extract_tiles.py`).
  - `Pillow` — the tiles step only (used for `--method lanczos`, the
    orchestrator's default, and for `--preview`).
  - Everything else (backdrops, comp, filter, xBRZ font, anim, sfx, music,
    mkbundle) is stdlib-only.

## Usage

```sh
python3 tools/hd_build.py               # default run (extraction steps run concurrently)
python3 tools/hd_build.py --list        # print the step plan + exact commands, no side effects
python3 tools/hd_build.py --tiles --sfx # include the opt-in heavy/experimental steps
python3 tools/hd_build.py --bundle-only # just (re)build the paks from existing hd* files
python3 tools/hd_build.py --jobs 2      # cap concurrency at 2 extraction steps at a time
python3 tools/hd_build.py --jobs 1      # force fully-serial execution (debuggability)
```

On Windows: `python tools\hd_build.py` (same flags).

Run `python3 tools/hd_build.py --help` for the full flag reference
(`--data`, `--previews`, `--font {vector,xbrz}`, `--only STEP[,STEP...]`,
`--jobs`/`-j`, `--continue-on-error`, `--bundle-out`/`--hd-out`, etc).

**Concurrency**: the extraction steps (`backdrops`, `comp`, `filter`, `font`,
`anim`, `tiles`, `sfx`, `music`) read the same base data dir and write disjoint
`hd*`-prefixed outputs, so they have no interdependencies and now run
concurrently in a thread pool (`--jobs`/`-j`, default
`min(os.cpu_count(), number of extraction steps selected)`). `bundle`
(`mkbundle.py`) reads what the extraction steps wrote, so it's the one
ordering constraint: it always runs last, alone, only after every selected
extraction step has finished. Each extraction step's combined stdout+stderr
is captured and printed as one delimited block when that step completes (to
avoid interleaving output from concurrent subprocesses); the final summary
table is still printed in canonical step order regardless of completion
order. `--jobs 1` runs the same code path with a single worker, reproducing
serial, in-order execution for debugging a step in isolation. Failure
semantics are unchanged: by default a failed extraction step skips `bundle`
(overall exit code 1); `--continue-on-error` still attempts `bundle`
regardless.

## Step list

Default steps (run unless `--only`/`--no-bundle` says otherwise), in order:

1. `backdrops` — `hd_extract.py` — backdrops + static sprite tables + title logo
2. `comp` — `hd_extract_comp.py` — in-flight comp sprites (11,856 frames)
3. `filter` — `hd_extract_filter.py` — enemy hue-band brightness maps
4. `font` — `hd_vectorize_font.py` (or `hd_extract_font.py` with `--font xbrz`)
5. `anim` — `hd_extract_anim.py` — ending cutscene
6. `music` — `stage_music.py` — stage committed HD music OGGs (`hdmusic/`)
   into the data dir
7. `bundle` — `mkbundle.py` — writes `tyrian.base` + `tyrian.hd`

Opt-in steps (off by default):

- `tiles` (`--tiles`) — `hd_extract_tiles.py`, level tilesets. **Heavy**
  (~1GB output). Orchestrator defaults `--method lanczos` (not the tool's
  own default of `nearest`) to match the shipped `tyrian21/hdtile_lanczos/`
  variant.
- `sfx` (`--sfx`) — `hd_extract_snd.py`. **Read the caveat below** — this
  does not feed the bundle by itself.

`music` stages the 41 frozen HD music renders (`hdmusic/hdmusic_01.ogg` …
`hdmusic/hdmusic_41.ogg`) that are committed as source in the repo via Git
LFS into the data dir (default `tyrian21/`), so both direct engine playback
(`dir_fopen`-based loading in `src/loudness.c`) and `mkbundle.py` — which
only ever reads files physically present in its single `--src` dir via a
flat `os.listdir()` — can find them. **A fresh clone needs `git lfs pull`**
to materialize the real OGGs; a shallow/no-LFS clone leaves `hdmusic/` full
of ~130-byte Git LFS pointer stub files instead of audio.
`tools/stage_music.py` detects any source file under 1 KB as such a stub and
refuses to stage it, erroring with a `git lfs pull` hint rather than copying
a broken pointer file into the data dir. See `tools/MUSIC_REMASTER.md` for
how the OGGs were originally rendered/encoded (that render/encode pipeline,
`hd_music_pipeline.sh`, is a separate, unwired, bash + external-soundfont +
compiled-C-tools process — not needed for normal builds now that the output
is committed).

Not wired (out of scope): the music *render/encode* pipeline
(`hd_music_pipeline.sh` — bash, needs external soundfonts + compiled C
tools). It produces the OGGs that live in `hdmusic/`; you don't need to run
it unless you're re-rendering the music from scratch.

## Caveats discovered while wiring this up

- **Five tools hardcode `<repo>/tyrian21` and ignore `--data` entirely**:
  `hd_extract.py`, `hd_extract_comp.py`, `hd_extract_filter.py`,
  `hd_extract_anim.py`, and `hd_extract_font.py` (the `--font xbrz` path).
  If you pass `--data` pointing elsewhere, `hd_build.py` prints a WARNING —
  base data must still be present at `<repo>/tyrian21` for those steps
  regardless of `--data`. (Only `hd_vectorize_font.py`, `hd_extract_tiles.py`,
  `hd_extract_snd.py`, and `mkbundle.py --src` actually honor a custom data
  dir.)
- **`hd_extract_snd.py` is an EXPERIMENT tool, not a bundle-feeding step** —
  its own docstring says so. It dumps pristine reference WAVs to
  `tools/hdsfx_previews/orig/`, not `hd*`-prefixed assets into the data dir,
  so `mkbundle.py`'s `hd*` glob never picks them up. Producing real
  bundle-ready HD sfx needs the further (unwired, out of scope)
  `hd_upsample_snd.py` + `mkhdsnd.py` steps — see `tools/HDSFX_EXPERIMENT.md`.
- **`hd_extract_tiles.py` does not need `scipy`** despite a same-repo
  neighbor script (`hd_upsample_snd.py`) needing it for an unrelated reason
  referenced in a docstring comment — don't over-provision the venv.
- **`hd_extract_comp.py` and `hd_extract_filter.py` take no CLI flags at
  all** (no `argparse` in either file) — they always write one hardcoded
  preview sheet regardless of `hd_build.py --previews`.

## Outputs

`tyrian.base` and `tyrian.hd` (default paths: repo root) are gitignored —
they're built locally / in CI and shipped as release artifacts, not
committed. `--bundle-out`/`--hd-out` override the paths.

## See also

- `internal/plan/REMASTER_ASSETS.md` — per-asset HD status/inventory.
- `internal/plan/STANDALONE_PLAN.md` — the zero-external-data effort this
  pipeline ultimately feeds.
