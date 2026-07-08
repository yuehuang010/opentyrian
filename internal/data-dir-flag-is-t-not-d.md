---
description: "OpenTyrian data-dir flag is -t/--data, NOT -d (which is network delay); CLAUDE.md/README are wrong"
---

# Data-dir flag is -t / --data, not -d

To run OpenTyrian, set the Tyrian data directory with `-t` / `--data`, e.g. `./opentyrian --data ./tyrian21`.

**Why:** CLAUDE.md and the README claim `-d`/`--data` sets the data dir. That is wrong. In `src/params.c`, short flag `-d` is bound to *network delay* (expects a number), so `./opentyrian -d ./tyrian21` exits with `error: invalid network delay value`. The real data-dir option is `-t` / `--data` (see the option table around params.c:53).

**How to apply:** Use `-t`/`--data` for the data directory. On this machine the freeware Tyrian 2.1 assets live in `./tyrian21/` (git-ignored). macOS build: `brew install sdl2 sdl2_net` then `make`.
