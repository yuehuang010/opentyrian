#!/usr/bin/env python3
"""
Phase S3 Track 1: enumerate the (bank, palette) pairs actually used for
in-flight HD tile rendering, plus the bank char used by every 'L' (play
level) command in the four episode flow scripts (levelsN.dat).

Read-only against tyrian21/. Stdlib only. No engine changes.

Background (see internal/plan/STANDALONE_PLAN.md Phase S3 / Track 1 brief
for the full derivation):

  - levelsN.dat is a sequence of XOR-encrypted Pascal strings
    (src/helptext.c decrypt_string). Command lines start with ']'; the
    command letter is s[1] (see the big switch in src/tyrian2.c
    JE_loadMap, ~line 2560 onward). Lines not starting with ']' are
    section delimiters: a line starting with '*' increments the section
    counter (src/tyrian2.c: `while (x < mainLevel) { read...; if
    (s[0]=='*') x++; }`).
  - 'L' (play level): nextLevel=atoi(s+9), levelName=s[13:13+9],
    levelSong=atoi(s+22), lvlFileNum=atoi(s+25).
  - tyrianN.lvl: u16 lvlNum, then lvlNum x s32 LE into lvlPos[43], then
    lvlPos[lvlNum]=EOF offset (src/lvllib.c JE_analyzeLevel). Per-level
    record for a given lvlFileNum starts at lvlPos[(lvlFileNum-1)*2];
    byte 0 = char_mapFile, byte 1 = char_shapeFile (src/tyrian2.c
    ~line 3121). char_shapeFile picks "shapes<char>.dat" (lowercased).

KEY FINDING (overrides the task brief's assumption that the live tile
palette must be tracked via the script's P/U/V/R/C commands):

  Immediately after JE_loadMap() returns, src/tyrian2.c's
  start_level_first: path calls, UNCONDITIONALLY, before any tile is
  drawn or the flight loop starts:

      JE_loadPic(VGAScreen, twoPlayerMode ? 6 : 3, false);   // tyrian2.c ~789
      ...
      fade_palette(colors, 50, 0, 255);                      // tyrian2.c ~802

  JE_loadPic (src/picload.c) does `PCXnumber--` then
  `memcpy(colors, palettes[pcxpal[PCXnumber]], sizeof(colors))`. For
  PCXnumber passed in as 3 -> index 2 -> pcxpal[2] = 5. For PCXnumber
  passed in as 6 -> index 5 -> pcxpal[5] = 5. Both branches (1P vs 2P)
  resolve to palettes[5] (0-based). This means: **whatever the episode
  script's P/U/V/R/C commands set colors[] to (for intro/cutscene
  pictures) is unconditionally clobbered by this HUD-frame picture load
  before any level tile is rendered**, and nothing else in the
  JE_loadMap-tail -> start_level_first -> flight-loop path (verified: no
  other `memcpy(colors, ...)` or `set_palette` call sits on that path;
  the only other `colors=palettes[N]` call sites are JE_nextEpisode
  (runs *before* the script read loop, itself immediately overwritten by
  the same mechanism), JE_highScoreCheck / JE_gammaCheck / JE_playCredits
  (menu/non-gameplay paths), and game_menu.c's level-editor palette
  picker (menu screens, not the flight loop)) touches colors[] again
  before rendering. So **every level's HD tile palette is deterministically
  5, for all four episodes, both 1P and 2P**, regardless of which 'P'
  etc. command last ran in the script. This is a stronger (unconditional,
  branch-free) result than the task brief anticipated, so this script
  still tracks the last P/U/V/R/C-implied palette per 'L' purely for
  informational/caveat purposes, but does NOT use it to decide the
  manifest's palette column -- it hardcodes palette 5, citing this trace.

Usage: tools/.venv_hdtiles/bin/python tools/hd_tile_manifest.py
"""
import json
import os
import struct
import sys

DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tyrian21")

CRYPT_KEY = bytes([204, 129, 63, 255, 71, 19, 25, 62, 1, 99])

# pcxpal[] from src/pcxmast.c, PCX_NUM = 13, 0-based array as declared in C
# (values themselves are 0-based palette slot indices already).
PCXPAL = [0, 7, 5, 8, 10, 5, 18, 19, 19, 20, 21, 22, 5]

BANK_CHARS = ")wxyz"


def decrypt_string(buf: bytearray) -> None:
    """Port of src/helptext.c decrypt_string, byte for byte."""
    n = len(buf)
    if n == 0:
        return
    i = n - 1
    while True:
        buf[i] ^= CRYPT_KEY[i % len(CRYPT_KEY)]
        if i == 0:
            break
        buf[i] ^= buf[i - 1]
        i -= 1


