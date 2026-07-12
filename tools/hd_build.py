#!/usr/bin/env python3
"""
hd_build.py -- Cross-platform orchestrator for the OpenTyrian HD-remaster
asset pipeline.

Runs the HD extractors -- which read the same base data dir and write
disjoint hd*-prefixed outputs, so they have no interdependencies --
concurrently, then builds the distributable paks (tyrian.base + tyrian.hd)
once every extraction step has finished, replacing what today is ~8 manual
tool invocations with inconsistent CLIs. Works on Windows, macOS, and Linux:
every sub-tool is invoked as `sys.executable <script> ...` (never a shell,
never a hardcoded POSIX path), so it needs nothing beyond a working Python
interpreter (plus whatever third-party deps a given step needs -- checked
up front, see "Preflight" below).

Pipeline (see --list for the live, flag-aware version of this table):

  1. backdrops  hd_extract.py           backdrops + static sprite tables + title logo
  2. comp       hd_extract_comp.py      in-flight comp sprites (11,856 frames)
  3. filter     hd_extract_filter.py    enemy hue-band brightness maps
  4. font       hd_vectorize_font.py    fonts (vector-trace; --font xbrz for the older path)
  5. anim       hd_extract_anim.py      ending cutscene
  6. tiles      hd_extract_tiles.py     level tilesets (opt-in: --tiles; HEAVY, ~1GB)
  7. sfx        hd_extract_snd.py       HD sfx reference WAVs (opt-in: --sfx)
  8. music      stage_music.py          stage committed HD music OGGs (hdmusic/) into the data dir
  9. bundle     mkbundle.py             build tyrian.base + tyrian.hd

Steps 1-5, 8, and 9 run by default; 6 and 7 are opt-in. Steps 1-8 (the
extractors) are mutually independent and run concurrently -- see --jobs --
in the order shown above only for display/summary purposes; step 9 (bundle)
always runs last, alone, once every selected extractor has finished.

Quirks worth knowing before you rely on this:

  - hd_extract.py, hd_extract_comp.py, hd_extract_filter.py, hd_extract_anim.py,
    and hd_extract_font.py (the --font xbrz path) all HARDCODE their data dir
    to <repo>/tyrian21 and take no --data flag. If you pass --data pointing
    elsewhere, this script prints a clear warning -- it does not, and cannot,
    redirect those five.
  - hd_extract_snd.py is labeled an "EXPERIMENT tool" in its own docstring:
    it dumps pristine reference WAVs to tools/hdsfx_previews/orig/, not
    hd*-prefixed assets into the data dir, so it does NOT feed mkbundle's HD
    pak by itself. Producing bundle-ready HD sfx needs the further
    hd_upsample_snd.py + mkhdsnd.py steps, which (like the music pipeline)
    are out of scope for this orchestrator.
  - The music *render/encode* pipeline (hd_music_pipeline.sh) is bash and
    needs external soundfonts plus compiled C tools; it is intentionally not
    wired here. What IS wired is the "music" step, which stages the 41
    already-rendered HD music OGGs committed as source under <repo>/hdmusic/
    (via Git LFS) into the data dir -- see tools/stage_music.py and
    tools/MUSIC_REMASTER.md. A shallow/no-LFS clone leaves those as tiny LFS
    pointer stubs; stage_music.py detects that and errors with a
    `git lfs pull` hint rather than staging broken files.

Preflight (runs automatically before any step, skipped for --list/--help):
  - Checks the third-party deps needed by the steps that will actually run
    (numpy for vector-font/tiles, Pillow for tiles) and prints one
    actionable `pip install ...` line if anything is missing.
  - Checks that palette.dat, tyrian.pic, and tyrian.shp exist in the
    effective data dir, and points at https://camanis.net/tyrian/tyrian21.zip
    if they don't.

Examples:
  # Default run: backdrops, comp, filter, font, anim, and music (staging the
  # committed hdmusic/ OGGs) run concurrently, then bundle once they've all
  # finished.
  python3 tools/hd_build.py

  # Same, on Windows:
  python tools\\hd_build.py

  # Also extract level tilesets and sfx reference WAVs.
  python3 tools/hd_build.py --tiles --sfx

  # Cap concurrency at 2 extraction steps at a time (default: one worker per
  # extraction step selected, up to os.cpu_count()).
  python3 tools/hd_build.py --jobs 2

  # Force fully-serial execution (steps run one at a time, in canonical
  # order) -- useful for debugging a step in isolation.
  python3 tools/hd_build.py --jobs 1

  # Re-pack the paks from already-extracted hd* files, nothing else.
  python3 tools/hd_build.py --bundle-only

  # Point at a data dir elsewhere and run only the font step.
  python3 tools/hd_build.py --data /path/to/tyrian21 --only font

  # Use the older xBRZ font path instead of vector-trace.
  python3 tools/hd_build.py --font xbrz

  # See the resolved step plan (and the exact commands) without running it.
  python3 tools/hd_build.py --list
"""

