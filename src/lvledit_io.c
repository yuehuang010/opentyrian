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
/** @file lvledit_io.c
 * Level editor archive I/O (Phase E0). See lvledit_io.h.
 *
 * Entry points: lvledit_load_archive(), lvledit_level_count(),
 * lvledit_parse_level(), lvledit_serialize_level(), lvledit_save_archive().
 */

#include "lvledit_io.h"

#include "file.h"
#include "memreader.h"
#include "memwriter.h"

#include <stdio.h>
#include <string.h>

// Largest known tyrian?.lvl (tyrian21 data) is ~800 KB; 2 MB is comfortable
// headroom for other data sets without resorting to malloc.
#define LVLEDIT_MAX_BLOB (2 * 1024 * 1024)

// lvlNum has never been observed above 41 in the wild; this is a generous
// upper bound on the number of raw offset-table entries.
#define LVLEDIT_MAX_OFFS 4096

// Worst-case serialized record size: header (2 + 6 + 2 + 512*2 + 2 +
// 2500*11) + fixed map section (3*128*2 + 300*14 + 600*14 + 15*600) bytes,
// rounded well up.
#define LVLEDIT_MAX_RECORD (96 * 1024)

static Uint8 archive_blob[LVLEDIT_MAX_BLOB];
static size_t archive_size = 0;
static Uint16 archive_lvlnum = 0;
static Sint32 archive_offs[LVLEDIT_MAX_OFFS];

// Scratch buffer for the record being serialized during a save.
static Uint8 serialize_scratch[LVLEDIT_MAX_RECORD];

bool lvledit_load_archive(const char *filename)
{
	archive_size = 0;
	archive_lvlnum = 0;

	FILE *f = dir_fopen_die(data_dir(), filename, "rb");

	fseek(f, 0, SEEK_END);
	long filelen = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (filelen < 2 || (size_t)filelen > sizeof(archive_blob))
	{
		fclose(f);
		return false;
	}

	fread_die(archive_blob, 1, (size_t)filelen, f);
	fclose(f);

	archive_size = (size_t)filelen;

	MemReader r = { archive_blob, archive_size, false };

	Uint16 lvlnum = memReadU16LE(&r);

	if (lvlnum == 0 || lvlnum >= LVLEDIT_MAX_OFFS || (lvlnum % 2) == 0)
	{
		// lvlNum is always odd in every observed archive (paired level
		// entries plus one trailing opaque record); reject anything else
		// rather than guess at a different layout.
		archive_size = 0;
		return false;
	}

	for (Uint16 i = 0; i < lvlnum; ++i)
		archive_offs[i] = (Sint32)memReadU32LE(&r);

	if (r.error)
	{
		archive_size = 0;
		return false;
	}

	// Sanity check: every offset must land inside the file, and the table
	// must be non-decreasing (record lengths are computed by subtraction).
	for (Uint16 i = 0; i < lvlnum; ++i)
	{
		if (archive_offs[i] < 0 || (size_t)archive_offs[i] > archive_size ||
		    (i > 0 && archive_offs[i] < archive_offs[i - 1]))
		{
			archive_size = 0;
			return false;
		}
	}

	archive_lvlnum = lvlnum;

	return true;
}

int lvledit_level_count(void)
{
	if (archive_lvlnum == 0)
		return 0;

	return archive_lvlnum / 2;
}

// Returns the absolute file offset where level record `index`'s data
// begins (the even offset-table entry).
static Sint32 record_start(int index)
{
	return archive_offs[2 * index];
}

// Returns the absolute file offset one past the end of level record
// `index` -- either the next record's start, or (for the last level) the
// trailing unpaired entry, which is exactly lvlNum-1 since lvlNum is odd.
static Sint32 record_end(int index)
{
	return archive_offs[2 * index + 2];
}

