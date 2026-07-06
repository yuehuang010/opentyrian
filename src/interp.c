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
#include "interp.h"

#include "backgrnd.h"
#include "config.h"
#include "keyboard.h"
#include "nortsong.h"
#include "nortvars.h"
#include "sprite.h"
#include "tyrian2.h"
#include "varz.h"
#include "video.h"

#include "SDL.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool highfps_mode = false;
bool interp_recording = false;

// ---------------------------------------------------------------------------
// Display list
//
// One op per playfield draw of a tick, in exact draw order (so replay preserves
// occlusion). Two buffers are kept: the current tick's list and the previous
// tick's, so each op can be paired with the same entity's prior-tick draw and
// interpolated between them.
// ---------------------------------------------------------------------------

typedef enum
{
	OP_SPRITE2,        // Sprite2 blit; `variant` selects the exact function
	OP_SPRITE_TABLE,   // table/index blit_sprite* (fonts, HUD); `variant` = kind
	OP_BG_ROW,         // blit_background_row
	OP_BG_ROW_BLEND,   // blit_background_row_blend
	OP_STARFIELD,      // marker: draw the interpolated starfield here
} OpType;

typedef struct
{
	Uint8 type;        // OpType
	Uint8 variant;     // InterpSprite2Kind for OP_SPRITE2
	Uint8 filter;      // filter byte for the *_FILTER variants
	Uint32 key;        // (tag << 10) | occurrence, for prev/curr pairing
	int x, y;
	Sprite2_array s2;  // OP_SPRITE2
	unsigned int index;
	Uint8 **map;       // OP_BG_ROW[_BLEND]
	unsigned int table; // OP_SPRITE_TABLE
	Uint8 hue;          // OP_SPRITE_TABLE (*_hv* variants)
	Sint8 value;        // OP_SPRITE_TABLE (*_hv* variants)
	bool black;         // OP_SPRITE_TABLE (dark variant)
} Op;

#define MAX_OPS 6144

static Op ops_a[MAX_OPS], ops_b[MAX_OPS];
static Op *curr_ops = ops_a, *prev_ops = ops_b;
static int curr_count = 0, prev_count = 0;

// Current recording tag + run-length occurrence (reset each tick). Ops of the same
// tag are emitted consecutively per entity, so a simple run counter assigns each a
// stable occurrence index that pairs across ticks.
static unsigned int cur_tag = INTERP_TAG(INTERP_TAG_STATIC, 0);
static unsigned int run_tag = 0xffffffffu;
static unsigned int run_occ = 0;

// Whether a full previous-tick list exists to interpolate against.
static bool have_prev = false;

// Positions to interpolate the starfield between (captured pre/post tick).
static JE_word star_prev[STARFIELD_STAR_COUNT];
static JE_word star_curr[STARFIELD_STAR_COUNT];

// Post-filter parameters snapshotted from the tick, re-applied read-only on
// interpolated frames (the colored level tint / brightness; see interp_draw).
static bool snap_filter_active;
static JE_shortint snap_level_filter;
static JE_shortint snap_level_brightness;

// Largest per-tick position delta (pixels) still treated as continuous motion.
// Larger deltas (a reused array slot holding a different entity, a teleport, a
// wrap we don't special-case) snap to the current position instead of smearing.
#define MAX_INTERP_JUMP 72

// ---- prev-list key -> index hash (open addressing, power of two) ----
#define HASH_SIZE 16384 // >= 2 * MAX_OPS, power of two
static int hash_idx[HASH_SIZE];
static Uint32 hash_key[HASH_SIZE];
static bool hash_built = false;

