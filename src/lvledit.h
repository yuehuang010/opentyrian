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
/** @file lvledit.h
 * Interactive level editor shell + tile editing (Phase E1).
 *
 * Boots into a self-contained screen loop (level select -> tilemap editor)
 * over the tyrian<episode>.lvl archive, using lvledit_io.h for all archive
 * I/O. Reachable only behind the hidden `--edit <episode>` flag; never
 * called from the normal game loop. See internal/plan/LEVEL_EDITOR_PLAN.md.
 */
#ifndef LVLEDIT_H
#define LVLEDIT_H

#include <stdbool.h>

// Runs the interactive level editor over tyrian<episode>.lvl (episode 1-4)
// until the user quits back out. Assumes video/keyboard/font/palette
// subsystems are already initialized (same prerequisites as titleScreen()).
void lvledit_run(int episode);

// Headless screenshot dump: loads tyrian<episode>.lvl (episode 1-4), parses
// level_index the same way the interactive editor would, then renders and
// saves a fixed set of editor screens (each map layer anchored at the
// game-flow start view, plus the first page of the event editor) as BMPs in
// the current directory. No interactive loop -- prints each saved path to
// stdout and returns. Same subsystem prerequisites as lvledit_run().
void lvledit_dump_screens(int episode, int level_index);

// Headless full-map PNG export: loads tyrian<episode>.lvl (episode 1-4),
// parses level_index the same way the interactive editor would, then
// renders and writes the whole level (all three tile layers plus a
// composite) as PNG files in the current directory -- see lvledit_png.h for
// the writer and lvledit.c's export_full_map() for the layer/composite
// layout. No interactive loop -- prints each saved path to stdout. Returns
// true iff the archive/level loaded and every PNG was written successfully.
// Same subsystem prerequisites as lvledit_run()/lvledit_dump_screens().
bool lvledit_export_map_cli(int episode, int level_index);

#endif /* LVLEDIT_H */
