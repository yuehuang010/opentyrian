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
  - Everything else (backdrops, comp, filter, xBRZ font, anim, sfx,
    mkbundle) is stdlib-only.

## Usage

```sh
python3 tools/hd_build.py               # default run
python3 tools/hd_build.py --list        # print the step plan + exact commands, no side effects
python3 tools/hd_build.py --tiles --sfx # include the opt-in heavy/experimental steps
python3 tools/hd_build.py --bundle-only # just (re)build the paks from existing hd* files
```

On Windows: `python tools\hd_build.py` (same flags).

Run `python3 tools/hd_build.py --help` for the full flag reference
(`--data`, `--previews`, `--font {vector,xbrz}`, `--only STEP[,STEP...]`,
`--continue-on-error`, `--bundle-out`/`--hd-out`, etc).

## Step list

Default steps (run unless `--only`/`--no-bundle` says otherwise), in order:

1. `backdrops` — `hd_extract.py` — backdrops + static sprite tables + title logo
2. `comp` — `hd_extract_comp.py` — in-flight comp sprites (11,856 frames)
3. `filter` — `hd_extract_filter.py` — enemy hue-band brightness maps
4. `font` — `hd_vectorize_font.py` (or `hd_extract_font.py` with `--font xbrz`)
5. `anim` — `hd_extract_anim.py` — ending cutscene
6. `bundle` — `mkbundle.py` — writes `tyrian.base` + `tyrian.hd`

Opt-in steps (off by default):

- `tiles` (`--tiles`) — `hd_extract_tiles.py`, level tilesets. **Heavy**
  (~1GB output). Orchestrator defaults `--method lanczos` (not the tool's
  own default of `nearest`) to match the shipped `tyrian21/hdtile_lanczos/`
  variant.
- `sfx` (`--sfx`) — `hd_extract_snd.py`. **Read the caveat below** — this
  does not feed the bundle by itself.

Not wired (out of scope): the music pipeline (`hd_music_pipeline.sh` — bash,
needs external soundfonts + compiled C tools). Run it separately; see
`internal/hd-music-*.md`.

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
