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
	// Record when high-fps interpolation is active OR when the HD flight compositor
	// needs the display list to emit HD overlays (which it does even with high-fps
	// off -- one HD present per tick at alpha=1).
	if (!highfps_mode && !(hd_mode && hd_flight_active))
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

// Computes an op's interpolated draw position for the given `back` weight
// (= 1 - alpha, the weight toward the previous-tick position). Shared by the
// replay (interp_draw) and the HD overlay emit (interp_flight_emit) so both use
// identical positions and the HD sprite rides exactly on top of its 8-bit base.
// When there is no previous list to pair against, returns the raw current-tick
// position (alpha = 1 behaviour).
static void interp_op_pos(const Op *op, float back, int *out_dx, int *out_dy)
{
	int dx = op->x, dy = op->y;

	if (have_prev)
	{
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
	}

	*out_dx = dx;
	*out_dy = dy;
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

		int dx, dy;
		interp_op_pos(op, back, &dx, &dy);

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
// HD in-flight overlay emit
// ---------------------------------------------------------------------------

// Emits a single 12x14 HD tile: loads the frame, and (on an atlas hit) pushes one
// queue entry at the playfield-relative position (dx-24, dy) with the given
// recolor params. A miss is a per-tile fallback (the 8-bit blit already drew it) --
// never suppress the base blit. `index` is the 1-based Sprite2 frame index; HD
// frame files are 0-based (hdcomp_<sheet>_00.dat == index 1).
static void flight_emit_tile(int sheet, unsigned int index, int dx, int dy,
                             SDL_BlendMode bm, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	if (index < 1)
		return;
	int frame = (int)index - 1;

	SDL_Texture *tex = load_hd_sheet_frame(sheet, frame);
	if (tex == NULL)
		return;

	// The HD asset (48x56) is a 4x upscale of the 12x14 Sprite2 cell. The -24
	// matches JE_starCompositeShow's `src += 24` playfield origin.
	SDL_Rect src = { 0, 0, 48, 56 };
	hd_flight_set(tex, src, dx - 24, dy, 12, 14, bm, r, g, b, a);
}

// Emits a single 12x14 HD tile through the hue-band recoloring parity path
// (blit_sprite2_filter's `filter | brightness` remap; see
// internal/plan/REMASTER_FLIGHT_COMPOSITOR.md §3 and load_hd_sheet_frame_filtered's
// comment in video.c for the exact byte-layout derivation). Synthesized frames
// already carry the final RGB + coverage alpha, so they draw opaque/BLEND with
// no colormod, same as flight_emit_tile's plain case. A cache/asset miss is a
// per-tile fallback (the 8-bit filtered blit already drew it) -- never suppress.
static void flight_emit_tile_filtered(int sheet, unsigned int index, Uint8 filter, int dx, int dy)
{
	if (index < 1)
		return;
	int frame = (int)index - 1;

	SDL_Texture *tex = load_hd_sheet_frame_filtered(sheet, frame, filter);
	if (tex == NULL)
		return;

	SDL_Rect src = { 0, 0, 48, 56 };
	hd_flight_set(tex, src, dx - 24, dy, 12, 14, SDL_BLENDMODE_BLEND, 255, 255, 255, 255);
}

void interp_flight_emit(float alpha)
{
	// Rebuild the video flight queue for this presented frame.
	hd_flight_begin();

	if (!hd_mode || curr_count == 0)
		return;

	// Mirror (1) / light (2) levels transform the whole 8-bit composite in
	// JE_starCompositeShow in ways window-space HD quads can't follow. Emit no HD
	// overlay these frames: the scaled 8-bit base already carries the transformed
	// sprites, so they render correctly (just not upscaled) instead of misplaced.
	if (starShowVGASpecialCode != 0)
		return;

	if (have_prev && !hash_built)
		hash_build_prev();

	const float back = 1.0f - alpha;

	// Ground structures are drawn before the foreground background layer (e.g. the
	// clouds of level 1, background2over==1) and are occluded by it in the 8-bit
	// composite. Backgrounds aren't HD, so re-drawing those sprites in the HD overlay
	// would float them on top of the clouds. Find the last background-row op; any
	// sprite op a later background layer paints over stays pure 8-bit (correctly
	// occluded), and only sprites above the last background layer (player, sky/top
	// enemies, shots, explosions) get the HD overlay.
	int last_bg = -1;
	for (int i = 0; i < curr_count; ++i)
		if (curr_ops[i].type == OP_BG_ROW || curr_ops[i].type == OP_BG_ROW_BLEND)
			last_bg = i;

	for (int i = 0; i < curr_count; ++i)
	{
		const Op *op = &curr_ops[i];

		if (op->type != OP_SPRITE2)
			continue;

		if (i < last_bg)
			continue;  // a later background layer occludes this sprite; stays 8-bit

		// Sheet identity: sheet8..12, explosion, and the 31 dynamic per-level
		// enemy banks (enemySpriteSheets[]) are wired. Any unresolved sheet (e.g.
		// an empty enemy slot, or a newsh file with no HD asset) returns -1 and
		// stays pure 8-bit (no HD overlay).
		int sheet = hd_sheet_id_for(op->s2.data);
		if (sheet < 0)
			continue;

		// Map the recorded blit variant to an SDL2 recolor per the parity table in
		// internal/plan/REMASTER_FLIGHT_COMPOSITOR.md §3. `_filter`/hue-band parity is deferred
		// (SDL colormod can't rotate hue) -- those variants stay pure 8-bit for now.
		SDL_BlendMode bm = SDL_BLENDMODE_BLEND;
		Uint8 r = 255, g = 255, b = 255, a = 255;
		bool is_2x2 = false;
		bool is_filter = false;

		switch ((InterpSprite2Kind)op->variant)
		{
		case INTERP_SPRITE2:
		case INTERP_SPRITE2_CLIP:
			// plain: opaque copy. (_clip differs only in screen-edge clipping, which
			// SDL's RenderCopy handles against the destination rect.)
			break;
		case INTERP_SPRITE2_DARKEN:
			// shadow: the 8-bit op halves destination brightness in the silhouette.
			// Approximate as the frame's own alpha mask drawn black at ~50%.
			r = g = b = 0;
			a = 128;
			break;
		case INTERP_SPRITE2_BLEND:
			// translucency/explosions: 8-bit averages sprite+dest brightness. Draw the
			// sprite at ~50% over the destination.
			a = 128;
			break;
		case INTERP_SPRITE2X2:
			is_2x2 = true;
			break;
		case INTERP_SPRITE2X2_DARKEN:
			r = g = b = 0;
			a = 128;
			is_2x2 = true;
			break;
		case INTERP_SPRITE2X2_BLEND:
			a = 128;
			is_2x2 = true;
			break;
		case INTERP_SPRITE2_FILTER:
		case INTERP_SPRITE2_FILTER_CLIP:
			// hue-band remap (enemy tints): resolved via the on-demand recolored-frame
			// cache (load_hd_sheet_frame_filtered, video.c) keyed on op->filter, which
			// is only ever produced by blit_enemy() -- see internal/plan/REMASTER_FLIGHT_COMPOSITOR.md §3.
			is_filter = true;
			break;
		case INTERP_SPRITE2X2_FILTER:
		case INTERP_SPRITE2X2_FILTER_CLIP:
			is_filter = true;
			is_2x2 = true;
			break;
		default:
			continue;
		}

		int dx, dy;
		interp_op_pos(op, back, &dx, &dy);

		if (is_filter)
		{
			// Same 2x2 decomposition note as the plain case below: blit_sprite2x2_filter*
			// (sprite.c) currently decomposes into four base blit_sprite2_filter* calls,
			// each recording its own INTERP_SPRITE2_FILTER[_CLIP] op, so enemies reach
			// here as four base-kind filtered ops; this 2x2 branch is a
			// correctness-preserving fallback should a 2x2 filter kind ever be recorded.
			if (is_2x2)
			{
				flight_emit_tile_filtered(sheet, op->index,      op->filter, dx,      dy);
				flight_emit_tile_filtered(sheet, op->index + 1,  op->filter, dx + 12, dy);
				flight_emit_tile_filtered(sheet, op->index + 19, op->filter, dx,      dy + 14);
				flight_emit_tile_filtered(sheet, op->index + 20, op->filter, dx + 12, dy + 14);
			}
			else
			{
				flight_emit_tile_filtered(sheet, op->index, op->filter, dx, dy);
			}
		}
		else if (is_2x2)
		{
			// 2x2 ship sprite composed exactly as blit_sprite2x2 (sprite.c): four
			// 12x14 tiles at base indices i, i+1, i+19, i+20 and pixel offsets
			// (0,0), (12,0), (0,14), (12,14). Each tile is its own HD frame; a missing
			// tile falls back to the 8-bit blit already drawn there.
			//
			// NOTE: in the current codebase blit_sprite2x2* is not a recording choke
			// point -- it decomposes into four base blit_sprite2* calls that each
			// record their own INTERP_SPRITE2/_DARKEN/_BLEND op, so the ship reaches
			// here as four base-kind ops (handled below) and this 2x2 branch is a
			// correctness-preserving fallback should a 2x2 kind ever be recorded.
			flight_emit_tile(sheet, op->index,      dx,      dy,      bm, r, g, b, a);
			flight_emit_tile(sheet, op->index + 1,  dx + 12, dy,      bm, r, g, b, a);
			flight_emit_tile(sheet, op->index + 19, dx,      dy + 14, bm, r, g, b, a);
			flight_emit_tile(sheet, op->index + 20, dx + 12, dy + 14, bm, r, g, b, a);
		}
		else
		{
			flight_emit_tile(sheet, op->index, dx, dy, bm, r, g, b, a);
		}
	}
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
		float alpha = have_prev ? getTickInterpAlpha() : 1.0f;
		interp_draw(alpha);
		interp_flight_emit(alpha); // HD overlay rides the same interpolated positions
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