bool lvledit_parse_level(int level_index, lvledit_level *lv)
{
	int count = lvledit_level_count();

	if (level_index < 0 || level_index >= count)
		return false;

	Sint32 start = record_start(level_index);
	Sint32 end = record_end(level_index);
	Sint32 map_start = archive_offs[2 * level_index + 1];

	if (start < 0 || end < start || map_start < start || map_start > end || (size_t)end > archive_size)
		return false;

	memset(lv, 0, sizeof(*lv));

	MemReader r = { archive_blob + start, (size_t)(end - start), false };

	lv->mapFile = memReadChar(&r);
	lv->shapeFile = memReadChar(&r);
	lv->mapX = memReadU16LE(&r);
	lv->mapX2 = memReadU16LE(&r);
	lv->mapX3 = memReadU16LE(&r);

	lv->enemy_count = memReadU16LE(&r);
	if (r.error || lv->enemy_count > LVLEDIT_MAX_ENEMY)
		return false;
	memReadU16LEArray(&r, lv->enemy, lv->enemy_count);

	lv->event_count = memReadU16LE(&r);
	if (r.error || lv->event_count > LVLEDIT_MAX_EVENT)
		return false;

	for (Uint16 i = 0; i < lv->event_count; ++i)
	{
		lvledit_event *e = &lv->event[i];

		e->time = memReadU16LE(&r);
		e->type = memReadU8(&r);
		e->dat  = (Sint16)memReadU16LE(&r);
		e->dat2 = (Sint16)memReadU16LE(&r);
		e->dat3 = (Sint8)memReadU8(&r);
		e->dat5 = (Sint8)memReadU8(&r);
		e->dat6 = (Sint8)memReadU8(&r);
		e->dat4 = memReadU8(&r);
	}

	if (r.error)
		return false;

	// We should now be sitting exactly at the map-section offset the
	// archive's odd offset-table entry records for this level.
	size_t consumed = (size_t)(end - start) - r.size;
	if (start + (Sint32)consumed != map_start)
		return false;

	// mapSh[3][128] is stored BIG-endian on disk: JE_loadMap() (tyrian2.c
	// ~3287-3294) does fread_u16_die() -- which is itself a little-endian
	// read -- immediately followed by an explicit SDL_Swap16() on every
	// entry. That extra swap only makes sense if the bytes on disk are
	// actually big-endian (an LE-correct table would never need swapping
	// after an LE read). Undo the wrong-endian LE read the same way here so
	// lv->mapSh holds real shape numbers, not byte-swapped garbage.
	for (int t = 0; t < 3; ++t)
	{
		memReadU16LEArray(&r, lv->mapSh[t], 128);
		for (int i = 0; i < 128; ++i)
			lv->mapSh[t][i] = SDL_Swap16(lv->mapSh[t][i]);
	}

	memReadU8Array(&r, &lv->map1[0][0], 300 * 14);
	memReadU8Array(&r, &lv->map2[0][0], 600 * 14);
	memReadU8Array(&r, &lv->map3[0][0], 600 * 15);

	if (r.error || r.size != 0)
		return false;

	return true;
}

size_t lvledit_serialize_level(const lvledit_level *lv, Uint8 *buf, size_t bufsize, size_t *out_map_offset)
{
	MemWriter w = { buf, bufsize, false };

	memWriteChar(&w, lv->mapFile);
	memWriteChar(&w, lv->shapeFile);
	memWriteU16LE(&w, lv->mapX);
	memWriteU16LE(&w, lv->mapX2);
	memWriteU16LE(&w, lv->mapX3);

	memWriteU16LE(&w, lv->enemy_count);
	memWriteU16LEArray(&w, lv->enemy, lv->enemy_count);

	memWriteU16LE(&w, lv->event_count);
	for (Uint16 i = 0; i < lv->event_count; ++i)
	{
		const lvledit_event *e = &lv->event[i];

		memWriteU16LE(&w, e->time);
		memWriteU8(&w, e->type);
		memWriteU16LE(&w, (Uint16)e->dat);
		memWriteU16LE(&w, (Uint16)e->dat2);
		memWriteU8(&w, (Uint8)e->dat3);
		memWriteU8(&w, (Uint8)e->dat5);
		memWriteU8(&w, (Uint8)e->dat6);
		memWriteU8(&w, e->dat4);
	}

	if (w.error)
		return 0;

	if (out_map_offset)
		*out_map_offset = bufsize - w.size;

	// Mirror the parse-side swap: re-swap each entry to big-endian before
	// handing it to the little-endian array writer, so the bytes that land
	// on disk match what JE_loadMap()'s post-read SDL_Swap16() expects (see
	// the comment in lvledit_parse_level() above).
	for (int t = 0; t < 3; ++t)
	{
		Uint16 mapSh_be[128];
		for (int i = 0; i < 128; ++i)
			mapSh_be[i] = SDL_Swap16(lv->mapSh[t][i]);
		memWriteU16LEArray(&w, mapSh_be, 128);
	}

	memWriteU8Array(&w, &lv->map1[0][0], 300 * 14);
	memWriteU8Array(&w, &lv->map2[0][0], 600 * 14);
	memWriteU8Array(&w, &lv->map3[0][0], 600 * 15);

	if (w.error)
		return 0;

	return bufsize - w.size;
}

