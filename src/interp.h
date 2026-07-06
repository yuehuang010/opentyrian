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

// Phase 6 (HD remaster): fixed-timestep simulation + render interpolation.
//
// The in-flight simulation still runs exactly once per game tick (byte-identical
// RNG, spawns, and per-tick movement), so gameplay is unchanged. When high-fps
// mode is on, the flight loop snapshots entity positions at the end of each tick
// and presents several interpolated frames -- blending the previous and current
// snapshots -- while waiting for the next tick's deadline, yielding smooth motion
// at the display's refresh rate without altering game speed or balance.
//
// Master toggle, persisted as [video] highfps. Default off (experimental); when
// off the game presents exactly one frame per tick, identical to the classic path.
extern bool highfps_mode;

// Called once per simulation tick, just before presenting: rotate the previous
// snapshot and capture the current live entity positions.
void flight_interp_capture(void);

// Present interpolated frames until the current tick's deadline, then schedule the
// next tick. Replaces JE_starShowVGA()'s pace-then-present-once for the flight loop.
void flight_present(void);

// Discard snapshots (call at level start) so the first tick does not interpolate
// from stale positions left over from a previous level.
void flight_interp_reset(void);

#endif /* INTERP_H */
