# OpenTyrian Standalone Plan — Zero External Data

> **Vision:** a fresh clone builds and runs the *complete* game — no
> `tyrian21.zip` download, no `--data` flag. Every asset the engine needs
> ships with the project, remastered where remastering makes sense
> (art, music, sound) and bundled byte-exact where it doesn't (level
> scripts, item tables, text).
>
> Companion docs: [`REMASTER_PLAN.md`](REMASTER_PLAN.md) (HD visual
> architecture, Phases 0–6) and [`REMASTER_ASSETS.md`](REMASTER_ASSETS.md)
> (per-asset visual status). This doc owns the *standalone* goal: audio,
> remaining art, data embedding, and packaging.

---

## 1. The two halves of "no external assets"

The tyrian21 distro splits cleanly into two kinds of content, and they need
opposite strategies:

1. **Presentational assets** (images, sprites, fonts, music, sound, the
   ending FMV) — these can be **remastered**: replaced by HD/high-quality
   derivatives that ship with the game. The HD visual pipeline
   (`tools/hd_extract*.py` + the truecolor compositor) already covers most
   of the art; audio is the untouched half.

2. **Pure game data** (episode scripts, level maps, enemy/item/weapon
   tables, help strings, datacube text, credits, demos, palettes) — these
   *define gameplay*. The remaster invariant is "plays identical", so this
   data must be **bundled byte-exact**, not recreated. There is nothing to
   "remaster" in `levels1.dat`; there is only shipping it.

**Distribution note:** Tyrian 2.1 was released as freeware in 2004 and the
data files are mirrored publicly (the README already points at
camanis.net). Bundling that data — and derivatives of it (the HD assets)
— is consistent with how the freeware release is redistributed today, but
this is a project-policy call, not legal advice. The classic
external-`--data` path stays supported regardless (Invariant: additive,
never destructive).

## 2. Inventory & disposition

Complete file-level inventory (from a code sweep of every `dir_fopen*`
call site; formats per `doc/files.txt`):

