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
#ifndef VIDEO_SCALE_H
#define VIDEO_SCALE_H

#include "opentyr.h"

#include "SDL.h"

typedef void (*ScalerFunction)(SDL_Surface *src, SDL_Texture *dst);

struct Scalers
{
	int width, height;
	ScalerFunction scaler16, scaler32;
	const char *name;
};

extern uint scaler;
extern const struct Scalers scalers[];
extern const uint scalers_count;

void set_scaler_by_name(const char *name);

// hq4x_32() itself is defined in video_scale_hqNx.c and forward-declared
// locally in video_scale.c for the scaler registry; exported here too so
// non-scaler callers (the HD HUD panel bake, src/hd_hud.c) can invoke it
// directly on their own scratch surface/texture without duplicating it.
void hq4x_32(SDL_Surface *src_surface, SDL_Texture *dst_texture);

#endif /* VIDEO_SCALE_H */
