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
#include "bundle.h"

#include "file.h"

#include "SDL.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BUNDLE_FILE "tyrian.base"
static const char BUNDLE_MAGIC[8] = { 'T', 'Y', 'B', 'U', 'N', 'D', 'L', '1' };

typedef struct
{
	char *name;              // lowercase bare filename
	Uint8 compression;       // 0 = store (only mode supported in S0)
	Uint32 uncompressed_size;
	Uint32 compressed_size;
	Uint32 offset;           // absolute offset of the blob within the bundle
} bundle_entry;

static bool loaded = false;    // load attempted (success or failure)
static bool present = false;   // a valid bundle is resident
static Uint8 *bundle_data = NULL;
static long bundle_size = 0;
static bundle_entry *entries = NULL;
static Uint32 entry_count = 0;

static Uint16 read_le16(const Uint8 *p) { return (Uint16)(p[0] | (p[1] << 8)); }
static Uint32 read_le32(const Uint8 *p)
{
	return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

// Locate and slurp tyrian.base into memory. Uses plain fopen against a fixed
// search list -- must NOT go through dir_fopen/data_dir (which call back into
// the bundle and would recurse).
static FILE *open_bundle_file(void)
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

		char *path = malloc(strlen(dirs[i]) + 1 + strlen(BUNDLE_FILE) + 1);
		sprintf(path, "%s/%s", dirs[i], BUNDLE_FILE);
		FILE *f = fopen(path, "rb");
		free(path);
		if (f != NULL)
			return f;
	}

	// Alongside the executable (a bundle shipped next to the binary).
	char *base = SDL_GetBasePath();
	if (base != NULL)
	{
		char *path = malloc(strlen(base) + strlen(BUNDLE_FILE) + 1);
		sprintf(path, "%s%s", base, BUNDLE_FILE);
		FILE *f = fopen(path, "rb");
		free(path);
		SDL_free(base);
		if (f != NULL)
			return f;
	}

	return NULL;
}

static void discard(void)
{
	if (entries != NULL)
	{
		for (Uint32 i = 0; i < entry_count; ++i)
			free(entries[i].name);
		free(entries);
		entries = NULL;
	}
	free(bundle_data);
	bundle_data = NULL;
	entry_count = 0;
	present = false;
}

static void parse(void)
{
	// Header: magic(8) + entry_count(4)
	if (bundle_size < 12 || memcmp(bundle_data, BUNDLE_MAGIC, 8) != 0)
	{
		fprintf(stderr, "warning: '%s' is not a valid asset bundle\n", BUNDLE_FILE);
		return;
	}

	const Uint8 *end = bundle_data + bundle_size;
	const Uint8 *p = bundle_data + 8;
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
		if ((long)tmp[i].offset + (long)tmp[i].compressed_size > bundle_size)
			goto malformed;
	}

	entries = tmp;
	entry_count = count;
	present = true;
	return;

malformed:
	fprintf(stderr, "warning: '%s' index is malformed; ignoring bundle\n", BUNDLE_FILE);
	for (Uint32 i = 0; i < count; ++i)
		free(tmp[i].name);
	free(tmp);
}

static void ensure_loaded(void)
{
	if (loaded)
		return;
	loaded = true;

	FILE *f = open_bundle_file();
	if (f == NULL)
		return;

	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return;
	}
	bundle_size = ftell(f);
	rewind(f);

	if (bundle_size <= 0)
	{
		fclose(f);
		return;
	}

	bundle_data = malloc(bundle_size);
	if (bundle_data == NULL)
	{
		fclose(f);
		return;
	}

	if (fread(bundle_data, 1, bundle_size, f) != (size_t)bundle_size)
	{
		fclose(f);
		discard();
		return;
	}
	fclose(f);

	parse();
	if (!present)
		discard();
}

static const bundle_entry *find(const char *name)
{
	if (!present)
		return NULL;

	// Match against the bare filename, case-insensitively. Callers pass a bare
	// name already, but strip any path just in case.
	const char *slash = strrchr(name, '/');
	const char *base = slash ? slash + 1 : name;

	for (Uint32 i = 0; i < entry_count; ++i)
		if (SDL_strcasecmp(entries[i].name, base) == 0)
			return &entries[i];

	return NULL;
}

bool bundle_available(void)
{
	ensure_loaded();
	return present;
}

bool bundle_has(const char *name)
{
	ensure_loaded();
	return find(name) != NULL;
}

FILE *bundle_fopen(const char *name)
{
	ensure_loaded();

	const bundle_entry *e = find(name);
	if (e == NULL)
		return NULL;

	if (e->compression != 0)
	{
		// S0 ships everything STORE; compressed blobs arrive with Phase S5.
		fprintf(stderr, "warning: bundled '%s' uses unsupported compression %u\n",
		        name, e->compression);
		return NULL;
	}

	// Zero-copy: the whole bundle is resident, so hand out a read-only memory
	// stream over the blob's slice. fmemopen does not take ownership of the
	// buffer and never frees it, so bundle_data safely outlives the FILE*.
	void *blob = bundle_data + e->offset;

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