import argparse
import concurrent.futures
import importlib.util
import os
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_ROOT / "tools"

# The data dir these five tools ALWAYS read from, no matter what --data says
# (see the module docstring). Kept as its own name so the warning text and
# the default for --data can't drift apart.
HARDCODED_DATA_DIR = REPO_ROOT / "tyrian21"
DEFAULT_DATA_DIR = HARDCODED_DATA_DIR

DEFAULT_BUNDLE_OUT = REPO_ROOT / "tyrian.base"
DEFAULT_HD_OUT = REPO_ROOT / "tyrian.hd"

REQUIRED_BASE_FILES = ["palette.dat", "tyrian.pic", "tyrian.shp"]

TYRIAN21_ZIP_URL = "https://camanis.net/tyrian/tyrian21.zip"

# Scripts that hardcode <repo>/tyrian21 and ignore --data entirely.
HARDCODED_SCRIPTS = {
    "backdrops": "hd_extract.py",
    "comp": "hd_extract_comp.py",
    "filter": "hd_extract_filter.py",
    "anim": "hd_extract_anim.py",
}
# hd_extract_font.py joins that list only when --font xbrz is selected.
HARDCODED_FONT_XBRZ_SCRIPT = "hd_extract_font.py"

STEP_ORDER = ["backdrops", "comp", "filter", "font", "anim", "tiles", "sfx", "music", "bundle"]
DEFAULT_STEP_KEYS = {"backdrops", "comp", "filter", "font", "anim", "music", "bundle"}
OPTIN_STEP_KEYS = {"tiles", "sfx"}

STEP_DESCRIPTIONS = {
    "backdrops": "backdrops + static sprite tables + title logo",
    "comp": "in-flight comp sprites (11,856 frames)",
    "filter": "enemy hue-band brightness maps",
    "font": "fonts (vector-trace by default; --font xbrz for the older xBRZ path)",
    "anim": "ending cutscene",
    "tiles": "level tilesets (HEAVY, ~1GB; opt-in via --tiles)",
    "sfx": "HD sfx reference WAVs (opt-in via --sfx; does NOT feed the bundle -- see docstring)",
    "music": "stage committed HD music OGGs (hdmusic/) into the data dir",
    "bundle": "build tyrian.base + tyrian.hd from the extracted hd* assets",
}


# ---------------------------------------------------------------------------
# Command builders -- one per step. Each takes the parsed args + the
# resolved data dir and returns an argv list (argv[0] is always
# sys.executable; no shell, no hardcoded path separators).
# ---------------------------------------------------------------------------

def _cmd_backdrops(args, data_dir):
    argv = [sys.executable, str(TOOLS_DIR / "hd_extract.py")]
    if not args.previews:
        argv.append("--no-preview")
    return argv


def _cmd_comp(args, data_dir):
    # hd_extract_comp.py takes no CLI arguments at all.
    return [sys.executable, str(TOOLS_DIR / "hd_extract_comp.py")]


def _cmd_filter(args, data_dir):
    # hd_extract_filter.py takes no CLI arguments at all.
    return [sys.executable, str(TOOLS_DIR / "hd_extract_filter.py")]


def _cmd_font(args, data_dir):
    if args.font == "xbrz":
        argv = [sys.executable, str(TOOLS_DIR / HARDCODED_FONT_XBRZ_SCRIPT)]
        if not args.previews:
            argv.append("--no-preview")
        return argv
    argv = [sys.executable, str(TOOLS_DIR / "hd_vectorize_font.py"),
            "--data", str(data_dir)]
    return argv


def _cmd_anim(args, data_dir):
    argv = [sys.executable, str(TOOLS_DIR / "hd_extract_anim.py")]
    if not args.previews:
        argv.append("--no-preview")
    return argv


