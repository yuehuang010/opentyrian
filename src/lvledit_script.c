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
/** @file lvledit_script.c
 * Level editor episode-script I/O (Phase E5a). See lvledit_script.h.
 *
 * Entry points: lvledit_script_load(), lvledit_script_save(),
 * lvledit_script_run_roundtrip_test(), plus the line classification
 * helpers.
 */

#include "lvledit_script.h"

#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// XOR key + descramble/scramble copied byte-for-byte from helptext.c's
// decrypt_string() (static to that file, so not directly reusable) and
// lvledit.c's read_script_line(), which itself notes it must stay identical
// to helptext.c's copy since it is undoing the same on-disk scramble. The
// encoder below is new for this phase -- the exact algebraic inverse, see
// the derivation in SCRIPT_REFERENCE.md.
static const Uint8 crypt_key[] = { 204, 129, 63, 255, 71, 19, 25, 62, 1, 99 };

// Every real levels<ep>.dat (tyrian21 data) is a few KB; this is a
// generous static upper bound on the fully-encoded byte stream (line-length
// bytes + payloads for LVLEDIT_SCRIPT_MAX_LINES lines of up to 255 bytes
// each), avoiding malloc per the house style (see lvledit_io.c).
#define LVLEDIT_SCRIPT_MAX_BLOB (LVLEDIT_SCRIPT_MAX_LINES * LVLEDIT_SCRIPT_MAX_LINE)

// Scratch used by lvledit_script_save() to build the full on-disk byte
// stream before it's written in one shot, and reused by the round-trip
// self-test below for the same purpose (the two are never in flight at the
// same time -- this is a single-threaded, one-command-at-a-time editor).
static Uint8 script_encode_scratch[LVLEDIT_SCRIPT_MAX_BLOB];

// Scratch used only by the round-trip self-test: the raw, still-encrypted
// bytes of the original file, held so the re-encoded stream (built into
// script_encode_scratch above) can be byte-compared against them.
static Uint8 script_roundtrip_original[LVLEDIT_SCRIPT_MAX_BLOB];

// ---------------------------------------------------------------------
// Codec primitives
// ---------------------------------------------------------------------

// Decodes one line's `len` encrypted payload bytes `enc[0..len-1]` into
// `out` (text + len), mirroring lvledit.c's read_script_line() /
// helptext.c's decrypt_string() exactly: walk i = len-1 downto 0, XOR-ing
// each byte with the key and (for i>0) with the byte one below it, which at
// the point it's read is STILL the original encrypted byte (the loop only
// ever rewrites index i in place, and i-1 hasn't been touched yet on this
// iteration). That is:
//     P[i] = E[i] ^ key[i%10] ^ (i>0 ? E[i-1] : 0)
// `enc` must hold at least `len` bytes; `out->text` is NUL-terminated and
// `out->len` is set to the original `len` (see lvledit_script.h's note on
// why len is stored separately from strlen(text)).
static void script_line_decode(const Uint8 *enc, Uint8 len, script_line *out)
{
	Uint8 buf[255];

	if (len > 0)
	{
		memcpy(buf, enc, len);

		for (int i = (int)len - 1; ; --i)
		{
			buf[i] ^= crypt_key[i % (int)sizeof(crypt_key)];
			if (i == 0)
				break;
			buf[i] ^= buf[i - 1];
		}

		memcpy(out->text, buf, len);
	}

	out->text[len] = '\0';
	out->len = len;
}

// Encodes one plaintext line (out->text/out->len from a loaded or
// hand-built script_line) into `enc` (must have room for line->len bytes):
// the algebraic inverse of script_line_decode() above, walking i = 0 ..
// len-1 so that E[i-1] (already written into `buf` by the previous
// iteration) is available when computing E[i]:
//     E[0]   = P[0] ^ key[0]
//     E[i>0] = P[i] ^ key[i%10] ^ E[i-1]
// Returns line->len (the number of bytes written to `enc`), purely for the
// caller's convenience -- callers already know line->len, this just avoids
// a second field access at call sites that want it inline.
static Uint8 script_line_encode(const script_line *line, Uint8 *enc)
{
	Uint8 buf[255];
	Uint8 len = line->len;

	if (len > 0)
	{
		memcpy(buf, line->text, len);

		for (int i = 0; i < (int)len; ++i)
		{
			buf[i] ^= crypt_key[i % (int)sizeof(crypt_key)];
			if (i > 0)
				buf[i] ^= buf[i - 1];
		}

		memcpy(enc, buf, len);
	}

	return len;
}

