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
/** @file lvledit_io.h
 * Level editor archive I/O (Phase E0) for the `tyrian?.lvl` format.
 *
 * Reads/writes single level records out of a `.lvl` archive independently of
 * the normal JE_loadMap()/JE_analyzeLevel() gameplay load path, so nothing
 * here may be reached except behind the `--edit*` flags. See
 * internal/plan/LEVEL_EDITOR_PLAN.md for the on-disk format this implements.
 */
#ifndef LVLEDIT_IO_H
#define LVLEDIT_IO_H

#include "opentyr.h"

#include <stdbool.h>
#include <stddef.h>

// Capacities are generous upper bounds, not exact sizes: the largest real
// tyrian?.lvl record (tyrian21 data) has 33 enemies and 2259 events.
#define LVLEDIT_MAX_ENEMY 512
#define LVLEDIT_MAX_EVENT 2500

typedef struct
{
	Uint16 time;
	Uint8  type;
	Sint16 dat, dat2;
	Sint8  dat3, dat5, dat6;
	Uint8  dat4;
} lvledit_event;

typedef struct
{
	char mapFile, shapeFile;
	Uint16 mapX, mapX2, mapX3;

	Uint16 enemy_count;
	Uint16 enemy[LVLEDIT_MAX_ENEMY];

	Uint16 event_count;
	lvledit_event event[LVLEDIT_MAX_EVENT];

	Uint16 mapSh[3][128];
	Uint8 map1[300][14];
	Uint8 map2[600][14];
	Uint8 map3[600][15];
} lvledit_level;

// Loads an entire .lvl archive (offset table + raw bytes) from data_dir()
// into a static blob buffer. Returns false on any read/size failure.
bool lvledit_load_archive(const char *filename);

// Number of playable level records in the currently loaded archive (0 if
// none loaded). This is lvlNum/2: lvlNum is always odd on disk, entries
// come in (record-start, map-section-start) pairs per level, and the one
// remaining unpaired trailing entry is opaque (episode item data for
// tyrian4.lvl, or a redundant EOF marker for the others) -- never a level.
int lvledit_level_count(void);

// Parses one level record (0-based) out of the loaded archive.
bool lvledit_parse_level(int level_index, lvledit_level *lv);

// Serializes lv into buf (little-endian, matching JE_loadMap()'s field
// order) and returns the number of bytes written, or 0 on failure (e.g.
// buffer too small). If out_map_offset is non-NULL, it receives the byte
// offset from the start of the record to the mapSh table -- the value the
// archive's odd offset-table entry must hold for this record.
size_t lvledit_serialize_level(const lvledit_level *lv, Uint8 *buf, size_t bufsize, size_t *out_map_offset);

// Writes a complete new archive to filename: rebuilt u16 lvlNum + s32
// offset table, then every record back-to-back. All records are blob-copied
// verbatim from the loaded archive except edited_index, which is
// re-serialized from *lv; the trailing unpaired record is always
// blob-copied verbatim. Pass edited_index < 0 for a pure round-trip (no
// record replaced). Requires a prior successful lvledit_load_archive().
bool lvledit_save_archive(const char *filename, int edited_index, const lvledit_level *lv);

// Appends a new level record to the loaded archive IN MEMORY, cloned from
// template_index's record (parsed then re-serialized, so the clone is
// guaranteed self-consistent), growing lvlNum by 2 and rebuilding the offset
// table so the new record sits just before the trailing opaque blob. A
// zero-filled level has no end event and would never terminate in-game --
// that's why "add level" clones an existing record rather than fabricating
// one. Returns the new level's 0-based index (== the pre-call
// lvledit_level_count()) on success, or -1 on failure (no archive loaded,
// template_index out of range, or capacity exceeded -- the in-memory
// archive is left untouched on failure). After a successful call,
// lvledit_level_count() reflects +1 and lvledit_parse_level(new_index, ...)
// succeeds. Purely in-memory; the caller persists with
// lvledit_save_archive().
int lvledit_add_level(int template_index);

// Hidden --edit-roundtrip <episode> self-test: loads tyrian<episode>.lvl,
// parses and re-serializes every level (byte-comparing each against the
// original record), then does a full-archive round-trip save to
// ./roundtrip_tyrian<episode>.lvl and byte-compares that against the
// original file. Prints per-level PASS/FAIL plus a summary. Returns true
// iff everything matched.
bool lvledit_run_roundtrip_test(int episode);

// Hidden --edit-addlevel <episode> self-test (Phase E4): loads
// tyrian<episode>.lvl, clones level 0 via lvledit_add_level(), verifies the
// clone parses and is byte-identical to the template, then saves the grown
// archive to a data_dir()-relative scratch file (removed before returning),
// reloads THAT file fresh, and verifies the count grew by one, every level
// still parses, and the last level still matches the clone. Prints per-step
// PASS/FAIL plus a summary. Returns true iff everything matched.
bool lvledit_run_addlevel_test(int episode);

#endif /* LVLEDIT_IO_H */