def _cmd_tiles(args, data_dir):
    argv = [sys.executable, str(TOOLS_DIR / "hd_extract_tiles.py"),
            "--data", str(data_dir),
            "--method", "lanczos"]
    if args.previews:
        argv.append("--preview")
    return argv


def _cmd_sfx(args, data_dir):
    # Positional data_dir -- its own default is a hardcoded absolute path,
    # so always pass it explicitly to override that.
    return [sys.executable, str(TOOLS_DIR / "hd_extract_snd.py"), str(data_dir)]


def _cmd_music(args, data_dir):
    return [sys.executable, str(TOOLS_DIR / "stage_music.py"),
            "--data", str(data_dir)]


def _cmd_bundle(args, data_dir):
    argv = [sys.executable, str(TOOLS_DIR / "mkbundle.py"),
            "--src", str(data_dir),
            "--out", str(args.bundle_out),
            "--hd-out", str(args.hd_out)]
    if not args.no_verify:
        # Re-read each pak and confirm every entry round-trips: cheap insurance
        # for an artifact meant to ship as-is.
        argv.append("--verify")
    return argv


STEP_CMD_BUILDERS = {
    "backdrops": _cmd_backdrops,
    "comp": _cmd_comp,
    "filter": _cmd_filter,
    "font": _cmd_font,
    "anim": _cmd_anim,
    "tiles": _cmd_tiles,
    "sfx": _cmd_sfx,
    "music": _cmd_music,
    "bundle": _cmd_bundle,
}


# ---------------------------------------------------------------------------
# Dependency preflight -- stdlib-only check via importlib.util.find_spec, so
# this module itself never imports numpy/PIL at top level (--list/--help
# must work with zero third-party deps installed).
# ---------------------------------------------------------------------------

def _deps_for_step(key, args):
    """Return [(import_name, pip_name), ...] required for this step, given
    the resolved args (so e.g. the font step's deps depend on --font)."""
    if key == "font" and args.font == "vector":
        return [("numpy", "numpy")]
    if key == "tiles":
        # numpy always; Pillow because we always run --method lanczos here
        # (and/or --preview, which also needs it).
        return [("numpy", "numpy"), ("PIL", "Pillow")]
    return []


def missing_pip_packages(step_keys, args):
    seen_imports = set()
    pip_names = []
    for key in step_keys:
        for import_name, pip_name in _deps_for_step(key, args):
            if import_name in seen_imports:
                continue
            seen_imports.add(import_name)
            if importlib.util.find_spec(import_name) is None:
                pip_names.append(pip_name)
    return pip_names


def missing_base_files(data_dir):
    return [name for name in REQUIRED_BASE_FILES if not (data_dir / name).is_file()]


# ---------------------------------------------------------------------------
# Step selection
# ---------------------------------------------------------------------------

def resolve_step_keys(args):
    """Return the ordered list of step keys this run will execute."""
    if args.bundle_only:
        return ["bundle"]

    if args.only:
        requested = [s.strip() for s in args.only.split(",") if s.strip()]
        unknown = [s for s in requested if s not in STEP_ORDER]
        if unknown:
            raise SystemExit(
                "error: --only: unknown step(s) %s (valid: %s)"
                % (", ".join(unknown), ", ".join(STEP_ORDER)))
        requested_set = set(requested)
        return [key for key in STEP_ORDER if key in requested_set]

    keys = set(DEFAULT_STEP_KEYS)
    if args.tiles:
        keys.add("tiles")
    if args.sfx:
        keys.add("sfx")
    if args.no_bundle:
        keys.discard("bundle")
    return [key for key in STEP_ORDER if key in keys]


def step_uses_hardcoded_data_dir(key, args):
    if key in HARDCODED_SCRIPTS:
        return True
    if key == "font" and args.font == "xbrz":
        return True
    return False


# ---------------------------------------------------------------------------
# --list
# ---------------------------------------------------------------------------