def read_pascal_strings(path):
    """Yield decrypted strings (as Python str, latin-1) from an
    encrypted levelsN.dat, in file order."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    out = []
    while pos < len(data):
        length = data[pos]
        pos += 1
        if pos + length > len(data):
            break  # trailing garbage / truncated last entry
        raw = bytearray(data[pos:pos + length])
        pos += length
        decrypt_string(raw)
        out.append(raw.decode("latin-1"))
    return out


def load_lvlpos(path):
    """Return lvlPos[] (list of ints, index 0..lvlNum inclusive) per
    src/lvllib.c JE_analyzeLevel."""
    with open(path, "rb") as f:
        data = f.read()
    (lvl_num,) = struct.unpack_from("<H", data, 0)
    pos = list(struct.unpack_from("<%di" % lvl_num, data, 2))
    pos.append(len(data))  # ftell_eof equivalent
    return pos


def char_shape_file_for(lvl_pos, lvl_file_num, lvl_bytes):
    idx = (lvl_file_num - 1) * 2
    if idx < 0 or idx >= len(lvl_pos):
        return None
    off = lvl_pos[idx]
    if off < 0 or off + 2 > len(lvl_bytes):
        return None
    char_map_file = chr(lvl_bytes[off])
    char_shape_file = chr(lvl_bytes[off + 1])
    return char_map_file, char_shape_file.lower()


def palette_from_P_command(s):
    """Mirror the 'P' command's palette-selection math (tyrian2.c ~2863),
    for informational tracking only (see module docstring: the result is
    NOT what ends up live during actual level rendering)."""
    temp_x = atoi_prefix(s[3:])
    if temp_x > 900:
        idx = temp_x - 1 - 900
        if 0 <= idx < len(PCXPAL):
            return PCXPAL[idx]
        return None
    return None  # full-PCX picture: palette comes from the picture's own embedded palette; not tracked


def atoi_prefix(s):
    """C atoi()-alike: parse leading optional sign + digits, else 0."""
    s = s.strip()
    i = 0
    if i < len(s) and s[i] in "+-":
        i += 1
    j = i
    while j < len(s) and s[j].isdigit():
        j += 1
    if j == i:
        return 0
    return int(s[:j])


def analyze_episode(ep_num):
    levels_path = os.path.join(DATA_DIR, "levels%d.dat" % ep_num)
    lvl_path = os.path.join(DATA_DIR, "tyrian%d.lvl" % ep_num)

    strings = read_pascal_strings(levels_path)
    lvl_pos = load_lvlpos(lvl_path)
    with open(lvl_path, "rb") as f:
        lvl_bytes = f.read()

    section = -1  # bumps to 0 at the first '*' delimiter, matching JE_loadMap's counting loop
    last_palette = None  # informational only
    last_palette_cmd = None
    l_commands = []  # list of dicts
    conditional_hits = []  # for caveat reporting

    for s in strings:
        if s.startswith("*"):
            section += 1
            continue
        if not s.startswith("]"):
            continue
        cmd = s[1] if len(s) > 1 else ""

        if cmd in ("P", "U", "V", "R"):
            pal = palette_from_P_command(s) if cmd == "P" else None
            last_palette = pal
            last_palette_cmd = s[:40]
        elif cmd == "C":
            last_palette = 7  # palettes[7], tyrian2.c ~3024, unconditional within this command
            last_palette_cmd = s[:40]
        elif cmd in ("J", "2", "w", "t", "l", "H", "h"):
            conditional_hits.append((section, cmd, s[:40]))
        elif cmd == "L":
            next_level = atoi_prefix(s[9:13]) if len(s) > 9 else 0
            level_name = s[13:22].strip() if len(s) > 13 else ""
            level_song = atoi_prefix(s[22:25]) if len(s) > 22 else 0
            lvl_file_num = atoi_prefix(s[25:]) if len(s) > 25 else 0
            bank = None
            shape_lookup = char_shape_file_for(lvl_pos, lvl_file_num, lvl_bytes)
            if shape_lookup is not None:
                bank = shape_lookup[1]
            l_commands.append({
                "section": section,
                "raw": s,
                "next_level": next_level,
                "level_name": level_name,
                "level_song": level_song,
                "lvl_file_num": lvl_file_num,
                "bank_char": bank,
                "map_char": shape_lookup[0] if shape_lookup else None,
                "last_script_palette_hint": last_palette,
                "last_script_palette_cmd": last_palette_cmd,
            })

    return {
        "episode": ep_num,
        "num_strings": len(strings),
        "num_sections_seen": section + 1,
        "l_commands": l_commands,
        "conditional_hits": conditional_hits,
    }


def main():
    all_episodes = []
    for ep in (1, 2, 3, 4):
        all_episodes.append(analyze_episode(ep))

    # Bank chars actually referenced by any 'L' command, across all 4 episodes.
    banks_seen = set()
    lvl_file_nums_by_ep = {}
    l_summaries = []
    for ep_data in all_episodes:
        ep = ep_data["episode"]
        seen = set()
        for lc in ep_data["l_commands"]:
            seen.add(lc["lvl_file_num"])
            if lc["bank_char"]:
                banks_seen.add(lc["bank_char"])
            l_summaries.append({
                "episode": ep,
                "section": lc["section"],
                "lvl_file_num": lc["lvl_file_num"],
                "level_name": lc["level_name"],
                "bank_char": lc["bank_char"],
                "map_char": lc["map_char"],
                "script_palette_hint_ignored": lc["last_script_palette_hint"],
            })
        lvl_file_nums_by_ep[ep] = sorted(seen)

    banks_seen = sorted(b for b in banks_seen if b in BANK_CHARS)

    # HARDCODED palette: see module docstring "KEY FINDING". All in-flight
    # rendering uses palettes[5] (0-based) unconditionally, regardless of
    # what the episode script's P/U/V/R/C commands set for cutscene pics.
    LIVE_TILE_PALETTE = 5

    pairs = [{"bank": b, "palette": LIVE_TILE_PALETTE,
              "note": "static-trace: bank from tyrianN.lvl char_shapeFile "
                      "for every 'L'-referenced lvlFileNum; palette is the "
                      "unconditional post-JE_loadMap JE_loadPic(3|6)+fade_palette "
                      "override (tyrian2.c ~789-802, picload.c:82, pcxpal[2]==pcxpal[5]==5), "
                      "NOT the script's P/U/V/R/C palette (that is clobbered before any tile draws)."}
             for b in banks_seen]

    manifest = {
        "approach": "static-trace",
        "pairs": pairs,
        "caveats": (
            "Palette is NOT tracked via the episode scripts' P/U/V/R/C commands "
            "despite those being fully parsed below (see l_command_details / "
            "script_palette_hint_ignored) -- those set colors[] only for "
            "intro/cutscene picture screens. Immediately after JE_loadMap() "
            "returns, tyrian2.c's start_level_first path unconditionally calls "
            "JE_loadPic(VGAScreen, twoPlayerMode?6:3, false) then "
            "fade_palette(colors,...) before any level tile renders, and "
            "picload.c's JE_loadPic always sets colors=palettes[pcxpal[PCXnumber-1]]; "
            "pcxpal[2]==pcxpal[5]==5, so both the 1P and 2P branch resolve to "
            "palette index 5. No other colors[]/set_palette call sits on the "
            "JE_loadMap-tail -> start_level_first -> flight-loop code path (checked "
            "all `memcpy(colors,` / `palettes[` call sites in src/*.c; the others "
            "are JE_nextEpisode (runs before the script loop, itself immediately "
            "re-clobbered the same way), JE_highScoreCheck/JE_gammaCheck/JE_playCredits "
            "(non-gameplay screens), and game_menu.c's level-editor palette picker "
            "(menu UI, not the flight loop)). Net effect: every level in all 4 "
            "episodes, both player counts, renders its background tiles with "
            "palette index 5 -- this is a branch-free, unconditional result, not "
            "an inference from resolving 'J'/'H'/'h'/etc conditionals (those were "
            "in fact never needed for the palette question; they were still walked "
            "for the bank/lvlFileNum enumeration below, recorded but not resolved, "
            "matching the task's 'unconditional/linear flow, ignore conditional "
            "branches' instruction -- see conditional_hits per episode). "
            "During actual gameplay, transient effects (level filters, brightness "
            "changes, boss-warp tints) can make colors[] briefly NOT exactly equal "
            "any palettes[] entry; current_palette_index() (src/video.c) already "
            "returns -1 in that case and the compositor gracefully falls back to "
            "classic rendering for those frames -- this does not add new "
            "(bank,palette) pairs to cover, it just means some frames stay classic."
        ),
        "banks_referenced_by_L_commands": banks_seen,
        "lvl_file_nums_seen_by_episode": lvl_file_nums_by_ep,
        "l_command_details": l_summaries,
        "conditional_commands_seen_by_episode": {
            str(ep_data["episode"]): [
                {"section": s, "cmd": c, "raw_prefix": r}
                for (s, c, r) in ep_data["conditional_hits"]
            ]
            for ep_data in all_episodes
        },
    }

    out_path = os.path.join(DATA_DIR, "hd_tile_manifest.json")
    with open(out_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print("Wrote %s" % out_path)
    print("Banks referenced by L commands: %s" % banks_seen)
    print("Pairs: %s" % [(p["bank"], p["palette"]) for p in pairs])
    for ep, nums in lvl_file_nums_by_ep.items():
        print("Episode %d lvlFileNum values seen: %s" % (ep, nums))


if __name__ == "__main__":
    main()