bool lvledit_save_archive(const char *filename, int edited_index, const lvledit_level *lv)
{
	int count = lvledit_level_count();

	if (archive_lvlnum == 0 || count == 0)
		return false;

	if (edited_index >= count)
		return false;

	// Serialize the edited record (if any) up front so we know its length
	// and map-section offset before laying out the new offset table.
	size_t edited_len = 0;
	size_t edited_map_off = 0;

	if (edited_index >= 0)
	{
		edited_len = lvledit_serialize_level(lv, serialize_scratch, sizeof(serialize_scratch), &edited_map_off);
		if (edited_len == 0)
			return false;
	}

	Sint32 new_offs[LVLEDIT_MAX_OFFS];
	Sint32 record_len[LVLEDIT_MAX_OFFS / 2];

	size_t header_size = 2 + (size_t)archive_lvlnum * 4;
	Sint32 cur = (Sint32)header_size;

	for (int i = 0; i < count; ++i)
	{
		Sint32 len, map_off;

		if (i == edited_index)
		{
			len = (Sint32)edited_len;
			map_off = (Sint32)edited_map_off;
		}
		else
		{
			len = record_end(i) - record_start(i);
			map_off = archive_offs[2 * i + 1] - record_start(i);
		}

		record_len[i] = len;
		new_offs[2 * i] = cur;
		new_offs[2 * i + 1] = cur + map_off;
		cur += len;
	}

	// Trailing unpaired entry: opaque blob copied verbatim, so it simply
	// starts wherever the rebuilt records end.
	new_offs[archive_lvlnum - 1] = cur;

	Sint32 trailing_start = archive_offs[archive_lvlnum - 1];
	if (trailing_start < 0 || (size_t)trailing_start > archive_size)
		return false;
	size_t trailing_len = archive_size - (size_t)trailing_start;

	FILE *f = fopen(filename, "wb");
	if (!f)
	{
		fprintf(stderr, "lvledit: could not open '%s' for writing\n", filename);
		return false;
	}

	fwrite_u16_die(&archive_lvlnum, f);
	for (Uint16 i = 0; i < archive_lvlnum; ++i)
		fwrite_s32_die(&new_offs[i], f);

	for (int i = 0; i < count; ++i)
	{
		if (i == edited_index)
			fwrite_die(serialize_scratch, 1, edited_len, f);
		else
			fwrite_die(archive_blob + record_start(i), 1, (size_t)record_len[i], f);
	}

	fwrite_die(archive_blob + trailing_start, 1, trailing_len, f);

	fclose(f);

	return true;
}

// Scratch used only by the self-test below.
static lvledit_level roundtrip_level;
static Uint8 roundtrip_scratch[LVLEDIT_MAX_RECORD];

bool lvledit_run_roundtrip_test(int episode)
{
	char filename[32];
	snprintf(filename, sizeof(filename), "tyrian%d.lvl", episode);

	if (!lvledit_load_archive(filename))
	{
		printf("edit-roundtrip: FAILED to load %s\n", filename);
		return false;
	}

	int count = lvledit_level_count();
	printf("edit-roundtrip: %s: lvlNum=%u, %d level record(s)\n", filename, archive_lvlnum, count);

	bool all_ok = true;

	for (int i = 0; i < count; ++i)
	{
		bool ok = lvledit_parse_level(i, &roundtrip_level);

		size_t serialized_len = 0;
		if (ok)
			serialized_len = lvledit_serialize_level(&roundtrip_level, roundtrip_scratch, sizeof(roundtrip_scratch), NULL);

		if (ok && serialized_len != 0)
		{
			Sint32 start = record_start(i);
			Sint32 end = record_end(i);
			size_t orig_len = (size_t)(end - start);

			ok = (serialized_len == orig_len) && memcmp(roundtrip_scratch, archive_blob + start, orig_len) == 0;
		}
		else
		{
			ok = false;
		}

		printf("  level %2d: %s\n", i, ok ? "PASS" : "FAIL");

		all_ok = all_ok && ok;
	}

	// Full-archive round-trip: rebuild with no edits and byte-compare
	// against the original file on disk.
	char out_filename[48];
	snprintf(out_filename, sizeof(out_filename), "roundtrip_tyrian%d.lvl", episode);

	bool archive_ok = lvledit_save_archive(out_filename, -1, NULL);

	if (archive_ok)
	{
		FILE *out_f = fopen(out_filename, "rb");
		if (!out_f)
		{
			archive_ok = false;
		}
		else
		{
			fseek(out_f, 0, SEEK_END);
			long out_len = ftell(out_f);
			fseek(out_f, 0, SEEK_SET);

			archive_ok = ((size_t)out_len == archive_size);

			for (long off = 0; off < out_len && archive_ok; )
			{
				Uint8 bufb[4096];
				size_t chunk = (size_t)((out_len - off) < (long)sizeof(bufb) ? (out_len - off) : (long)sizeof(bufb));
				fread_die(bufb, 1, chunk, out_f);
				if (memcmp(bufb, archive_blob + off, chunk) != 0)
					archive_ok = false;
				off += (long)chunk;
			}

			fclose(out_f);
		}
	}

	printf("  archive round-trip (%s): %s\n", out_filename, archive_ok ? "PASS" : "FAIL");

	all_ok = all_ok && archive_ok;

	printf("edit-roundtrip: %s: %s\n", filename, all_ok ? "ALL PASS" : "FAIL");

	return all_ok;
}
