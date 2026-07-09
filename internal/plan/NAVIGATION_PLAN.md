# Code Navigation Improvement Plan

Plan to make the codebase easier to navigate. Ordered by return-on-effort
(cheap + high-impact first). Grounded in the current state of the tree.

## Motivating findings

- **356 `extern` globals** across `src/*.h` (112 in `varz.h` alone). This shared
  mutable state — not file layout — is the real navigation tax: understanding a
  gameplay change means tracing which globals it touches and who else touches them.
- **Doxygen exists but is inert.** `Doxyfile` has `EXTRACT_ALL=NO`, `HAVE_DOT=NO`,
  and only ~40 functions carry `/**` comments codebase-wide. It generates almost
  nothing useful today.
- **No machine index.** No `compile_commands.json`, `tags`, or `.clangd`, so
  editor/LSP "go to definition" / "find references" don't work — everything is grep.
- **`internal/` is a good model to extend** — markdown index + pointer files is the
  established house style.
- **A few monolith files:** `mainint.c` (5132 lines, ~40 defs), `tyrian2.c` (5443,
  ~28), `game_menu.c` (3296), `destruct.c` (2812). Finding behavior inside them is
  grep, not browse.

## Phase 1 — Machine-readable index (zero source changes, hours)

Gets real "jump to definition / find references" working in any editor, LSP, or agent.

1. **`compile_commands.json` + clangd.** Add a `make compdb` target (via `bear -- make`
   or `compiledb`) and commit a `.clangd`. Enables cross-file symbol jumps and
   find-all-references — directly attacks the "grep the 356 globals by hand" problem.
   Risk: none (build-adjacent).
2. **`make tags` target.** ctags/etags for grep-free symbol lookup as a fallback.
   Risk: none.
3. **Revive Doxygen as a global cross-reference.** Flip `EXTRACT_ALL=YES` and
   `HAVE_DOT=YES` (needs Graphviz) in `Doxyfile`. Turns the 356 globals + all
   functions into browsable HTML with caller/callee and variable cross-reference
   graphs — i.e. "who reads/writes this global" answered mechanically. Risk: config-only.

## Phase 2 — Human-oriented map (docs only, low risk, delegatable to Sonnet)

4. **`internal/CODEMAP.md`.** One page: each subsystem → its files → entry-point
   functions (e.g. `JE_main` in `mainint.c`) → the key globals it owns. Extends the
   `internal/README.md` index style. Highest-ROI human artifact — the first thing a
   newcomer or agent reads.
5. **File-header banners.** A top-of-file `/** @file ... */` comment on each `.c`
   stating its role + main entry points. Mechanical, ~53 files, splittable across
   Sonnet subagents; also feeds Doxygen's file pages.
6. **Annotate `varz.h`.** Group the 112 externs under subsystem banner comments
   (player state / flight state / shop / palette / …) with a one-line purpose each.
   Documentation-only, but the single biggest discoverability win for game logic.

## Phase 3 — Structural (higher effort/risk, optional, review from Opus)

7. **Section-banner the monoliths first, split later.** Start with non-invasive
   `// ===== SECTION =====` banners + a table-of-contents comment at the top of
   `mainint.c` / `tyrian2.c` (zero behavior risk). Only if that's insufficient,
   extract cohesive groups into new `src/*.c` files (the Makefile auto-globs, so no
   build edits). These are direct Pascal ports; any real split needs Opus-level
   review to preserve behavior.

## Sequencing rationale

Phases 1–2 are almost pure upside — no behavior risk, and they compound (Doxygen +
file banners + CODEMAP reinforce each other). Phase 3 is where cost and risk climb,
so it is gated behind "did 1–2 solve it?".

**Suggested first cut:** Phase 1 (index tooling) + item 4 (CODEMAP) — roughly a day
of work, no source-behavior risk, removes most of the daily friction.
