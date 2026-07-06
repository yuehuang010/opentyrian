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

#include "keyboard.h"
#include "nortsong.h"
#include "tyrian2.h"
#include "varz.h"

#include "SDL.h"

bool highfps_mode = false;

// True once a full snapshot exists, so the first tick of a level presents the
// current frame without blending against uninitialized "previous" positions.
static bool have_prev = false;

/**
 * Renders the playfield into game_screen from the previous/current snapshots
 * blended by alpha (0.0 = previous tick, 1.0 = current tick).
 *
 * STAGE 1 (this commit): a no-op placeholder. The simulation has already drawn
 * the current-tick frame into game_screen, so presenting it repeatedly is
 * behavior-identical to the classic single present -- this proves the timing
 * harness with zero visual change. Subsequent stages fill this in per entity
 * class (backgrounds/starfield, player/shots, enemies, particles).
 */
static void interp_draw(float alpha)
{
	(void)alpha;
}

void flight_interp_reset(void)
{
	have_prev = false;
}

void flight_interp_capture(void)
{
	// STAGE 1: no per-entity snapshot state yet. Later stages copy live entity
	// positions into a "current" buffer here and rotate the previous buffer.
	have_prev = true;
}

void flight_present(void)
{
	// Fill the wait until this tick's scheduled deadline (frameCountEnd) with
	// interpolated presents, mirroring JE_starShowVGA()'s
	// delayUntilElapsed() + setFrameCount() pacing but drawing intermediate
	// frames instead of sleeping. The loop always presents at least once (even
	// if the simulation overran the tick period), and never advances the
	// simulation faster than real time.
	do
	{
		interp_draw(have_prev ? getTickInterpAlpha() : 1.0f);
		JE_starCompositeShow();
		handleSdlEvents();

		// Yield rather than busy-present when there is still time on the clock;
		// SDL_Delay(1) caps the interpolated frame rate to a sane ceiling.
		if (getFrameCountTicks() > 1)
			SDL_Delay(1);
	}
	while (getFrameCountTicks() > 0);

	setFrameCount(frameCountMax);

	// Match the per-tick resets JE_starShowVGA() performs.
	quitRequested = false;
	skipStarShowVGA = false;
}
