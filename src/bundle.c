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
/** @file bundle.c
 * Reader for the standalone-bundle .pak asset archive format (HD asset overlay).
 *
 * Entry points: bundle_available(), bundle_has(), bundle_fopen().
 */

#include "bundle.h"

#include "file.h"
#include "qoi.h"

#include "SDL.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Standalone asset bundles (Phases S0 + S5 of internal/plan/STANDALONE_PLAN.md).
//
// Two paks, searched in this order (first match wins) after a loose on-disk
// file is not found:
//
//   tyrian.base -- the byte-exact classic/required game-data set. Always
//                  STORE (uncompressed); this is the S0 pak, unchanged.
//   tyrian.hd   -- the `hd*` HD-remaster assets. HDPX images are stored
//                  QOI-compressed (compression byte 1); everything else is
//                  STORE. Optional: if absent, the game still boots and runs
//                  fully on the classic tier alone.
//
// Compression happens entirely at this layer -- src/video.c's HDPX loaders
// are untouched and always see plain, uncompressed HDPX bytes.

#define BUNDLE_MAGIC_LEN 8
static const char BUNDLE_MAGIC[BUNDLE_MAGIC_LEN] = { 'T', 'Y', 'B', 'U', 'N', 'D', 'L', '1' };

enum
{
	BUNDLE_COMPRESS_STORE = 0,
	BUNDLE_COMPRESS_QOI = 1,
};

typedef struct
{
	char *name;              // lowercase bare filename
	Uint8 compression;       // BUNDLE_COMPRESS_*
	Uint32 uncompressed_size; // size of the reconstructed (original) file
	Uint32 compressed_size;   // size of the blob as stored in the pak
	Uint32 offset;            // absolute offset of the blob within the pak
} bundle_entry;

typedef struct
{
	const char *filename;   // pak file name searched for on disk
	bool loaded;             // load attempted (success or failure)
	bool present;             // a valid pak is resident
	Uint8 *data;
	long size;
	bundle_entry *entries;
	Uint32 entry_count;
} bundle_pak;

// Search order for resolving a name: base (classic/required) first, then hd.
// Loose on-disk files already win over both (handled in file.c's dir_fopen
// before bundle_fopen is ever called).
static bundle_pak paks[] =
{
	{ "tyrian.base", false, false, NULL, 0, NULL, 0 },
	{ "tyrian.hd",   false, false, NULL, 0, NULL, 0 },
};
#define BUNDLE_PAK_COUNT (sizeof(paks) / sizeof(paks[0]))

static Uint16 read_le16(const Uint8 *p) { return (Uint16)(p[0] | (p[1] << 8)); }
static Uint32 read_le32(const Uint8 *p)
{
	return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

// Locate and slurp a pak into memory. Uses plain fopen against a fixed
// search list -- must NOT go through dir_fopen/data_dir (which call back into
// the bundle and would recurse).
static FILE *open_pak_file(const char *filename)
{
	const char *dirs[] =
	{
		custom_data_dir,
		TYRIAN_DIR,
		"data",
		".",
	};

	for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); ++i)
	{
		if (dirs[i] == NULL)
			continue;

		char *path = malloc(strlen(dirs[i]) + 1 + strlen(filename) + 1);
		sprintf(path, "%s/%s", dirs[i], filename);
		FILE *f = fopen(path, "rb");
		free(path);
		if (f != NULL)
			return f;
	}

	// Alongside the executable (a bundle shipped next to the binary).
	char *base = SDL_GetBasePath();
	if (base != NULL)
	{
		char *path = malloc(strlen(base) + strlen(filename) + 1);
		sprintf(path, "%s%s", base, filename);
		FILE *f = fopen(path, "rb");
		free(path);
		SDL_free(base);
		if (f != NULL)
			return f;
	}

	return NULL;
}

static void discard(bundle_pak *pak)
{
	if (pak->entries != NULL)
	{
		for (Uint32 i = 0; i < pak->entry_count; ++i)
			free(pak->entries[i].name);
		free(pak->entries);
		pak->entries = NULL;
	}
	free(pak->data);
	pak->data = NULL;
	pak->entry_count = 0;
	pak->present = false;
}

