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

// ---------------------------------------------------------------------
// Structural mutations (Phase E5b): section-ordinal-aware editing.
//
// Sections are delimited by '*' marker lines; jump/branch opcodes select a
// section by its 1-based ORDINAL (the Nth '*'-delimited section), read at a
// fixed byte offset with atoi() by the interpreter. The opcodes that carry a
// section-ordinal target are ]J ]2 ]w ]t ]l (target at s+3), ]H (s+4), and
// ]G (each choice's section at s+4+(i+1)*8, count at s+7). Any structural
// edit that renumbers sections MUST rewrite every one of those targets so it
// still points at the same logical section -- that rewrite is the single
// biggest correctness hazard in the whole feature, and it lives HERE in the
// model layer (headlessly testable via lvledit_script_run_retarget_test())
// rather than in the UI.
// ---------------------------------------------------------------------

// mapPlanet[5]/mapSection[5] (varz.h) cap a ]G branch at five choices.
#define LVLEDIT_SCRIPT_MAX_G_CHOICES 5

// Number of '*' section-marker lines in the document (== the highest 1-based
// section ordinal a jump can target; content before the first marker is the
// implicit "section 0").
int lvledit_script_section_count(const script_doc *doc);

// Insert a new blank section marker ("*") BEFORE the section currently at
// 1-based ordinal `at` (at == section_count+1 appends a trailing empty
// section). Rewrites every jump/]G section target with value >= `at` by +1 so
// each still points at the same logical section body. Returns false if `at`
// is out of range (1..section_count+1) or the document is already full.
bool lvledit_script_insert_section(script_doc *doc, int at);

// Delete the section marker for 1-based ordinal `sec`, merging its body into
// the previous section's flow, and decrement every target with value > sec by
// 1. Targets whose value == sec cannot follow their section (it is gone): they
// are LEFT numerically unchanged -- which now resolves to what has become the
// following section -- and *out_dangling (may be NULL) receives the count of
// such retargeted-onto-neighbor references so the UI can warn instead of
// silently corrupting. Returns false if `sec` is out of range (1..
// section_count).
bool lvledit_script_delete_section(script_doc *doc, int sec, int *out_dangling);

// Generic raw line ops that do NOT touch section ordinals -- for editing
// command lines WITHIN a section. Callers must not use these to move/insert/
// delete a '*' marker line (that would silently renumber sections); marker
// structural edits go exclusively through insert_section/delete_section
// above. Moving a command line across a section boundary is fine (it changes
// no ordinals). insert_line copies at most 255 payload bytes from `text` and
// sets len = strlen(text). All three return false on a bad index / full doc.
bool lvledit_script_insert_line(script_doc *doc, int index, const char *text); // shift down
bool lvledit_script_delete_line(script_doc *doc, int index);
bool lvledit_script_move_line(script_doc *doc, int index, int dir); // dir -1/+1, swap

// ---------------------------------------------------------------------
// Fixed-width field access (Phase E5c): the parameterized opcodes.
//
// The interpreter parses fields with atoi(s+OFFSET) at FIXED byte offsets,
// ASCII digits space/zero-padded on disk (see SCRIPT_REFERENCE.md). Editing a
// numeric field OVERWRITES the fixed-width digit substring in place, zero-
// padded and right-aligned, preserving every other byte offset (the line is
// never reformatted/re-packed -- that would move the other fields). A text
// field (the ]L level name) is overwritten space-padded and left-aligned.
// ---------------------------------------------------------------------

#define LVLEDIT_SCRIPT_MAX_FIELDS  16
#define LVLEDIT_SCRIPT_FIELD_LABEL 12

// One editable field of a command line. `offset`/`width` locate the fixed
// substring; numeric fields clamp to [min,max]; a text field is flagged with
// is_text (min/max unused). is_section marks a jump/]G ordinal target so the
// UI can label it "-> sec N" -- the stored value IS the ordinal, so editing it
// just changes the number (section marker moves are handled separately, by
// insert_section/delete_section, not by editing this field).
typedef struct
{
	int  offset;
	int  width;
	char label[LVLEDIT_SCRIPT_FIELD_LABEL];
	long min, max;
	bool is_text;
	bool is_section;
} script_field;

// Fills `out` (>= LVLEDIT_SCRIPT_MAX_FIELDS entries) with the editable fields
// for `line`'s opcode, in display order, and returns the count (0 for a
// non-command line or a no-parameter opcode). ]G's choice fields are emitted
// dynamically from its own count byte, so the list length varies with the
// line's content -- same idiom as the event editor's build_inspector_fields().
int lvledit_script_line_fields(const script_line *line, script_field out[LVLEDIT_SCRIPT_MAX_FIELDS]);

// Read/write field `field_id` (an index into the list lvledit_script_line_
// fields() returns for this line). _get returns the numeric value (0 for a
// text field or a bad id); _set clamps to the field's [min,max] and overwrites
// in place (see the fixed-width note above). The _text pair does the same for
// a text field (trailing spaces trimmed on read; truncated to width on write).
long lvledit_script_field_get(const script_line *line, int field_id);
void lvledit_script_field_set(script_line *line, int field_id, long value);
void lvledit_script_field_get_text(const script_line *line, int field_id, char *out, size_t out_sz);
void lvledit_script_field_set_text(script_line *line, int field_id, const char *text);

// Short human name for an opcode character (text[1]) -- e.g. 'L' -> "Play",
// 'J' -> "Jump", '\0'/unknown -> "?". For the editor's list + inspector.
const char *lvledit_script_opcode_name(char opcode);

// Hidden --edit-script-retarget-test <episode> self-test (Phase E5b): proves
// the section-ordinal retargeting invariant headlessly. Loads levels<ep>.dat,
// snapshots every jump/]G target resolved to the line index of the section it
// points at, inserts a section at an interior ordinal and asserts every target
// still resolves to the SAME section body (ordinals shifted, logical targets
// preserved), then deletes it and asserts the targets are restored and the
// codec still round-trips byte-identically. Prints per-step PASS/FAIL and a
// summary; returns true iff every step passed.
bool lvledit_script_run_retarget_test(int episode);

#endif /* LVLEDIT_SCRIPT_H */