| File(s) | Contents | Required? | Strategy |
|---|---|:--:|---|
| `tyrian.pic` (13 pics), `palette.dat` | full-screen backdrops + palettes | startup/per-screen | ✅ **HD done** (`hdpic01–13`); still bundle originals for classic fallback |
| `tyrian.shp` / `tyrianc.shp` (12 tables) | fonts, UI sprites, player ships/shots/powerups | startup | ✅ **HD done** (fonts, logo, sheets 8–12); bundle original |
| `newsh?.shp` (34 banks) | enemy sprites, shop icons, explosions, destruct | per-level | ✅ **HD done** (11,856 `hdcomp*` frames); bundle originals |
| `shapes?.dat` (34 banks) | **level tileset graphics** (terrain tiles) | per-level | 🔨 **remaster — the big remaining art gap** (Phase S3) |
| `estsc.shp` | ending/score-screen pics | optional | ⬜ extracted, wire (Phase S3) |
| `tshp2.pcx` | interlude image | optional | ⬜ extracted, wiring deferred (Phase S3) |
| `tyrend.anm` | ending FMV (111 frames) | for ending | ✅ **HD done**; playback still *reads the .anm* for timing → bundle it (Phase S4) |
| `music.mus` | 41 LDS/OPL2 sequenced tracks | startup | 🔨 **remaster** (Phase S1) + bundle original for classic path |
| `tyrian.snd` | ~38 SFX, signed 8-bit PCM @ 11025 Hz | startup | 🔨 **remaster** (Phase S2) + bundle |
| `voices.snd` / `voicesc.snd` | voice samples, same format | startup | 🔨 **remaster** (Phase S2) + bundle |
| `tyrian.hdt` | XOR-crypted strings + ep 1–3 item/weapon/enemy tables | startup | 📦 **bundle byte-exact** (Phase S4) |
| `tyrian?.lvl` (1–4) | level tilemaps + event scripts (+ ep-4 item data) | per-episode | 📦 bundle byte-exact |
| `levels?.dat` (1–4) | episode flow scripts (shops, planets, cubes) | per-episode | 📦 bundle byte-exact |
| `cubetxt?.dat` (1–4) | datacube story text | on demand | 📦 bundle byte-exact |
| `tyrian.cdt` | credits text | end-game | 📦 bundle byte-exact |
| `demo.1–5` | recorded attract-mode input | optional | 📦 bundle byte-exact (attract mode is our headless test rig — keep it) |
| everything else in the distro (`*.exe`, net tools' `*.pcx`, `*.doc`, `dpmi16bi.ovl`, `setup.*`, `user?.shp`, `loudness.awe`, …) | DOS-tool assets | never opened | ❌ **drop** — the engine never reads them |

The engine-side single choke point makes this tractable: **all** game-data
IO goes through `dir_fopen*()` in `src/file.c`. One fallback layer there
covers every consumer.

## 3. Invariants

1. **Plays identical.** Game-data files ship byte-exact; only
   presentational assets are replaced. The A/B toggle philosophy from the
   HD plan carries over: remastered audio is `[audio] hd_music` /
   `hd_sfx`, default on, classic path intact.
2. **`--data` still works.** Pointing at a real tyrian21 dir keeps
   overriding the bundle (modders, purists, Christmas mode testing).
3. **Additive fallback chain, never a crash:** explicit `--data` →
   bundled data → helpful error.
4. **Ship size is a feature.** Raw HD assets are currently ~GBs (111
   ending frames × 4 MB alone). The bundle must be compressed to something
   a repo/release can carry (Phase S5).

---

## 4. Phase breakdown

| Phase | Name | Goal | Effort | Risk |
|------:|------|------|:------:|:----:|
| **S0** | Bundle VFS | `dir_fopen` falls back to a bundled pak; game boots with no data dir | S | Low |
| **S1** | Music remaster | 41 tracks rendered/remastered to streamed OGG behind a toggle | M | Med |
| **S2** | SFX & voice remaster | HD 16-bit samples for all SFX + voices (incl. Christmas) | S–M | Low — 🔨 experiment done, method chosen (see §S2) |
| **S3** | Remaining art | `shapes?.dat` tilesets, `estsc.shp`, `tshp2.pcx`, menu-sprite dormancy | M | Med |
| **S4** | Data bundling | All required game-data files packed into the bundle | S | Low |
| **S5** | Packaging & zero-data boot | Compressed asset formats, repo/release hosting, clean-checkout verification | M | Med |

Effort: S ≈ days · M ≈ weeks, hobby-pace. S0 and S4 are the same tool at
two moments; S1–S3 are independent and parallelizable (separate subagents).

---

### Phase S0 — Bundle VFS (the enabler)

Add the delivery mechanism first, with a trivial payload, so every later
phase just adds files to it.

- **Pak format:** one `tyrian.base` archive (simple index: name → offset →
  size → zstd/deflate-compressed blob). Built by a new
  `tools/mkbundle.py` from a manifest listing the files in §2.
  *Alternative considered:* `xxd`-style embedded C arrays — rejected as
  the default because the HD payload is far too big to live in the
  binary; embedding remains an option later for the small game-data
  subset only.
- **Engine:** in `file.c`, extend the `dir_fopen*` resolution chain: after
  the on-disk search fails, serve the file from the bundle via
  `SDL_RWFromConstMem`/`fmemopen`-style access. Callers are untouched —
  they already all go through this choke point. `dir_file_exists` learns
  the same fallback (Christmas detection, episode scan).
- **Loose-file override wins:** disk beats bundle, so `--data` and drop-in
  modding keep working (Invariant 2).

**Exit criteria:** delete/rename `tyrian21/`, run
`SDL_VIDEODRIVER=dummy ./opentyrian` with a bundle containing the classic
required set — full boot to title, attract demo runs.

### Phase S1 — Music remaster

`music.mus` is 41 LDS-sequenced tracks synthesized through the OPL2
emulator (`lds_play.c` + `opl.c`) at runtime. Remaster path:

1. **Faithful render first:** build a tiny offline renderer that links the
   *existing* `lds_play.c`/`opl.c` against a WAV writer and renders each
   track to 44.1 kHz PCM, capturing the natural loop point (LDS songs
   loop; detect the sequencer's loop-around and render intro + loop
   sections separately). This alone is already a "remaster" — clean
   full-rate OPL output with no runtime resampling.
2. **Enhancement pass (optional, per-track curation):** mastering EQ /
   stereo widening, or genuinely re-orchestrated covers. Keep it a
   tooling-only decision, exactly like the Lanczos→AI-upscaler swap in the
   visual pipeline.
3. **Encode** to OGG Vorbis (`hdmusic_NN.ogg`, NN = song number per
   `musmast.c`), with loop metadata.
4. **Engine:** a streamed-music path in `loudness.c` behind
   `[audio] hd_music` (default on when assets present). Decode with
   `stb_vorbis` (single-file, public domain — no new link dependency).
   Must honor the existing API surface: `play_song`, volume/fade
   multipliers, `restart_song`, jukebox. Missing track → LDS fallback
   (which needs `music.mus` from the bundle).

**Risks:** loop-point fidelity (audible seam), and the jukebox UI exposes
all tracks — test with `jukebox.c`. Mixer integration touches the audio
callback; keep the LDS path untouched and A/B by toggle.

### Phase S2 — SFX & voice remaster

Small format, big win: samples are signed 8-bit mono @ 11025 Hz.

**Status: upsampling experiment complete (2026-07-08); method chosen.**
Prototype tooling + full metrics live on worktree branch
`worktree-agent-ae4e628680beb001d` (`tools/hd_extract_snd.py`,
`tools/hd_upsample_snd.py`, `tools/HDSFX_EXPERIMENT.md`,
`tools/hdsfx_previews/`) — pending review/promotion into `hd-remaster`.

1. **Extractor** `tools/hd_extract_snd.py` *(built, verified)*: splits
   `tyrian.snd` + `voices.snd` + `voicesc.snd` into WAVs (names from
   `sndmast.c`), replicating the last-100-bytes voice strip. Actual counts,
   verified against `SFX_COUNT`/`VOICE_COUNT`: **29 SFX + 9 voices + 9
   Christmas voices = 47 samples**.
2. **Enhance — chosen method: DC-offset removal + soxr VHQ polyphase
   resample + TPDF dither, NO extra low-pass.** Findings from the 5-method
   experiment (47 samples × 5 candidates, per-sample metrics):
   - The naive linear control (≈ today's `SDL_ConvertAudio`) leaves ~90×
     more above-Nyquist imaging fizz than soxr (0.175% vs 0.002% of energy
     above the source's 5.5 kHz band) — the status quo is measurably bad.
   - Voices carry large DC offsets (up to 0.36 normalized) → audible
     clicks; DC removal is a genuine repair, not a tonal change (the
     ~−0.3 dB RMS delta is DC energy leaving).
   - An extra Butterworth low-pass was **rejected on evidence**: it rings
     and migrates explosion transient peaks. soxr/linear keep the
     full-scale peak on the exact sample.
   - Offline-tooling deps: soxr + scipy in a venv (engine untouched);
     `scipy.signal.resample_poly` is the dependency-lighter fallback.
   - AI super-resolution remains an optional later curation pass.
3. **Package** as a baked 16-bit/44.1 kHz bank mirroring the `.snd`
   layout (count + offsets + PCM); `loadSndFile()` in `nortsong.c` prefers
   the HD bank per index, falls back per-sample to classic. Same 8-channel
   mixer, same trigger points → cadence identical.

**Exit criteria:** side-by-side listen of every effect; no timing change
(sample *durations* must match the originals exactly — gameplay cues like
the item-screen voices are timed by sample length). The duration
constraint is already proven for the pipeline: all 235 experiment outputs
are exactly 4× the source sample count. Remaining: the human listening
pass and the engine-side loader wiring.

### Phase S3 — Remaining art (closing the visual inventory)

- **`shapes?.dat` level tilesets (the headline item):** 34 banks of level
  terrain tiles drawn by `backgrnd.c` — the only in-game art category with
  no HD pipeline yet. Work: new `tools/hd_extract_tiles.py` (format per
  `doc/files.txt` §shapes), xBRZ upscale like the comp sheets, and wiring
  through the flight display-list compositor (background rows are already
  recorded ops in `interp.c` — see `REMASTER_FLIGHT_COMPOSITOR.md`).
  This is the riskiest piece of S3: tiles seam-match against neighbors, so
  the upscaler must treat tile edges consistently (upscale the assembled
  tilemap rows, or pad-sample neighboring tiles) or the parallax layers
  will show grid seams.
- **`estsc.shp`** ending pictures: extracted already; wire like other
  sprite tables.
- **`tshp2.pcx`**: extracted; wiring was deferred for enter/exit-path leak
  risk — solve or consciously ship classic.
- **Menu-sprite dormancy** (carried from `REMASTER_ASSETS.md`): shop HD
  backdrop + persistent overlay for FACE/WEAPON portraits.
- **Christmas variants** (`tyrianc.shp`, `voicesc.snd`): decide bundle-and-
  remaster vs. classic-only. Cheap to bundle; remastering is a curation
  pass over the same pipelines.

### Phase S4 — Game-data bundling

Add the byte-exact set to the S0 bundle: `tyrian.hdt`, `tyrian1–4.lvl`,
`levels1–4.dat`, `cubetxt1–4.dat`, `tyrian.cdt`, `demo.1–5`,
`palette.dat`, `tyrend.anm` — plus the classic art/audio needed as
fallback floor (`tyrian.pic`, `tyrian.shp`, `newsh?.shp`, `shapes?.dat`,
`music.mus`, `*.snd`). That classic floor is ~6 MB total — cheap insurance
that Invariant 3 (never a crash) holds even if every HD asset is missing.

Note `tyrend.anm` stays required even with HD frames wired: HD playback
overlays the classic decode loop, which still drives frame timing. (An
alternative — making HD anim playback self-timed — is engine work for zero
user-visible gain; just bundle the 2 MB file.)

**Exit criteria:** all of §2's 📦 rows served from the bundle; episode
scan, datacubes, credits, demos, and Christmas detection all work with no
data dir on disk.

### Phase S5 — Packaging, size & zero-data boot

The current HD assets are raw RGBA (`HDPX`) — fine as a local pipeline
output, unshippable as a distribution (ending anim alone ≈ 450 MB raw).

- **Compress the HD formats:** move `HDPX`/frame blobs to PNG or QOI (QOI:
  ~stb-sized decoder, fast, no new dependency) inside the zstd'd pak;
  expect > 10× reduction on flat-shaded pixel-art upscales. The `hdcomp*`
  frame swarm (~24k files planned incl. track B) should become per-sheet
  **atlases** — fewer opens, smaller index, faster level load.
- **Hosting decision:** the bundle likely lands 100–300 MB — too big for
  a plain git repo. Options: Git LFS, a release artifact fetched into the
  build tree by `make` (but that re-introduces a download — acceptable at
  *build* time?), or a two-tier bundle: tiny `tyrian.base` (classic data,
  ~6 MB, committed) + optional `tyrian.hd` pak. **Recommended: two-tier**
  — the game is fully standalone-classic from the repo alone, and
  standalone-HD with the release pak; the VFS chain from S0 handles both
  identically.
- **Boot & docs:** default run needs no flags; `data_dir()` diagnostics
  updated; README/CLAUDE.md updated. CI-style verification: clean
  checkout → `make` → headless boot → attract demo (the demo files are in
  the bundle, so the existing headless test loop keeps working) → episode
  1 load, with `tyrian21/` absent.

---

## 5. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Music loop seams / jukebox regressions | Render intro+loop segments; A/B toggle vs live LDS; test all 41 in `jukebox.c` |
| SFX duration drift changes game feel | ✅ retired for the resample pipeline (all 235 experiment outputs exactly 4× source length); constraint re-applies if AI enhancement is added later |
| Tileset upscale shows grid seams | Upscale with neighbor-tile padding or whole-row assembly; visual A/B per level |
| Bundle too big to host | Two-tier pak (classic ≈ 6 MB committed; HD as release artifact), QOI/atlas + zstd |
| VFS fallback breaks an exotic loader | Single choke point (`file.c`); exhaustive call-site inventory already done (this doc §2) |
| Redistribution policy questions | Freeware-status note in README; `--data` path preserved; drop all DOS-tool files |
| Christmas variants forgotten | Explicit S3/S4 line items; `dir_file_exists` fallback covers detection |

## 6. Open decisions

- **Music enhancement depth** — faithful OPL render only, vs. mastered,
  vs. re-orchestrated covers (settle with a 3-track test batch in S1).
- **QOI vs PNG** for compressed HD assets (decode-speed test on the anim).
- **HD pak hosting** — Git LFS vs release artifact (decide when the real
  post-compression size is known).
- **Embed the ~6 MB classic tier in the binary** (true single-file game)
  vs pak-next-to-binary — revisit after S0 lands.

## 7. Immediate next step

Land **S0** (bundle VFS + `mkbundle.py`) with the classic required set as
payload — it is small, low-risk, and every other phase deposits into it.
Then run **S1–S3 in parallel** as independent subagent tracks.