static void parse(bundle_pak *pak)
{
	// Header: magic(8) + entry_count(4)
	if (pak->size < 12 || memcmp(pak->data, BUNDLE_MAGIC, BUNDLE_MAGIC_LEN) != 0)
	{
		fprintf(stderr, "warning: '%s' is not a valid asset bundle\n", pak->filename);
		return;
	}

	const Uint8 *end = pak->data + pak->size;
	const Uint8 *p = pak->data + 8;
	Uint32 count = read_le32(p);
	p += 4;

	bundle_entry *tmp = calloc(count, sizeof(*tmp));
	if (tmp == NULL && count != 0)
		return;

	for (Uint32 i = 0; i < count; ++i)
	{
		if (p + 2 > end)
			goto malformed;
		Uint16 name_len = read_le16(p);
		p += 2;
		if (p + name_len + 1 + 4 + 4 + 4 > end)
			goto malformed;

		tmp[i].name = malloc(name_len + 1);
		memcpy(tmp[i].name, p, name_len);
		tmp[i].name[name_len] = '\0';
		p += name_len;

		tmp[i].compression = *p++;
		tmp[i].uncompressed_size = read_le32(p); p += 4;
		tmp[i].compressed_size = read_le32(p);   p += 4;
		tmp[i].offset = read_le32(p);            p += 4;

		// Validate the blob range now so bundle_fopen can trust it.
		if ((long)tmp[i].offset + (long)tmp[i].compressed_size > pak->size)
			goto malformed;
	}

	pak->entries = tmp;
	pak->entry_count = count;
	pak->present = true;
	return;

malformed:
	fprintf(stderr, "warning: '%s' index is malformed; ignoring bundle\n", pak->filename);
	for (Uint32 i = 0; i < count; ++i)
		free(tmp[i].name);
	free(tmp);
}

static void ensure_pak_loaded(bundle_pak *pak)
{
	if (pak->loaded)
		return;
	pak->loaded = true;

	FILE *f = open_pak_file(pak->filename);
	if (f == NULL)
		return;

	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return;
	}
	pak->size = ftell(f);
	rewind(f);

	if (pak->size <= 0)
	{
		fclose(f);
		return;
	}

	pak->data = malloc(pak->size);
	if (pak->data == NULL)
	{
		fclose(f);
		return;
	}

	if (fread(pak->data, 1, pak->size, f) != (size_t)pak->size)
	{
		fclose(f);
		discard(pak);
		return;
	}
	fclose(f);

	parse(pak);
	if (!pak->present)
		discard(pak);
}

static void ensure_loaded(void)
{
	for (size_t i = 0; i < BUNDLE_PAK_COUNT; ++i)
		ensure_pak_loaded(&paks[i]);
}

static const bundle_entry *find_in(bundle_pak *pak, const char *base)
{
	if (!pak->present)
		return NULL;

	for (Uint32 i = 0; i < pak->entry_count; ++i)
		if (SDL_strcasecmp(pak->entries[i].name, base) == 0)
			return &pak->entries[i];

	return NULL;
}

// Match against the bare filename, case-insensitively. Callers pass a bare
// name already, but strip any path just in case. Searches tyrian.base first,
// then tyrian.hd, so a name present in both resolves to the classic asset.
static const bundle_entry *find(const char *name, bundle_pak **out_pak)
{
	const char *slash = strrchr(name, '/');
	const char *base = slash ? slash + 1 : name;

	for (size_t i = 0; i < BUNDLE_PAK_COUNT; ++i)
	{
		const bundle_entry *e = find_in(&paks[i], base);
		if (e != NULL)
		{
			if (out_pak != NULL)
				*out_pak = &paks[i];
			return e;
		}
	}

	return NULL;
}

bool bundle_available(void)
{
	ensure_loaded();
	for (size_t i = 0; i < BUNDLE_PAK_COUNT; ++i)
		if (paks[i].present)
			return true;
	return false;
}

bool bundle_has(const char *name)
{
	ensure_loaded();
	return find(name, NULL) != NULL;
}