static void hash_build_prev(void)
{
	memset(hash_idx, -1, sizeof hash_idx);
	for (int i = 0; i < prev_count; ++i)
	{
		Uint32 k = prev_ops[i].key;
		unsigned int h = (k * 2654435761u) & (HASH_SIZE - 1);
		// First writer wins, matching the first (lowest-index) prior op of this key
		// to the first current op of the key, preserving per-entity sub-order.
		while (hash_idx[h] != -1)
		{
			if (hash_key[h] == k)
				break; // keep the earliest index already stored
			h = (h + 1) & (HASH_SIZE - 1);
		}
		if (hash_idx[h] == -1)
		{
			hash_idx[h] = i;
			hash_key[h] = k;
		}
	}
	hash_built = true;
}

// Find the prev op matching `key` at the given occurrence. Because equal keys
// already encode occurrence, a direct lookup suffices.
static const Op *hash_find_prev(Uint32 key)
{
	unsigned int h = (key * 2654435761u) & (HASH_SIZE - 1);
	int guard = 0;
	while (hash_idx[h] != -1 && guard++ < HASH_SIZE)
	{
		if (hash_key[h] == key)
			return &prev_ops[hash_idx[h]];
		h = (h + 1) & (HASH_SIZE - 1);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void interp_tag(unsigned int tag)
{
	cur_tag = tag;
}

static Op *push_op(void)
{
	if (curr_count >= MAX_OPS)
		return NULL;

	// Assign this op an occurrence within its tag's consecutive run.
	if (cur_tag == run_tag)
		++run_occ;
	else
	{
		run_tag = cur_tag;
		run_occ = 0;
	}

	Op *op = &curr_ops[curr_count++];
	op->key = (cur_tag << 10) | (run_occ & 0x3ff);
	return op;
}

void interp_record_sprite2(SDL_Surface *dst, int x, int y, Sprite2_array s2, unsigned int index, InterpSprite2Kind kind, Uint8 filter)
{
	if (!interp_recording || dst != game_screen)
		return;

	Op *op = push_op();
	if (op == NULL)
		return;

	op->type = OP_SPRITE2;
	op->variant = (Uint8)kind;
	op->filter = filter;
	op->x = x;
	op->y = y;
	op->s2 = s2;
	op->index = index;
}

void interp_record_sprite_table(SDL_Surface *dst, int x, int y, unsigned int table, unsigned int index, InterpSpriteTableKind kind, Uint8 hue, Sint8 value, bool black)
{
	if (!interp_recording || dst != game_screen)
		return;

	Op *op = push_op();
	if (op == NULL)
		return;

	op->type = OP_SPRITE_TABLE;
	op->variant = (Uint8)kind;
	op->x = x;
	op->y = y;
	op->table = table;
	op->index = index;
	op->hue = hue;
	op->value = value;
	op->black = black;
}

void interp_record_bg_row(SDL_Surface *dst, int x, int y, Uint8 **map, bool blend)
{
	if (!interp_recording || dst != game_screen)
		return;

	Op *op = push_op();
	if (op == NULL)
		return;

	op->type = blend ? OP_BG_ROW_BLEND : OP_BG_ROW;
	op->x = x;
	op->y = y;
	op->map = map;
}

void interp_record_starfield(SDL_Surface *dst)
{
	if (!interp_recording || dst != game_screen)
		return;

	Op *op = push_op();
	if (op == NULL)
		return;

	op->type = OP_STARFIELD;
}

// ---------------------------------------------------------------------------
// Tick lifecycle
// ---------------------------------------------------------------------------

void interp_record_begin(void)
{
	if (!highfps_mode)
	{
		interp_recording = false;
		return;
	}

	// Rotate: this tick records into what was the previous list.
	Op *tmp = prev_ops;
	prev_ops = curr_ops;
	curr_ops = tmp;
	prev_count = curr_count;

	curr_count = 0;
	run_tag = 0xffffffffu;
	run_occ = 0;
	hash_built = false;

	interp_recording = true;
}

void flight_interp_reset(void)
{
	have_prev = false;
	interp_recording = false;
	curr_count = prev_count = 0;
}

void flight_interp_capture(void)
{
	interp_recording = false;

	// Snapshot the interpolation inputs that live outside the display list.
	memcpy(star_prev, star_curr, sizeof star_prev);
	starfield_snapshot_positions(star_curr);

	snap_filter_active = filterActive;
	snap_level_filter = levelFilter;
	snap_level_brightness = levelBrightness;

	// A usable previous list exists once we've captured two ticks in a row.
	have_prev = (prev_count > 0);
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

static void dispatch_sprite2(SDL_Surface *dst, const Op *op, int x, int y)
{
	switch ((InterpSprite2Kind)op->variant)
	{
	case INTERP_SPRITE2:              blit_sprite2(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2_BLEND:        blit_sprite2_blend(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2_DARKEN:       blit_sprite2_darken(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2_FILTER:       blit_sprite2_filter(dst, x, y, op->s2, op->index, op->filter); break;
	case INTERP_SPRITE2_CLIP:         blit_sprite2_clip(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2_FILTER_CLIP:  blit_sprite2_filter_clip(dst, x, y, op->s2, op->index, op->filter); break;
	case INTERP_SPRITE2X2:            blit_sprite2x2(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2X2_BLEND:      blit_sprite2x2_blend(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2X2_DARKEN:     blit_sprite2x2_darken(dst, x, y, op->s2, op->index); break;
	case INTERP_SPRITE2X2_FILTER:     blit_sprite2x2_filter(dst, x, y, op->s2, op->index, op->filter); break;
	case INTERP_SPRITE2X2_FILTER_CLIP:blit_sprite2x2_filter_clip(dst, x, y, op->s2, op->index, op->filter); break;
	}
}

static void dispatch_sprite_table(SDL_Surface *dst, const Op *op, int x, int y)
{
	switch ((InterpSpriteTableKind)op->variant)
	{
	case INTERP_ST_PLAIN:     blit_sprite(dst, x, y, op->table, op->index); break;
	case INTERP_ST_BLEND:     blit_sprite_blend(dst, x, y, op->table, op->index); break;
	case INTERP_ST_HV:        blit_sprite_hv(dst, x, y, op->table, op->index, op->hue, op->value); break;
	case INTERP_ST_HV_UNSAFE: blit_sprite_hv_unsafe(dst, x, y, op->table, op->index, op->hue, op->value); break;
	case INTERP_ST_HV_BLEND:  blit_sprite_hv_blend(dst, x, y, op->table, op->index, op->hue, op->value); break;
	case INTERP_ST_DARK:      blit_sprite_dark(dst, x, y, op->table, op->index, op->black); break;
	}
}

// Re-applies the tick's colored level filter / brightness to the playfield,
// read-only (does not advance the fade). Mirrors JE_filterScreen's two passes so
// filtered levels don't strobe between filtered and unfiltered on interp frames.
static void reapply_filter(SDL_Surface *surface)
{
	if (!snap_filter_active)
		return;

	Uint8 *s;
	int x, y;

	if (snap_level_filter != -99 && filtrationAvail)
	{
		s = (Uint8 *)surface->pixels + 24;
		int col = snap_level_filter << 4;
		for (y = 184; y; y--)
		{
			for (x = 264; x; x--)
			{
				*s = col | (*s & 0x0f);
				s++;
			}
			s += surface->pitch - 264;
		}
	}

	if (snap_level_brightness != -99 && explosionTransparent)
	{
		s = (Uint8 *)surface->pixels + 24;
		for (y = 184; y; y--)
		{
			for (x = 264; x; x--)
			{
				unsigned int temp = (*s & 0x0f) + snap_level_brightness;
				*s = (*s & 0xf0) | (temp >= 0x1f ? 0 : (temp >= 0x0f ? 0x0f : temp));
				s++;
			}
			s += surface->pitch - 264;
		}
	}
}

// Renders the interpolated playfield into game_screen for the given alpha
// (0.0 = previous tick, 1.0 = current tick) by replaying the current tick's
// display list, offsetting each op toward its previous-tick position.
static void interp_draw(float alpha)
{
	if (!have_prev || curr_count == 0)
	{
		// Can't interpolate (no previous list, or nothing recorded): the
		// simulation already drew the current-tick frame into game_screen, so
		// present it as-is -- full-quality, including framebuffer-read effects.
		return;
	}

	// With a previous list, *every* present -- including alpha == 1 -- is a replay,
	// so all frames render through the same path and never pop. At alpha == 1 the
	// replay reproduces the simulation's own sprite/background draws exactly (same
	// positions, same order); it omits only the framebuffer-reading effects that
	// can't be replayed op-by-op (superpixel sparkles, smoothie/lava/water
	// filters), which therefore do not appear while high-fps mode is active. The
	// flat colored level tint is preserved (reapply_filter, below).

	if (!hash_built)
		hash_build_prev();

	// The blit_sprite* helpers reference VGAScreen->pitch for row strides; point
	// VGAScreen at the buffer we're drawing into so replay is unambiguous. Restored
	// before returning so the subsequent composite/present sees VGAScreenSeg.
	SDL_Surface *saved_vga = VGAScreen;
	VGAScreen = game_screen;

	JE_clr256(game_screen);

	const float back = 1.0f - alpha; // weight toward the previous position

	for (int i = 0; i < curr_count; ++i)
	{
		const Op *op = &curr_ops[i];

		if (op->type == OP_STARFIELD)
		{
			draw_starfield_interp(game_screen, star_prev, star_curr, alpha);
			continue;
		}

		int dx = op->x, dy = op->y;

		const Op *p = hash_find_prev(op->key);
		if (p != NULL)
		{
			int ddx = p->x - op->x;
			int ddy = p->y - op->y;

			// Background layers scroll and wrap every 28 px; unwrap so a wrap tick
			// interpolates forward rather than snapping back a whole tile row.
			if (op->type == OP_BG_ROW || op->type == OP_BG_ROW_BLEND)
			{
				if (ddy > 14)
					ddy -= 28;
				else if (ddy < -14)
					ddy += 28;
			}

			if (abs(ddx) <= MAX_INTERP_JUMP && abs(ddy) <= MAX_INTERP_JUMP)
			{
				dx = op->x + (int)lrintf(ddx * back);
				dy = op->y + (int)lrintf(ddy * back);
			}
		}

		switch (op->type)
		{
		case OP_SPRITE2:      dispatch_sprite2(game_screen, op, dx, dy); break;
		case OP_SPRITE_TABLE: dispatch_sprite_table(game_screen, op, dx, dy); break;
		case OP_BG_ROW:       blit_background_row(game_screen, dx, dy, op->map); break;
		case OP_BG_ROW_BLEND: blit_background_row_blend(game_screen, dx, dy, op->map); break;
		case OP_STARFIELD:    break; // handled above
		}
	}

	reapply_filter(game_screen);

	VGAScreen = saved_vga;
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------

void flight_present(void)
{
	// Fill the wait until this tick's scheduled deadline (frameCountEnd) with
	// interpolated presents, mirroring JE_starShowVGA()'s
	// delayUntilElapsed() + setFrameCount() pacing but drawing intermediate frames
	// instead of sleeping. Always presents at least once (even if the simulation
	// overran the tick period) and never advances the simulation faster than real
	// time.
	do
	{
		interp_draw(have_prev ? getTickInterpAlpha() : 1.0f);
		JE_starCompositeShow();
		handleSdlEvents();

		// Yield rather than busy-present when there is still time on the clock.
		if (getFrameCountTicks() > 1)
			SDL_Delay(1);
	}
	while (getFrameCountTicks() > 0);

	setFrameCount(frameCountMax);

	// Match the per-tick resets JE_starShowVGA() performs.
	quitRequested = false;
	skipStarShowVGA = false;
}
