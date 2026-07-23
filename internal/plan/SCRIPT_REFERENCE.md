# Episode Script Reference (`levels<ep>.dat`)

Source of truth for the semantic **script editor** (Level-Editor phase E5), the
way [EVENT_REFERENCE.md](EVENT_REFERENCE.md) is the source of truth for the
per-level event editor. Everything here is derived from the interpreter in
`src/tyrian2.c` (the `if (s[0] == ']') switch (s[1])` block starting at
`tyrian2.c:2710`) and the section-seek loop just above it (`tyrian2.c:2681`).

## What this file is

`levels<ep>.dat` (one per episode: `levels1.dat`..`levels5.dat`) is the
**interlevel driver** — a small line-oriented command language that sequences a
whole episode: shop menus, which record plays as the next level, map-branch
choices, cutscene pictures/animations, story-cube data, difficulty branches, and
the end-of-episode wrap. A `tyrian<ep>.lvl` record is **inert data** until a
`]L` line here binds it into the sequence, so this script — not the archive — is
what makes a level actually playable.

## On-disk encoding

A flat sequence of **encrypted length-prefixed "pascal strings"**: 1 length byte
+ `len` payload bytes, XOR-descrambled. The decrypt is
`read_encrypted_pascal_string()` (`helptext.c`), mirrored non-fatally by the
editor's [`read_script_line()`](../../src/lvledit.c) (`lvledit.c:2876`). The
scramble inverts cleanly, so the **encoder is ~6 lines**:

```
decrypt (on-disk E[] -> plaintext P[]), i = len-1 .. 0:
    P[i] = E[i] ^ key[i%10] ^ (i>0 ? E[i-1] : 0)
encrypt (plaintext P[] -> on-disk E[]), i = 0 .. len-1:
    E[i] = P[i] ^ key[i%10] ^ (i>0 ? E[i-1] : 0)     // uses already-written E[i-1]
key[] = { 204,129,63,255,71,19,25,62,1,99 }
```

Round-trip self-test (mirror `--edit-roundtrip`): decrypt every line of a real
`levels<ep>.dat`, re-encrypt, byte-compare the whole file. Must be identical
before the encoder is trusted.

## Structure model (what the semantic editor edits)

The script is **not flat** — it is a tree with three levels:

1. **Sections.** A line whose first byte is `*` ends the current section and
   starts the next. `mainLevel` (and every jump opcode) selects a section by
   *ordinal*: the seek loop counts `*` markers until it has passed `mainLevel`
   of them, then runs the commands that follow. **Jumps reference sections by
   this 1-based ordinal**, so inserting/deleting/reordering a section
   renumbers every jump target — the editor MUST treat section numbers as
   symbolic and rewrite `]J`/`]2`/`]w`/`]t`/`]l`/`]H`/`]G` targets on any
   structural edit (this is the single biggest correctness hazard in E5).
2. **Commands.** Within a section, each `]X` line is one command (table below).
   Lines that are neither `]`- nor `*`-prefixed at top level are inert (ignored
   by the interpreter) — treat as comments/padding, preserve verbatim.
3. **Owned sub-line blocks.** Three commands consume following lines as payload
   and the editor must model them as children, not siblings:
   - `]I` (menu) consumes **exactly the next 9 lines** (item-availability rows).
   - `]W` (text) and `]Q` (end) consume following lines **until a line whose
     first byte is `#`** (the `#` terminator is part of the block).
   - `]h` consumes/skips **the single next line** (the "hard-difficulty" line).

## Command table

`atoi(s+N)` reads an integer starting at byte offset `N` of the decrypted line;
fixed-width fields are ASCII digits space-padded on disk. Offsets below are
exact (from the interpreter). "sub" = owns follow-on lines per the model above.