def print_step_list(args):
    selected = set(resolve_step_keys(args)) if not _would_raise_only_error(args) else set()
    data_dir = Path(args.data).resolve()

    print("OpenTyrian HD-build step plan (data dir: %s)" % data_dir)
    print()
    n = 0
    for key in STEP_ORDER:
        n += 1
        group = "default" if key in DEFAULT_STEP_KEYS else "opt-in"
        mark = "x" if key in selected else " "
        argv = STEP_CMD_BUILDERS[key](args, data_dir)
        cmd_str = " ".join(argv[1:])  # omit sys.executable for readability
        print("  [%s] %d. %-10s (%-7s) %s" % (mark, n, key, group, STEP_DESCRIPTIONS[key]))
        print("        python %s" % cmd_str)
    print()
    print("x = would run with the current flags; edit --only/--tiles/--sfx/--no-bundle/")
    print("--bundle-only to change the selection. See the module docstring (top of this")
    print("file) for the hardcoded-data-dir and sfx-experiment-tool caveats.")


def _would_raise_only_error(args):
    if not args.only:
        return False
    requested = [s.strip() for s in args.only.split(",") if s.strip()]
    return any(s not in STEP_ORDER for s in requested)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

EPILOG = """\
Examples:
  # Default run: backdrops, comp, filter, font, anim, and music (staging the
  # committed hdmusic/ OGGs) run concurrently, then bundle once they've all
  # finished.
  python3 tools/hd_build.py

  # Same, on Windows:
  python tools\\hd_build.py

  # Also extract level tilesets and sfx reference WAVs.
  python3 tools/hd_build.py --tiles --sfx

  # Cap concurrency at 2 extraction steps at a time (default: one worker per
  # extraction step selected, up to os.cpu_count()).
  python3 tools/hd_build.py --jobs 2

  # Force fully-serial execution -- useful for debugging a step in isolation.
  python3 tools/hd_build.py --jobs 1

  # Re-pack the paks from already-extracted hd* files, nothing else.
  python3 tools/hd_build.py --bundle-only

  # Point at a data dir elsewhere and run only the font step.
  python3 tools/hd_build.py --data /path/to/tyrian21 --only font

  # Use the older xBRZ font path instead of vector-trace.
  python3 tools/hd_build.py --font xbrz

  # See the resolved step plan (and the exact commands) without running it.
  python3 tools/hd_build.py --list

See this file's module docstring for the hardcoded-data-dir tools and the
sfx-experiment-tool caveat.
"""


def _positive_int(value):
    """argparse type= for --jobs: plain int(), but reject 0/negative with a
    clean argparse error instead of a traceback or a silently-broken
    ThreadPoolExecutor(max_workers=0)."""
    try:
        ivalue = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError("invalid int value: %r" % value)
    if ivalue < 1:
        raise argparse.ArgumentTypeError("--jobs must be >= 1 (got %d)" % ivalue)
    return ivalue


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Run the OpenTyrian HD-remaster extraction pipeline and build the "
                     "distributable paks (tyrian.base + tyrian.hd).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=EPILOG,
    )
    parser.add_argument(
        "--data", default=str(DEFAULT_DATA_DIR),
        help="base data dir (default: %s)" % DEFAULT_DATA_DIR)
    parser.add_argument(
        "--previews", action="store_true",
        help="write PNG previews where the underlying tool supports toggling it "
             "(default: off, passes --no-preview to tools that accept it)")
    parser.add_argument(
        "--tiles", action="store_true",
        help="enable the opt-in level-tileset extraction step (HEAVY, ~1GB)")
    parser.add_argument(
        "--sfx", action="store_true",
        help="enable the opt-in sfx reference-WAV extraction step")
    parser.add_argument(
        "--font", choices=["vector", "xbrz"], default="vector",
        help="font extraction path (default: vector)")
    parser.add_argument(
        "--only",
        help="comma-separated list of steps to run instead of the default/opt-in "
             "selection (still runs in canonical order): %s" % ", ".join(STEP_ORDER))
    parser.add_argument(
        "--jobs", "-j", type=_positive_int, default=None, metavar="N",
        help="max extraction steps to run concurrently (backdrops/comp/filter/font/"
             "anim/tiles/sfx/music have no interdependencies; bundle always runs alone, "
             "last, after all selected extraction steps finish). Default: "
             "min(os.cpu_count(), number of extraction steps selected). --jobs 1 "
             "forces fully-serial execution, in canonical order, for debuggability.")
    parser.add_argument(
        "--list", action="store_true",
        help="print the ordered step plan (with the exact commands) and exit")
    parser.add_argument(
        "--continue-on-error", action="store_true",
        help="run bundle even if an extraction step failed (default: skip bundle on "
             "any extraction failure). Note: all selected extraction steps are always "
             "run to completion regardless of this flag, since they run concurrently.")
    parser.add_argument(
        "--no-verify", action="store_true",
        help="skip mkbundle's --verify round-trip check on the built paks (faster)")
    parser.add_argument(
        "--bundle-out", default=str(DEFAULT_BUNDLE_OUT),
        help="output path for the classic-tier pak (default: %s)" % DEFAULT_BUNDLE_OUT)
    parser.add_argument(
        "--hd-out", default=str(DEFAULT_HD_OUT),
        help="output path for the HD-tier pak (default: %s)" % DEFAULT_HD_OUT)

    bundle_group = parser.add_mutually_exclusive_group()
    bundle_group.add_argument(
        "--no-bundle", action="store_true",
        help="run extraction steps only; skip mkbundle")
    bundle_group.add_argument(
        "--bundle-only", action="store_true",
        help="skip all extraction; only (re)build the paks from existing hd* files")

    return parser