// Encodes every line of *doc into buf (1 length byte + encrypted payload
// per line, back to back, exactly the on-disk layout) and returns the
// total number of bytes written, or 0 if bufsize is too small to hold the
// whole stream. Shared by lvledit_script_save() and the round-trip
// self-test so both use the literal same encoding path.
static size_t script_doc_encode(const script_doc *doc, Uint8 *buf, size_t bufsize)
{
	size_t pos = 0;

	for (int i = 0; i < doc->line_count; ++i)
	{
		const script_line *line = &doc->lines[i];
		size_t need = 1 + (size_t)line->len;

		if (pos + need > bufsize)
			return 0;

		buf[pos] = line->len;
		script_line_encode(line, buf + pos + 1);
		pos += need;
	}

	return pos;
}

// ---------------------------------------------------------------------
// Load / save
// ---------------------------------------------------------------------

bool lvledit_script_load(int episode, script_doc *doc)
{
	doc->line_count = 0;

	char filename[32];
	snprintf(filename, sizeof(filename), "levels%d.dat", episode);

	// Non-dying open (unlike lvledit_io.c's archive load, which uses
	// dir_fopen_die): a script load can be asked for episodes/files that
	// don't exist and must report failure, not exit the process.
	FILE *f = dir_fopen(data_dir(), filename, "rb");
	if (f == NULL)
		return false;

	bool ok = true;

	for (;;)
	{
		Uint8 len;
		if (fread(&len, 1, 1, f) != 1)
			break; // clean EOF at a line boundary -- normal end of script

		Uint8 enc[255];
		if (len > 0 && fread(enc, 1, len, f) != (size_t)len)
		{
			ok = false; // truncated mid-payload -- treat as a load failure
			break;
		}

		if (doc->line_count >= LVLEDIT_SCRIPT_MAX_LINES)
		{
			ok = false; // more lines than the model can hold
			break;
		}

		script_line_decode(enc, len, &doc->lines[doc->line_count]);
		doc->line_count++;
	}

	fclose(f);

	if (!ok)
		doc->line_count = 0;

	return ok;
}

bool lvledit_script_save(int episode, const script_doc *doc)
{
	char filename[32];
	snprintf(filename, sizeof(filename), "levels%d.dat", episode);

	// One-time backup of whatever is still on disk right now, before it's
	// overwritten -- same pattern as lvledit.c's save_current_level().
	char bak_name[40];
	snprintf(bak_name, sizeof(bak_name), "%s.bak", filename);

	if (!dir_file_exists(data_dir(), bak_name))
	{
		FILE *src = dir_fopen(data_dir(), filename, "rb");
		FILE *dst = (src != NULL) ? dir_fopen(data_dir(), bak_name, "wb") : NULL;

		if (src != NULL && dst != NULL)
		{
			Uint8 buf[8192];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
				fwrite(buf, 1, n, dst);
		}

		if (dst != NULL) fclose(dst);
		if (src != NULL) fclose(src);
	}

	size_t len = script_doc_encode(doc, script_encode_scratch, sizeof(script_encode_scratch));
	if (len == 0 && doc->line_count > 0)
		return false; // didn't fit the scratch buffer

	FILE *f = dir_fopen(data_dir(), filename, "wb");
	if (f == NULL)
		return false;

	size_t written = fwrite(script_encode_scratch, 1, len, f);
	fclose(f);

	return written == len;
}

// ---------------------------------------------------------------------
// Round-trip self-test (Phase E5a's go/no-go gate)
// ---------------------------------------------------------------------

bool lvledit_script_run_roundtrip_test(int episode)
{
	char filename[32];
	snprintf(filename, sizeof(filename), "levels%d.dat", episode);

	// Read the original raw (still-encrypted) bytes straight off disk, via
	// the dying open -- the self-test assumes the real game data is
	// present, same posture as lvledit_io.c's --edit-roundtrip test.
	FILE *f = dir_fopen_die(data_dir(), filename, "rb");

	fseek(f, 0, SEEK_END);
	long filelen = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (filelen < 0 || (size_t)filelen > sizeof(script_roundtrip_original))
	{
		fclose(f);
		printf("edit-script-roundtrip: %s: FAILED (bad size %ld)\n", filename, filelen);
		return false;
	}

	if (filelen > 0)
		fread_die(script_roundtrip_original, 1, (size_t)filelen, f);
	fclose(f);

	script_doc doc;
	bool loaded = lvledit_script_load(episode, &doc);

	if (!loaded)
	{
		printf("edit-script-roundtrip: %s: FAILED to load\n", filename);
		return false;
	}

	printf("edit-script-roundtrip: %s: %d line(s), %ld byte(s)\n", filename, doc.line_count, filelen);

	size_t reencoded_len = script_doc_encode(&doc, script_encode_scratch, sizeof(script_encode_scratch));

	bool ok = (reencoded_len == (size_t)filelen) &&
	          (reencoded_len == 0 || memcmp(script_encode_scratch, script_roundtrip_original, reencoded_len) == 0);

	if (!ok)
	{
		size_t min_len = MIN(reencoded_len, (size_t)filelen);
		size_t diff_offset = min_len;

		for (size_t i = 0; i < min_len; ++i)
		{
			if (script_encode_scratch[i] != script_roundtrip_original[i])
			{
				diff_offset = i;
				break;
			}
		}

		printf("  re-encoded %zu byte(s) vs original %ld byte(s); first differing offset: %zu\n",
		       reencoded_len, filelen, diff_offset);
	}

	printf("edit-script-roundtrip: %s: %s\n", filename, ok ? "ALL PASS" : "FAIL");

	return ok;
}

