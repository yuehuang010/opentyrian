/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
/** @file lvledit_script.h
 * Level editor episode-script I/O (Phase E5a) for the `levels<ep>.dat`
 * format -- the interlevel driver that sequences a whole episode (shop
 * menus, which .lvl record plays next, branching, cutscenes, cube data,
 * difficulty gates, end-of-episode wrap).
 *
 * This phase is codec + lossless in-memory model + round-trip self-test
 * ONLY -- no interactive UI. See internal/plan/SCRIPT_REFERENCE.md for the
 * full structure model (sections -> commands -> owned sub-line blocks) and
 * the complete opcode table this is derived from, and
 * internal/plan/LEVEL_EDITOR_PLAN.md's E5 section for how this slots into
 * the eventual semantic script editor (E5b/c).
 *
 * The model deliberately stays FLAT (a plain list of lines), not a parsed
 * tree: the flat list is the single source of truth, and it is what
 * guarantees the round-trip is byte-identical -- a tree would have to be
 * lowered back to bytes through some serialization policy, which is exactly
 * the kind of subtle mismatch that could silently corrupt an episode's
 * sequencing. The section/command/sub-block *structure* is exposed only as
 * pure read-only classification helpers over that flat list, for a future
 * editor UI (E5b/c) to consume -- they carry no authority over how the
 * document round-trips.
 *
 * Nothing here may be reached except behind the `--edit*` flags, same
 * convention as lvledit_io.h.
 */
#ifndef LVLEDIT_SCRIPT_H
#define LVLEDIT_SCRIPT_H

#include "opentyr.h"

#include <stdbool.h>
#include <stddef.h>

// Generous upper bounds, not exact sizes -- the largest real levels<ep>.dat
// (tyrian21 data) is levels4.dat at ~11 KB / a few hundred lines.
#define LVLEDIT_SCRIPT_MAX_LINES 4096
#define LVLEDIT_SCRIPT_MAX_LINE  256 // 255 payload bytes + NUL terminator

// One decoded script line. `text` is the plaintext, NUL-terminated. `len` is
// the ORIGINAL on-disk payload length (0..255), kept separately from
// strlen(text) so that re-encoding a line loaded from disk always
// reproduces its original encrypted bytes exactly -- even in the (never
// observed, script lines are ASCII commands/text) case of an embedded NUL
// or other strlen/len mismatch. Lines the editor creates or edits from
// scratch should set len = strlen(text).
typedef struct
{
	char text[LVLEDIT_SCRIPT_MAX_LINE];
	Uint8 len;
} script_line;

// The whole script: a flat, ordered list of lines, exactly as they appear
// in the file. This flat list is the single source of truth -- there is no
// parsed tree sitting "above" it that could disagree with it.
typedef struct
{
	script_line lines[LVLEDIT_SCRIPT_MAX_LINES];
	int line_count;
} script_doc;

// Loads and decodes every line of levels<episode>.dat (1-4) into *doc, in
// file order. Returns false if the file can't be opened, a line is
// truncated (short read mid-payload), or the script has more lines than
// LVLEDIT_SCRIPT_MAX_LINES can hold. Uses a non-fatal decode that stops
// cleanly at a length-byte EOF, the same posture as lvledit.c's
// read_script_line() -- NOT helptext.c's read_encrypted_pascal_string(),
// which dies on a short read (fine for the game loading known-good data,
// wrong for an editor that must report failure instead of exiting).
bool lvledit_script_load(int episode, script_doc *doc);

// Encodes and writes *doc back to levels<episode>.dat under data_dir(),
// first making a one-time levels<episode>.dat.bak backup of whatever is
// still on disk (same pattern as lvledit.c's save_current_level(): only if
// the .bak doesn't already exist, so the very first save is the only one
// that can create it). Returns false on any IO error or if the encoded
// document doesn't fit the internal scratch buffer.
bool lvledit_script_save(int episode, const script_doc *doc);

// Hidden --edit-script-roundtrip <episode> self-test (Phase E5a): reads the
// raw bytes of levels<episode>.dat, loads+decodes it into a script_doc,
// re-encodes that doc into an in-memory buffer (the exact same encoding
// lvledit_script_save() uses, factored out so the test never touches disk),
// and byte-compares the re-encoded stream against the original file bytes.
// Prints the line count, and on failure the first differing offset, plus a
// final "ALL PASS"/"FAIL" summary line. Returns true iff identical -- this
// is the go/no-go gate for the whole script-editing feature (E5b/c).
bool lvledit_script_run_roundtrip_test(int episode);

// ---------------------------------------------------------------------
// Line classification helpers (pure functions over a line's text; for a
// future editor UI to consume -- see SCRIPT_REFERENCE.md's structure model
// and opcode table). These carry no authority over encode/decode/round-trip;
// they are read-only queries over an already-loaded script_doc.
// ---------------------------------------------------------------------

// A section marker ends the current section and starts the next. Jump
// opcodes (]J, ]2, ]w, ]t, ]l, ]H, ]G) and mainLevel select a section by
// 1-based ordinal count of these markers -- see SCRIPT_REFERENCE.md's note
// that section numbers are symbolic and renumber on any structural edit
// (not this phase's concern, but why this helper exists at all).
bool lvledit_script_is_section_marker(const char *text); // text[0] == '*'

// A command line dispatches on text[1] (tyrian2.c's script interpreter:
// `if (s[0] == ']') switch (s[1])`). Lines that are neither ']'- nor
// '*'-prefixed at top level are inert (ignored by the interpreter) and
// should be treated as comments/padding, preserved verbatim.
bool lvledit_script_is_command(const char *text); // text[0] == ']'

// The command's opcode character (text[1]), or 0 if text isn't a command
// line (per lvledit_script_is_command()) or is too short to carry one.
char lvledit_script_opcode(const char *text);

// How many of the lines FOLLOWING cmd_index are consumed as this command's
// owned sub-block payload (SCRIPT_REFERENCE.md's structure model), i.e. how
// far a UI must treat following rows as children rather than siblings:
//   ]I -> 9 (fixed: nine item-availability rows)
//   ]W, ]Q -> count of lines up to and INCLUSIVE of the next line whose
//             text[0] == '#' (the terminator is part of the block; if no
//             such line exists before the document ends, the block is
//             clamped to run to the end of the document)
//   ]h -> 1 (the single "hard-difficulty" line it conditionally skips)
//   anything else -> 0
// `doc` supplies both the line to classify (doc->lines[cmd_index].text) and
// the following lines to scan for a '#' terminator; cmd_index must be a
// valid index into doc->lines (< doc->line_count). Scanning never reads
// past doc->line_count.
int lvledit_script_subblock_len(const script_doc *doc, int cmd_index);

#endif /* LVLEDIT_SCRIPT_H */
