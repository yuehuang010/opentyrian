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
#ifndef SPRITE_H
#define SPRITE_H

#include "opentyr.h"

#include "SDL.h"

#include <assert.h>
#include <stdio.h>

#define FONT_SHAPES       0
#define SMALL_FONT_SHAPES 1
#define TINY_FONT         2
#define PLANET_SHAPES     3
#define FACE_SHAPES       4
#define OPTION_SHAPES     5 /*Also contains help shapes*/
#define WEAPON_SHAPES     6
#define EXTRA_SHAPES      7 /*Used for Ending pics*/

#define SPRITE_TABLES_MAX        8
#define SPRITES_PER_TABLE_MAX  151

typedef struct
{
	Uint16 width, height;
	Uint16 size;
	Uint8 *data;
}
Sprite;

typedef struct
{
	unsigned int count;
	Sprite sprite[SPRITES_PER_TABLE_MAX];
}
Sprite_array;

extern Sprite_array sprite_table[SPRITE_TABLES_MAX];  // fka shapearray, shapex, shapey, shapesize, shapexist, maxshape

static inline Sprite *sprite(unsigned int table, unsigned int index)
{
	assert(table < COUNTOF(sprite_table));
	assert(index < COUNTOF(sprite_table->sprite));
	return &sprite_table[table].sprite[index];
}

static inline bool sprite_exists(unsigned int table, unsigned int index)
{
	return (sprite(table, index)->data != NULL);
}
static inline Uint16 get_sprite_width(unsigned int table, unsigned int index)
{
	return (sprite_exists(table, index) ? sprite(table, index)->width : 0);
}
static inline Uint16 get_sprite_height(unsigned int table, unsigned int index)
{
	return (sprite_exists(table, index) ? sprite(table, index)->height : 0);
}

// Decodes a sprite into width*height bytes: 0 where transparent, else
// (0x10 | brightness nibble) -- the same low nibble blit_sprite_hv() recolors by.
bool sprite_shade_mask(unsigned int table, unsigned int index, Uint8 *mask);

void load_sprites_file(unsigned int table, const char *filename);
void load_sprites(unsigned int table, FILE *f);
void free_sprites(unsigned int table);

void blit_sprite(SDL_Surface *, int x, int y, unsigned int table, unsigned int index); // JE_newDrawCShapeNum
void blit_sprite_blend(SDL_Surface *, int x, int y, unsigned int table, unsigned int index); // JE_newDrawCShapeTrick
void blit_sprite_hv_unsafe(SDL_Surface *, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value); // JE_newDrawCShapeBright
void blit_sprite_hv(SDL_Surface *, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value); // JE_newDrawCShapeAdjust
void blit_sprite_hv_blend(SDL_Surface *, int x, int y, unsigned int table, unsigned int index, Uint8 hue, Sint8 value); // JE_newDrawCShapeModify
void blit_sprite_dark(SDL_Surface *, int x, int y, unsigned int table, unsigned int index, bool black); // JE_newDrawCShapeDarken, JE_newDrawCShapeShadow

typedef struct
{
	size_t size;
	Uint8 *data;
}
Sprite2_array;

// Shop icons and arrows sprite sheet.
extern Sprite2_array shopSpriteSheet;  // fka shapes6

// Explosions sprite sheet.
extern Sprite2_array explosionSpriteSheet;  // fka shapes6

// Enemy sprite sheet banks.
extern Sprite2_array enemySpriteSheets[4];  // fka eShapes1, eShapes2, eShapes3, eShapes4
extern Uint8 enemySpriteSheetIds[4];  // fka enemyShapeTables

// Destruct sprite sheet.
extern Sprite2_array destructSpriteSheet;  // fka shapes6

// Static sprite sheets.  Player shots, player ships, power-ups, coins, etc.
extern Sprite2_array spriteSheet8;  // fka shapesC1
extern Sprite2_array spriteSheet9;  // fka shapes9
extern Sprite2_array spriteSheet10;  // fka eShapes6
extern Sprite2_array spriteSheet11;  // fka eShapes5
extern Sprite2_array spriteSheet12;  // fka shapesW2

void JE_loadCompShapes(Sprite2_array *, char s);
void JE_loadCompShapesB(Sprite2_array *, FILE *f);
void free_sprite2s(Sprite2_array *);

// HD in-flight compositor: stable small ids for the sprite sheets that have HD
// assets. STATIC-IDENTITY sheets (loaded once at startup) get fixed ids below.
// The per-level enemy sheets (enemySpriteSheets[0..3]) are dynamic identity --
// each runtime slot is loaded from one of 31 possible newsh?.shp files, one per
// level event -- so their ids are assigned per distinct newsh source (one id per
// `enemy_<suffix>` stem the extractor emits) and resolved at runtime via
// enemy_slot_stem_id[]; see hd_enemy_slot_set()/hd_enemy_slot_clear() below.
enum
{
	HD_SHEET_SHEET8 = 0,    // player shots (spriteSheet8)
	HD_SHEET_SHEET9,        // player ship (spriteSheet9)
	HD_SHEET_SHEET10,       // spriteSheet10
	HD_SHEET_SHEET11,       // spriteSheet11
	HD_SHEET_SHEET12,       // spriteSheet12
	HD_SHEET_EXPLOSION,     // explosionSpriteSheet
	HD_SHEET_ENEMY_FIRST,   // first of 31 stable per-newsh-file enemy ids
	HD_SHEET_ENEMY_COUNT = 31,
};

// Per-slot HD sheet id for enemySpriteSheets[0..3], or -1 if that slot is empty
// or holds a newsh file with no known enemy_<suffix> HD asset (e.g. the unused
// 'Q'/'@' shapeFile entries -- see hd_enemy_stem_id_for_char()). Updated by
// hd_enemy_slot_set()/hd_enemy_slot_clear() at the same call sites that load or
// free enemySpriteSheets[i] (src/tyrian2.c). Declared here (not static) so
// hd_sheet_id_for() can read it inline; do not write it from outside
// hd_enemy_slot_set()/hd_enemy_slot_clear().
extern int enemy_slot_stem_id[4];

// Records that enemySpriteSheets[slot] now holds the newsh<lower(shape_file_char)>.shp
// bank (the same `shape_file_char` passed to JE_loadCompShapes()); resolves it to
// a stable HD_SHEET_ENEMY_FIRST+n id (or -1 if that newsh file has no HD asset).
void hd_enemy_slot_set(int slot, char shape_file_char);

// Records that enemySpriteSheets[slot] has been freed / holds no bank.
void hd_enemy_slot_clear(int slot);

// Maps a Sprite2 sheet's stable `.data` pointer to its HD sheet id, or -1 if the
// sheet has no HD asset wired yet.
int hd_sheet_id_for(const Uint8 *data);

// The asset filename stem for an HD sheet id ("sheet8" for HD_SHEET_SHEET8), or
// NULL for an unknown id.
const char *hd_sheet_stem(int sheet_id);

void blit_sprite2(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2_clip(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2_blend(SDL_Surface *,  int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2_darken(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2_filter(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index, Uint8 filter);
void blit_sprite2_filter_clip(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index, Uint8 filter);

void blit_sprite2x2(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2x2_clip(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2x2_blend(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2x2_darken(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index);
void blit_sprite2x2_filter(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index, Uint8 filter);
void blit_sprite2x2_filter_clip(SDL_Surface *, int x, int y, Sprite2_array, unsigned int index, Uint8 filter);

void JE_loadMainShapeTables(const char *shpfile);
void free_main_shape_tables(void);

#endif // SPRITE_H