// ---------------------------------------------------------------------
// Line classification helpers
// ---------------------------------------------------------------------

bool lvledit_script_is_section_marker(const char *text)
{
	return text[0] == '*';
}

bool lvledit_script_is_command(const char *text)
{
	return text[0] == ']';
}

char lvledit_script_opcode(const char *text)
{
	if (!lvledit_script_is_command(text) || text[1] == '\0')
		return 0;

	return text[1];
}

int lvledit_script_subblock_len(const script_doc *doc, int cmd_index)
{
	if (cmd_index < 0 || cmd_index >= doc->line_count)
		return 0;

	char opcode = lvledit_script_opcode(doc->lines[cmd_index].text);

	switch (opcode)
	{
		case 'I': // Menu: fixed nine item-availability rows.
		{
			int remaining = doc->line_count - cmd_index - 1;
			return MIN(9, remaining);
		}

		case 'W': // Show text
		case 'Q': // End of episode
		{
			// Lines up to and including the next '#' terminator; clamp to
			// the end of the document if no terminator is found (a
			// malformed/truncated script shouldn't make the scanner read
			// out of bounds).
			int n = 0;
			for (int i = cmd_index + 1; i < doc->line_count; ++i)
			{
				++n;
				if (doc->lines[i].text[0] == '#')
					break;
			}
			return n;
		}

		case 'h': // Skip-if-hard: the single next line.
		{
			int remaining = doc->line_count - cmd_index - 1;
			return MIN(1, remaining);
		}

		default:
			return 0;
	}
}

// ---------------------------------------------------------------------
// Fixed-width field primitives (in-place, offset-preserving)
// ---------------------------------------------------------------------

// 10^width - 1, the largest value a zero-padded width-digit numeric field can
// hold; used to cap a written value so it never spills past its fixed width
// (widths here are tiny -- 1..9 -- so no overflow concern).
static long field_num_cap(int width)
{
	long cap = 1;
	for (int i = 0; i < width; ++i)
		cap *= 10;
	return cap - 1;
}

// Reads the integer the interpreter would see at byte `offset` of `line`,
// mirroring atoi(s + offset): the text is NUL-terminated at line->len (see
// script_line_decode()), so atoi() stops cleanly at the field's trailing
// space/'['/'\0'. Offsets at or past the payload read as 0 (a hand-built or
// truncated line that doesn't reach the field).
static long field_read_num(const script_line *line, int offset)
{
	if (offset < 0 || offset >= (int)line->len)
		return 0;

	return atoi(line->text + offset);
}

// Ensures line->text is at least `end` bytes long, right-padding with spaces
// (and re-terminating) if it was shorter, so a field write can land at a fixed
// offset even on a hand-built short line. Clamped to the buffer; returns false
// if `end` can't fit (255 payload max), in which case the line is untouched.
static bool field_ensure_len(script_line *line, int end)
{
	if (end > 255)
		return false;

	if (end > (int)line->len)
	{
		for (int i = (int)line->len; i < end; ++i)
			line->text[i] = ' ';
		line->text[end] = '\0';
		line->len = (Uint8)end;
	}

	return true;
}

// Overwrites the fixed-width numeric field at `offset` with `value`, zero-
// padded and right-aligned to `width` digits, preserving every byte outside
// [offset, offset+width). `value` is clamped to [0, 10^width-1] so it can
// never spill its width (schema-level [min,max] clamping happens in
// lvledit_script_field_set() before this is reached). No-op if the field
// can't fit the buffer.
static void field_write_num(script_line *line, int offset, int width, long value)
{
	if (offset < 0 || width <= 0)
		return;
	if (!field_ensure_len(line, offset + width))
		return;

	if (value < 0)
		value = 0;
	long cap = field_num_cap(width);
	if (value > cap)
		value = cap;

	// snprintf into a small scratch (width <= 9 here) then copy the digits in
	// place -- writing "%0*ld" straight into line->text would drop a NUL over
	// the byte just past the field.
	char buf[16];
	snprintf(buf, sizeof(buf), "%0*ld", width, value);
	memcpy(line->text + offset, buf, (size_t)width);
}

