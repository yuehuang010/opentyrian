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
#ifndef HD_HUD_H
#define HD_HUD_H

#include "SDL.h"

/**
 * HD in-flight HUD overlay (REMASTER_HUD.md phase H2.5). Draws a fully
 * PROCEDURAL vector panel for the sidebar (VGA x in [264,320), y in [0,200))
 * and bottom bar (VGA y in [184,200), x in [0,264)) strips -- flat dark fills,
 * hairline steel frames, inset wells, and crisp HD-font labels -- plus
 * vectorized shield/armor/power bars, weapon-power dots, and sidekick ammo
 * gauges, at output resolution. No bitmap panel art is used.
 *
 * Called from the hd_mode && hd_flight_active branch of scale_and_flip()
 * (src/video.c), after the flight-sprite clip rect is released and before
 * draw_hd_font_queue() (so the labels queued here render this same present).
 * `dst_rect` is the same output rect the flight-sprite queue scales against
 * (src/video.c's classic_scale_base() output).
 *
 * Gated internally to 1P only; 2P falls through, leaving the classic 8-bit
 * panel (already present in the base image) showing through. Elements not yet
 * vectorized (sidekick icons, rear-config buttons, the message-bar interior)
 * stay as punch-outs: the procedural fills skip them so the classic pixels show.
 */
void hd_hud_draw(SDL_Renderer *renderer, const SDL_Rect *dst_rect);

#endif /* HD_HUD_H */