| `s[1]` | Name | Params (offset) | Notes |
|---|---|---|---|
| `L` | **Play level** | nextLevel `s+9`; name `s+13` (9 ch); song `s+22`; **lvlFileNum `s+25`** (1-based record #); bonus flags `s[27]`/`s[28]=='$'` | The one opcode that binds an archive record. `nextLevel==0` => `mainLevel+1`. This is what "Add level" writes. |
| `G` | Set map/branch choices | origin `s+4`; count `s+7`; then per choice i: planet `s+1+(i+1)*8`, section `s+4+(i+1)*8` | The branching map screen; section targets are ordinals (renumber-sensitive). |
| `I` | **Shop menu** | *sub:* next 9 lines, each = 8-char skip + space-separated item ids -> `itemAvail[row][]` | Opens `JE_itemScreen()`. |
| `i` | Set menu music | song `s+3` (`songBuy = n-1`) | |
| `M` | Play music | song `s+3` (`n-1`) | |
| `?` | Set data cubes | count `s+4`; cube ids `s+3+(i+1)*4` (4-char cells) | Story cube list. |
| `!` | Set cubes acquired | count `s+4` | |
| `+` | Add cubes acquired | delta `s+4` | |
| `J` | Jump to section | section `s+3` | Unconditional; **ordinal target**. |
| `2` | Jump if arcade | section `s+3` | If `(2P && !campaignCoop) \|\| 1P-arcade`. |
| `w` | Jump if Stalker | section `s+3` | If ship == 13. |
| `t` | Jump if timer expired | section `s+3` | |
| `l` | Jump if player died | section `s+3` | |
| `H` | Jump if diff < hard | section `s+4` | Note the `+4` offset (not `+3`). |
| `h` | Skip line if hard+ | *sub:* skips next 1 line | Difficulty-gated branch. |
| `s` | Set savepoint | — | `saveLevel = mainLevel`. |
| `b` | Auto-save | — | Saves slot 11 (or 22 in 2P). |
| `Q` | **End of episode** | *sub:* `#`-terminated block(s), `secretHint`-selected | Runs `JE_nextEpisode()`; SuperTyrian codes. |
| `W` | Show warning text | `s[2]=='y'` flashers; frames `s+4` (red=/10, %10); *sub:* lines until `#` | `JE_displayText()`. |
| `A` | Show animation | — | Plays `tyrend.anm`. |
| `P` | Show picture / palette | arg `s+3`; `>900` => set palette `pcxpal[arg-900-1]`; `0` => `tshp2.pcx`; else `JE_loadPic(arg)` | |
| `U` | Pan up to picture | pic `s+3` | Transition. |
| `V` | Slide picture up | pic `s+3` | Transition. |
| `R` | Pan right to picture | pic `s+3` | Transition. |
| `C` | Fade/clear/reset pal | — | Resets to `palettes[7]`. |
| `B` | Fade to black | — | |
| `F` | Flash and clear | — | |
| `@` | Toggle text color bank | — | `useLastBank ^= 1`. |
| `n` | End of scene | — | Clears `ESCPressed`. |
| `g` | Enable GALAGA mode | — | Bonus-game setup. |
| `x` | Enable bonus game | — | |
| `e` | Enable ENGAGE mode | — | SuperTyrian bonus loadout. |
| `S` | (unused) | — | Net-sync only; preserve, don't expose. |

## Editor implications (feed into E5 design)

- **Section ordinals are symbolic.** Store jump/`]G` targets as references to a
  section object, not raw ints; serialize to ordinals last. Never let a raw
  int target survive a structural edit.
- **Blocks are children.** `]I`/`]W`/`]Q`/`]h` payload lines are edited within
  the parent, never as free-standing rows, or the 9-line / `#`-terminator
  framing breaks and the interpreter mis-parses everything after.
- **Preserve the unknown.** Inert non-`]` lines and the `]S` no-op must
  round-trip verbatim; the editor is not a rewriter of the whole file, only of
  what the user touches (same posture as the archive editor's blob-copy).
- **`]L` is the pivot for E4.** "Add level" appends a record to the archive
  (E4) and inserts a matching `]L` line here with `lvlFileNum` = new 1-based
  record index.