// Copies the width-byte text field at `offset` into `out`, trimming trailing
// spaces and NUL-terminating (out_sz must be > width). Bytes past the payload
// read as spaces (trimmed away).
static void field_read_text(const script_line *line, int offset, int width, char *out, size_t out_sz)
{
	int n = 0;
	for (int i = 0; i < width && (size_t)n + 1 < out_sz; ++i)
	{
		int p = offset + i;
		out[n++] = (p >= 0 && p < (int)line->len) ? line->text[p] : ' ';
	}
	while (n > 0 && out[n - 1] == ' ')
		--n;
	out[n] = '\0';
}

// Overwrites the fixed-width text field at `offset` with `text`, left-aligned
// and space-padded to `width`, truncating `text` past `width`; preserves every
// byte outside the field. No-op if the field can't fit the buffer.
static void field_write_text(script_line *line, int offset, int width, const char *text)
{
	if (offset < 0 || width <= 0)
		return;
	if (!field_ensure_len(line, offset + width))
		return;

	size_t tlen = strlen(text);
	for (int i = 0; i < width; ++i)
		line->text[offset + i] = ((size_t)i < tlen) ? text[i] : ' ';
}

// ---------------------------------------------------------------------
// Per-opcode field schema
// ---------------------------------------------------------------------

// Convenience builders for lvledit_script_line_fields(); `s` is the script_
// field being filled. Kept as a tiny local helper rather than designated-
// initializer literals so the variable-length ]G loop can reuse it.
static script_field mk_num(int offset, int width, const char *label, long min, long max, bool is_section)
{
	script_field f;
	f.offset = offset;
	f.width = width;
	snprintf(f.label, sizeof(f.label), "%s", label);
	f.min = min;
	f.max = max;
	f.is_text = false;
	f.is_section = is_section;
	return f;
}

static script_field mk_text(int offset, int width, const char *label)
{
	script_field f;
	f.offset = offset;
	f.width = width;
	snprintf(f.label, sizeof(f.label), "%s", label);
	f.min = 0;
	f.max = 0;
	f.is_text = true;
	f.is_section = false;
	return f;
}

int lvledit_script_line_fields(const script_line *line, script_field out[LVLEDIT_SCRIPT_MAX_FIELDS])
{
	char op = lvledit_script_opcode(line->text);
	int n = 0;

	switch (op)
	{
		case 'L': // Play level: nextLevel@9(3), name@13(9), song@22(2), record@25(2)
			out[n++] = mk_num(9, 3, "Next", 0, 999, false);
			out[n++] = mk_text(13, 9, "Name");
			out[n++] = mk_num(22, 2, "Song", 0, 99, false);
			out[n++] = mk_num(25, 2, "Record#", 1, 99, false);
			break;

		case 'G': // Branch: origin@4(2), count@7(1), per choice planet@(9+8i)(2)+section@(12+8i)(3)
		{
			out[n++] = mk_num(4, 2, "Origin", 0, 99, false);
			out[n++] = mk_num(7, 1, "Choices", 0, LVLEDIT_SCRIPT_MAX_G_CHOICES, false);

			long count = field_read_num(line, 7);
			if (count < 0)
				count = 0;
			if (count > LVLEDIT_SCRIPT_MAX_G_CHOICES)
				count = LVLEDIT_SCRIPT_MAX_G_CHOICES;

			for (int i = 0; i < (int)count && n + 2 <= LVLEDIT_SCRIPT_MAX_FIELDS; ++i)
			{
				char lbl[LVLEDIT_SCRIPT_FIELD_LABEL];
				snprintf(lbl, sizeof(lbl), "Planet%d", i + 1);
				out[n++] = mk_num(9 + 8 * i, 2, lbl, 0, 99, false);
				snprintf(lbl, sizeof(lbl), "-> sec%d", i + 1);
				out[n++] = mk_num(12 + 8 * i, 3, lbl, 0, 999, true);
			}
			break;
		}

		case 'H': // Jump if diff < hard: section@4(3)
			out[n++] = mk_num(4, 3, "-> sec", 0, 999, true);
			break;

		case 'J': // Jump (unconditional / conditional): section@3(3)
		case '2':
		case 'w':
		case 't':
		case 'l':
			out[n++] = mk_num(3, 3, "-> sec", 0, 999, true);
			break;

		case 'M': // Play music: song@3(3)
		case 'i': // Set menu music: song@3(3)
			out[n++] = mk_num(3, 3, "Song", 0, 999, false);
			break;

		case 'P': // Show picture / set palette: arg@3(3) (>900 => palette)
			out[n++] = mk_num(3, 3, "Arg", 0, 999, false);
			break;

		case '?': // Set data cubes: count@4(2) (cube ids are free-text)
			out[n++] = mk_num(4, 2, "Count", 0, 99, false);
			break;

		case '!': // Set cubes acquired: count@4(2)
			out[n++] = mk_num(4, 2, "Count", 0, 99, false);
			break;

		case '+': // Add cubes acquired: delta@4(2)
			out[n++] = mk_num(4, 2, "Delta", 0, 99, false);
			break;

		case 'W': // Show warning text: frames@4(2) (the y/n flasher + text block are free-text)
			out[n++] = mk_num(4, 2, "Frames", 0, 99, false);
			break;

		default:
			break;
	}

	return n;
}

