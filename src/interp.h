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
#ifndef INTERP_H
#define INTERP_H

#include "opentyr.h"
#include "sprite.h"

#include "SDL.h"

// Phase 6 (HD remaster): fixed-timestep simulation + render interpolation.
//
// The in-flight simulation still runs exactly once per game tick (byte-identical
// RNG, spawns, and per-tick movement), so gameplay is unchanged. When high-fps
// mode is on, the flight loop records every playfield draw of a tick into a
// display list (tagged by entity), then -- while waiting for the next tick's
// deadline -- replays that list several times at interpolated positions, blending
// each entity between its previous and current snapshot. The result is smooth
// motion at the display's refresh rate without altering game speed or balance.
//
// Draws that read the framebuffer (superpixels) or depend on prior contents
// (lava/water filters) can't be replayed op-by-op; those degrade to tick-rate on
// the interpolated frames (documented in interp.c).
//
// Master toggle, persisted as [video] highfps. Default off (experimental); when
// off the game presents exactly one frame per tick, identical to the classic path.
extern bool highfps_mode;

// ---- Flight-loop entry points (called from tyrian2.c) ----

// Begin a new tick's recording: rotate the previous display list, clear the
// current one, and (when high-fps mode is on) enable recording. Call at the top
// of the flight loop body, before any playfield drawing.
void interp_record_begin(void);

// Finish the tick: stop recording and snapshot per-frame interpolation inputs
// (starfield positions, post-filter parameters). Call just before presenting.
void flight_interp_capture(void);

// Present interpolated frames until the current tick's deadline, then schedule the
// next tick. Replaces JE_starShowVGA()'s pace-then-present-once for the flight loop.
void flight_present(void);

// Discard snapshots (call at level start) so the first tick does not interpolate
// from stale positions left over from a previous level.
void flight_interp_reset(void);

// HD in-flight compositor (Track B): walk the current tick's display list and push
// HD sprite overlays into the video flight queue at the given interpolation alpha
// (1.0 = current tick, no motion interpolation). Clears the queue first. A no-op
// unless hd_mode is on. Emits for the wired static-identity sheets (sheet8..12 +
// explosion) across plain/darken/blend variants (hue-band `_filter` deferred); the
// 8-bit blit is never suppressed, so this is purely additive.
void interp_flight_emit(float alpha);

// ---- Recording (called from the draw choke points; no-ops unless recording) ----

// True only during a tick's draw pass while high-fps mode is active. The blit
// choke points check this (cheap) before recording.
extern bool interp_recording;

// Interpolation group tags: identify which logical entity a recorded draw belongs
// to, so the replay can pair it with the same entity's previous-tick draw. The
// index distinguishes array slots (enemy i, shot z, ...); repeated draws of the
// same tag within a tick are paired by their order of appearance.
enum
{
	INTERP_TAG_STATIC = 0,  // fixed-position (no interpolation): HUD text in playfield
	INTERP_TAG_BG1,
	INTERP_TAG_BG2,
	INTERP_TAG_BG3,
	INTERP_TAG_STAR,
	INTERP_TAG_ENEMY,
	INTERP_TAG_PLAYER_SHOT,
	INTERP_TAG_ENEMY_SHOT,
	INTERP_TAG_PLAYER,
	INTERP_TAG_SIDEKICK,
	INTERP_TAG_EXPLOSION,
	INTERP_TAG_REP_EXPLOSION,
};
#define INTERP_TAG(type, idx) (((unsigned int)(type) << 9) | ((unsigned int)(idx) & 0x1ff))

// Set the tag applied to subsequent recorded draws. Call before drawing each
// entity in the flight loop; harmless (and cheap) when not recording.
void interp_tag(unsigned int tag);

// Recording hooks, called at the top of the corresponding blit functions. They
// record into the current display list only while interp_recording is set and the
// destination is the playfield buffer (game_screen); otherwise they return
// immediately. `kind` selects which blit variant to re-issue on replay.
typedef enum
{
	INTERP_SPRITE2,
	INTERP_SPRITE2_BLEND,
	INTERP_SPRITE2_DARKEN,
	INTERP_SPRITE2_FILTER,
	INTERP_SPRITE2_CLIP,
	INTERP_SPRITE2_FILTER_CLIP,
	INTERP_SPRITE2X2,
	INTERP_SPRITE2X2_BLEND,
	INTERP_SPRITE2X2_DARKEN,
	INTERP_SPRITE2X2_FILTER,
	INTERP_SPRITE2X2_FILTER_CLIP,
} InterpSprite2Kind;

void interp_record_sprite2(SDL_Surface *dst, int x, int y, Sprite2_array s2, unsigned int index, InterpSprite2Kind kind, Uint8 filter);
void interp_record_bg_row(SDL_Surface *dst, int x, int y, Uint8 **map, bool blend);
void interp_record_starfield(SDL_Surface *dst);

// Table/index sprites and font glyphs (the blit_sprite* family). Playfield HUD
// text (score, cash, armor readouts) draws through these; recording them keeps
// that text visible on interpolated frames. Tag them INTERP_TAG_STATIC so they
// hold their fixed position.
typedef enum
{
	INTERP_ST_PLAIN,
	INTERP_ST_BLEND,
	INTERP_ST_HV,
	INTERP_ST_HV_UNSAFE,
	INTERP_ST_HV_BLEND,
	INTERP_ST_DARK,
} InterpSpriteTableKind;

void interp_record_sprite_table(SDL_Surface *dst, int x, int y, unsigned int table, unsigned int index, InterpSpriteTableKind kind, Uint8 hue, Sint8 value, bool black);

#endif /* INTERP_H */
