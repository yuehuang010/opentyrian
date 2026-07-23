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
#ifndef PARAMS_H
#define PARAMS_H

#include "opentyr.h"

extern JE_boolean richMode, constantPlay, constantDie;

// Hidden level-editor flag (Phase E0): --edit-roundtrip <episode 1-4>.
// 0 means "not requested"; otherwise the requested episode number.
extern int edit_roundtrip_episode;

// Hidden level-editor flag (Phase E4): --edit-addlevel <episode 1-4>. Same
// "0 means not requested" sentinel as edit_roundtrip_episode above.
extern int edit_addlevel_episode;

// Hidden level-editor flag (Phase E5a): --edit-script-roundtrip <episode
// 1-4>, the levels<ep>.dat script-codec round-trip self-test. Same "0 means
// not requested" sentinel as edit_roundtrip_episode above.
extern int edit_script_roundtrip_episode;

// Hidden level-editor flag (Phase E5b): --edit-script-retarget-test <episode
// 1-4>, the section-ordinal retargeting invariant self-test. Same "0 means
// not requested" sentinel as edit_roundtrip_episode above.
extern int edit_script_retarget_episode;

// Hidden level-editor flag (Phase E1): --edit (no argument) boots the
// interactive editor (lvledit_run()) instead of the normal title screen.
// The editor now presents its own in-app episode picker followed by the
// existing per-episode level-select, so no episode need be given here.
extern bool edit_requested;

// Hidden level-editor flag: --edit-shot <episode>,<level> renders a fixed set
// of editor screens for one level and saves them as BMPs in the current
// directory, then exits -- no interactive loop. edit_shot_requested is false
// unless the flag was given (level 0 is a valid level index, so it can't
// double as its own "not requested" sentinel the way a plain int would).
extern bool edit_shot_requested;
extern int edit_shot_episode;
extern int edit_shot_level;

// Hidden level-editor flag: --edit-export <episode>,<level> renders the
// entire level as PNGs (one per tile layer plus a composite) and saves them
// in the current directory, then exits -- no interactive loop. Same
// "requested" sentinel pattern as edit_shot_requested, for the same reason
// (level 0 is valid).
extern bool edit_export_requested;
extern int edit_export_episode;
extern int edit_export_level;

void JE_paramCheck(int argc, char *argv[]);

#endif /* PARAMS_H */