long lvledit_script_field_get(const script_line *line, int field_id)
{
	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int count = lvledit_script_line_fields(line, fields);

	if (field_id < 0 || field_id >= count || fields[field_id].is_text)
		return 0;

	return field_read_num(line, fields[field_id].offset);
}

void lvledit_script_field_set(script_line *line, int field_id, long value)
{
	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int count = lvledit_script_line_fields(line, fields);

	if (field_id < 0 || field_id >= count)
		return;

	const script_field *f = &fields[field_id];
	if (f->is_text)
		return;

	if (value < f->min)
		value = f->min;
	if (value > f->max)
		value = f->max;

	field_write_num(line, f->offset, f->width, value);
}

void lvledit_script_field_get_text(const script_line *line, int field_id, char *out, size_t out_sz)
{
	if (out_sz == 0)
		return;
	out[0] = '\0';

	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int count = lvledit_script_line_fields(line, fields);

	if (field_id < 0 || field_id >= count || !fields[field_id].is_text)
		return;

	field_read_text(line, fields[field_id].offset, fields[field_id].width, out, out_sz);
}

void lvledit_script_field_set_text(script_line *line, int field_id, const char *text)
{
	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int count = lvledit_script_line_fields(line, fields);

	if (field_id < 0 || field_id >= count || !fields[field_id].is_text)
		return;

	field_write_text(line, fields[field_id].offset, fields[field_id].width, text);
}

const char *lvledit_script_opcode_name(char opcode)
{
	switch (opcode)
	{
		case 'L': return "Play";
		case 'G': return "Branch";
		case 'I': return "Menu";
		case 'i': return "MenuSong";
		case 'M': return "Music";
		case '?': return "Cubes";
		case '!': return "CubesSet";
		case '+': return "CubesAdd";
		case 'J': return "Jump";
		case '2': return "JumpArcade";
		case 'w': return "JumpStalker";
		case 't': return "JumpTimer";
		case 'l': return "JumpDied";
		case 'H': return "JumpEasy";
		case 'h': return "SkipIfHard";
		case 's': return "Savepoint";
		case 'b': return "Autosave";
		case 'Q': return "EpisodeEnd";
		case 'W': return "WarnText";
		case 'A': return "Anim";
		case 'P': return "Picture";
		case 'U': return "PanUp";
		case 'V': return "SlideUp";
		case 'R': return "PanRight";
		case 'C': return "ClearPal";
		case 'B': return "FadeBlack";
		case 'F': return "Flash";
		case '@': return "TextBank";
		case 'n': return "SceneEnd";
		case 'g': return "Galaga";
		case 'x': return "BonusGame";
		case 'e': return "Engage";
		case 'S': return "NetSync";
		default:  return "?";
	}
}

// ---------------------------------------------------------------------
// Section-ordinal-aware structural mutations (Phase E5b)
// ---------------------------------------------------------------------

int lvledit_script_section_count(const script_doc *doc)
{
	int n = 0;
	for (int i = 0; i < doc->line_count; ++i)
		if (doc->lines[i].text[0] == '*')
			++n;
	return n;
}

// Line index of the `ordinal`-th (1-based) '*' marker, or -1 if there is no
// such marker (ordinal <= 0 or > section_count).
static int marker_line_index(const script_doc *doc, int ordinal)
{
	if (ordinal <= 0)
		return -1;

	int seen = 0;
	for (int i = 0; i < doc->line_count; ++i)
	{
		if (doc->lines[i].text[0] == '*')
		{
			if (++seen == ordinal)
				return i;
		}
	}
	return -1;
}

// Line index where the body of section `ordinal` begins (the line just after
// the ordinal-th marker). Ordinal 0 -> 0 (content before the first marker);
// an ordinal past the last marker -> line_count. This is the resolution the
// interpreter's seek loop performs, expressed as a line index -- the fixed
// point the retarget self-test pins targets to.
static int section_body_start(const script_doc *doc, int ordinal)
{
	if (ordinal <= 0)
		return 0;

	int m = marker_line_index(doc, ordinal);
	return (m < 0) ? doc->line_count : m + 1;
}