// Reconstruct a QOI-compressed entry's original HDPX byte stream into a
// malloc'd buffer. `blob` is the stored payload: the original 12-byte HDPX
// header (magic + LE width + LE height), followed by the QOI chunk stream.
// Returns a malloc'd buffer of `e->uncompressed_size` bytes, or NULL on
// malformed/corrupt data.
static Uint8 *reconstruct_qoi_hdpx(const bundle_entry *e, const Uint8 *blob)
{
	if (e->compressed_size < 12 || memcmp(blob, "HDPX", 4) != 0)
		return NULL;

	Uint32 width = read_le32(blob + 4);
	Uint32 height = read_le32(blob + 8);

	if ((Uint64)width * height > (Uint64)0xffffffffu / 4)
		return NULL;
	Uint32 pixel_bytes = width * height * 4;

	if ((Uint64)12 + pixel_bytes != e->uncompressed_size)
		return NULL;

	const Uint8 *stream = blob + 12;
	size_t stream_len = e->compressed_size - 12;

	Uint8 *pixels = qoi_decode(stream, stream_len, width, height);
	if (pixels == NULL)
		return NULL;

	Uint8 *out = malloc(e->uncompressed_size);
	if (out == NULL)
	{
		free(pixels);
		return NULL;
	}

	memcpy(out, "HDPX", 4);
	// Header fields are stored little-endian on disk, matching how
	// src/video.c reads them back with SDL_SwapLE32.
	out[4] = (Uint8)(width);       out[5] = (Uint8)(width >> 8);
	out[6] = (Uint8)(width >> 16); out[7] = (Uint8)(width >> 24);
	out[8] = (Uint8)(height);       out[9] = (Uint8)(height >> 8);
	out[10] = (Uint8)(height >> 16); out[11] = (Uint8)(height >> 24);
	memcpy(out + 12, pixels, pixel_bytes);

	free(pixels);
	return out;
}

FILE *bundle_fopen(const char *name)
{
	ensure_loaded();

	bundle_pak *pak = NULL;
	const bundle_entry *e = find(name, &pak);
	if (e == NULL)
		return NULL;

	if (e->compression == BUNDLE_COMPRESS_STORE)
	{
		// Zero-copy: the whole pak is resident, so hand out a read-only memory
		// stream over the blob's slice. fmemopen does not take ownership of the
		// buffer and never frees it, so pak->data safely outlives the FILE*.
		void *blob = pak->data + e->offset;

#if defined(_WIN32)
		// fmemopen is POSIX; on Windows fall back to a temp-file copy.
		FILE *f = tmpfile();
		if (f == NULL)
			return NULL;
		if (fwrite(blob, 1, e->uncompressed_size, f) != e->uncompressed_size)
		{
			fclose(f);
			return NULL;
		}
		rewind(f);
		return f;
#else
		return fmemopen(blob, e->uncompressed_size, "rb");
#endif
	}
	else if (e->compression == BUNDLE_COMPRESS_QOI)
	{
		const Uint8 *blob = pak->data + e->offset;
		Uint8 *reconstructed = reconstruct_qoi_hdpx(e, blob);
		if (reconstructed == NULL)
		{
			fprintf(stderr, "warning: bundled '%s' failed QOI decode\n", name);
			return NULL;
		}

		// Unlike the STORE path (a zero-copy fmemopen slice of the resident pak),
		// this is a fresh allocation that must be freed once the caller closes the
		// FILE. fmemopen never frees its buffer and fclose gives no hook to do so,
		// so copy into a tmpfile and free immediately -- otherwise every bundled
		// HDPX decode would leak its full size for the life of the process (the
		// streamed 111-frame ending anim alone would accumulate hundreds of MB).
		// HD assets load at startup/level-load, not per frame, so the temp-file
		// copy is not a hot path. Same path on every platform.
		FILE *f = tmpfile();
		if (f == NULL)
		{
			free(reconstructed);
			return NULL;
		}
		if (fwrite(reconstructed, 1, e->uncompressed_size, f) != e->uncompressed_size)
		{
			free(reconstructed);
			fclose(f);
			return NULL;
		}
		free(reconstructed);
		rewind(f);
		return f;
	}
	else
	{
		fprintf(stderr, "warning: bundled '%s' uses unsupported compression %u\n",
		        name, e->compression);
		return NULL;
	}
}