def run_step(key, argv, cwd):
    """Run a single step with stdout/stderr inherited (streams live). Used
    for "bundle", which always runs alone (never concurrently with anything
    else), so there's no interleaving risk and no reason to buffer it."""
    print("$ %s" % " ".join(argv))
    start = time.monotonic()
    try:
        result = subprocess.run(argv, cwd=str(cwd), check=False)
        rc = result.returncode
    except OSError as e:
        print("error: failed to launch step %r: %s" % (key, e), file=sys.stderr)
        rc = 1
    duration = time.monotonic() - start
    return rc, duration


def run_step_capture(key, argv, cwd):
    """Run a single step with combined stdout+stderr captured rather than
    inherited. Used for the extraction steps, which may run concurrently in
    a thread pool -- letting them all write to the real stdout/stderr at
    once would interleave their output into garbage, so each one's output is
    captured whole and printed as a single block once it finishes (see
    print_step_block). Returns (key, argv, rc, duration, output)."""
    start = time.monotonic()
    try:
        result = subprocess.run(
            argv, cwd=str(cwd), check=False,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        rc = result.returncode
        output = result.stdout
    except OSError as e:
        rc = 1
        output = "error: failed to launch step %r: %s\n" % (key, e)
    duration = time.monotonic() - start
    return key, argv, rc, duration, output


def print_step_block(key, argv, rc, duration, output):
    """Print one extraction step's captured output as a single, clearly
    delimited block -- safe to call from as_completed() in any order."""
    script_name = Path(argv[1]).name
    status = "ok" if rc == 0 else "FAILED"
    print()
    print("----- [%s] %s -- %s (exit %d, %.1fs) -----" % (key, script_name, status, rc, duration))
    print("$ %s" % " ".join(argv))
    if output:
        sys.stdout.write(output if output.endswith("\n") else output + "\n")
    print("----- [%s] end -----" % key)


def main(argv=None):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.list:
        print_step_list(args)
        return 0

    try:
        step_keys = resolve_step_keys(args)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2

    if args.only and (args.tiles or args.sfx or args.no_bundle):
        print("note: --only was given; ignoring --tiles/--sfx/--no-bundle for step selection",
              file=sys.stderr)
    if args.bundle_only and args.only:
        print("note: --bundle-only was given; ignoring --only", file=sys.stderr)

    if not step_keys:
        print("error: no steps selected (check --only/--tiles/--sfx/--no-bundle)", file=sys.stderr)
        return 1

    data_dir = Path(args.data).resolve()

    # --- preflight: third-party deps -------------------------------------
    missing_pkgs = missing_pip_packages(step_keys, args)
    if missing_pkgs:
        print("error: missing Python packages for the selected steps: %s"
              % ", ".join(missing_pkgs), file=sys.stderr)
        print("  pip install %s" % " ".join(missing_pkgs), file=sys.stderr)
        return 1

    # --- preflight: base data files ---------------------------------------
    missing_files = missing_base_files(data_dir)
    if missing_files:
        print("error: missing base Tyrian 2.1 data file(s) in %s: %s"
              % (data_dir, ", ".join(missing_files)), file=sys.stderr)
        print("  Download and unpack the freeware Tyrian 2.1 data files from:", file=sys.stderr)
        print("    %s" % TYRIAN21_ZIP_URL, file=sys.stderr)
        print("  into %s" % data_dir, file=sys.stderr)
        return 1

    # --- preflight: warn about the hardcoded-data-dir tools ----------------
    hardcoded_steps = [k for k in step_keys if step_uses_hardcoded_data_dir(k, args)]
    if hardcoded_steps and data_dir != HARDCODED_DATA_DIR:
        scripts = sorted({
            HARDCODED_SCRIPTS.get(k, HARDCODED_FONT_XBRZ_SCRIPT) for k in hardcoded_steps
        })
        print("WARNING: %s always read their data from %s, regardless of --data."
              % (", ".join(scripts), HARDCODED_DATA_DIR), file=sys.stderr)
        print("         You passed --data %s -- base Tyrian 2.1 files must still be present "
              "at %s for these steps to work." % (data_dir, HARDCODED_DATA_DIR), file=sys.stderr)
        print(file=sys.stderr)

    # --- run steps -----------------------------------------------------
    # Extraction steps (everything but "bundle") read the same base data dir
    # and write disjoint hd*-prefixed outputs, so they have no
    # interdependencies and can run concurrently. "bundle" reads what the
    # extraction steps wrote, so it's the one ordering constraint: it must
    # run after all selected extraction steps finish, and it never runs
    # concurrently with anything else.
    extraction_keys = [key for key in step_keys if key != "bundle"]
    run_bundle = "bundle" in step_keys

    num_extraction = len(extraction_keys)
    if args.jobs is not None:
        jobs = args.jobs
    elif num_extraction:
        jobs = min(os.cpu_count() or 1, num_extraction)
    else:
        jobs = 1  # nothing to parallelize (e.g. --bundle-only); avoid a 0-worker pool

    results = {}  # key -> (status, duration)

    if num_extraction:
        print()
        print("Running %d extraction step(s) with up to %d concurrent job(s): %s"
              % (num_extraction, jobs, ", ".join(extraction_keys)))

        # Failure semantics: every extraction step is submitted to the pool
        # up front regardless of --continue-on-error -- once steps are
        # launched concurrently there's no useful "stop before starting the
        # next one" point to enforce, so the simplest correct behavior is to
        # let every submitted extraction step run to completion and only
        # gate *bundle* on the outcome below. --jobs 1 still goes through
        # this same pool (with a single worker), so a step failure there
        # does *not* short-circuit remaining extraction steps the way the
        # old sequential loop did -- the old "stop at first failure" applied
        # only to bundle, which is preserved.
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            future_to_key = {}
            for key in extraction_keys:
                argv = STEP_CMD_BUILDERS[key](args, data_dir)
                future_to_key[pool.submit(run_step_capture, key, argv, REPO_ROOT)] = key

            for future in concurrent.futures.as_completed(future_to_key):
                key, argv, rc, duration, output = future.result()
                print_step_block(key, argv, rc, duration, output)
                if rc == 0:
                    results[key] = ("ok", duration)
                else:
                    results[key] = ("failed", duration)
                    print("[%s] FAILED (exit %d, %.1fs)" % (key, rc, duration), file=sys.stderr)

    any_extraction_failed = any(status == "failed" for status, _ in results.values())

    if run_bundle:
        if any_extraction_failed and not args.continue_on_error:
            results["bundle"] = ("skipped", 0.0)
        else:
            argv = STEP_CMD_BUILDERS["bundle"](args, data_dir)
            script_name = Path(argv[1]).name
            print()
            print("[bundle] %s" % script_name)
            rc, duration = run_step("bundle", argv, REPO_ROOT)
            if rc == 0:
                results["bundle"] = ("ok", duration)
                print("[bundle] ok (%.1fs)" % duration)
            else:
                results["bundle"] = ("failed", duration)
                print("[bundle] FAILED (exit %d, %.1fs)" % (rc, duration), file=sys.stderr)

    # --- summary ---------------------------------------------------------
    # Printed in canonical STEP_ORDER regardless of the (possibly
    # out-of-order) completion order extraction steps finished in above.
    print()
    print("Summary:")
    print("  %-12s %-8s %s" % ("step", "status", "duration"))
    any_failed = False
    for key in STEP_ORDER:
        if key not in results:
            continue
        status, duration = results[key]
        if status == "failed":
            any_failed = True
        dur_str = "%.1fs" % duration if status != "skipped" else "-"
        print("  %-12s %-8s %s" % (key, status, dur_str))

    return 1 if any_failed else 0


if __name__ == "__main__":
    sys.exit(main())