// Fills `out_offsets` with the byte offsets of every width-3 section-ordinal
// target field in `line` (]J/]2/]w/]t/]l @3, ]H @4, ]G each choice @12+8i) and
// returns the count. This is the authoritative enumeration the retargeting
// walks -- every target listed here is rewritten on a structural edit, and
// ONLY these. All are three digits wide, so a single width-3 read/write covers
// them uniformly.
static int line_target_offsets(const script_line *line, int out_offsets[1 + LVLEDIT_SCRIPT_MAX_G_CHOICES])
{
	char op = lvledit_script_opcode(line->text);
	int n = 0;

	switch (op)
	{
		case 'J':
		case '2':
		case 'w':
		case 't':
		case 'l':
			out_offsets[n++] = 3;
			break;

		case 'H':
			out_offsets[n++] = 4;
			break;

		case 'G':
		{
			long count = field_read_num(line, 7);
			if (count < 0)
				count = 0;
			if (count > LVLEDIT_SCRIPT_MAX_G_CHOICES)
				count = LVLEDIT_SCRIPT_MAX_G_CHOICES;
			for (int i = 0; i < (int)count; ++i)
				out_offsets[n++] = 12 + 8 * i;
			break;
		}

		default:
			break;
	}

	return n;
}

// Shifts every section target in the document by the remap rule for an insert
// (delta = +1, at `pivot`) or the pre-delete decrement (delta = -1). Targets
// with value >= `pivot` are adjusted by `delta`; smaller targets are left
// alone. Returns nothing -- the caller owns the marker line insert/delete that
// accompanies this.
static void retarget_shift(script_doc *doc, int pivot, int delta)
{
	for (int i = 0; i < doc->line_count; ++i)
	{
		script_line *line = &doc->lines[i];
		int offs[1 + LVLEDIT_SCRIPT_MAX_G_CHOICES];
		int nt = line_target_offsets(line, offs);

		for (int t = 0; t < nt; ++t)
		{
			long v = field_read_num(line, offs[t]);
			if (v >= pivot)
				field_write_num(line, offs[t], 3, v + delta);
		}
	}
}

bool lvledit_script_insert_section(script_doc *doc, int at)
{
	int section_count = lvledit_script_section_count(doc);

	if (at < 1 || at > section_count + 1)
		return false;
	if (doc->line_count >= LVLEDIT_SCRIPT_MAX_LINES)
		return false;

	// Physical insert point: the line of the marker that currently holds
	// ordinal `at` (the new marker takes its slot and pushes it to at+1); or
	// end-of-document for the append case (at == section_count+1).
	int ins = (at <= section_count) ? marker_line_index(doc, at) : doc->line_count;
	if (ins < 0)
		ins = doc->line_count;

	// Retarget FIRST (operates on ordinal values, independent of line indices),
	// then splice in the marker line. Every target >= at moves down one section.
	retarget_shift(doc, at, +1);

	for (int i = doc->line_count; i > ins; --i)
		doc->lines[i] = doc->lines[i - 1];

	script_line *m = &doc->lines[ins];
	m->text[0] = '*';
	m->text[1] = '\0';
	m->len = 1;
	doc->line_count++;

	return true;
}

bool lvledit_script_delete_section(script_doc *doc, int sec, int *out_dangling)
{
	int section_count = lvledit_script_section_count(doc);

	if (sec < 1 || sec > section_count)
		return false;

	int m = marker_line_index(doc, sec);
	if (m < 0)
		return false;

	// Count references that pointed AT `sec` before we touch anything: those
	// become dangling (they keep value == sec, which now resolves to what has
	// become the following section). Reported so the UI can warn.
	int dangling = 0;
	for (int i = 0; i < doc->line_count; ++i)
	{
		script_line *line = &doc->lines[i];
		int offs[1 + LVLEDIT_SCRIPT_MAX_G_CHOICES];
		int nt = line_target_offsets(line, offs);
		for (int t = 0; t < nt; ++t)
			if (field_read_num(line, offs[t]) == sec)
				++dangling;
	}
	if (out_dangling != NULL)
		*out_dangling = dangling;

	// Decrement every target strictly past `sec` (targets == sec are left
	// as-is -- the dangling case above -- and targets < sec are unaffected).
	retarget_shift(doc, sec + 1, -1);

	for (int i = m; i + 1 < doc->line_count; ++i)
		doc->lines[i] = doc->lines[i + 1];
	doc->line_count--;

	return true;
}

bool lvledit_script_insert_line(script_doc *doc, int index, const char *text)
{
	if (doc->line_count >= LVLEDIT_SCRIPT_MAX_LINES)
		return false;
	if (index < 0)
		index = 0;
	if (index > doc->line_count)
		index = doc->line_count;

	for (int i = doc->line_count; i > index; --i)
		doc->lines[i] = doc->lines[i - 1];

	script_line *line = &doc->lines[index];
	size_t tlen = (text != NULL) ? strlen(text) : 0;
	if (tlen > 255)
		tlen = 255;
	if (tlen > 0)
		memcpy(line->text, text, tlen);
	line->text[tlen] = '\0';
	line->len = (Uint8)tlen;

	doc->line_count++;
	return true;
}

