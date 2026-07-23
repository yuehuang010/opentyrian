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