bool lvledit_script_delete_line(script_doc *doc, int index)
{
	if (index < 0 || index >= doc->line_count)
		return false;

	for (int i = index; i + 1 < doc->line_count; ++i)
		doc->lines[i] = doc->lines[i + 1];
	doc->line_count--;

	return true;
}

bool lvledit_script_move_line(script_doc *doc, int index, int dir)
{
	if (dir != -1 && dir != 1)
		return false;

	int j = index + dir;
	if (index < 0 || index >= doc->line_count || j < 0 || j >= doc->line_count)
		return false;

	script_line tmp = doc->lines[index];
	doc->lines[index] = doc->lines[j];
	doc->lines[j] = tmp;

	return true;
}

// ---------------------------------------------------------------------
// Section-retarget self-test (Phase E5b's go/no-go gate)
// ---------------------------------------------------------------------

// A snapshotted section target: the line it lives on, its byte offset, the
// ordinal it held, and the line index that ordinal resolved to at snapshot
// time. Sized to the doc's line cap * the max targets per line; a static BSS
// buffer to keep it off the ~1 MB-doc stack frame.
typedef struct
{
	int ordinal;
	int body_line;
} target_snapshot;

static target_snapshot retarget_snap[LVLEDIT_SCRIPT_MAX_LINES * (1 + LVLEDIT_SCRIPT_MAX_G_CHOICES)];

// Walks every section target in `doc` in a fixed order (line order, then
// offset order within a line) and records ordinal + resolved body line into
// retarget_snap, returning the count. The order is stable across an insert/
// delete of a marker line (a marker carries no targets), so the i-th entry
// before and after a mutation is the SAME logical target -- which is what lets
// the test compare them positionally.
static int snapshot_targets(const script_doc *doc)
{
	int n = 0;
	for (int i = 0; i < doc->line_count; ++i)
	{
		const script_line *line = &doc->lines[i];
		int offs[1 + LVLEDIT_SCRIPT_MAX_G_CHOICES];
		int nt = line_target_offsets(line, offs);
		for (int t = 0; t < nt; ++t)
		{
			int ord = (int)field_read_num(line, offs[t]);
			retarget_snap[n].ordinal = ord;
			retarget_snap[n].body_line = section_body_start(doc, ord);
			++n;
		}
	}
	return n;
}

bool lvledit_script_run_retarget_test(int episode)
{
	char filename[32];
	snprintf(filename, sizeof(filename), "levels%d.dat", episode);

	// Original raw bytes (for the closing codec round-trip check), read via the
	// dying open -- the self-test assumes the real game data is present, same
	// posture as the E5a round-trip test.
	FILE *f = dir_fopen_die(data_dir(), filename, "rb");
	fseek(f, 0, SEEK_END);
	long filelen = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (filelen < 0 || (size_t)filelen > sizeof(script_roundtrip_original))
	{
		fclose(f);
		printf("edit-script-retarget: %s: FAILED (bad size %ld)\n", filename, filelen);
		return false;
	}
	if (filelen > 0)
		fread_die(script_roundtrip_original, 1, (size_t)filelen, f);
	fclose(f);

	static script_doc doc; // ~1 MB -- keep off the stack
	if (!lvledit_script_load(episode, &doc))
	{
		printf("edit-script-retarget: %s: FAILED to load\n", filename);
		return false;
	}

	int section_count = lvledit_script_section_count(&doc);
	int k = section_count / 2;
	if (k < 1)
		k = 1;

	printf("edit-script-retarget: %s: %d line(s), %d section(s), pivot k=%d\n",
	       filename, doc.line_count, section_count, k);

	bool all_ok = true;

	// Step 0: field-API no-op check. For every command line and every field,
	// write back the value it already holds (numeric or text). Each write must
	// be a byte-exact in-place overwrite that preserves all other offsets, so
	// after rewriting everything the document must still encode identically to
	// the original file -- this proves lvledit_script_field_get/set (and the
	// _text pair) never disturb a line's other fields, on real data.
	int fields_touched = 0;
	for (int i = 0; i < doc.line_count; ++i)
	{
		script_field fs[LVLEDIT_SCRIPT_MAX_FIELDS];
		int nf = lvledit_script_line_fields(&doc.lines[i], fs);
		for (int j = 0; j < nf; ++j)
		{
			if (fs[j].is_text)
			{
				char t[LVLEDIT_SCRIPT_MAX_LINE];
				lvledit_script_field_get_text(&doc.lines[i], j, t, sizeof(t));
				lvledit_script_field_set_text(&doc.lines[i], j, t);
			}
			else
			{
				lvledit_script_field_set(&doc.lines[i], j, lvledit_script_field_get(&doc.lines[i], j));
			}
			++fields_touched;
		}
	}
	{
		size_t enc = script_doc_encode(&doc, script_encode_scratch, sizeof(script_encode_scratch));
		bool step0 = (enc == (size_t)filelen) &&
		             (enc == 0 || memcmp(script_encode_scratch, script_roundtrip_original, enc) == 0);
		if (!step0)
		{
			size_t min_len = MIN(enc, (size_t)filelen);
			size_t diff = min_len;
			for (size_t x = 0; x < min_len; ++x)
				if (script_encode_scratch[x] != script_roundtrip_original[x]) { diff = x; break; }
			printf("  step0 field-set no-op broke bytes: first diff offset %zu\n", diff);
		}
		printf("  step0 field-API no-op (%d field%s rewritten): %s\n",
		       fields_touched, fields_touched == 1 ? "" : "s", step0 ? "PASS" : "FAIL");
		all_ok = all_ok && step0;
	}

	// Step 1: snapshot every target's (ordinal -> body line) before the edit.
	static target_snapshot before[LVLEDIT_SCRIPT_MAX_LINES * (1 + LVLEDIT_SCRIPT_MAX_G_CHOICES)];
	int nbefore = snapshot_targets(&doc);
	for (int i = 0; i < nbefore; ++i)
		before[i] = retarget_snap[i];
	printf("  step1 snapshot: %d target(s)  PASS\n", nbefore);

	// Step 2: insert a section at interior ordinal k.
	bool ins_ok = lvledit_script_insert_section(&doc, k);
	if (!ins_ok)
		all_ok = false;

	// Step 3: every target must still resolve to the SAME section body. An
	// insert at ordinal k shifts every ordinal >= k up by one, and shifts every
	// body line at/after the insertion point down by one -- so a preserved
	// target reads ordinal+1 / body_line+1 iff its old ordinal was >= k, else
	// unchanged.
	int nafter = snapshot_targets(&doc);
	bool step3 = ins_ok && (nafter == nbefore);
	for (int i = 0; step3 && i < nbefore; ++i)
	{
		int expect_ord  = before[i].ordinal   + (before[i].ordinal >= k ? 1 : 0);
		int expect_line = before[i].body_line + (before[i].ordinal >= k ? 1 : 0);
		if (retarget_snap[i].ordinal != expect_ord || retarget_snap[i].body_line != expect_line)
		{
			step3 = false;
			printf("  step3 target %d: ord %d->%d (want %d), body %d->%d (want %d)\n",
			       i, before[i].ordinal, retarget_snap[i].ordinal, expect_ord,
			       before[i].body_line, retarget_snap[i].body_line, expect_line);
		}
	}
	printf("  step3 insert-retarget (%d target%s preserved): %s\n",
	       nafter, nafter == 1 ? "" : "s", step3 ? "PASS" : "FAIL");
	all_ok = all_ok && step3;

	// Step 4: delete the section just inserted -- it inverts the insert. No
	// original target pointed at the blank section, so dangling must be 0, and
	// every target must be restored to its snapshot ordinal + body line.
	int dangling = -1;
	bool del_ok = lvledit_script_delete_section(&doc, k, &dangling);
	int nrestore = snapshot_targets(&doc);
	bool step4 = del_ok && (dangling == 0) && (nrestore == nbefore);
	for (int i = 0; step4 && i < nbefore; ++i)
	{
		if (retarget_snap[i].ordinal != before[i].ordinal ||
		    retarget_snap[i].body_line != before[i].body_line)
		{
			step4 = false;
			printf("  step4 target %d: ord %d (want %d), body %d (want %d)\n",
			       i, retarget_snap[i].ordinal, before[i].ordinal,
			       retarget_snap[i].body_line, before[i].body_line);
		}
	}
	printf("  step4 delete-inverts (dangling=%d): %s\n", dangling, step4 ? "PASS" : "FAIL");
	all_ok = all_ok && step4;

	// Step 5: the doc is now structurally identical to the original; re-encode
	// and byte-compare against the file to prove the codec still round-trips.
	size_t reencoded = script_doc_encode(&doc, script_encode_scratch, sizeof(script_encode_scratch));
	bool step5 = (reencoded == (size_t)filelen) &&
	             (reencoded == 0 || memcmp(script_encode_scratch, script_roundtrip_original, reencoded) == 0);
	if (!step5)
	{
		size_t min_len = MIN(reencoded, (size_t)filelen);
		size_t diff = min_len;
		for (size_t i = 0; i < min_len; ++i)
			if (script_encode_scratch[i] != script_roundtrip_original[i]) { diff = i; break; }
		printf("  step5 re-encoded %zu vs original %ld; first diff offset %zu\n", reencoded, filelen, diff);
	}
	printf("  step5 codec round-trip: %s\n", step5 ? "PASS" : "FAIL");
	all_ok = all_ok && step5;

	printf("edit-script-retarget: %s: %s\n", filename, all_ok ? "ALL PASS" : "FAIL");
	return all_ok;
}
