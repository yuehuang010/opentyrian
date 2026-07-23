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
/** @file lvledit.c
 * Interactive level editor shell + tile editing (Phase E1). See lvledit.h.
 *
 * Two screens, jukebox.c-style event pump/present loops:
 *   - level select: pick a level record out of the loaded archive
 *   - map editor: view/edit one of the level's three tile layers
 *
 * Tiles are 24x28 8-bit paletted blits straight into VGAScreen->pixels,
 * self-loaded from shapes<c>.dat (600 entries) independently of the normal
 * JE_loadMap() path. Only reachable via the hidden `--edit <episode>` flag.
 */

#include "lvledit.h"

#include "lvledit_io.h"
#include "lvledit_png.h"
#include "lvledit_script.h"

#include "config.h"
#include "episodes.h"
#include "file.h"
#include "fonthand.h"
#include "keyboard.h"
#include "loudness.h"
#include "lvllib.h"
#include "mainint.h"
#include "mouse.h"
#include "nortsong.h"
#include "opentyr.h"
#include "palette.h"
#include "sprite.h"
#include "tyrian2.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_TILE_W    24
#define ED_TILE_H    28
#define ED_VIEW_ROWS 6   //  6 * 28 = 168 px
#define ED_STATUS_Y  170
#define ED_HELP_Y1   179
#define ED_HELP_Y2   188

// Left mini-map strip (map editor screen only): a slim vertical density map
// of the WHOLE active layer, pinned to x=0, so a level far taller than the
// ED_VIEW_ROWS-row viewport (up to 600 rows for map2/map3) still shows the
// player's place in it at a glance -- see draw_minimap(). Width includes its
// own 1px divider column (the strip's rightmost pixel, x = ED_MINIMAP_W - 1),
// mirroring draw_sidebar()'s divider further below. The viewport's tile
// columns are shifted right by this same amount (draw_map_viewport()'s `dx`).
#define ED_MINIMAP_W 12
#define ED_MINIMAP_H (ED_VIEW_ROWS * ED_TILE_H)  // 168 px, same height as the viewport

// Event guide colour (mini-map ticks in draw_minimap(), viewport row tabs
// in draw_map_viewport()): palette 5 (palettes[5], loaded via set_palette()
// in lvledit_run() -- see the comment above save_screenshot_bmp()) index
// 205 is a vivid green (~(32,230,72)), verified against a decoded
// palette.dat dump to read clearly against the density plot's grey (11),
// the viewport band's white (15), and the black (0) background. Green
// (not red -- red reads as an error) marks a neutral "here's an event".
#define ED_EVENT_COLOR 205

// Right-hand tile-slot sidebar geometry (draw_sidebar(), further below).
// Pulled up here because view_cols() needs ED_SIDEBAR_W -- the fixed width
// the sidebar consumes when open -- to size the viewport before
// draw_sidebar() itself ever runs.
#define ED_SIDEBAR_COL_W (ED_TILE_W + 2)  // 26
#define ED_SIDEBAR_COLS  2
#define ED_SIDEBAR_GAP   4  // px between the sidebar's divider column and its tile grid
#define ED_SIDEBAR_W     (ED_SIDEBAR_GAP + ED_SIDEBAR_COLS * ED_SIDEBAR_COL_W)  // 56

// One decoded 24x28 tile from shapes<c>.dat; `blank` mirrors the on-disk
// "shape blank" flag (JE_loadMap's shapeBlank), i.e. no pixel data at all.
typedef struct
{
	bool blank;
	Uint8 px[ED_TILE_W * ED_TILE_H];
} EditorTile;

static EditorTile tileset[600];
static bool tileset_loaded = false;
static char tileset_shape_char = 0;

static int cur_episode;
static char cur_lvl_filename[32];

static lvledit_level cur_level;
static int cur_level_index = -1;
static bool level_dirty = false;

// ---------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------
//
// Bounded history of whole-level snapshots. lvledit_level is ~52KB
// (mostly map1/map2/map3 + the event table), so ED_UNDO_CAP*2 static
// snapshots land around 6.6MB of BSS -- cheap enough next to export_rgb_buf
// (~18MB) below to just keep as plain static arrays rather than malloc/free
// them (no allocation-failure path to reason about, no leak to avoid).
#define ED_UNDO_CAP 64

static lvledit_level undo_stack[ED_UNDO_CAP];
static int undo_count = 0;
static lvledit_level redo_stack[ED_UNDO_CAP];
static int redo_count = 0;

// Clears both stacks. Call whenever a level is freshly parsed into
// cur_level -- there's nothing meaningful to undo/redo across a fresh load.
static void undo_reset(void)
{
	undo_count = 0;
	redo_count = 0;
}

// Pushes `level` onto `stack`/`*count` (capacity ED_UNDO_CAP), dropping the
// oldest entry (ring behavior) once full.
static void snapshot_push(lvledit_level *stack, int *count, const lvledit_level *level)
{
	if (*count < ED_UNDO_CAP)
	{
		stack[*count] = *level;
		++*count;
	}
	else
	{
		memmove(&stack[0], &stack[1], sizeof(lvledit_level) * (ED_UNDO_CAP - 1));
		stack[ED_UNDO_CAP - 1] = *level;
	}
}

// Records the CURRENT cur_level as an undo point (the "before" state) and
// clears the redo stack (a fresh edit invalidates any prior redo history).
// Call this immediately BEFORE applying any real mutation to cur_level.
static void undo_push(void)
{
	snapshot_push(undo_stack, &undo_count, &cur_level);
	redo_count = 0;
}

// Pops the most recent undo snapshot into cur_level, pushing the current
// state onto the redo stack first. Returns false (no-op) if there's nothing
// to undo.
static bool undo_apply(void)
{
	if (undo_count == 0)
		return false;

	snapshot_push(redo_stack, &redo_count, &cur_level);
	cur_level = undo_stack[--undo_count];
	return true;
}

// Symmetric with undo_apply(): pops the most recent redo snapshot into
// cur_level, pushing the current state onto the undo stack first. Returns
// false (no-op) if there's nothing to redo.
static bool redo_apply(void)
{
	if (redo_count == 0)
		return false;

	snapshot_push(undo_stack, &undo_count, &cur_level);
	cur_level = redo_stack[--redo_count];
	return true;
}

static int active_layer = 0;  // 0 = map1, 1 = map2, 2 = map3
static int cursor_x = 0, cursor_y = 0;
static int scroll_x = 0, scroll_y = 0;
static int brush_slot[3] = { 0, 0, 0 };

// Toggleable right-hand tile-slot sidebar (T key), see draw_sidebar()/
// view_cols() below. Persistent across frames but starts closed.
static bool sidebar_open = false;

// Row scroll for the sidebar's 2-column slot grid; auto-scrolled by
// draw_sidebar() to keep the active layer's current brush slot in view.
static int sidebar_scroll = 0;

// Cheap per-level summary (just enough to list levels without keeping every
// parsed record around at once).
typedef struct
{
	char mapFile, shapeFile;
} LevelSummary;

#define ED_MAX_LEVEL_SUMMARIES 256
static LevelSummary level_summaries[ED_MAX_LEVEL_SUMMARIES];
static int level_summary_count = 0;

// Remembers the last-selected ARCHIVE INDEX (a record's permanent identity,
// not a row number) across re-entries into run_level_select() within one
// editor session (e.g. after Esc-ing out of the map/event editor back to the
// level list, or after toggling sort mode), so the list doesn't keep jumping
// back to the top and a remembered selection survives a re-sort.
static int last_level_sel = 0;

// Level list can be displayed sorted by in-game play order (default) or by
// raw archive index. Either way the *archive index is always the record's
// real, permanent identity* -- sorting only changes which row of the list
// shows which record; it never reorders anything on disk.
typedef enum { SORT_PLAY_ORDER, SORT_ARCHIVE_INDEX } lvledit_sort_mode;
static lvledit_sort_mode sort_mode = SORT_PLAY_ORDER;

// display_order[row] = archive index shown at that row of the level list,
// per the current sort_mode. play_order[] is the SORT_PLAY_ORDER permutation
// computed by compute_play_order(); display_order[] is rebuilt from it (or
// from the identity permutation) by build_display_order() below.
static int play_order[ED_MAX_LEVEL_SUMMARIES];
static int display_order[ED_MAX_LEVEL_SUMMARIES];

// level_title[archive_index] = the level's in-game title, captured from the
// SAME ]L script line compute_play_order() already parses for play order
// (levelName = s+13, up to 9 chars, per tyrian2.c's `SDL_strlcpy(levelName,
// s + 13, 10)`), trimmed of the trailing space-padding the field is stored
// with. Keyed by archive index (not by row/sort), so it reads the same in
// both sort modes. FIRST script appearance wins, same rule as play_order.
// Left empty ("") for archive records the script never references -- shown
// as "(unnamed)" in the level list.
static char level_title[ED_MAX_LEVEL_SUMMARIES][12];

// ---------------------------------------------------------------------
// Event editor state (Phase E2)
// ---------------------------------------------------------------------

// ROWS_VISIBLE trimmed from 19 to 18 (freeing one ED_EVENT_ROW_H's worth of
// vertical space) so the help text -- which overflows 320px on one line --
// can be split across two rows without running off the bottom of the
// 320x200 surface; STATUS_Y/HELP_Y1/HELP_Y2 shifted up to match.
#define ED_EVENT_ROWS_VISIBLE 18
#define ED_EVENT_ROW_Y0       22
#define ED_EVENT_ROW_H        8
#define ED_EVENT_STATUS_Y     174
#define ED_EVENT_HELP_Y1      183
#define ED_EVENT_HELP_Y2      191

// Collapsed layout: when F1 hides the two help lines, the freed vertical space
// is handed back to the list instead of left blank -- ROWS_VISIBLE grows by 2
// and the status line drops toward where the help used to sit. Status sits at
// 191 (not lower) so a transient input prompt -- number entry / insert mode,
// always drawn at HELP_Y1 (183, spanning 183..190) -- never shares a pixel row
// with it; the 20th list row ends at y=181, so nothing overlaps.
#define ED_EVENT_ROWS_VISIBLE_COLLAPSED 20
#define ED_EVENT_STATUS_Y_COLLAPSED     191

static int event_rows_visible(bool show_help)
{
	return show_help ? ED_EVENT_ROWS_VISIBLE : ED_EVENT_ROWS_VISIBLE_COLLAPSED;
}
static int event_status_y(bool show_help)
{
	return show_help ? ED_EVENT_STATUS_Y : ED_EVENT_STATUS_Y_COLLAPSED;
}

static int event_sel = 0;
static int event_scroll = 0;

// Index into the SELECTED event's inspector field list (built fresh each
// time by build_inspector_fields(), just above) -- entry 0 is always TIME,
// entry 1 always TYPE, and entries 2.. are that event's schema fields (or
// the raw dat..dat6 fallback for a type the schema table doesn't cover).
// NOT a raw 0..7 field id anymore -- clamp_event_view() re-clamps this to
// the current event's list length every frame, since switching to a
// shorter-schema type (directly, or by editing TYPE) can shrink it out from
// under an in-range cursor.
static int event_field = 0;

// Whether the event editor's RIGHT inspector "sidecar" is shown. T toggles it
// (mirroring the map editor's own T tile-sidebar toggle); closing it hands the
// full screen width to the event list so the summaries read wider. Defaults
// open so the inspector is there the first time you enter the editor.
static bool event_sidebar_open = true;

static bool entering_number = false;
static char number_entry_buf[8];
static int number_entry_len = 0;

// Short (<=14 char) names for JE_eventSystem()'s eventtype switch
// (tyrian2.c, case labels ~4601-5449). Derived from the case bodies and
// Pascal-carried comments; any type not covered here (including gaps like
// 58/59 that the switch never handles) falls back to "?" -- purely
// cosmetic, does not affect editing or saving.
static const char *event_type_name(int type)
{
	static const char *names[] =
	{
		[0]  = "?",
		[1]  = "STARFIELD SPD",
		[2]  = "BG MOVE",
		[3]  = "BG MOVE RESET",
		[4]  = "STOP BG",
		[5]  = "ENEMY SHAPES",
		[6]  = "GROUND ENEMY",
		[7]  = "TOP ENEMY",
		[8]  = "STAR OFF",
		[9]  = "STAR ON",
		[10] = "GROUND ENEMY2",
		[11] = "END LEVEL",
		[12] = "GROUND 4X4",
		[13] = "ENEMIES OFF",
		[14] = "ENEMIES ON",
		[15] = "SKY ENEMY",
		[16] = "TEXT WINDOW",
		[17] = "GROUND BOTTOM",
		[18] = "SKY BOTTOM",
		[19] = "ENEMY GLB MOVE",
		[20] = "ENEMY GLB ACC",
		[21] = "BG3 OVER ON",
		[22] = "BG3 OVER OFF",
		[23] = "SKY BOTTOM2",
		[24] = "ENEMY GLB ANIM",
		[25] = "ENEMY GLB DMG",
		[26] = "SM ENEMY ADJ",
		[27] = "ENEMY GLB ACCR",
		[28] = "TOPENEMY OVR0",
		[29] = "TOPENEMY OVR1",
		[30] = "BG MOVE2",
		[31] = "ENEMY FIRE OVR",
		[32] = "CREATE ENEMY",
		[33] = "ENEMY DIE FROM",
		[34] = "MUSIC FADE",
		[35] = "PLAY SONG",
		[36] = "READY END LVL",
		[37] = "ENEMY FREQ",
		[38] = "JUMP TO TIME",
		[39] = "ENEMY LINK CHG",
		[40] = "ENEMY CONT DMG",
		[41] = "ENEMY AVAIL RST",
		[42] = "BG3 OVER 2",
		[43] = "BG2 OVER",
		[44] = "SCREEN FILTER",
		[45] = "ARCADE ENMY DIE",
		[46] = "DIFFICULTY CHG",
		[47] = "ENEMY GLB ARMOR",
		[48] = "BG2 OPAQUE",
		[49] = "GROUND ENEMY B",
		[50] = "SKY ENEMY B",
		[51] = "TOP ENEMY B",
		[52] = "GROUND2 ENMY B",
		[53] = "FORCE EVENTS",
		[54] = "EVENT JUMP",
		[55] = "ENEMY GLB ACC2",
		[56] = "GROUND2 BOTTOM",
		[57] = "ENEMY254 JUMP",
		[58] = "?",
		[59] = "?",
		[60] = "ENEMY SPECIAL",
		[61] = "FLAG SKIP",
		[62] = "PLAY SOUND",
		[63] = "SKIP NOT 2P",
		[64] = "SMOOTHIE",
		[65] = "BG3 X1",
		[66] = "SKIP DIFFCLTY",
		[67] = "LEVEL TIMER",
		[68] = "RANDOM EXPLODE",
		[69] = "PLAYER INVULN",
		[70] = "JUMP IF GONE",
		[71] = "JUMP MAPY POS",
		[72] = "BG3 X1B",
		[73] = "SKY OVER ALL",
		[74] = "ENEMY GLB BNCE",
		[75] = "RAND LINK PICK",
		[76] = "RETURN ACTIVE",
		[77] = "MAPY POS SET",
		[78] = "GALAGA FREQ+",
		[79] = "BOSS BAR LINK",
		[80] = "SKIP IF 2P",
		[81] = "WRAP2",
		[82] = "GIVE SPECIAL",
	};

	if (type < 0 || (size_t)type >= COUNTOF(names) || names[type] == NULL)
		return "?";

	return names[type];
}

// Field ranges, matching lvledit_event's on-disk types (lvledit_io.h).
// Moved up here (out of the "Event editor screen" section below, where
// these historically lived) because the schema/inspector machinery just
// below reuses them directly -- see the schema section's own comment for
// why it reuses rather than duplicates get/set/range/undo.
static void event_field_range(int field, long *out_min, long *out_max)
{
	switch (field)
	{
	case 0: *out_min = 0;      *out_max = 65535; break;  // time  (u16)
	case 1: *out_min = 0;      *out_max = 255;   break;  // type  (u8)
	case 2: *out_min = -32768; *out_max = 32767; break;  // dat   (s16)
	case 3: *out_min = -32768; *out_max = 32767; break;  // dat2  (s16)
	case 4: *out_min = -128;   *out_max = 127;   break;  // dat3  (s8)
	case 5: *out_min = 0;      *out_max = 255;   break;  // dat4  (u8)
	case 6: *out_min = -128;   *out_max = 127;   break;  // dat5  (s8)
	default:
	case 7: *out_min = -128;   *out_max = 127;   break;  // dat6  (s8)
	}
}

static long event_field_get(const lvledit_event *ev, int field)
{
	switch (field)
	{
	case 0:  return ev->time;
	case 1:  return ev->type;
	case 2:  return ev->dat;
	case 3:  return ev->dat2;
	case 4:  return ev->dat3;
	case 5:  return ev->dat4;
	case 6:  return ev->dat5;
	default:
	case 7:  return ev->dat6;
	}
}

static void event_field_set(lvledit_event *ev, int field, long v)
{
	long lo, hi;
	event_field_range(field, &lo, &hi);
	if (v < lo) v = lo;
	if (v > hi) v = hi;

	switch (field)
	{
	case 0: ev->time = (Uint16)v; break;
	case 1: ev->type = (Uint8)v;  break;
	case 2: ev->dat  = (Sint16)v; break;
	case 3: ev->dat2 = (Sint16)v; break;
	case 4: ev->dat3 = (Sint8)v;  break;
	case 5: ev->dat4 = (Uint8)v;  break;
	case 6: ev->dat5 = (Sint8)v;  break;
	case 7: ev->dat6 = (Sint8)v;  break;
	}
}

// ---------------------------------------------------------------------
// Event field schema (Phase E2.1: semantic event editor)
// ---------------------------------------------------------------------
//
// The raw event record is 8 generic fields (time, type, dat..dat6 -- see
// event_field_get/set/range() just above); on its own "dat3=-1"
// means nothing to a human. This table maps each event TYPE to the subset
// of dat fields JE_eventSystem() (tyrian2.c, cases ~4599-5457) actually
// reads for that type, with a short human label and (for the handful of
// true booleans/small enums) a name for each value.
//
// This is purely a display/edit convenience layered on top of the existing
// event_field_get/set/range() -- every field it exposes is still read and
// written through those exact functions, so it can never change what ends
// up on disk (save_current_level()/lvledit_io.c are untouched). If a
// mapping below ever looks wrong, JE_eventSystem()'s switch is the ground
// truth, not this table.

// EF_ENUM is used sparingly, for the clear small enums (type 4's layer
// choice, type 11's end mode, type 12's spawn layer, and the plain on/off
// toggles) -- everywhere else a field just reads as a signed/unsigned
// number (EF_INT), which is the honest thing to show when the reference
// doesn't name a closed set of values.
typedef enum { EF_INT, EF_ENUM } ef_kind;

// One named value of an EF_ENUM field, e.g. { 1, "Force End" }.
typedef struct
{
	long        value;
	const char *name;
} ef_enumval;

// One field a given event TYPE exposes in the inspector. `field_id` is one
// of event_field_get/set/range()'s own ids (2=dat .. 7=dat6 -- mind the
// disk-order quirk noted on those functions below), so editing a schema
// field always goes through the existing, already-clamped get/set/range/
// undo machinery; this struct only adds a label and (optionally) an enum
// decoding on top of that.
typedef struct
{
	int               field_id;
	const char       *label;      // <=10 chars: the sidebar is ~104px wide
	ef_kind           kind;
	const ef_enumval *enums;      // NULL unless kind==EF_ENUM
	int               enum_count;
} ef_field;

// The set of fields (in display order) a given TYPE uses, or {NULL,0} for a
// type that genuinely takes no dat fields at all (e.g. "BG MOVE RESET").
// NOT the same thing as a type this table doesn't cover -- see
// event_schema_for()'s NULL return, which raw-fallback handles separately.
typedef struct
{
	const ef_field *fields;
	int              field_count;
} ef_schema;

// Shorthand for the two kinds of table entry above.
#define EF(id, label)          { (id), (label), EF_INT,  NULL, 0 }
#define EFE(id, label, enumar) { (id), (label), EF_ENUM, (enumar), (int)COUNTOF(enumar) }

// --- small enums (see the spec list: types 4, 11, 12, and the plain on/off
// toggles 53, 60(dat2), 65, 67, 68, 72, 73) --------------------------------

// Type 4 (STOP BG): dat selects which background layer keeps scrolling; 0
// and 1 both mean "layer 1" (JE_eventSystem() doesn't distinguish them).
static const ef_enumval en_stopbg_layer[] =
{
	{ 0, "Layer 1" }, { 1, "Layer 1" }, { 2, "Layer 2" }, { 3, "Layer 3" },
};

// Type 11 (END LEVEL): dat==1 forces an immediate end; anything else starts
// the normal end sequence. Only 1 and 0 are named; any other stored value
// (never written by this editor, but tolerated on read) just falls back to
// showing the raw number -- see ef_enum_name()'s "no match" case.
static const ef_enumval en_endmode[] =
{
	{ 1, "Force End" }, { 0, "Normal" },
};

// Type 12 (GROUND 4X4): dat6 picks the spawn layer/offset, then
// JE_eventSystem() zeroes dat6 back to 0 right after spawning -- so from
// the editor's POV this is a "which layer" choice made at edit time, same
// as the enemyOffset the plain spawn types (6/7/10/15/etc.) bake in as a
// literal 0/25/50/75 rather than a stored field.
static const ef_enumval en_spawnlayer[] =
{
	{ 0, "Ground" }, { 1, "Ground" }, { 2, "Sky" }, { 3, "Top" }, { 4, "Ground2" },
};

// The plain on/off toggle types differ only in which raw value means "on"
// vs "off"; three small tables cover every one of them.
static const ef_enumval en_onoff_1on[] = { { 1, "On" }, { 0, "Off" } };   // types 60(dat2),67,68,72,73
static const ef_enumval en_onoff_0on[] = { { 0, "On" }, { 1, "Off" } };   // type 65
static const ef_enumval en_off99[]     = { { 99, "Off" }, { 0, "On" } };  // type 53 (99 is the off sentinel)

// --- field sets shared verbatim across several types ----------------------

// Types 6,7,10,15 (Ground/Top/Ground2/Sky enemy spawn) and their
// "...Bottom" variants 17,18,23, which only change how dat5 combines with a
// fixed Y base inside JE_createNewEventEnemy() -- the same six fields
// either way, per JE_createNewEventEnemy()'s dat->dat6 reading (see the
// reference doc's header note).
static const ef_field fl_enemy_spawn[] =
{
	EF(2, "Enemy #"), EF(3, "X pos"), EF(4, "Y vel"),
	EF(5, "Link"),    EF(6, "Y off"), EF(7, "Move"),
};

// Types 32,56: forced-bottom spawn variants where JE_eventSystem() sets ey
// to a fixed constant, so dat5 (Y off) is read but never applied -- omitted
// here rather than shown as a field that visibly does nothing.
static const ef_field fl_enemy_spawn_noyoff[] =
{
	EF(2, "Enemy #"), EF(3, "X pos"), EF(4, "Y vel"),
	EF(5, "Link"),    EF(7, "Move"),
};

// Types 49-52 (Custom Ground/Sky/Top/Ground2 enemy): a temporary-override
// spawn that restores dat/dat3/dat6 afterward, but from the editor's point
// of view it's just these six fields.
static const ef_field fl_enemy_custom[] =
{
	EF(2, "Graphic"), EF(3, "X pos"), EF(4, "Shape Idx"),
	EF(5, "Link"),    EF(6, "Y off"), EF(7, "Armor"),
};

// Types 2,30: identical background-speed triple (30 is the same effect
// without type 2's extra guard/reset-delay logic).
static const ef_field fl_bg_speed[] =
{
	EF(2, "BG1 Spd"), EF(3, "BG2 Spd"), EF(4, "BG3 Spd"),
};

// Types 33,45 (Enemy From Other Enemies; 45 is the arcade-only variant).
static const ef_field fl_enemy_diefrom[] =
{
	EF(2, "Spawn Die"), EF(5, "Link"),
};

// Types 25,47 (Enemy Global Damage set: difficulty-scaled vs. direct).
static const ef_field fl_enemy_armor[] =
{
	EF(2, "Armor"), EF(5, "Link"),
};

// --- one-off field sets, one per type --------------------------------------

static const ef_field fl_1[]  = { EF(2, "Speed") };                                                                    // STARFIELD SPD
static const ef_field fl_4[]  = { EFE(2, "Layer", en_stopbg_layer) };                                                  // STOP BG
static const ef_field fl_5[]  = { EF(2, "Bank 1"), EF(3, "Bank 2"), EF(4, "Bank 3"), EF(5, "Bank 4") };                // ENEMY SHAPES
static const ef_field fl_11[] = { EFE(2, "End Mode", en_endmode) };                                                    // END LEVEL
static const ef_field fl_12[] = { EF(2, "Enemy #"), EF(3, "X pos"), EF(4, "Y vel"), EF(5, "Link"), EF(6, "Y off"), EFE(7, "Layer", en_spawnlayer) }; // GROUND 4X4
static const ef_field fl_16[] = { EF(2, "Window") };                                                                   // TEXT WINDOW
static const ef_field fl_19[] = { EF(2, "X vel"), EF(3, "Y vel"), EF(4, "Scope"), EF(5, "Link"), EF(6, "Cycle"), EF(7, "Move") };  // ENEMY GLB MOVE
static const ef_field fl_20[] = { EF(2, "X acc"), EF(3, "Y acc"), EF(4, "Scope"), EF(5, "Link"), EF(6, "Cycle"), EF(7, "Anim") };  // ENEMY GLB ACC
static const ef_field fl_24[] = { EF(2, "Anim"), EF(3, "Cycle"), EF(4, "Mode"), EF(5, "Link") };                       // ENEMY GLB ANIM
static const ef_field fl_26[] = { EF(2, "Adjust") };                                                                   // SM ENEMY ADJ
static const ef_field fl_27[] = { EF(2, "X rev"), EF(3, "Y rev"), EF(4, "Scope"), EF(5, "Link") };                     // ENEMY GLB ACCR
static const ef_field fl_31[] = { EF(2, "Freq 1"), EF(3, "Freq 2"), EF(4, "Freq 3"), EF(5, "Link"), EF(6, "LnchFreq") }; // ENEMY FIRE OVR
static const ef_field fl_35[] = { EF(2, "Song") };                                                                     // PLAY SONG
static const ef_field fl_37[] = { EF(2, "Freq") };                                                                     // ENEMY FREQ
static const ef_field fl_38[] = { EF(2, "Tgt Time") };                                                                 // JUMP TO TIME
static const ef_field fl_39[] = { EF(2, "Old Link"), EF(3, "New Link") };                                              // ENEMY LINK CHG
static const ef_field fl_41[] = { EF(2, "Scope") };                                                                    // ENEMY AVAIL RST
static const ef_field fl_43[] = { EF(2, "BG2 Over") };                                                                 // BG2 OVER
static const ef_field fl_44[] = { EF(2, "Filter"), EF(3, "LvlFilter"), EF(4, "Bright"), EF(5, "TgtFilt"), EF(6, "BrtStep"), EF(7, "FadeStart") }; // SCREEN FILTER
static const ef_field fl_46[] = { EF(2, "DiffDelta"), EF(3, "Gate"), EF(4, "DmgRate") };                               // DIFFICULTY CHG
static const ef_field fl_53[] = { EFE(2, "State", en_off99) };                                                         // FORCE EVENTS
static const ef_field fl_54[] = { EF(2, "Jump To") };                                                                  // EVENT JUMP
static const ef_field fl_55[] = { EF(2, "X Accel"), EF(3, "Y Accel"), EF(4, "Scope"), EF(5, "Link") };                 // ENEMY GLB ACC2
static const ef_field fl_57[] = { EF(2, "Value") };                                                                    // ENEMY254 JUMP
static const ef_field fl_60[] = { EF(2, "Flag #"), EFE(3, "Value", en_onoff_1on), EF(5, "Link") };                     // ENEMY SPECIAL
static const ef_field fl_61[] = { EF(2, "Flag #"), EF(3, "Compare"), EF(4, "Skip") };                                  // FLAG SKIP
static const ef_field fl_62[] = { EF(2, "Sound") };                                                                    // PLAY SOUND
static const ef_field fl_63[] = { EF(2, "Skip") };                                                                     // SKIP NOT 2P
static const ef_field fl_64[] = { EF(2, "Index"), EF(3, "Value"), EF(4, "Extra") };                                    // SMOOTHIE
static const ef_field fl_65[] = { EFE(2, "State", en_onoff_0on) };                                                     // BG3 X1
static const ef_field fl_66[] = { EF(2, "Diff Thr"), EF(3, "Skip") };                                                  // SKIP DIFFCLTY
static const ef_field fl_67[] = { EFE(2, "Timer", en_onoff_1on), EF(3, "Jump To"), EF(4, "Ticks") };                   // LEVEL TIMER
static const ef_field fl_68[] = { EFE(2, "State", en_onoff_1on) };                                                     // RANDOM EXPLODE
static const ef_field fl_69[] = { EF(2, "Ticks") };                                                                    // PLAYER INVULN
static const ef_field fl_70[] = { EF(2, "Jump To"), EF(3, "Mode"), EF(4, "Type A"), EF(5, "Type B") };                 // JUMP IF GONE
static const ef_field fl_71[] = { EF(2, "Jump To"), EF(3, "Map Y") };                                                  // JUMP MAPY POS
static const ef_field fl_72[] = { EFE(2, "State", en_onoff_1on) };                                                     // BG3 X1B
static const ef_field fl_73[] = { EFE(2, "State", en_onoff_1on) };                                                     // SKY OVER ALL
static const ef_field fl_74[] = { EF(2, "X Max"), EF(3, "Y Max"), EF(5, "Link"), EF(6, "X Min"), EF(7, "Y Min") };     // ENEMY GLB BNCE
static const ef_field fl_75[] = { EF(2, "Link Lo"), EF(3, "Link Hi"), EF(4, "Var Slot"), EF(5, "Skip") };              // RAND LINK PICK
static const ef_field fl_77[] = { EF(2, "BG1 Off"), EF(3, "BG2 Off") };                                                // MAPY POS SET
static const ef_field fl_79[] = { EF(2, "Bar0 Link"), EF(3, "Bar1 Link") };                                            // BOSS BAR LINK
static const ef_field fl_80[] = { EF(2, "Skip") };                                                                     // SKIP IF 2P
static const ef_field fl_81[] = { EF(2, "Wrap Start"), EF(3, "Wrap End") };                                            // WRAP2
static const ef_field fl_82[] = { EF(2, "Weapon") };                                                                   // GIVE SPECIAL

// Generic raw fallback: dat..dat6 as plain "dat".."dat6" rows. Used for any
// type event_schema_for() returns NULL for (currently just 58 and 59, which
// JE_eventSystem()'s switch never handles at all -- see the reference doc's
// closing note) -- this guarantees every field of every event stays
// reachable even for a type this table doesn't know the meaning of.
static const ef_field fl_raw_fallback[] =
{
	EF(2, "dat"), EF(3, "dat2"), EF(4, "dat3"), EF(5, "dat4"), EF(6, "dat5"), EF(7, "dat6"),
};

// Maps an event TYPE to its ef_schema, or NULL if this table doesn't cover
// the type at all (event_summary()/build_inspector_fields() then fall back
// to fl_raw_fallback above). A schema with field_count==0 is a *confirmed*
// "no fields" type (JE_eventSystem() reads none of dat..dat6 for it) --
// deliberately distinct from the NULL/unmapped case.
//
// A lookup function rather than a plain indexed table (the two alternatives
// the spec allows) because C's array designated-initializer defaults would
// otherwise make "not covered" indistinguishable from "confirmed 0 fields"
// (both zero-initialize to {NULL,0}); a switch keeps that distinction
// explicit at each case.
static const ef_schema *event_schema_for(int type)
{
	static const ef_schema empty = { NULL, 0 };

#define SCHEMA(arr) do { static const ef_schema s = { (arr), (int)COUNTOF(arr) }; return &s; } while (0)

	switch (type)
	{
	case 1:  SCHEMA(fl_1);
	case 2:  SCHEMA(fl_bg_speed);
	case 3:  return &empty;
	case 4:  SCHEMA(fl_4);
	case 5:  SCHEMA(fl_5);
	case 6:  SCHEMA(fl_enemy_spawn);
	case 7:  SCHEMA(fl_enemy_spawn);
	case 8:  return &empty;
	case 9:  return &empty;
	case 10: SCHEMA(fl_enemy_spawn);
	case 11: SCHEMA(fl_11);
	case 12: SCHEMA(fl_12);
	case 13: return &empty;
	case 14: return &empty;
	case 15: SCHEMA(fl_enemy_spawn);
	case 16: SCHEMA(fl_16);
	case 17: SCHEMA(fl_enemy_spawn);
	case 18: SCHEMA(fl_enemy_spawn);
	case 19: SCHEMA(fl_19);
	case 20: SCHEMA(fl_20);
	case 21: return &empty;
	case 22: return &empty;
	case 23: SCHEMA(fl_enemy_spawn);
	case 24: SCHEMA(fl_24);
	case 25: SCHEMA(fl_enemy_armor);
	case 26: SCHEMA(fl_26);
	case 27: SCHEMA(fl_27);
	case 28: return &empty;
	case 29: return &empty;
	case 30: SCHEMA(fl_bg_speed);
	case 31: SCHEMA(fl_31);
	case 32: SCHEMA(fl_enemy_spawn_noyoff);
	case 33: SCHEMA(fl_enemy_diefrom);
	case 34: return &empty;
	case 35: SCHEMA(fl_35);
	case 36: return &empty;
	case 37: SCHEMA(fl_37);
	case 38: SCHEMA(fl_38);
	case 39: SCHEMA(fl_39);
	case 40: return &empty;
	case 41: SCHEMA(fl_41);
	case 42: return &empty;
	case 43: SCHEMA(fl_43);
	case 44: SCHEMA(fl_44);
	case 45: SCHEMA(fl_enemy_diefrom);
	case 46: SCHEMA(fl_46);
	case 47: SCHEMA(fl_enemy_armor);
	case 48: return &empty;
	case 49: SCHEMA(fl_enemy_custom);
	case 50: SCHEMA(fl_enemy_custom);
	case 51: SCHEMA(fl_enemy_custom);
	case 52: SCHEMA(fl_enemy_custom);
	case 53: SCHEMA(fl_53);
	case 54: SCHEMA(fl_54);
	case 55: SCHEMA(fl_55);
	case 56: SCHEMA(fl_enemy_spawn_noyoff);
	case 57: SCHEMA(fl_57);
	// 58,59: genuinely not handled by JE_eventSystem()'s switch -- fall
	// through to the raw dat..dat6 fallback (default, below).
	case 60: SCHEMA(fl_60);
	case 61: SCHEMA(fl_61);
	case 62: SCHEMA(fl_62);
	case 63: SCHEMA(fl_63);
	case 64: SCHEMA(fl_64);
	case 65: SCHEMA(fl_65);
	case 66: SCHEMA(fl_66);
	case 67: SCHEMA(fl_67);
	case 68: SCHEMA(fl_68);
	case 69: SCHEMA(fl_69);
	case 70: SCHEMA(fl_70);
	case 71: SCHEMA(fl_71);
	case 72: SCHEMA(fl_72);
	case 73: SCHEMA(fl_73);
	case 74: SCHEMA(fl_74);
	case 75: SCHEMA(fl_75);
	case 76: return &empty;
	case 77: SCHEMA(fl_77);
	case 78: return &empty;
	case 79: SCHEMA(fl_79);
	case 80: SCHEMA(fl_80);
	case 81: SCHEMA(fl_81);
	case 82: SCHEMA(fl_82);
	default: return NULL;  // unmapped (58, 59, 0, or >82) -> raw fallback
	}

#undef SCHEMA
}

// Returns the enum member name matching `v`, or NULL if none match (the
// caller then falls back to printing the raw number -- see ef_format_value).
static const char *ef_enum_name(const ef_field *f, long v)
{
	for (int i = 0; i < f->enum_count; ++i)
		if (f->enums[i].value == v)
			return f->enums[i].name;

	return NULL;
}

// Formats field `f`'s current value on `ev`: the decoded enum name if it's
// an EF_ENUM field and the stored value matches one of its entries,
// otherwise the plain signed decimal (which also covers every EF_INT field,
// and any EF_ENUM value the enum table doesn't happen to name).
static void ef_format_value(const lvledit_event *ev, const ef_field *f, char *buf, size_t buf_sz)
{
	long v = event_field_get(ev, f->field_id);

	if (f->kind == EF_ENUM)
	{
		const char *name = ef_enum_name(f, v);
		if (name != NULL)
		{
			snprintf(buf, buf_sz, "%s", name);
			return;
		}
	}

	snprintf(buf, buf_sz, "%ld", v);
}

// TIME and TYPE are always the first two inspector rows, ahead of whatever
// the schema (or raw fallback) contributes -- editing TYPE changes which
// schema applies, so the rest of the list is rebuilt fresh every call
// rather than cached.
#define ED_EVENT_MAX_INSPECTOR_FIELDS (2 + 6)  // TIME, TYPE, + up to 6 dat fields

// Builds the ordered field list the inspector sidebar shows/edits for `ev`
// into `out` (caller-owned, must hold at least ED_EVENT_MAX_INSPECTOR_
// FIELDS entries) and returns how many entries it wrote. Always >=2: TIME
// and TYPE are unconditional, followed by either ev->type's schema fields
// or (event_schema_for() returned NULL) the raw dat..dat6 fallback -- see
// event_schema_for()'s own comment for what "returned NULL" means here.
static int build_inspector_fields(const lvledit_event *ev, ef_field out[ED_EVENT_MAX_INSPECTOR_FIELDS])
{
	static const ef_field f_time = EF(0, "Time");
	static const ef_field f_type = EF(1, "Type");

	int n = 0;
	out[n++] = f_time;
	out[n++] = f_type;

	const ef_schema *schema = event_schema_for(ev->type);

	if (schema != NULL)
	{
		for (int i = 0; i < schema->field_count; ++i)
			out[n++] = schema->fields[i];
	}
	else
	{
		for (size_t i = 0; i < COUNTOF(fl_raw_fallback); ++i)
			out[n++] = fl_raw_fallback[i];
	}

	return n;
}

// Plain-language one-line summary for the LEFT list: "<type name>" alone
// for a confirmed-empty-schema type, or "<type name>: <label> <val>,
// <label> <val>, ..." over the type's schema fields (a type this table
// doesn't cover gets "<type name>: dat <val>, dat2 <val>, ..." via the raw
// fallback, same as the inspector). Truncated (no ellipsis -- matches
// fit_name()'s reasoning, TINY_FONT has no glyph reserved for one) to fit
// `max_w` pixels.
static void event_summary(const lvledit_event *ev, char *buf, size_t buf_sz, int max_w)
{
	snprintf(buf, buf_sz, "%s", event_type_name(ev->type));

	const ef_schema *schema = event_schema_for(ev->type);

	const ef_field *fields = fl_raw_fallback;
	int field_count = (int)COUNTOF(fl_raw_fallback);

	if (schema != NULL)
	{
		fields = schema->fields;
		field_count = schema->field_count;
	}

	for (int i = 0; i < field_count; ++i)
	{
		char val[16];
		ef_format_value(ev, &fields[i], val, sizeof(val));

		char piece[32];
		snprintf(piece, sizeof(piece), "%s %s %s", i == 0 ? ":" : ",", fields[i].label, val);

		size_t len = strlen(buf);
		snprintf(buf + len, buf_sz - len, "%s", piece);
	}

	while (strlen(buf) > 0 && JE_textWidth(buf, TINY_FONT) > max_w)
		buf[strlen(buf) - 1] = '\0';
}

// ---------------------------------------------------------------------
// Tileset loading (shapes<c>.dat)
// ---------------------------------------------------------------------

static void load_tileset(char shapeFile)
{
	char c = (char)tolower((unsigned char)shapeFile);

	if (tileset_loaded && tileset_shape_char == c)
		return;

	char fname[32];
	snprintf(fname, sizeof(fname), "shapes%c.dat", c);

	FILE *f = dir_fopen_die(data_dir(), fname, "rb");

	for (int z = 0; z < 600; ++z)
	{
		bool blank;
		fread_bool_die(&blank, f);

		tileset[z].blank = blank;

		if (blank)
			memset(tileset[z].px, 0, sizeof(tileset[z].px));
		else
			fread_u8_die(tileset[z].px, sizeof(tileset[z].px), f);
	}

	fclose(f);

	tileset_loaded = true;
	tileset_shape_char = c;
}

// ---------------------------------------------------------------------
// Layer/cell helpers
// ---------------------------------------------------------------------

static int layer_width(int layer)
{
	return layer == 2 ? 15 : 14;
}

static int layer_height(int layer)
{
	return layer == 0 ? 300 : 600;
}

static Uint8 get_cell(int layer, int y, int x)
{
	switch (layer)
	{
	case 0:  return cur_level.map1[y][x];
	case 1:  return cur_level.map2[y][x];
	default: return cur_level.map3[y][x];
	}
}

static void set_cell(int layer, int y, int x, Uint8 v)
{
	switch (layer)
	{
	case 0:  cur_level.map1[y][x] = v; break;
	case 1:  cur_level.map2[y][x] = v; break;
	default: cur_level.map3[y][x] = v; break;
	}
}

// True iff `slot` is a real, renderable slot for `layer`. Mirrors the
// loader's quirks (tyrian2.c ~3350-3390): layer 2 (map2) slot 71 and layer 3
// (map3) slots >= 70 are always forced NULL, regardless of mapSh content.
static bool slot_usable(int layer, int slot)
{
	if (slot < 0 || slot > 71)
		return false;
	if (layer == 1 && slot == 71)
		return false;
	if (layer == 2 && slot >= 70)
		return false;
	return true;
}

// Resolves a layer+slot pair to the tile it draws as, or NULL if the cell is
// empty/unused (unassigned mapSh entry, blank shape, or an unusable slot).
static const EditorTile *resolve_slot_tile(int layer, int slot)
{
	if (!slot_usable(layer, slot))
		return NULL;

	Uint16 z = cur_level.mapSh[layer][slot];
	if (z == 0 || z > 600)
		return NULL;

	const EditorTile *tile = &tileset[z - 1];
	if (tile->blank)
		return NULL;

	return tile;
}

// ---------------------------------------------------------------------
// Event time <-> map row (nominal/default-scroll mapping)
// ---------------------------------------------------------------------
//
// An event's `time` field is the curLoc (scroll position) at which it
// fires. At level start curLoc == 0 and mapY == 292 (300 - 8, tyrian2.c:897);
// as the level scrolls, curLoc += backMove and backPos += backMove in
// lockstep, and backPos wraps every ED_TILE_H (28) units -> one map-1
// (layer-0) row. So, nominally:
//
//     map1_row(time) = ED_EVENT_ROW0 - time/28
//     time(map1_row) = (ED_EVENT_ROW0 - map1_row) * 28
//
// This is the nominal/default-scroll mapping -- speed changes and
// force-events (e.g. JE_eventSystem's dat3 == -99 handling) can shift an
// event's effective firing row at runtime. It's good enough for an editor
// guide, not a simulation of variable scroll; don't treat it as exact.
//
// Events live in layer-0 row space. To place an event on any active layer
// (map2/map3 are twice as tall as map1), go through the level-progress
// FRACTION so the mapping is layer-agnostic:
//
//     frac(event)        = map1_row(event.time) / 300.0   // 0=top .. ~1=bottom
//     active_row(event)  = round(frac(event) * layer_height(active_layer))
//
// All of this stays in array-row order (row 0 at the TOP, the same axis
// scroll_y/cursor_y and draw_minimap()/draw_map_viewport() already use),
// NOT row_progress()'s game-start-at-the-bottom reframing further below --
// nothing here needs flipping.
#define ED_EVENT_ROW0 (300 - 8)  // mapY at level start, tyrian2.c:897

// Layer-0 (map1) array row an event's time nominally pins to, clamped to
// the valid [0, 299] row range.
static int event_map1_row(const lvledit_event *ev)
{
	long row = ED_EVENT_ROW0 - (long)ev->time / ED_TILE_H;
	if (row < 0) row = 0;
	if (row > 299) row = 299;
	return (int)row;
}

// Same event, reframed as an array row of the CURRENTLY ACTIVE layer
// (which may be map2/map3, twice as tall as map1) via the layer-agnostic
// level-progress fraction. Rounds rather than truncates.
static int event_active_row(const lvledit_event *ev)
{
	int row1 = event_map1_row(ev);
	int h = layer_height(active_layer);
	int row = (int)(((long)row1 * h + 150) / 300);
	if (row < 0) row = 0;
	if (row >= h) row = h - 1;
	return row;
}

// Inverse of event_active_row(): the event `time` a given active-layer
// cursor row nominally corresponds to. Used by the E-key jump-to-nearest
// feature (run_event_editor()) to find the event closest to the map
// cursor. Clamped to >= 0.
static long row_target_time(int active_row)
{
	int h = layer_height(active_layer);
	int row1 = (int)(((long)active_row * 300 + h / 2) / h);
	if (row1 < 0) row1 = 0;
	if (row1 > 299) row1 = 299;

	long time = (long)(ED_EVENT_ROW0 - row1) * ED_TILE_H;
	if (time < 0) time = 0;
	return time;
}

// ---------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------

// Pixel layout verified against the game's own tile blitter,
// backgrnd.c:blit_background_row(): for each of the 28 rows it does
// `data += y * 24` then walks 24 consecutive bytes -- i.e. row-major,
// 24 bytes per row, 28 rows, no column-major or interleaved packing. The
// EditorTile buffer here uses the same row-major indexing (`ty * ED_TILE_W
// + tx`), so this already matches the real in-game layout; no fix needed.
static void blit_tile(SDL_Surface *screen, const EditorTile *tile, int dst_x, int dst_y, bool opaque)
{
	if (tile == NULL)
		return;

	for (int ty = 0; ty < ED_TILE_H; ++ty)
	{
		for (int tx = 0; tx < ED_TILE_W; ++tx)
		{
			Uint8 c = tile->px[ty * ED_TILE_W + tx];

			// Layers 2/3 treat pixel value 0 as transparent; layer 1 is an
			// opaque background and paints every pixel, including 0 (black).
			if (c == 0 && !opaque)
				continue;

			JE_pix(screen, dst_x + tx, dst_y + ty, c);
		}
	}
}

// Empty/transparent-cell placeholder: a plain two-tone checkerboard so empty
// slots read as "nothing here" rather than looking like a rendering bug.
static void draw_checker_cell(SDL_Surface *screen, int dst_x, int dst_y)
{
	for (int ty = 0; ty < ED_TILE_H; ++ty)
	{
		for (int tx = 0; tx < ED_TILE_W; ++tx)
		{
			bool even = ((tx / 4) + (ty / 4)) % 2 == 0;
			JE_pix(screen, dst_x + tx, dst_y + ty, even ? 0 : 1);
		}
	}
}

// Map viewport width in tile columns: the 320px-wide screen (vga_width)
// minus the fixed left mini-map strip (ED_MINIMAP_W, always present on this
// screen) and, when the tile sidebar (T key) is open, its fixed ED_SIDEBAR_W
// on the right; full width (minus just the mini-map) when the sidebar is
// closed. Floor division -- any remainder is unused blank margin between the
// viewport and whatever is to its right, never a partial/clipped column.
static int view_cols(void)
{
	int avail = vga_width - ED_MINIMAP_W - (sidebar_open ? ED_SIDEBAR_W : 0);
	return avail / ED_TILE_W;
}

static void clamp_view(void)
{
	int w = layer_width(active_layer);
	int h = layer_height(active_layer);
	int cols = view_cols();

	if (cursor_x < 0) cursor_x = 0;
	if (cursor_x >= w) cursor_x = w - 1;
	if (cursor_y < 0) cursor_y = 0;
	if (cursor_y >= h) cursor_y = h - 1;

	int max_scroll_x = w > cols ? w - cols : 0;
	int max_scroll_y = h > ED_VIEW_ROWS ? h - ED_VIEW_ROWS : 0;

	if (scroll_x > max_scroll_x) scroll_x = max_scroll_x;
	if (scroll_y > max_scroll_y) scroll_y = max_scroll_y;
	if (scroll_x < 0) scroll_x = 0;
	if (scroll_y < 0) scroll_y = 0;

	if (cursor_x < scroll_x) scroll_x = cursor_x;
	if (cursor_x >= scroll_x + cols) scroll_x = cursor_x - cols + 1;
	if (cursor_y < scroll_y) scroll_y = cursor_y;
	if (cursor_y >= scroll_y + ED_VIEW_ROWS) scroll_y = cursor_y - ED_VIEW_ROWS + 1;
}

// ---------------------------------------------------------------------
// Left mini-map / scroll indicator (map editor screen only)
// ---------------------------------------------------------------------

// True iff any column of map row `y` on `layer` is non-empty. Collapses
// draw_map_viewport()'s tri-state fill logic (real tile / layer-1
// assigned-but-blank solid black / true empty checkerboard) into a single
// bool for the mini-map's density plot -- a row counts as "filled" under
// either of the first two cases.
static bool row_has_content(int layer, int y)
{
	int w = layer_width(layer);

	for (int x = 0; x < w; ++x)
	{
		Uint8 slot = get_cell(layer, y, x);

		if (resolve_slot_tile(layer, slot) != NULL)
			return true;

		// Layer 1 (map1) assigned-but-blank slots still paint solid black
		// in-game -- see draw_map_viewport()'s comment on the same check.
		if (layer == 0 && slot_usable(0, slot) && cur_level.mapSh[0][slot] != 0)
			return true;
	}

	return false;
}

// Plots the WHOLE active layer's height (up to 600 rows for map2/map3) into
// the fixed ED_MINIMAP_H=168px-tall strip pinned at the left edge, then
// overlays the on-screen viewport slice and the cursor's row so the current
// position is always visible at a glance, however tall the level is.
//
// Orientation: strip row 0 is array row 0, the SAME axis draw_map_viewport()
// and scroll_y already use -- not row_progress()'s game-start-at-the-bottom
// reframing. That keeps the viewport band below a direct, unflipped scale of
// scroll_y (and matches the viewport itself, whose own row 0 is at the top
// of the screen), so there's nothing to invert here.
static void draw_minimap(void)
{
	int h = layer_height(active_layer);

	for (int sy = 0; sy < ED_MINIMAP_H; ++sy)
	{
		int row0 = sy * h / ED_MINIMAP_H;
		int row1 = (sy + 1) * h / ED_MINIMAP_H;
		if (row1 <= row0) row1 = row0 + 1;
		if (row1 > h) row1 = h;

		bool filled = false;
		for (int y = row0; y < row1 && !filled; ++y)
			filled = row_has_content(active_layer, y);

		// Filled rows use a light grey (palette-5 index 11, ~162) so the
		// density plot reads clearly against the black (0) background and
		// stays distinct from the near-white (15) viewport band drawn over
		// it below; the divider stays at the dimmer 8 like draw_sidebar()'s.
		Uint8 color = filled ? 11 : 0;
		for (int x = 0; x < ED_MINIMAP_W - 1; ++x)
			JE_pix(VGAScreen, x, sy, color);
	}

	// Divider, mirroring draw_sidebar()'s.
	fill_rectangle_xy(VGAScreen, ED_MINIMAP_W - 1, 0, ED_MINIMAP_W - 1, ED_MINIMAP_H - 1, 8);

	// Viewport band: the slice of the level currently on screen.
	int band_top = scroll_y * ED_MINIMAP_H / h;
	int band_bot = (scroll_y + ED_VIEW_ROWS) * ED_MINIMAP_H / h;
	if (band_bot <= band_top) band_bot = band_top + 1;
	if (band_bot > ED_MINIMAP_H) band_bot = ED_MINIMAP_H;

	for (int sy = band_top; sy < band_bot; ++sy)
		for (int x = 0; x < ED_MINIMAP_W - 1; ++x)
			JE_pix(VGAScreen, x, sy, 15);

	// Cursor-row tick: a dark notch inside the bright band (or, if the
	// cursor's row somehow falls outside the band, a dark mark on the
	// otherwise density-colored strip) so the exact row reads at a glance.
	int cursor_sy = cursor_y * ED_MINIMAP_H / h;
	if (cursor_sy >= ED_MINIMAP_H) cursor_sy = ED_MINIMAP_H - 1;
	for (int x = 0; x < ED_MINIMAP_W - 1; ++x)
		JE_pix(VGAScreen, x, cursor_sy, 0);

	// Event ticks: one per event, at the layer-agnostic fraction of level
	// progress (event_map1_row()/300 -- see its comment above row_progress()),
	// so they land at the right strip row whichever layer is active. Drawn
	// last, over the density plot/band/cursor tick, in the saturated
	// ED_EVENT_COLOR so they don't get lost under the grey (11) density
	// plot or the white (15) viewport band; a short tick on the strip's
	// right half (x = (ED_MINIMAP_W-1)/2 .. ED_MINIMAP_W-2) leaves the left
	// half free for the density plot underneath. Many events can land on
	// the same mini-map row; that's fine, they just overplot.
	for (int i = 0; i < cur_level.event_count; ++i)
	{
		int sy = event_map1_row(&cur_level.event[i]) * ED_MINIMAP_H / 300;
		if (sy < 0) sy = 0;
		if (sy >= ED_MINIMAP_H) sy = ED_MINIMAP_H - 1;

		for (int x = (ED_MINIMAP_W - 1) / 2; x <= ED_MINIMAP_W - 2; ++x)
			JE_pix(VGAScreen, x, sy, ED_EVENT_COLOR);
	}
}

// Draws only the active layer, fully lit; empty/transparent cells (and cells
// past the layer's real bounds) fall back to the checkerboard placeholder.
static void draw_map_viewport(void)
{
	int w = layer_width(active_layer);
	int h = layer_height(active_layer);
	int cols = view_cols();

	for (int row = 0; row < ED_VIEW_ROWS; ++row)
	{
		int my = scroll_y + row;
		int dy = row * ED_TILE_H;

		for (int col = 0; col < cols; ++col)
		{
			int mx = scroll_x + col;
			int dx = ED_MINIMAP_W + col * ED_TILE_W;

			if (mx >= w || my >= h)
			{
				draw_checker_cell(VGAScreen, dx, dy);
				continue;
			}

			Uint8 slot = get_cell(active_layer, my, mx);
			const EditorTile *tile = resolve_slot_tile(active_layer, slot);

			if (tile != NULL)
				blit_tile(VGAScreen, tile, dx, dy, active_layer == 0);
			else if (active_layer == 0 && slot_usable(0, slot) && cur_level.mapSh[0][slot] != 0)
				// Layer 1 blank-but-assigned slots draw SOLID BLACK in-game:
				// JE_loadMap's Match-1 loop registers blank shapes as zeroed
				// pixels rather than skipping them (tyrian2.c ~3325-3335), and
				// layer 1 paints index 0. Checkerboard would misread as
				// "missing texture" -- reserve it for truly unassigned cells.
				fill_rectangle_xy(VGAScreen, dx, dy, dx + ED_TILE_W - 1, dy + ED_TILE_H - 1, 0);
			else
				draw_checker_cell(VGAScreen, dx, dy);

			if (mx == cursor_x && my == cursor_y)
				JE_rectangle(VGAScreen, dx, dy, dx + ED_TILE_W - 1, dy + ED_TILE_H - 1, 15);
		}
	}

	// Event row markers: a thin left-edge tab on every on-screen row that's
	// an event trigger. Events are full-width row bands, not per-cell (see
	// event_active_row()'s comment), so this marks the ROW, not a cell --
	// drawn over the row's leftmost few px rather than picking one column.
	// One pass over cur_level.event[] (not rows * events): bucket each
	// event's active-layer row into the on-screen row it falls in, if any.
	bool row_has_event[ED_VIEW_ROWS] = { false };
	for (int i = 0; i < cur_level.event_count; ++i)
	{
		int row = event_active_row(&cur_level.event[i]) - scroll_y;
		if (row >= 0 && row < ED_VIEW_ROWS)
			row_has_event[row] = true;
	}

	for (int row = 0; row < ED_VIEW_ROWS; ++row)
	{
		if (!row_has_event[row])
			continue;

		int dy = row * ED_TILE_H;
		fill_rectangle_xy(VGAScreen, ED_MINIMAP_W, dy, ED_MINIMAP_W + 2, dy + ED_TILE_H - 1, ED_EVENT_COLOR);
	}
}

// ---------------------------------------------------------------------
// Tile sidebar (T toggle) -- the map-editor-resident replacement for the
// old modal tile palette overlay.
// ---------------------------------------------------------------------
//
// Geometry: the viewport is ED_MINIMAP_W + view_cols()*ED_TILE_W wide when
// the sidebar is open (12 + 10*24 = 252px, see view_cols()), leaving a 1px
// divider right after it and the sidebar itself in a fixed ED_SIDEBAR_W-wide
// strip through the screen's right edge (two ED_TILE_W-wide columns plus a
// small inter-column gap -- ED_SIDEBAR_COL_W/COLS/GAP, defined up with
// ED_MINIMAP_W near the top of the file since view_cols() needs them too).
// The divider's x therefore moves with view_cols() rather than being a fixed
// column -- computed locally below, not a macro. Vertically it stops at the
// same y as the viewport (ED_VIEW_ROWS*ED_TILE_H = 168px), well above
// ED_STATUS_Y/ED_HELP_Y1/Y2.
#define ED_SIDEBAR_ROWS_VISIBLE ED_VIEW_ROWS                          // 6 (12 slots/window)
#define ED_SIDEBAR_TOTAL_ROWS   ((72 + ED_SIDEBAR_COLS - 1) / ED_SIDEBAR_COLS)  // 36

// Draws the active layer's 72 tile slots as a scrolling 2-column grid in
// the right sidebar (only called when sidebar_open). All 72 slots are
// shown, with unusable ones (slot_usable()) hatched out exactly like the
// old modal overlay did; the current brush_slot[active_layer] gets a
// bright (color 15) highlight box, and the grid auto-scrolls to keep it in
// view.
static void draw_sidebar(void)
{
	int divider_x = ED_MINIMAP_W + view_cols() * ED_TILE_W;
	int x0 = divider_x + ED_SIDEBAR_GAP;

	fill_rectangle_xy(VGAScreen, divider_x, 0, divider_x, ED_VIEW_ROWS * ED_TILE_H - 1, 8);

	int brush = brush_slot[active_layer];
	int brush_row = brush / ED_SIDEBAR_COLS;

	if (brush_row < sidebar_scroll)
		sidebar_scroll = brush_row;
	if (brush_row >= sidebar_scroll + ED_SIDEBAR_ROWS_VISIBLE)
		sidebar_scroll = brush_row - ED_SIDEBAR_ROWS_VISIBLE + 1;

	int max_scroll = ED_SIDEBAR_TOTAL_ROWS > ED_SIDEBAR_ROWS_VISIBLE ? ED_SIDEBAR_TOTAL_ROWS - ED_SIDEBAR_ROWS_VISIBLE : 0;
	if (sidebar_scroll > max_scroll) sidebar_scroll = max_scroll;
	if (sidebar_scroll < 0) sidebar_scroll = 0;

	for (int row = 0; row < ED_SIDEBAR_ROWS_VISIBLE; ++row)
	{
		int slot_row = sidebar_scroll + row;
		int dy = row * ED_TILE_H;

		for (int col = 0; col < ED_SIDEBAR_COLS; ++col)
		{
			int slot = slot_row * ED_SIDEBAR_COLS + col;
			if (slot >= 72)
				continue;

			int dx = x0 + col * ED_SIDEBAR_COL_W;

			const EditorTile *tile = resolve_slot_tile(active_layer, slot);
			if (tile != NULL)
				blit_tile(VGAScreen, tile, dx, dy, active_layer == 0);
			else
				draw_checker_cell(VGAScreen, dx, dy);

			if (!slot_usable(active_layer, slot))
			{
				// Hatch out unusable slots so they read as off-limits, same
				// as the old modal overlay.
				for (int ty = 0; ty < ED_TILE_H; ty += 2)
					for (int tx = 0; tx < ED_TILE_W; tx += 2)
						JE_pix(VGAScreen, dx + tx, dy + ty, 0);
			}

			if (slot == brush)
				JE_rectangle(VGAScreen, dx, dy, dx + ED_TILE_W - 1, dy + ED_TILE_H - 1, 15);
		}
	}
}

// ---------------------------------------------------------------------
// Mouse input (map editor screen)
// ---------------------------------------------------------------------

// True iff screen point (px,py) sits over a real, in-bounds map cell of the
// viewport (not the mini-map strip, not the sidebar, not past the layer's
// real width/height); writes that cell's map coordinates to *out_mx/*out_my.
static bool point_in_viewport(int px, int py, int *out_mx, int *out_my)
{
	if (px < ED_MINIMAP_W)
		return false;
	if (py < 0 || py >= ED_VIEW_ROWS * ED_TILE_H)
		return false;

	int col = (px - ED_MINIMAP_W) / ED_TILE_W;
	if (col < 0 || col >= view_cols())
		return false;

	int row = py / ED_TILE_H;

	int mx = scroll_x + col;
	int my = scroll_y + row;

	if (mx >= layer_width(active_layer) || my >= layer_height(active_layer))
		return false;

	*out_mx = mx;
	*out_my = my;
	return true;
}

// True iff screen point (px,py) sits over the left mini-map strip (its
// divider column at x = ED_MINIMAP_W - 1 counts as part of the strip).
static bool point_in_minimap(int px, int py)
{
	return px >= 0 && px < ED_MINIMAP_W && py >= 0 && py < ED_MINIMAP_H;
}

// Inverse of draw_minimap()'s row scaling: maps a mini-map-strip screen y
// back to a map row of the active layer, clamped to a valid row index.
static int minimap_row(int py)
{
	int h = layer_height(active_layer);
	int row = py * h / ED_MINIMAP_H;
	if (row < 0) row = 0;
	if (row >= h) row = h - 1;
	return row;
}

// True iff screen point (px,py) sits over a sidebar tile slot (only
// meaningful while sidebar_open), writing the slot number to *out_slot.
// Rejects the inter-column gap and any row/column past the fixed 2-column,
// 72-slot grid -- caller still needs to check slot_usable() itself.
static bool sidebar_slot_at(int px, int py, int *out_slot)
{
	int divider_x = ED_MINIMAP_W + view_cols() * ED_TILE_W;
	int x0 = divider_x + ED_SIDEBAR_GAP;

	if (px < x0)
		return false;

	int within_x = px - x0;
	int col = within_x / ED_SIDEBAR_COL_W;
	if (col < 0 || col >= ED_SIDEBAR_COLS)
		return false;

	int within = within_x % ED_SIDEBAR_COL_W;
	if (within >= ED_TILE_W)
		return false;  // the 2px inter-column gap

	if (py < 0 || py >= ED_SIDEBAR_ROWS_VISIBLE * ED_TILE_H)
		return false;

	int row = py / ED_TILE_H;
	int slot = (sidebar_scroll + row) * ED_SIDEBAR_COLS + col;
	if (slot >= 72)
		return false;

	*out_slot = slot;
	return true;
}

// Draws a small crosshair at the current mouse position: color-15 arms
// spanning d=-3..3 (skipping the center, which is drawn as color 0
// separately), so the pointer is always visible against any background.
// JE_pix() only clips the upper bound, so every pixel is guarded against
// negative coordinates and >=vga_width/vga_height here as well; a pointer
// fully off-screen simply draws nothing.
//
// Suppressed entirely while the mouse is inactive (mouseInactive, set true by
// any keypress and cleared on mouse motion/click -- keyboard.c): a
// keyboard-only user never sees a stray pointer, and the startup (0,0)
// crosshair doesn't sit in the corner until the mouse is first moved.
static void draw_mouse_pointer(void)
{
	if (mouseInactive)
		return;

	for (int d = -3; d <= 3; ++d)
	{
		if (d == 0)
			continue;

		int hx = mouseX + d, hy = mouseY;
		if (hx >= 0 && hx < vga_width && hy >= 0 && hy < vga_height)
			JE_pix(VGAScreen, hx, hy, 15);

		int vx = mouseX, vy = mouseY + d;
		if (vx >= 0 && vx < vga_width && vy >= 0 && vy < vga_height)
			JE_pix(VGAScreen, vx, vy, 15);
	}

	if (mouseX >= 0 && mouseX < vga_width && mouseY >= 0 && mouseY < vga_height)
		JE_pix(VGAScreen, mouseX, mouseY, 0);
}

// True while a left-button drag started by a press INSIDE the map screen is
// in progress. The drag-continuation block only follows the held button when
// this is set, so a button still physically held from the level selector as
// the map editor opens (mouseClearInput() flushes queued click events but not
// the held-button bitmask) can't hijack the cursor on the first frame. Armed
// by an in-screen left press over the viewport/mini-map, cleared on release.
//
// The mouse only ever MOVES the cursor (select) or scrolls -- it never places
// a tile. Placement stays on Enter/Space so a stray click (e.g. right after
// opening a level) can't drop an asset by accident.
static bool mouse_dragging = false;

// Consumes the accumulated wheel delta as a SINGLE step in the scroll
// direction (-1/0/+1), zeroing mouseWheelY. A macOS trackpad / high-res wheel
// queues many wheel events per frame that sum to a large mouseWheelY, so
// scaling by it (worse, times a multiplier) scrolled whole pages per notch;
// clamping to one row-per-frame keeps wheel scrolling to a controllable pace
// (the mini-map drag remains the tool for big jumps).
static int take_wheel_step(void)
{
	int w = mouseWheelY;
	mouseWheelY = 0;
	return w > 0 ? 1 : (w < 0 ? -1 : 0);
}

// The game plays a map from the bottom row upward (mapY starts at
// `height - 8` in JE_main() and is decremented as the level scrolls;
// draw_background_*() in backgrnd.c likewise walk mapYPos *backwards*
// toward row 0 -- row 0 is the *end* of the level, not the start). Raw
// array row `y` is therefore backwards from how the level actually plays;
// "progress" reframes it in game-flow terms: 0 at the bottom (start of the
// level) counting up toward `height - 1` at row 0 (the end).
static int row_progress(int layer, int y)
{
	return (layer_height(layer) - 1) - y;
}

static void draw_status(bool show_help)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "L%d.%d p%d,%d pr%d sl%u br%d mX%u,%u,%u%s",
	         cur_level_index, active_layer + 1, cursor_x, cursor_y,
	         row_progress(active_layer, cursor_y),
	         get_cell(active_layer, cursor_y, cursor_x), brush_slot[active_layer],
	         cur_level.mapX, cur_level.mapX2, cur_level.mapX3,
	         level_dirty ? " *unsaved*" : "");
	JE_outText(VGAScreen, 4, ED_STATUS_Y, buf, 0, 2);

	if (show_help)
	{
		JE_outText(VGAScreen, 4, ED_HELP_Y1, "Arrows move Shift/PgUp/PgDn fast Tab layer Enter/Space place", 0, 0);
		JE_outText(VGAScreen, 4, ED_HELP_Y2, "P pick [/] tile T panel U/R undo E event F5 fly S save Esc", 0, 0);
	}
}

static void render_map_screen(bool show_help)
{
	SDL_FillRect(VGAScreen, NULL, 0);
	draw_minimap();
	draw_map_viewport();
	if (sidebar_open)
		draw_sidebar();
	draw_status(show_help);
}

// ---------------------------------------------------------------------
// Modal sub-screens (unsaved-changes confirm, toast)
// ---------------------------------------------------------------------

// Steps brush_slot[active_layer] to the previous (dir < 0) or next (dir >
// 0) USABLE slot (see slot_usable()), skipping over unusable ones. Does not
// wrap past slot 0/71; a no-usable-neighbor situation (or dir being neither
// -1 nor 1) is simply a no-op -- it never lands on an unusable slot and the
// loop is bounded by the fixed 0..71 slot range, so it can't spin forever.
// Only changes the brush -- does not place a tile or touch level_dirty.
static void step_brush_slot(int dir)
{
	for (int s = brush_slot[active_layer] + dir; s >= 0 && s < 72; s += dir)
	{
		if (slot_usable(active_layer, s))
		{
			brush_slot[active_layer] = s;
			return;
		}
	}
}

// Blocking Y/N prompt over the current map view. Returns true iff the user
// confirmed discarding unsaved changes.
static bool confirm_discard(void)
{
	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		SDL_FillRect(VGAScreen, NULL, 0);
		draw_map_viewport();
		JE_outText(VGAScreen, 4, ED_STATUS_Y, "Unsaved changes -- discard and go back? (Y/N)", 0, 4);

		JE_showVGA();
		waitUntilElapsed();

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			if (ki.scancode == SDL_SCANCODE_Y)
			{
				mouseClearInput();
				return true;
			}
			if (ki.scancode == SDL_SCANCODE_N || ki.scancode == SDL_SCANCODE_ESCAPE)
			{
				mouseClearInput();
				return false;
			}
		}
	}
}

// Brief non-interactive status toast (e.g. "SAVED") shown over the map view.
static void show_message(const char *msg, int frames)
{
	for (int i = 0; i < frames; ++i)
	{
		setFrameCount(1);
		handleSdlEvents();

		SDL_FillRect(VGAScreen, NULL, 0);
		JE_outText(VGAScreen, 4, ED_STATUS_Y, msg, 0, 4);

		JE_showVGA();
		waitUntilElapsed();

		// Swallow any input that arrives during the toast so it doesn't leak
		// into the editor loop as soon as it resumes.
		KeyboardInput ki;
		while (keyboardGetInput(&ki))
			(void)ki;
		mouseClearInput();
		mouseWheelY = 0;
	}
}

// ---------------------------------------------------------------------
// Screenshots (F12 in-editor, and the headless --edit-shot dump)
// ---------------------------------------------------------------------

// Writes the CURRENT VGAScreen contents (whatever was last drawn into it) to
// `path` as a 24-bit BMP, mapping each 8-bit index through the live
// palette. VGAScreen is an 8-bit indexed SDL_Surface (see video.c's
// init_video(): SDL_CreateRGBSurface(0, w, h, 8, 0,0,0,0)) and while SDL
// does give it an attached SDL_Palette, that palette is left at its
// all-black/grayscale creation default outside of one transient HD-mode
// code path in scale_and_flip() (which syncs it from the live palette only
// to build a texture, then explicitly restores the original so classic-mode
// 8-bit-to-8-bit blits keep matching by RGB) -- so it can't be trusted to
// reflect the actual game colors here.
//
// The palette source is get_live_palette() (palette.c's internal `palette`,
// what set_palette()/step_fade_palette() actually maintain and what
// rgb_palette[]/the classic scaler render from), *not* the global `colors`
// extern (palette.h): `colors` is just a caller-side "target palette"
// staging variable that most call sites pass into set_palette()/
// fade_palette() -- lvledit_run() calls set_palette(palettes[5], ...)
// directly without ever touching `colors`, so `colors` would be stale
// leftover state (from whatever screen ran before the editor) rather than
// what's actually on VGAScreen right now.
static bool save_screenshot_bmp(const char *path)
{
	SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, VGAScreen->w, VGAScreen->h, 24, SDL_PIXELFORMAT_RGB24);
	if (out == NULL)
		return false;

	if (SDL_LockSurface(VGAScreen) != 0)
	{
		SDL_FreeSurface(out);
		return false;
	}

	const SDL_Color *live_palette = get_live_palette();

	for (int y = 0; y < VGAScreen->h; ++y)
	{
		const Uint8 *srow = (const Uint8 *)VGAScreen->pixels + (size_t)y * VGAScreen->pitch;
		Uint8 *drow = (Uint8 *)out->pixels + (size_t)y * out->pitch;

		for (int x = 0; x < VGAScreen->w; ++x)
		{
			SDL_Color c = live_palette[srow[x]];
			drow[x * 3 + 0] = c.r;
			drow[x * 3 + 1] = c.g;
			drow[x * 3 + 2] = c.b;
		}
	}

	SDL_UnlockSurface(VGAScreen);

	bool ok = (SDL_SaveBMP(out, path) == 0);
	SDL_FreeSurface(out);
	return ok;
}

// Session-static counter so repeated F12 presses within one run don't
// overwrite each other's BMPs.
static int screenshot_seq = 0;

// Set by the F12 handler in the map/event editor key loops; consumed right
// after that loop iteration's render call (render_map_screen()/
// draw_event_screen()) but before JE_showVGA(), so the saved BMP matches
// exactly what that frame drew -- see maybe_save_screenshot()'s call sites.
static bool screenshot_pending = false;

// Captures whatever is currently in VGAScreen (must be called right after a
// render_map_screen()/draw_event_screen(), before JE_showVGA() flips it) and
// shows a toast reporting the result. No-op unless screenshot_pending was
// set by an F12 press since the last call.
static void maybe_save_screenshot(void)
{
	if (!screenshot_pending)
		return;
	screenshot_pending = false;

	char path[64];
	snprintf(path, sizeof(path), "lvledit_ep%d_lvl%d_%d.bmp", cur_episode, cur_level_index, screenshot_seq++);

	char msg[96];
	if (save_screenshot_bmp(path))
		snprintf(msg, sizeof(msg), "SAVED %s", path);
	else
		snprintf(msg, sizeof(msg), "SCREENSHOT FAILED");

	show_message(msg, 30);
}

// ---------------------------------------------------------------------
// Full map export (X in the map editor, and the headless --edit-export CLI)
// ---------------------------------------------------------------------

// Largest pixel canvas any export needs: layer 3 (map3) is the biggest
// single layer at 15*24 x 600*28 = 360x16800, and the composite reuses that
// same canvas size. One static buffer, reused across every layer/composite
// write in a single export call -- avoids repeated ~18MB malloc/free and
// keeps this off the stack.
#define ED_EXPORT_MAX_W (15 * ED_TILE_W)
#define ED_EXPORT_MAX_H (600 * ED_TILE_H)
static Uint8 export_rgb_buf[(size_t)ED_EXPORT_MAX_W * ED_EXPORT_MAX_H * 3];

// Blits one decoded tile's pixels into an RGB buffer of width `buf_w`,
// mapping each palette index through `pal`. Mirrors blit_tile()'s
// opaque/transparent-pixel-0 rule, but targets a plain RGB array (the export
// buffer) instead of VGAScreen. NULL `tile` is a no-op, same as blit_tile().
static void blit_tile_rgb(Uint8 *buf, int buf_w, const EditorTile *tile, int dst_x, int dst_y, bool opaque, const SDL_Color *pal)
{
	if (tile == NULL)
		return;

	for (int ty = 0; ty < ED_TILE_H; ++ty)
	{
		for (int tx = 0; tx < ED_TILE_W; ++tx)
		{
			Uint8 c = tile->px[ty * ED_TILE_W + tx];

			if (c == 0 && !opaque)
				continue;

			SDL_Color col = pal[c];
			size_t idx = ((size_t)(dst_y + ty) * buf_w + (dst_x + tx)) * 3;
			buf[idx + 0] = col.r;
			buf[idx + 1] = col.g;
			buf[idx + 2] = col.b;
		}
	}
}

// Renders one layer (array index 0..2, matching get_cell()/resolve_slot_tile
// et al.) into export_rgb_buf at its full native size and writes it out as a
// PNG. Buffer is zero-filled (literal black) first, which doubles as both
// "blank-but-assigned" black (layer 0, mirrors draw_map_viewport's opaque
// fill) and "transparent" black (layers 1/2, per the task spec: standalone
// layer PNGs render pixel-0/empty cells as solid black rather than an actual
// alpha channel). Returns false if the PNG write fails.
static bool export_layer_png(int layer, int episode, const SDL_Color *pal)
{
	int w = layer_width(layer) * ED_TILE_W;
	int h = layer_height(layer) * ED_TILE_H;

	memset(export_rgb_buf, 0, (size_t)w * h * 3);

	bool opaque = (layer == 0);
	int mw = layer_width(layer);
	int mh = layer_height(layer);

	for (int my = 0; my < mh; ++my)
	{
		for (int mx = 0; mx < mw; ++mx)
		{
			Uint8 slot = get_cell(layer, my, mx);
			const EditorTile *tile = resolve_slot_tile(layer, slot);

			if (tile != NULL)
				blit_tile_rgb(export_rgb_buf, w, tile, mx * ED_TILE_W, my * ED_TILE_H, opaque, pal);
			// else: cell is empty/unassigned/blank -- already black from the memset above.
		}
	}

	char path[64];
	snprintf(path, sizeof(path), "lvledit_map_ep%d_lvl%d_layer%d.png", episode, cur_level_index, layer + 1);

	if (!lvledit_png_write(path, export_rgb_buf, w, h))
	{
		fprintf(stderr, "lvledit: failed to write %s\n", path);
		return false;
	}

	printf("%s\n", path);
	return true;
}

// Renders the composite (layer 1 doubled + layer 2 + layer 3, all aligned at
// x=0 the way the game's default flight rendering stacks them) into
// export_rgb_buf and writes it out. Canvas is layer 3's size (360x16800):
// layer 1 is only 336px wide and 300 map-rows tall (it scrolls at half the
// speed of layers 2/3 in-game), so each of its tile rows is drawn twice --
// at 2*y and 2*y+1 -- to align its length with the other two layers, and the
// rightmost 24px column is left black (layer 1/2 are 14 slots/336px wide,
// layer 3 is 15 slots/360px). Returns false if the PNG write fails.
static bool export_composite_png(int episode, const SDL_Color *pal)
{
	int w = layer_width(2) * ED_TILE_W;
	int h = layer_height(2) * ED_TILE_H;

	memset(export_rgb_buf, 0, (size_t)w * h * 3);

	// Layer 1 (opaque), doubled vertically.
	{
		int mw = layer_width(0);
		int mh = layer_height(0);

		for (int my = 0; my < mh; ++my)
		{
			for (int mx = 0; mx < mw; ++mx)
			{
				Uint8 slot = get_cell(0, my, mx);
				const EditorTile *tile = resolve_slot_tile(0, slot);
				if (tile == NULL)
					continue;

				blit_tile_rgb(export_rgb_buf, w, tile, mx * ED_TILE_W, (2 * my) * ED_TILE_H, true, pal);
				blit_tile_rgb(export_rgb_buf, w, tile, mx * ED_TILE_W, (2 * my + 1) * ED_TILE_H, true, pal);
			}
		}
	}

	// Layer 2 over that (pixel 0 transparent), then layer 3 over both.
	for (int layer = 1; layer <= 2; ++layer)
	{
		int mw = layer_width(layer);
		int mh = layer_height(layer);

		for (int my = 0; my < mh; ++my)
		{
			for (int mx = 0; mx < mw; ++mx)
			{
				Uint8 slot = get_cell(layer, my, mx);
				const EditorTile *tile = resolve_slot_tile(layer, slot);
				blit_tile_rgb(export_rgb_buf, w, tile, mx * ED_TILE_W, my * ED_TILE_H, false, pal);
			}
		}
	}

	char path[64];
	snprintf(path, sizeof(path), "lvledit_map_ep%d_lvl%d_composite.png", episode, cur_level_index);

	if (!lvledit_png_write(path, export_rgb_buf, w, h))
	{
		fprintf(stderr, "lvledit: failed to write %s\n", path);
		return false;
	}

	printf("%s\n", path);
	return true;
}

// Exports the currently-loaded cur_level as PNGs (three per-layer files plus
// a composite) in the current directory. Uses the same live-palette rules as
// save_screenshot_bmp() -- see its comment for why get_live_palette() and
// not the `colors` extern. Returns true iff every file was written.
static bool export_full_map(int episode)
{
	const SDL_Color *pal = get_live_palette();

	bool ok = true;
	for (int layer = 0; layer < 3; ++layer)
		ok = export_layer_png(layer, episode, pal) && ok;

	ok = export_composite_png(episode, pal) && ok;

	return ok;
}

// ---------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------

static bool save_current_level(void)
{
	// One-time backup of the untouched archive, made from whatever is still
	// on disk right now (the real file hasn't been overwritten yet).
	char bak_name[40];
	snprintf(bak_name, sizeof(bak_name), "%s.bak", cur_lvl_filename);

	if (!dir_file_exists(data_dir(), bak_name))
	{
		FILE *src = dir_fopen(data_dir(), cur_lvl_filename, "rb");
		FILE *dst = (src != NULL) ? dir_fopen(data_dir(), bak_name, "wb") : NULL;

		if (src != NULL && dst != NULL)
		{
			Uint8 buf[8192];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
				fwrite(buf, 1, n, dst);
		}

		if (dst != NULL) fclose(dst);
		if (src != NULL) fclose(src);
	}

	char full_path[512];
	snprintf(full_path, sizeof(full_path), "%s/%s", data_dir(), cur_lvl_filename);

	if (lvledit_save_archive(full_path, cur_level_index, &cur_level))
	{
		// Reload from disk so the in-memory archive blob matches what we
		// just wrote, then re-parse the level we were editing (per
		// lvledit_io.h's save_archive note: it replaces exactly one record).
		lvledit_load_archive(cur_lvl_filename);
		lvledit_parse_level(cur_level_index, &cur_level);
		level_dirty = false;
		return true;
	}

	fprintf(stderr, "lvledit: save to '%s' failed\n", full_path);
	return false;
}

// ---------------------------------------------------------------------
// Event editor screen (Phase E2)
// ---------------------------------------------------------------------

// Fixed pixel geometry for the two-pane event screen. The header row
// (draw_event_screen), every LEFT-list row (draw_event_list_row) and the
// RIGHT inspector (draw_inspector_sidebar) all draw from these same
// constants, so they cannot drift apart.
//
// Usable x is 4..316 (312px of the 320px-wide VGAScreen). TINY_FONT is a
// proportional bitmap font (JE_outText advances by sprite->width + 1 per
// glyph -- see fonthand.c):
//   TIME     right-aligned in a narrow fixed column; u16 0..65535, 5 digits
//            measures 25px, so 28px leaves a little breathing room.
//   SUMMARY  the rest of the left pane, out to just short of the divider.
//   divider  a single-pixel vertical rule (see fill_rectangle_xy() below).
//   sidebar  label left-aligned at its left edge, value right-aligned 2px
//            short of the screen's own right edge (316).
#define ED_EVENT_TIME_X             4
#define ED_EVENT_TIME_W             28
#define ED_EVENT_SUMMARY_X          (ED_EVENT_TIME_X + ED_EVENT_TIME_W + 4)
#define ED_EVENT_DIVIDER_X          208
#define ED_EVENT_SUMMARY_W          (ED_EVENT_DIVIDER_X - 4 - ED_EVENT_SUMMARY_X)
#define ED_EVENT_SIDEBAR_LABEL_X    212
#define ED_EVENT_SIDEBAR_VALUE_RIGHT 314

// Pixel width the summary column gets: the narrow "stops before the divider"
// value above when the inspector sidecar is open, or the full run out to the
// screen's usable right edge (316) when T has closed it.
static int event_summary_w(void)
{
	return event_sidebar_open ? ED_EVENT_SUMMARY_W : (316 - ED_EVENT_SUMMARY_X);
}

// Defensive backstop for any label/name text that might overflow its box:
// measurement shows no current event_type_name() entry exceeds the space
// available, but if a future entry ever did, silently overflowing would be
// worse than a clipped label. Trims from the end until it fits (no
// ellipsis -- TINY_FONT has no glyph reserved for one).
static void fit_name(char *buf, int max_w)
{
	size_t len = strlen(buf);
	while (len > 0 && JE_textWidth(buf, TINY_FONT) > max_w)
	{
		buf[--len] = '\0';
	}
}

// Renders the LEFT-list row for event `index` at row-top `y`: TIME
// (right-aligned in its narrow column) and a plain-language SUMMARY
// synthesized from the event's type + whichever dat fields that type uses
// (event_summary(), above). This pane is a pure picker now -- editing
// happens in the RIGHT inspector (draw_inspector_sidebar()) -- so a
// selected row is just brightened; there's no per-column field cursor here
// anymore (that was the old raw-grid design's job).
static void draw_event_list_row(int index, int y, bool selected)
{
	const lvledit_event *ev = &cur_level.event[index];

	char f_time[8];
	snprintf(f_time, sizeof(f_time), "%u", ev->time);

	char summary[128];
	event_summary(ev, summary, sizeof(summary), event_summary_w());

	int bright = selected ? 4 : 0;

	int time_w = JE_textWidth(f_time, TINY_FONT);
	JE_outText(VGAScreen, ED_EVENT_TIME_X + ED_EVENT_TIME_W - time_w, y, f_time, 0, bright);
	JE_outText(VGAScreen, ED_EVENT_SUMMARY_X, y, summary, 0, bright);
}

// Renders the RIGHT-hand inspector for the selected event `ev`: a header
// naming its type, then one row per field build_inspector_fields() returns
// for it (TIME, TYPE, then that type's schema fields, or the raw
// dat..dat6 fallback for a type the schema table doesn't cover). Label
// left-aligned at the sidebar's left edge, value right-aligned at its right
// edge; the row at `event_field` gets the same box+bright(8) cursor style
// draw_event_row() used to give individual columns in the old layout, since
// this is now the ONLY place a field cursor exists.
static void draw_inspector_sidebar(const lvledit_event *ev)
{
	char header[24];
	snprintf(header, sizeof(header), "%s", event_type_name(ev->type));
	fit_name(header, ED_EVENT_SIDEBAR_VALUE_RIGHT + 2 - ED_EVENT_SIDEBAR_LABEL_X);
	JE_outText(VGAScreen, ED_EVENT_SIDEBAR_LABEL_X, 13, header, 0, 4);

	ef_field fields[ED_EVENT_MAX_INSPECTOR_FIELDS];
	int count = build_inspector_fields(ev, fields);

	for (int i = 0; i < count; ++i)
	{
		int y = ED_EVENT_ROW_Y0 + i * ED_EVENT_ROW_H;
		bool is_cursor = (i == event_field);

		char val[16];
		ef_format_value(ev, &fields[i], val, sizeof(val));

		int val_w = JE_textWidth(val, TINY_FONT);
		int val_x = ED_EVENT_SIDEBAR_VALUE_RIGHT - val_w;

		int bright = is_cursor ? 8 : 0;

		if (is_cursor)
			JE_rectangle(VGAScreen, ED_EVENT_SIDEBAR_LABEL_X - 1, y - 1, ED_EVENT_SIDEBAR_VALUE_RIGHT, y + 7, 15);

		JE_outText(VGAScreen, ED_EVENT_SIDEBAR_LABEL_X, y, fields[i].label, 0, bright);
		JE_outText(VGAScreen, val_x, y, val, 0, bright);
	}
}

// Returns the SELECTED event's inspector field at `event_field` (clamped by
// clamp_event_view() every frame, but re-checked here defensively), or
// false if there's no event to inspect at all. Used by every place that
// edits "the current field" (+/-, Enter-to-type) so they all agree on what
// that means with the caller who does the actual get/set.
static bool current_inspector_field(ef_field *out)
{
	if (cur_level.event_count == 0)
		return false;

	ef_field fields[ED_EVENT_MAX_INSPECTOR_FIELDS];
	int count = build_inspector_fields(&cur_level.event[event_sel], fields);

	if (event_field < 0 || event_field >= count)
		return false;

	*out = fields[event_field];
	return true;
}

static void clamp_event_view(bool show_help)
{
	int rows = event_rows_visible(show_help);
	int count = cur_level.event_count;

	if (count == 0)
	{
		event_sel = 0;
		event_scroll = 0;
		event_field = 0;
		return;
	}

	if (event_sel < 0) event_sel = 0;
	if (event_sel >= count) event_sel = count - 1;

	if (event_sel < event_scroll) event_scroll = event_sel;
	if (event_sel >= event_scroll + rows) event_scroll = event_sel - rows + 1;

	int max_scroll = count > rows ? count - rows : 0;
	if (event_scroll > max_scroll) event_scroll = max_scroll;
	if (event_scroll < 0) event_scroll = 0;

	// event_field indexes the SELECTED event's inspector list, which is
	// shorter than the old fixed 8 for most types; re-clamp here (called
	// every frame, and specifically right after any event_sel change) so
	// switching to a shorter-schema type -- directly, via Up/Down/PgUp/...,
	// or by editing TYPE into one -- can never leave the cursor pointing
	// past the end of the new list.
	ef_field fields[ED_EVENT_MAX_INSPECTOR_FIELDS];
	int field_count = build_inspector_fields(&cur_level.event[event_sel], fields);
	if (event_field < 0) event_field = 0;
	if (event_field >= field_count) event_field = field_count - 1;
}

static void draw_event_screen(bool show_help)
{
	SDL_FillRect(VGAScreen, NULL, 0);

	char title[64];
	snprintf(title, sizeof(title), "Event Editor - Lvl %d", cur_level_index);
	JE_outText(VGAScreen, 4, 4, title, 0, 4);

	JE_outText(VGAScreen, ED_EVENT_TIME_X, 13, "TIME", 0, 2);
	JE_outText(VGAScreen, ED_EVENT_SUMMARY_X, 13, "EVENT", 0, 2);

	// Vertical divider between the LEFT event list and the RIGHT inspector --
	// same "1px-wide filled rect" trick the map editor's own tile-sidebar
	// divider uses (see draw_map_viewport()'s divider_x). Only when the
	// inspector sidecar is open (T); closed, the list owns the full width.
	if (event_sidebar_open)
		fill_rectangle_xy(VGAScreen, ED_EVENT_DIVIDER_X, 0, ED_EVENT_DIVIDER_X, event_status_y(show_help) - 4, 8);

	int count = cur_level.event_count;

	if (count == 0)
	{
		JE_outText(VGAScreen, 4, ED_EVENT_ROW_Y0, "(no events -- press I to insert one)", 0, 2);
	}
	else
	{
		for (int row = 0; row < event_rows_visible(show_help); ++row)
		{
			int idx = event_scroll + row;
			if (idx >= count)
				break;

			draw_event_list_row(idx, ED_EVENT_ROW_Y0 + row * ED_EVENT_ROW_H, idx == event_sel);
		}

		if (event_sidebar_open)
			draw_inspector_sidebar(&cur_level.event[event_sel]);
	}

	char status[96];
	snprintf(status, sizeof(status), "Event %d/%d (cap %d)%s", count > 0 ? event_sel : -1, count, LVLEDIT_MAX_EVENT,
	         level_dirty ? "  *unsaved*" : "");
	JE_outText(VGAScreen, 4, event_status_y(show_help), status, 0, 2);

	if (entering_number)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "Enter value: %s_", number_entry_buf);
		JE_outText(VGAScreen, 4, ED_EVENT_HELP_Y1, buf, 0, 4);
	}
	else if (show_help)
	{
		JE_outText(VGAScreen, 4, ED_EVENT_HELP_Y1, "Up/Dn/PgUp/PgDn/Home/End event  Click row/field  L/R field", 0, 0);
		JE_outText(VGAScreen, 4, ED_EVENT_HELP_Y2, "+/-(Sft=10) I ins D del R sort T panel U/Y undo S save Esc", 0, 0);
	}
}

// Applies the pending numeric-entry buffer to the SELECTED event's current
// inspector field (event_field, an index -- see current_inspector_field()),
// clamped to that field's real range. No-op if the buffer is empty (bare
// Enter, or everything got backspaced away) or there's no field to apply to.
static void commit_number_entry(void)
{
	ef_field f;

	if (number_entry_len > 0 && current_inspector_field(&f))
	{
		long v = strtol(number_entry_buf, NULL, 10);
		lvledit_event *ev = &cur_level.event[event_sel];
		long before = event_field_get(ev, f.field_id);

		// Pre-clamp so we can tell (before mutating anything) whether this
		// will actually change the field -- mirrors event_field_set()'s own
		// clamping, so a no-op entry (e.g. re-entering the same value, or a
		// value that clamps back to `before`) pushes no undo step.
		long lo, hi;
		event_field_range(f.field_id, &lo, &hi);
		if (v < lo) v = lo;
		if (v > hi) v = hi;

		if (v != before)
		{
			undo_push();
			event_field_set(ev, f.field_id, v);
			level_dirty = true;
		}
	}

	entering_number = false;
	number_entry_len = 0;
	number_entry_buf[0] = '\0';
}

// Inserts a copy of the selected event immediately after it (or a
// zero-filled event at index 0 if the list is currently empty), clamped to
// LVLEDIT_MAX_EVENT capacity. Selects the new event.
static void insert_event(void)
{
	if (cur_level.event_count >= LVLEDIT_MAX_EVENT)
		return;

	undo_push();

	int insert_at;
	lvledit_event new_event;

	if (cur_level.event_count == 0)
	{
		memset(&new_event, 0, sizeof(new_event));
		insert_at = 0;
	}
	else
	{
		new_event = cur_level.event[event_sel];
		insert_at = event_sel + 1;
	}

	for (int i = cur_level.event_count; i > insert_at; --i)
		cur_level.event[i] = cur_level.event[i - 1];

	cur_level.event[insert_at] = new_event;
	cur_level.event_count++;
	event_sel = insert_at;
	level_dirty = true;
}

// Deletes the selected event, shifting later events down. Allowed to empty
// the list entirely.
static void delete_event(void)
{
	if (cur_level.event_count == 0)
		return;

	undo_push();

	for (int i = event_sel; i + 1 < cur_level.event_count; ++i)
		cur_level.event[i] = cur_level.event[i + 1];

	cur_level.event_count--;
	if (event_sel >= cur_level.event_count)
		event_sel = cur_level.event_count - 1;
	if (event_sel < 0)
		event_sel = 0;

	level_dirty = true;
}

// Stable ascending sort by event time. The game's event dispatch
// (JE_eventSystem/eventLoc walk in tyrian2.c) assumes events are stored in
// non-decreasing eventtime order; an edited level with times out of order
// won't fire correctly in-game, so this is offered as an explicit, opt-in
// fixup rather than applied automatically on every edit.
static void sort_events_by_time(void)
{
	int n = cur_level.event_count;
	if (n < 2)
		return;

	undo_push();

	// Simple stable insertion sort -- n is capped at LVLEDIT_MAX_EVENT (2500)
	// and this only runs on an explicit keypress, so O(n^2) is acceptable.
	for (int i = 1; i < n; ++i)
	{
		lvledit_event key = cur_level.event[i];
		int j = i - 1;
		while (j >= 0 && cur_level.event[j].time > key.time)
		{
			cur_level.event[j + 1] = cur_level.event[j];
			--j;
		}
		cur_level.event[j + 1] = key;
	}

	level_dirty = true;
}

// Modal event-list editor over cur_level.event[]/event_count. Returns to the
// map editor (not level select) on Esc; S saves via save_current_level()
// same as the map editor.
static void run_event_editor(void)
{
	bool show_help = true;

	entering_number = false;
	number_entry_len = 0;
	number_entry_buf[0] = '\0';

	mouseClearInput();
	mouseWheelY = 0;

	// Jump straight to the event nearest the map cursor's scroll position
	// (rather than leaving event_sel wherever a previous visit left it, or
	// defaulting to 0): minimise |ev.time - target|, earlier index wins
	// ties. Pure selection change -- no event data is touched.
	if (cur_level.event_count > 0)
	{
		long target = row_target_time(cursor_y);
		int best = 0;
		long best_diff = labs((long)cur_level.event[0].time - target);

		for (int i = 1; i < cur_level.event_count; ++i)
		{
			long diff = labs((long)cur_level.event[i].time - target);
			if (diff < best_diff)
			{
				best = i;
				best_diff = diff;
			}
		}

		event_sel = best;
	}

	clamp_event_view(show_help);

	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		bool quit_to_map = false;

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			if (entering_number)
			{
				if (ki.scancode == SDL_SCANCODE_RETURN || ki.scancode == SDL_SCANCODE_KP_ENTER)
				{
					commit_number_entry();
				}
				else if (ki.scancode == SDL_SCANCODE_ESCAPE)
				{
					entering_number = false;
					number_entry_len = 0;
					number_entry_buf[0] = '\0';
				}
				else if (ki.scancode == SDL_SCANCODE_BACKSPACE)
				{
					if (number_entry_len > 0)
						number_entry_buf[--number_entry_len] = '\0';
				}
				else if (ki.ch == '-' && number_entry_len == 0)
				{
					number_entry_buf[number_entry_len++] = '-';
					number_entry_buf[number_entry_len] = '\0';
				}
				else if (ki.ch >= '0' && ki.ch <= '9' && number_entry_len < (int)sizeof(number_entry_buf) - 1)
				{
					number_entry_buf[number_entry_len++] = (char)ki.ch;
					number_entry_buf[number_entry_len] = '\0';
				}

				continue;
			}

			bool fast = (ki.mod & KMOD_SHIFT) != 0;

			switch (ki.scancode)
			{
			case SDL_SCANCODE_ESCAPE:
				quit_to_map = true;
				break;

			case SDL_SCANCODE_F1:
				show_help = !show_help;
				break;

			case SDL_SCANCODE_F12:
				screenshot_pending = true;
				break;

			case SDL_SCANCODE_UP:
				event_sel -= 1;
				break;
			case SDL_SCANCODE_DOWN:
				event_sel += 1;
				break;
			case SDL_SCANCODE_PAGEUP:
				event_sel -= event_rows_visible(show_help);
				break;
			case SDL_SCANCODE_PAGEDOWN:
				event_sel += event_rows_visible(show_help);
				break;
			case SDL_SCANCODE_HOME:
				event_sel = 0;
				break;
			case SDL_SCANCODE_END:
				event_sel = cur_level.event_count - 1;
				break;

			// Moves the field cursor within the SELECTED event's inspector
			// list -- its length varies per type (2 for a no-fields type up
			// to 8), so the wrap modulus is computed fresh each press rather
			// than the old fixed "% 8".
			case SDL_SCANCODE_LEFT:
				if (cur_level.event_count > 0)
				{
					ef_field fields[ED_EVENT_MAX_INSPECTOR_FIELDS];
					int field_count = build_inspector_fields(&cur_level.event[event_sel], fields);
					event_field = (event_field + field_count - 1) % field_count;
				}
				break;
			case SDL_SCANCODE_RIGHT:
				if (cur_level.event_count > 0)
				{
					ef_field fields[ED_EVENT_MAX_INSPECTOR_FIELDS];
					int field_count = build_inspector_fields(&cur_level.event[event_sel], fields);
					event_field = (event_field + 1) % field_count;
				}
				break;

			case SDL_SCANCODE_EQUALS:
			case SDL_SCANCODE_KP_PLUS:
			{
				ef_field f;
				if (current_inspector_field(&f))
				{
					lvledit_event *ev = &cur_level.event[event_sel];
					long before = event_field_get(ev, f.field_id);

					long lo, hi;
					event_field_range(f.field_id, &lo, &hi);
					long want = before + (fast ? 10 : 1);
					if (want < lo) want = lo;
					if (want > hi) want = hi;

					if (want != before)
					{
						undo_push();
						event_field_set(ev, f.field_id, want);
						level_dirty = true;
					}
				}
				break;
			}

			case SDL_SCANCODE_MINUS:
			case SDL_SCANCODE_KP_MINUS:
			{
				ef_field f;
				if (current_inspector_field(&f))
				{
					lvledit_event *ev = &cur_level.event[event_sel];
					long before = event_field_get(ev, f.field_id);

					long lo, hi;
					event_field_range(f.field_id, &lo, &hi);
					long want = before - (fast ? 10 : 1);
					if (want < lo) want = lo;
					if (want > hi) want = hi;

					if (want != before)
					{
						undo_push();
						event_field_set(ev, f.field_id, want);
						level_dirty = true;
					}
				}
				break;
			}

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
				if (cur_level.event_count > 0)
				{
					entering_number = true;
					number_entry_len = 0;
					number_entry_buf[0] = '\0';
				}
				break;

			case SDL_SCANCODE_I:
				insert_event();
				break;

			case SDL_SCANCODE_D:
				delete_event();
				break;

			case SDL_SCANCODE_R:
				sort_events_by_time();
				break;

			case SDL_SCANCODE_T:
				event_sidebar_open = !event_sidebar_open;
				break;

			case SDL_SCANCODE_U:
				if (undo_apply())
				{
					level_dirty = true;
					clamp_event_view(show_help);
					show_message("UNDO", 20);
				}
				else
				{
					show_message("NOTHING TO UNDO", 20);
				}
				break;

			// R is already taken by sort_events_by_time() above, so redo
			// uses Y here instead (unlike the map editor, where R is free).
			case SDL_SCANCODE_Y:
				if (redo_apply())
				{
					level_dirty = true;
					clamp_event_view(show_help);
					show_message("REDO", 20);
				}
				else
				{
					show_message("NOTHING TO REDO", 20);
				}
				break;

			case SDL_SCANCODE_S:
				show_message(save_current_level() ? "SAVED" : "SAVE FAILED", 30);
				break;

			default:
				break;
			}

			if (quit_to_map)
				break;
		}

		if (quit_to_map)
			return;

		if (!entering_number)
		{
			MouseInput mi;
			while (mouseGetInput(INPUT_NO_MOTION, &mi))
			{
				if (mi.button != SDL_BUTTON_LEFT || cur_level.event_count == 0)
					continue;
				if (mi.y < ED_EVENT_ROW_Y0)
					continue;

				int row = (mi.y - ED_EVENT_ROW_Y0) / ED_EVENT_ROW_H;

				// With the sidecar closed the list spans the full width, so a
				// click anywhere is a list click; open, x past the divider is
				// an inspector field click.
				if (!event_sidebar_open || mi.x < ED_EVENT_DIVIDER_X)
				{
					// LEFT list: pick the event under the click -- same row
					// math as before, minus the old column->field mapping
					// (there's nothing to edit on this side anymore; that's
					// the RIGHT inspector's job, just below).
					if (row < 0 || row >= event_rows_visible(show_help))
						continue;
					int idx = event_scroll + row;
					if (idx >= cur_level.event_count)
						continue;
					event_sel = idx;
				}
				else
				{
					// RIGHT inspector: pick the field row under the click
					// (TIME, TYPE, or one of the selected event's schema
					// fields) -- clamped the same way clamp_event_view()
					// clamps event_field every frame.
					ef_field fields[ED_EVENT_MAX_INSPECTOR_FIELDS];
					int field_count = build_inspector_fields(&cur_level.event[event_sel], fields);
					if (row >= 0 && row < field_count)
						event_field = row;
				}
			}

			event_sel -= take_wheel_step();
		}

		clamp_event_view(show_help);

		draw_event_screen(show_help);
		maybe_save_screenshot();
		draw_mouse_pointer();

		JE_showVGA();
		waitUntilElapsed();
	}
}

// ---------------------------------------------------------------------
// Map editor screen
// ---------------------------------------------------------------------

static bool load_level_for_edit(int level_index)
{
	if (!lvledit_parse_level(level_index, &cur_level))
		return false;

	cur_level_index = level_index;
	level_dirty = false;
	undo_reset();  // fresh parse -- nothing meaningful to undo/redo yet

	load_tileset(cur_level.shapeFile);

	active_layer = 0;
	cursor_x = 0;
	// Game flow (see row_progress()): the level plays from the bottom row
	// upward toward row 0, so open the editor there instead of at row 0 --
	// Up/PgUp then naturally move "forward" through the level.
	cursor_y = layer_height(active_layer) - 1;
	scroll_x = 0;
	scroll_y = 0;  // clamp_view() (called before the first render) scrolls to show cursor_y
	brush_slot[0] = brush_slot[1] = brush_slot[2] = 0;

	return true;
}

// ---------------------------------------------------------------------
// In-editor playtest -- the "F5" experience (Phase E7)
// ---------------------------------------------------------------------
//
// Fly the level currently open in the map editor, then return here. Reuses the
// engine's demo-mode skeleton: under editorPlaytest, JE_loadMap() skips the
// levels<ep>.dat script entirely and JE_main() returns to us at level end (see
// the editorPlaytest carve-outs in tyrian2.c), but input stays live. To exit a
// running playtest, open the in-game menu (Esc) and pick Quit -- that sets
// reallyEndLevel/playerEndLevel, which routes to JE_main()'s early return.
//
// The record under test -- INCLUDING unsaved edits -- is staged into a scratch
// archive in data_dir() via the normal save path (blob-copy every other record,
// re-serialize the edited one), so the real tyrian<ep>.lvl and its .bak are
// never touched. The name must fit levelFile[13]; "_edtest.lvl" (11) does, like
// "tyrian4.lvl".
#define ED_PLAYTEST_ARCHIVE "_edtest.lvl"

static void playtest_current_level(void)
{
	char stage_path[512];
	snprintf(stage_path, sizeof(stage_path), "%s/%s", data_dir(), ED_PLAYTEST_ARCHIVE);
	if (!lvledit_save_archive(stage_path, cur_level_index, &cur_level))
	{
		show_message("PLAYTEST STAGE FAILED", 40);
		return;
	}

	// Load this episode's item/enemy/ship tables + real lvlPos. Force
	// JE_initEpisode() to actually run (it early-returns when episodeNum already
	// matches). JE_initPlayerData() reads ships[].dmg, so it must follow.
	episodeNum = 0;
	JE_initEpisode(cur_episode);
	JE_initPlayerData();

	// Redirect the level archive to the staged copy so JE_loadMap() reads the
	// edited record; refresh lvlPos from it. Record index is 1-based on disk.
	SDL_strlcpy(levelFile, ED_PLAYTEST_ARCHIVE, sizeof(levelFile));
	JE_analyzeLevel();
	lvlFileNum = (JE_byte)(cur_level_index + 1);

	// The .lvl record carries no name or song (those live in the episode script
	// we're bypassing), so supply flyable defaults.
	difficultyLevel = oldDifficultyLevel = initialDifficulty = DIFFICULTY_NORMAL;
	levelSong = 2;
	SDL_strlcpy(levelName, "EDIT TEST", sizeof(levelName));

	editorPlaytest = true;
	keyboardClearInput();
	mouseClearInput();

	JE_main();  // returns at level end via the editorPlaytest carve-out

	editorPlaytest = false;

	// Stop the level music: JE_main() only fades/stops the song on its
	// play_demo path (tyrian2.c ~727), which the editorPlaytest early return
	// skips, so without this the flight track keeps playing back in the editor.
	stop_song();

	// Restore the editor: drop the scratch archive, put the mouse back to
	// absolute, re-apply the fixed tileset palette (flight loaded its own) and
	// force a tile-sprite reload. cur_level and lvledit_io's own archive blob
	// are untouched by the game-side globals above, so edits survive intact.
	remove(stage_path);
	mouseSetRelative(false);
	keyboardClearInput();
	mouseClearInput();
	mouseWheelY = 0;
	mouse_dragging = false;
	set_palette(palettes[5], 0, 255);
	tileset_loaded = false;
}

static void run_map_editor(int level_index)
{
	if (!load_level_for_edit(level_index))
	{
		fprintf(stderr, "lvledit: failed to parse level %d\n", level_index);
		return;
	}

	bool show_help = true;

	mouseClearInput();
	mouseWheelY = 0;
	mouse_dragging = false;  // ignore any button still held from the selector

	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		bool quit_to_select = false;

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			bool fast = (ki.mod & KMOD_SHIFT) != 0;

			switch (ki.scancode)
			{
			case SDL_SCANCODE_ESCAPE:
				if (!level_dirty || confirm_discard())
					quit_to_select = true;
				break;

			case SDL_SCANCODE_F1:
				show_help = !show_help;
				break;

			case SDL_SCANCODE_F12:
				screenshot_pending = true;
				break;

			case SDL_SCANCODE_UP:
				cursor_y -= fast ? ED_VIEW_ROWS : 1;
				break;
			case SDL_SCANCODE_DOWN:
				cursor_y += fast ? ED_VIEW_ROWS : 1;
				break;
			case SDL_SCANCODE_LEFT:
				cursor_x -= fast ? view_cols() : 1;
				break;
			case SDL_SCANCODE_RIGHT:
				cursor_x += fast ? view_cols() : 1;
				break;
			case SDL_SCANCODE_PAGEUP:
				cursor_y -= ED_VIEW_ROWS;
				break;
			case SDL_SCANCODE_PAGEDOWN:
				cursor_y += ED_VIEW_ROWS;
				break;

			case SDL_SCANCODE_TAB:
			{
				int old_h = layer_height(active_layer);
				active_layer = (active_layer + 1) % 3;
				int new_h = layer_height(active_layer);

				if (new_h != old_h)
				{
					// Different-height layer: re-anchor to its bottom row
					// (game-flow start) rather than carry over a row index
					// that meant something different in the old layer.
					// clamp_view() below scrolls the viewport to show it.
					cursor_y = new_h - 1;
					scroll_y = 0;
				}
				break;
			}

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
			case SDL_SCANCODE_SPACE:
			{
				Uint8 wanted = (Uint8)brush_slot[active_layer];
				if (get_cell(active_layer, cursor_y, cursor_x) != wanted)
				{
					undo_push();
					set_cell(active_layer, cursor_y, cursor_x, wanted);
					level_dirty = true;
				}
				break;
			}

			case SDL_SCANCODE_P:
				brush_slot[active_layer] = get_cell(active_layer, cursor_y, cursor_x);
				break;

			case SDL_SCANCODE_T:
				sidebar_open = !sidebar_open;
				break;

			case SDL_SCANCODE_LEFTBRACKET:
				step_brush_slot(-1);
				break;

			case SDL_SCANCODE_RIGHTBRACKET:
				step_brush_slot(1);
				break;

			case SDL_SCANCODE_U:
				if (undo_apply())
				{
					level_dirty = true;
					clamp_view();
					show_message("UNDO", 20);
				}
				else
				{
					show_message("NOTHING TO UNDO", 20);
				}
				break;

			case SDL_SCANCODE_R:
				if (redo_apply())
				{
					level_dirty = true;
					clamp_view();
					show_message("REDO", 20);
				}
				else
				{
					show_message("NOTHING TO REDO", 20);
				}
				break;

			case SDL_SCANCODE_S:
				show_message(save_current_level() ? "SAVED" : "SAVE FAILED", 30);
				break;

			case SDL_SCANCODE_E:
				run_event_editor();
				break;

			case SDL_SCANCODE_F5:
				playtest_current_level();
				break;

			case SDL_SCANCODE_X:
			{
				char msg[64];
				if (export_full_map(cur_episode))
					snprintf(msg, sizeof(msg), "EXPORTED lvledit_map_ep%d_lvl%d_*.png", cur_episode, cur_level_index);
				else
					snprintf(msg, sizeof(msg), "MAP EXPORT FAILED");
				show_message(msg, 30);
				break;
			}

			default:
				break;
			}

			if (quit_to_select)
				break;
		}

		if (quit_to_select)
			return;

		// Discrete clicks (press events). The mouse selects/scrolls only --
		// it never places a tile (that stays on Enter/Space), so a stray
		// click can't drop an asset by accident.
		MouseInput mi;
		while (mouseGetInput(INPUT_NO_MOTION, &mi))
		{
			int mx, my, slot;
			if (mi.button == SDL_BUTTON_LEFT)
			{
				if (point_in_viewport(mi.x, mi.y, &mx, &my))
				{
					cursor_x = mx; cursor_y = my;  // select the cell
					mouse_dragging = true;
				}
				else if (point_in_minimap(mi.x, mi.y))
				{
					cursor_y = minimap_row(mi.y);
					mouse_dragging = true;
				}
				else if (sidebar_open && sidebar_slot_at(mi.x, mi.y, &slot) && slot_usable(active_layer, slot))
					brush_slot[active_layer] = slot;
			}
			else if (mi.button == SDL_BUTTON_RIGHT)
			{
				if (point_in_viewport(mi.x, mi.y, &mx, &my))
				{
					cursor_x = mx; cursor_y = my;
					brush_slot[active_layer] = get_cell(active_layer, my, mx);  // eyedropper, like P
				}
			}
		}

		// Drag continuation -- follow the held button only if the drag was
		// started by a press in THIS screen (mouse_dragging). Guards against a
		// button still held over from the level selector jumping the cursor on
		// the first map frame. Drag moves the cursor (viewport) or scrolls
		// (mini-map); it never paints.
		if ((mouseButtonsDown & SDL_BUTTON(SDL_BUTTON_LEFT)) && mouse_dragging)
		{
			int mx, my;
			if (point_in_viewport(mouseX, mouseY, &mx, &my))
			{
				cursor_x = mx; cursor_y = my;
			}
			else if (point_in_minimap(mouseX, mouseY))
				cursor_y = minimap_row(mouseY);
		}

		if (!(mouseButtonsDown & SDL_BUTTON(SDL_BUTTON_LEFT)))
			mouse_dragging = false;  // released -- next press must re-arm

		// Wheel = vertical scroll (moves cursor; clamp_view scrolls to follow).
		cursor_y -= take_wheel_step();

		clamp_view();

		render_map_screen(show_help);
		maybe_save_screenshot();
		draw_mouse_pointer();

		JE_showVGA();
		waitUntilElapsed();
	}
}

// ---------------------------------------------------------------------
// Level select screen
// ---------------------------------------------------------------------

static void build_level_summaries(void)
{
	static lvledit_level scratch;

	level_summary_count = lvledit_level_count();
	if (level_summary_count > ED_MAX_LEVEL_SUMMARIES)
		level_summary_count = ED_MAX_LEVEL_SUMMARIES;

	for (int i = 0; i < level_summary_count; ++i)
	{
		if (lvledit_parse_level(i, &scratch))
		{
			level_summaries[i].mapFile = scratch.mapFile;
			level_summaries[i].shapeFile = scratch.shapeFile;
		}
		else
		{
			level_summaries[i].mapFile = '?';
			level_summaries[i].shapeFile = '?';
		}
	}
}

// ---------------------------------------------------------------------
// Play-order derivation (levels<episode>.dat episode script)
// ---------------------------------------------------------------------
//
// The .lvl archive itself has no level name and no play order -- that only
// exists in the episode script, as the sequence of 'L' ("play level")
// commands it issues (see tyrian2.c's script interpreter, case 'L' around
// tyrian2.c:2865). This section re-scans that script independently, purely
// to recover a sensible default ordering for the level-select list; nothing
// here feeds back into gameplay, saving, or the archive itself.

// XOR key + descramble copied byte-for-byte from helptext.c's
// decrypt_string() (that function is static to helptext.c, so not directly
// reusable, and it's paired there with fread_*_die() helpers that call
// exit() at EOF -- fine for the real game loading a known-good script, fatal
// for a scanner that must run a whole file to a clean EOF). Must stay
// identical to helptext.c's copy since it is undoing the same on-disk
// scramble.
static const unsigned char crypt_key[] = { 204, 129, 63, 255, 71, 19, 25, 62, 1, 99 };

// Non-dying analog of read_encrypted_pascal_string() (helptext.c): reads one
// length-prefixed encrypted "pascal string" (1 length byte + `len` payload
// bytes) from `f`, descrambles it, and copies min(len, size-1) bytes into
// `s` (NUL-terminated). Returns false on EOF or a short read (no partial
// data written to `s`) instead of dying, so a scan can simply stop at a
// script's real end.
static bool read_script_line(char *s, size_t size, FILE *f)
{
	Uint8 len;
	if (fread(&len, 1, 1, f) != 1)
		return false;

	char buf[255];
	if (len > 0 && fread(buf, 1, len, f) != (size_t)len)
		return false;

	if (len > 0)
	{
		for (int i = (int)len - 1; ; --i)
		{
			buf[i] ^= crypt_key[i % (int)sizeof(crypt_key)];
			if (i == 0)
				break;
			buf[i] ^= buf[i - 1];
		}
	}

	if (size > 0)
	{
		size_t n = MIN((size_t)len, size - 1);
		memcpy(s, buf, n);
		s[n] = '\0';
	}

	return true;
}

// Fills play_order[] (paired with level_summary_count, which the caller
// must already have set via build_level_summaries()): the archive index of
// every level the episode script plays, in FIRST-APPEARANCE order, followed
// by any archive indices the script never references ("orphans" -- e.g.
// unused maps), appended in ascending index order. The result is always a
// full permutation of [0, level_summary_count). Also fills level_title[]
// (indexed by archive index, not by play_order position) from the same ]L
// lines -- first appearance wins there too, and orphan records are left
// with an empty title.
//
// A "play level" script line is `]L...` -- tyrian2.c's interpreter (2710:
// `if (s[0] == ']') switch (s[1]) { ... case 'L': ...`) dispatches on s[1]
// once s[0] == ']' has already selected the command block, so the two-char
// prefix (not a bare leading 'L') is what actually marks this command;
// checked against tyrian21's levels1.dat directly (script lines there read
// like "]L[ 9999 004 TYRIAN   18 09", confirming both the prefix and that
// s+25 lands on the lvlFileNum field, "09").
//
// This is play order by first script appearance, not a strict runtime
// trace: the script can jump/branch between sections (the ']'-prefixed
// commands in tyrian2.c's interpreter), and this scan just walks the file
// top-to-bottom without resolving any of that, so a level reachable only
// through a branch still gets *a* position (wherever its 'L' line sits in
// the file), just not necessarily its true in-game turn order. That's good
// enough and fully deterministic for editor navigation, which is all this
// is for. Falls back to identity order (0,1,2,...) if the script can't be
// opened or read at all -- the "append orphans in ascending order" pass
// below runs unconditionally and covers that case on its own, since nothing
// will have been marked seen.
// Extracts a ]L script line's level title into `out` (must be at least 10
// bytes): 9 chars at s+13, mirroring tyrian2.c's own
// `SDL_strlcpy(levelName, s + 13, 10)`, trimmed of the trailing
// space-padding the field is stored with. No-op (leaves `out` untouched) if
// `s` is too short to carry a title field at all -- caller is expected to
// have already pre-cleared `out` (compute_play_order() does, once per
// archive index, before scanning).
static void extract_level_title(const char *s, char *out)
{
	if (strlen(s) <= 13)
		return;

	size_t n = MIN((size_t)9, strlen(s) - 13);
	memcpy(out, s + 13, n);
	out[n] = '\0';

	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';
}

static void compute_play_order(void)
{
	int play_order_count = 0;
	bool seen[ED_MAX_LEVEL_SUMMARIES] = { false };

	for (int i = 0; i < level_summary_count; ++i)
		level_title[i][0] = '\0';

	char fname[32];
	snprintf(fname, sizeof(fname), "levels%d.dat", cur_episode);

	FILE *f = dir_fopen(data_dir(), fname, "rb");
	if (f != NULL)
	{
		char s[256];
		while (read_script_line(s, sizeof(s), f))
		{
			if (s[0] != ']' || s[1] != 'L' || strlen(s) <= 25)
				continue;

			int lvl_file_num = atoi(s + 25);
			int idx = lvl_file_num - 1;  // script's 1-based record # -> editor's 0-based archive index

			if (idx < 0 || idx >= level_summary_count || seen[idx])
				continue;

			seen[idx] = true;
			extract_level_title(s, level_title[idx]);
			if (play_order_count < ED_MAX_LEVEL_SUMMARIES)
				play_order[play_order_count++] = idx;
		}

		fclose(f);
	}

	for (int i = 0; i < level_summary_count && play_order_count < ED_MAX_LEVEL_SUMMARIES; ++i)
	{
		if (!seen[i])
			play_order[play_order_count++] = i;
	}
}

// Rebuilds display_order[] (length level_summary_count) from the current
// sort_mode. Caller must have already refreshed play_order[] via
// compute_play_order() for this episode/summary set.
static void build_display_order(void)
{
	for (int i = 0; i < level_summary_count; ++i)
		display_order[i] = (sort_mode == SORT_PLAY_ORDER) ? play_order[i] : i;
}

// Row in display_order[] whose archive index is `archive_index`, or 0 if
// not found (e.g. a stale last_level_sel left over from a different episode
// or archive size).
static int row_for_archive_index(int archive_index)
{
	for (int row = 0; row < level_summary_count; ++row)
		if (display_order[row] == archive_index)
			return row;
	return 0;
}

// ---------------------------------------------------------------------
// Episode-script editor (Phase E5b/c): a semantic editor over the
// levels<ep>.dat interlevel script -- the encrypted command language that
// sequences an episode (shop menus, which record plays as each level,
// map-branch choices, cutscenes, end-of-episode). It reads/writes through
// the E5a codec + flat script_doc model, and all structural mutation / field
// parsing lives in lvledit_script.c (headlessly testable via
// --edit-script-retarget-test); this screen is purely rendering + input, the
// same split the event editor uses (event_field_get/set vs run_event_editor).
//
// Two panes, mirroring the event editor: a LEFT list of script lines rendered
// semantically (section dividers, named commands + a field summary, indented
// sub-block payload, dimmed inert lines) and a RIGHT inspector (T toggles) of
// the selected command's named fields, edited via Left/Right + +/- or a typed
// value. Structural keys route section-marker edits through the section-aware
// model functions (N/K) and plain command edits through the raw line ops
// (I/D/move), never mixing the two (see lvledit_script.h's note).
// ---------------------------------------------------------------------

#define ED_SCRIPT_ROW_Y0        22
#define ED_SCRIPT_ROW_H         8
#define ED_SCRIPT_ROWS_VISIBLE  18
#define ED_SCRIPT_DIVIDER_X     176
#define ED_SCRIPT_LABEL_X       (ED_SCRIPT_DIVIDER_X + 4)
#define ED_SCRIPT_VALUE_RIGHT   314
#define ED_SCRIPT_STATUS_Y      174
#define ED_SCRIPT_HELP_Y1       183
#define ED_SCRIPT_HELP_Y2       191

// Collapsed layout: F1 hides the two help lines and the freed rows go back to
// the list (see the matching ED_EVENT_*_COLLAPSED note above). Status at 191
// clears the transient insert/entry prompt drawn at HELP_Y1 (183).
#define ED_SCRIPT_ROWS_VISIBLE_COLLAPSED 20
#define ED_SCRIPT_STATUS_Y_COLLAPSED     191

static int script_rows_visible(bool show_help)
{
	return show_help ? ED_SCRIPT_ROWS_VISIBLE : ED_SCRIPT_ROWS_VISIBLE_COLLAPSED;
}
static int script_status_y(bool show_help)
{
	return show_help ? ED_SCRIPT_STATUS_Y : ED_SCRIPT_STATUS_Y_COLLAPSED;
}

// The document is ~1 MB (LVLEDIT_SCRIPT_MAX_LINES * MAX_LINE); file-static, not
// on any stack frame, per the plan's explicit instruction.
static script_doc script_doc_v;
static int  script_episode = 0;
static bool script_dirty = false;

static int  script_sel = 0;      // selected line index
static int  script_scroll = 0;
static int  script_field_sel = 0; // index into the selected line's field list
static bool script_inspector_open = true;

// Sub-block child depth per line (0 = top-level, 1 = owned payload row),
// recomputed each frame from lvledit_script_subblock_len() so indentation and
// the "don't treat a payload row as a command" rendering stay in sync with the
// live document after any edit.
static Uint8 script_child[LVLEDIT_SCRIPT_MAX_LINES];

// Numeric/text field entry (like the event editor's entering_number, but with
// a text mode for the ]L name field).
static bool script_entering = false;
static bool script_entering_text = false;
static char script_entry_buf[32];
static int  script_entry_len = 0;

// Modal "insert command" template picker (the I key): [/] cycle a template,
// Enter inserts it after the selection, Esc cancels. Kept to a few well-formed
// templates (exact fixed-width byte layouts) so an inserted line always parses
// -- the alternative, free-typing a whole command line with its rigid offsets,
// is far more error-prone. Section markers are NOT a template (they must go
// through N -> lvledit_script_insert_section for ordinal safety).
static bool script_insert_mode = false;
static int  script_insert_idx = 0;

typedef struct { const char *label; const char *text; } script_template;
static const script_template script_templates[] =
{
	{ "Play level",  "]L[ 0000 001 NEWLEVEL 01 01" },
	{ "Jump",        "]J 001[" },
	{ "Play music",  "]M 001[" },
	{ "Show picture","]P 001[" },
	{ "Menu song",   "]i 001[" },
	{ "Comment",     "" },
};

// Whole-document undo/redo ring. A script_doc is ~1 MB, so the cap is kept
// low (16) as the plan directs -- 2 * 16 * ~1 MB ~= 32 MB of BSS, noted and
// accepted (dwarfed by export_rgb_buf ~18 MB already present). Separate from
// the level editor's undo_stack (different type/state).
#define SCRIPT_UNDO_CAP 16
static script_doc script_undo[SCRIPT_UNDO_CAP];
static int script_undo_count = 0;
static script_doc script_redo[SCRIPT_UNDO_CAP];
static int script_redo_count = 0;

static void script_snapshot_push(script_doc *stack, int *count, const script_doc *doc)
{
	if (*count < SCRIPT_UNDO_CAP)
	{
		stack[*count] = *doc;
		++*count;
	}
	else
	{
		memmove(&stack[0], &stack[1], sizeof(script_doc) * (SCRIPT_UNDO_CAP - 1));
		stack[SCRIPT_UNDO_CAP - 1] = *doc;
	}
}

// Records the current document as an undo point (call immediately BEFORE a
// real mutation) and invalidates the redo history.
static void script_undo_push(void)
{
	script_snapshot_push(script_undo, &script_undo_count, &script_doc_v);
	script_redo_count = 0;
}

static bool script_undo_apply(void)
{
	if (script_undo_count == 0)
		return false;
	script_snapshot_push(script_redo, &script_redo_count, &script_doc_v);
	script_doc_v = script_undo[--script_undo_count];
	return true;
}

static bool script_redo_apply(void)
{
	if (script_redo_count == 0)
		return false;
	script_snapshot_push(script_undo, &script_undo_count, &script_doc_v);
	script_doc_v = script_redo[--script_redo_count];
	return true;
}

// Recomputes script_child[] for every line: a command that owns a sub-block
// (lvledit_script_subblock_len) marks its following N lines as depth-1 payload
// rows. A payload row is never itself treated as a block owner (the game's
// framing is strictly one level deep), so the scan skips past a block's own
// span before looking for the next owner.
static void script_recompute_children(void)
{
	for (int i = 0; i < script_doc_v.line_count; ++i)
		script_child[i] = 0;

	for (int i = 0; i < script_doc_v.line_count; )
	{
		int sub = lvledit_script_subblock_len(&script_doc_v, i);
		if (sub > 0)
		{
			for (int j = 1; j <= sub && i + j < script_doc_v.line_count; ++j)
				script_child[i + j] = 1;
			i += sub + 1;
		}
		else
		{
			++i;
		}
	}
}

// 1-based ordinal of the section CONTAINING line `index` (== the count of '*'
// markers at or before it; 0 if `index` precedes the first marker). If the
// line itself is a marker, that marker is counted, so this returns the ordinal
// that marker begins -- which is what N/K want as "the current section".
static int script_current_section(int index)
{
	int n = 0;
	for (int i = 0; i <= index && i < script_doc_v.line_count; ++i)
		if (script_doc_v.lines[i].text[0] == '*')
			++n;
	return n;
}

// Builds a compact one-line summary of a command for the LEFT list: the
// opcode's human name plus its key fields (numeric fields shown as "label val",
// the ]L name field shown bare). Truncated to fit `max_w` px, no ellipsis
// (TINY_FONT has no glyph reserved for one -- same reasoning as event_summary).
static void script_command_summary(const script_line *line, char *buf, size_t buf_sz, int max_w)
{
	char op = lvledit_script_opcode(line->text);
	snprintf(buf, buf_sz, "%s", lvledit_script_opcode_name(op));

	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int count = lvledit_script_line_fields(line, fields);

	for (int i = 0; i < count; ++i)
	{
		char piece[32];
		if (fields[i].is_text)
		{
			char txt[16];
			lvledit_script_field_get_text(line, i, txt, sizeof(txt));
			snprintf(piece, sizeof(piece), " %s", txt);
		}
		else if (fields[i].is_section)
		{
			snprintf(piece, sizeof(piece), " ->s%ld", lvledit_script_field_get(line, i));
		}
		else
		{
			snprintf(piece, sizeof(piece), " %ld", lvledit_script_field_get(line, i));
		}

		size_t len = strlen(buf);
		snprintf(buf + len, buf_sz - len, "%s", piece);
	}

	while (strlen(buf) > 0 && JE_textWidth(buf, TINY_FONT) > max_w)
		buf[strlen(buf) - 1] = '\0';
}

// Renders one LEFT-list row for line `index`. Section markers become a bright
// "== SEC n ==" divider; commands show name + summary; owned sub-block rows are
// indented and dimmed as raw text; inert non-command/non-marker lines are
// dimmed raw text too. Selected row is brightened.
static void draw_script_list_row(int index, int y, bool selected)
{
	const script_line *line = &script_doc_v.lines[index];
	int bright = selected ? 4 : 0;
	int list_w = (script_inspector_open ? ED_SCRIPT_DIVIDER_X : 316) - 8;

	if (lvledit_script_is_section_marker(line->text))
	{
		int ord = script_current_section(index);
		char buf[64];
		snprintf(buf, sizeof(buf), "== SECTION %d ==", ord);
		JE_outText(VGAScreen, 6, y, buf, 0, selected ? 5 : 3);
		return;
	}

	if (script_child[index])
	{
		// Owned payload row (]I item line, ]W/]Q text, ]h line): raw text,
		// indented under its owner, dim unless selected.
		char buf[80];
		snprintf(buf, sizeof(buf), "%s", line->text);
		fit_name(buf, list_w - 16);
		JE_outText(VGAScreen, 18, y, buf, 0, selected ? 4 : 1);
		return;
	}

	if (lvledit_script_is_command(line->text))
	{
		char buf[96];
		script_command_summary(line, buf, sizeof(buf), list_w - 6);
		JE_outText(VGAScreen, 6, y, buf, 0, bright);
		return;
	}

	// Inert line (comment/padding): dim raw text.
	char buf[80];
	snprintf(buf, sizeof(buf), "%s", line->text);
	fit_name(buf, list_w - 6);
	JE_outText(VGAScreen, 6, y, buf, 0, selected ? 4 : 1);
}

// Renders the RIGHT inspector for the selected line's command fields (or a
// hint if it has none). Mirrors draw_inspector_sidebar(): label left, value
// right-aligned, the field at script_field_sel boxed.
static void draw_script_inspector(void)
{
	const script_line *line = &script_doc_v.lines[script_sel];
	char op = lvledit_script_opcode(line->text);

	char header[24];
	snprintf(header, sizeof(header), "%s", lvledit_script_opcode_name(op));
	JE_outText(VGAScreen, ED_SCRIPT_LABEL_X, 13, header, 0, 4);

	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int count = lvledit_script_line_fields(line, fields);

	if (count == 0)
	{
		JE_outText(VGAScreen, ED_SCRIPT_LABEL_X, ED_SCRIPT_ROW_Y0, "(no fields)", 0, 2);
		return;
	}

	for (int i = 0; i < count; ++i)
	{
		int y = ED_SCRIPT_ROW_Y0 + i * ED_SCRIPT_ROW_H;
		bool is_cursor = (i == script_field_sel);

		char val[20];
		if (fields[i].is_text)
			lvledit_script_field_get_text(line, i, val, sizeof(val));
		else
			snprintf(val, sizeof(val), "%ld", lvledit_script_field_get(line, i));

		int val_w = JE_textWidth(val, TINY_FONT);
		int val_x = ED_SCRIPT_VALUE_RIGHT - val_w;
		int bright = is_cursor ? 8 : 0;

		if (is_cursor)
			JE_rectangle(VGAScreen, ED_SCRIPT_LABEL_X - 1, y - 1, ED_SCRIPT_VALUE_RIGHT, y + 7, 15);

		JE_outText(VGAScreen, ED_SCRIPT_LABEL_X, y, fields[i].label, 0, bright);
		JE_outText(VGAScreen, val_x, y, val, 0, bright);
	}
}

// Clamps selection/scroll/field cursor to the live document, same shape as
// clamp_event_view(): selection in range, scroll follows selection, and the
// field cursor re-clamped to the selected line's (variable) field count.
static void clamp_script_view(bool show_help)
{
	int rows = script_rows_visible(show_help);
	int count = script_doc_v.line_count;

	if (count == 0)
	{
		script_sel = script_scroll = script_field_sel = 0;
		return;
	}

	if (script_sel < 0) script_sel = 0;
	if (script_sel >= count) script_sel = count - 1;

	if (script_sel < script_scroll) script_scroll = script_sel;
	if (script_sel >= script_scroll + rows) script_scroll = script_sel - rows + 1;

	int max_scroll = count > rows ? count - rows : 0;
	if (script_scroll > max_scroll) script_scroll = max_scroll;
	if (script_scroll < 0) script_scroll = 0;

	script_field field_scratch[LVLEDIT_SCRIPT_MAX_FIELDS];
	int field_count = lvledit_script_line_fields(&script_doc_v.lines[script_sel], field_scratch);
	if (script_field_sel < 0) script_field_sel = 0;
	if (field_count == 0) script_field_sel = 0;
	else if (script_field_sel >= field_count) script_field_sel = field_count - 1;
}

static void draw_script_screen(bool show_help)
{
	SDL_FillRect(VGAScreen, NULL, 0);

	char title[64];
	snprintf(title, sizeof(title), "Script Editor - levels%d.dat", script_episode);
	JE_outText(VGAScreen, 4, 4, title, 0, 4);

	JE_outText(VGAScreen, 6, 13, "LINE", 0, 2);

	if (script_inspector_open)
		fill_rectangle_xy(VGAScreen, ED_SCRIPT_DIVIDER_X, 0, ED_SCRIPT_DIVIDER_X, script_status_y(show_help) - 4, 8);

	int count = script_doc_v.line_count;
	if (count == 0)
	{
		JE_outText(VGAScreen, 6, ED_SCRIPT_ROW_Y0, "(empty script)", 0, 2);
	}
	else
	{
		for (int row = 0; row < script_rows_visible(show_help); ++row)
		{
			int idx = script_scroll + row;
			if (idx >= count)
				break;
			draw_script_list_row(idx, ED_SCRIPT_ROW_Y0 + row * ED_SCRIPT_ROW_H, idx == script_sel);
		}

		if (script_inspector_open)
			draw_script_inspector();
	}

	char status[96];
	snprintf(status, sizeof(status), "Line %d/%d  sec %d/%d%s", count > 0 ? script_sel : -1, count,
	         script_current_section(script_sel), lvledit_script_section_count(&script_doc_v),
	         script_dirty ? "  *unsaved*" : "");
	JE_outText(VGAScreen, 4, script_status_y(show_help), status, 0, 2);

	if (script_insert_mode)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "Insert: %s   [/] change  Enter add  Esc cancel", script_templates[script_insert_idx].label);
		JE_outText(VGAScreen, 4, ED_SCRIPT_HELP_Y1, buf, 0, 4);
	}
	else if (script_entering)
	{
		char buf[48];
		snprintf(buf, sizeof(buf), "Enter %s: %s_", script_entering_text ? "text" : "value", script_entry_buf);
		JE_outText(VGAScreen, 4, ED_SCRIPT_HELP_Y1, buf, 0, 4);
	}
	else if (show_help)
	{
		JE_outText(VGAScreen, 4, ED_SCRIPT_HELP_Y1, "Up/Dn/PgUp/PgDn line L/R field +/- edit Enter type T panel", 0, 0);
		JE_outText(VGAScreen, 4, ED_SCRIPT_HELP_Y2, "I ins D del [/] move N/K sec+/- U/Y undo S save Esc", 0, 0);
	}
}

// Applies the pending field-entry buffer to the selected line's current field.
static void commit_script_entry(void)
{
	if (script_entry_len > 0 && script_doc_v.line_count > 0)
	{
		script_line *line = &script_doc_v.lines[script_sel];
		script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
		int fcount = lvledit_script_line_fields(line, fields);

		if (script_field_sel >= 0 && script_field_sel < fcount)
		{
			script_undo_push();
			if (fields[script_field_sel].is_text)
				lvledit_script_field_set_text(line, script_field_sel, script_entry_buf);
			else
				lvledit_script_field_set(line, script_field_sel, strtol(script_entry_buf, NULL, 10));
			script_dirty = true;
		}
	}

	script_entering = false;
	script_entering_text = false;
	script_entry_len = 0;
	script_entry_buf[0] = '\0';
}

// Nudges the selected line's current numeric field by delta (clamped in the
// model). No-op for a text field (must be typed via Enter) or no field.
static void script_nudge_field(long delta)
{
	if (script_doc_v.line_count == 0)
		return;

	script_line *line = &script_doc_v.lines[script_sel];
	script_field fields[LVLEDIT_SCRIPT_MAX_FIELDS];
	int fcount = lvledit_script_line_fields(line, fields);

	if (script_field_sel < 0 || script_field_sel >= fcount || fields[script_field_sel].is_text)
		return;

	long before = lvledit_script_field_get(line, script_field_sel);
	long want = before + delta;
	if (want < fields[script_field_sel].min) want = fields[script_field_sel].min;
	if (want > fields[script_field_sel].max) want = fields[script_field_sel].max;

	if (want != before)
	{
		script_undo_push();
		lvledit_script_field_set(line, script_field_sel, want);
		script_dirty = true;
	}
}

// Moves the selected line by `dir` (-1/+1). Refuses to move a '*' marker
// (section reorders must go through N/K for ordinal safety) and pre-checks the
// boundary so a no-op at the top/bottom pushes no undo snapshot. Bound to both
// Shift+Up/Down and [/].
static void script_try_move_line(int dir)
{
	if (script_doc_v.line_count == 0)
		return;

	if (lvledit_script_is_section_marker(script_doc_v.lines[script_sel].text))
	{
		show_message("USE N/K FOR SECTIONS", 20);
		return;
	}

	int j = script_sel + dir;
	if (j < 0 || j >= script_doc_v.line_count)
		return; // at a boundary -- nothing to swap with

	script_undo_push();
	lvledit_script_move_line(&script_doc_v, script_sel, dir);
	script_sel = j;
	script_dirty = true;
}

// Interactive semantic editor over levels<episode>.dat. Loads the script,
// runs its own event-pump loop, returns to level-select on Esc (confirming a
// discard if there are unsaved changes).
static void run_script_editor(int episode)
{
	script_episode = episode;

	if (!lvledit_script_load(episode, &script_doc_v))
	{
		show_message("SCRIPT LOAD FAILED", 40);
		return;
	}

	script_dirty = false;
	script_sel = 0;
	script_scroll = 0;
	script_field_sel = 0;
	script_undo_count = 0;
	script_redo_count = 0;
	script_entering = false;
	script_insert_mode = false;

	bool show_help = true;

	mouseClearInput();
	mouseWheelY = 0;

	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		script_recompute_children();
		clamp_script_view(show_help);

		bool quit = false;

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			// --- modal: insert-template picker ---
			if (script_insert_mode)
			{
				if (ki.scancode == SDL_SCANCODE_LEFTBRACKET)
					script_insert_idx = (script_insert_idx + (int)COUNTOF(script_templates) - 1) % (int)COUNTOF(script_templates);
				else if (ki.scancode == SDL_SCANCODE_RIGHTBRACKET)
					script_insert_idx = (script_insert_idx + 1) % (int)COUNTOF(script_templates);
				else if (ki.scancode == SDL_SCANCODE_RETURN || ki.scancode == SDL_SCANCODE_KP_ENTER)
				{
					script_undo_push();
					int at = (script_doc_v.line_count == 0) ? 0 : script_sel + 1;
					if (lvledit_script_insert_line(&script_doc_v, at, script_templates[script_insert_idx].text))
					{
						script_sel = at;
						script_dirty = true;
					}
					script_insert_mode = false;
				}
				else if (ki.scancode == SDL_SCANCODE_ESCAPE)
					script_insert_mode = false;
				continue;
			}

			// --- modal: field value/text entry ---
			if (script_entering)
			{
				if (ki.scancode == SDL_SCANCODE_RETURN || ki.scancode == SDL_SCANCODE_KP_ENTER)
					commit_script_entry();
				else if (ki.scancode == SDL_SCANCODE_ESCAPE)
				{
					script_entering = false;
					script_entering_text = false;
					script_entry_len = 0;
					script_entry_buf[0] = '\0';
				}
				else if (ki.scancode == SDL_SCANCODE_BACKSPACE)
				{
					if (script_entry_len > 0)
						script_entry_buf[--script_entry_len] = '\0';
				}
				else if (script_entry_len < (int)sizeof(script_entry_buf) - 1)
				{
					bool ok = script_entering_text
						? (ki.ch >= 32 && ki.ch < 127)
						: ((ki.ch >= '0' && ki.ch <= '9') || (ki.ch == '-' && script_entry_len == 0));
					if (ok)
					{
						script_entry_buf[script_entry_len++] = (char)ki.ch;
						script_entry_buf[script_entry_len] = '\0';
					}
				}
				continue;
			}

			bool fast = (ki.mod & KMOD_SHIFT) != 0;

			switch (ki.scancode)
			{
			case SDL_SCANCODE_ESCAPE:
				if (!script_dirty || confirm_discard())
					quit = true;
				break;

			case SDL_SCANCODE_F1:
				show_help = !show_help;
				break;

			case SDL_SCANCODE_F12:
				screenshot_pending = true;
				break;

			case SDL_SCANCODE_UP:
				// Shift+Up moves the selected (non-marker) line up.
				if (fast)
					script_try_move_line(-1);
				else
					--script_sel;
				break;

			case SDL_SCANCODE_DOWN:
				if (fast)
					script_try_move_line(+1);
				else
					++script_sel;
				break;

			case SDL_SCANCODE_PAGEUP:
				script_sel -= script_rows_visible(show_help);
				break;
			case SDL_SCANCODE_PAGEDOWN:
				script_sel += script_rows_visible(show_help);
				break;
			case SDL_SCANCODE_HOME:
				script_sel = 0;
				break;
			case SDL_SCANCODE_END:
				script_sel = script_doc_v.line_count - 1;
				break;

			case SDL_SCANCODE_LEFT:
			{
				script_field fs[LVLEDIT_SCRIPT_MAX_FIELDS];
				int fc = lvledit_script_line_fields(&script_doc_v.lines[script_sel], fs);
				if (fc > 0)
					script_field_sel = (script_field_sel + fc - 1) % fc;
				break;
			}
			case SDL_SCANCODE_RIGHT:
			{
				script_field fs[LVLEDIT_SCRIPT_MAX_FIELDS];
				int fc = lvledit_script_line_fields(&script_doc_v.lines[script_sel], fs);
				if (fc > 0)
					script_field_sel = (script_field_sel + 1) % fc;
				break;
			}

			case SDL_SCANCODE_EQUALS:
			case SDL_SCANCODE_KP_PLUS:
				script_nudge_field(fast ? 10 : 1);
				break;
			case SDL_SCANCODE_MINUS:
			case SDL_SCANCODE_KP_MINUS:
				script_nudge_field(fast ? -10 : -1);
				break;

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
			{
				// Open value/text entry for the current field.
				script_field fs[LVLEDIT_SCRIPT_MAX_FIELDS];
				int fc = lvledit_script_line_fields(&script_doc_v.lines[script_sel], fs);
				if (script_field_sel >= 0 && script_field_sel < fc)
				{
					script_entering = true;
					script_entering_text = fs[script_field_sel].is_text;
					script_entry_len = 0;
					script_entry_buf[0] = '\0';
				}
				break;
			}

			case SDL_SCANCODE_I:
				script_insert_mode = true;
				script_insert_idx = 0;
				break;

			case SDL_SCANCODE_D:
				if (script_doc_v.line_count == 0)
					break;
				if (lvledit_script_is_section_marker(script_doc_v.lines[script_sel].text))
				{
					show_message("USE K TO DELETE A SECTION", 25);
					break;
				}
				script_undo_push();
				if (lvledit_script_delete_line(&script_doc_v, script_sel))
					script_dirty = true;
				else
					script_undo_apply();
				break;

			// [/] move the selected (non-marker) line up/down -- a second way to
			// reorder besides Shift+Up/Down, matching the map editor's bracket
			// habit. Markers are refused (route through N/K).
			case SDL_SCANCODE_LEFTBRACKET:
				script_try_move_line(-1);
				break;
			case SDL_SCANCODE_RIGHTBRACKET:
				script_try_move_line(+1);
				break;

			case SDL_SCANCODE_N:
			{
				// New section before the current one (section-ordinal-safe).
				int at = script_current_section(script_sel);
				if (at < 1) at = 1;
				script_undo_push();
				if (lvledit_script_insert_section(&script_doc_v, at))
				{
					int m = -1, seen = 0;
					for (int i = 0; i < script_doc_v.line_count; ++i)
						if (script_doc_v.lines[i].text[0] == '*' && ++seen == at) { m = i; break; }
					if (m >= 0) script_sel = m;
					script_dirty = true;
					show_message("SECTION INSERTED", 20);
				}
				else
				{
					script_undo_apply();
					show_message("INSERT SECTION FAILED", 25);
				}
				break;
			}

			case SDL_SCANCODE_K:
			{
				// Delete the current section marker (ordinal-safe, warns on
				// any references left dangling onto the neighbor).
				int sec = script_current_section(script_sel);
				if (sec < 1)
				{
					show_message("NOT IN A SECTION", 20);
					break;
				}
				int dangling = 0;
				script_undo_push();
				if (lvledit_script_delete_section(&script_doc_v, sec, &dangling))
				{
					script_dirty = true;
					if (dangling > 0)
					{
						char msg[64];
						snprintf(msg, sizeof(msg), "SECTION DELETED - %d JUMP(S) NOW DANGLING", dangling);
						show_message(msg, 45);
					}
					else
						show_message("SECTION DELETED", 20);
				}
				else
				{
					script_undo_apply();
					show_message("DELETE SECTION FAILED", 25);
				}
				break;
			}

			case SDL_SCANCODE_T:
				script_inspector_open = !script_inspector_open;
				break;

			case SDL_SCANCODE_U:
				if (script_undo_apply())
				{
					script_dirty = true;
					show_message("UNDO", 20);
				}
				else
					show_message("NOTHING TO UNDO", 20);
				break;

			case SDL_SCANCODE_Y:
				if (script_redo_apply())
				{
					script_dirty = true;
					show_message("REDO", 20);
				}
				else
					show_message("NOTHING TO REDO", 20);
				break;

			case SDL_SCANCODE_S:
				if (lvledit_script_save(episode, &script_doc_v))
				{
					script_dirty = false;
					show_message("SAVED", 30);
				}
				else
					show_message("SAVE FAILED", 30);
				break;

			default:
				break;
			}

			if (quit)
				break;
		}

		if (quit)
			return;

		if (!script_entering && !script_insert_mode)
		{
			MouseInput mi;
			while (mouseGetInput(INPUT_NO_MOTION, &mi))
			{
				if (mi.button != SDL_BUTTON_LEFT || script_doc_v.line_count == 0)
					continue;
				if (mi.y < ED_SCRIPT_ROW_Y0)
					continue;
				int row = (mi.y - ED_SCRIPT_ROW_Y0) / ED_SCRIPT_ROW_H;
				if (row < 0 || row >= script_rows_visible(show_help))
					continue;

				if (!script_inspector_open || mi.x < ED_SCRIPT_DIVIDER_X)
				{
					int idx = script_scroll + row;
					if (idx < script_doc_v.line_count)
						script_sel = idx;
				}
				else
				{
					script_field fs[LVLEDIT_SCRIPT_MAX_FIELDS];
					int fc = lvledit_script_line_fields(&script_doc_v.lines[script_sel], fs);
					if (row >= 0 && row < fc)
						script_field_sel = row;
				}
			}

			script_sel -= take_wheel_step();
		}

		clamp_script_view(show_help);

		draw_script_screen(show_help);
		maybe_save_screenshot();
		draw_mouse_pointer();

		JE_showVGA();
		waitUntilElapsed();
	}
}

// Returns the chosen level ARCHIVE INDEX, or -1 if the user quit the editor.
// The list itself is rendered/navigated via display_order[] (row -> archive
// index) so it can be shown in play order or archive order, but every
// record is always identified by -- and this always returns -- its real
// archive index; sorting is display-only.
static int run_level_select(void)
{
	mouseClearInput();
	mouseWheelY = 0;

	build_level_summaries();

	int count = level_summary_count;
	if (count == 0)
		return -1;

	compute_play_order();
	build_display_order();

	int sel = row_for_archive_index(last_level_sel);
	int scroll = 0;
	const int rows_visible = 18;
	const int row_h = 8;

	// Fixed pixel columns: JE_outText uses a proportional font, so the old
	// "%2d  %-9s ..." space-padding never lined up. Each field is drawn at its
	// own x instead. list_y0 leaves room for the column header row above it.
	const int list_y0 = 24;
	const int col_num = 12, col_name = 44, col_map = 168, col_shp = 224;

	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		if (sel < scroll) scroll = sel;
		if (sel >= scroll + rows_visible) scroll = sel - rows_visible + 1;

		SDL_FillRect(VGAScreen, NULL, 0);

		char title[80];
		snprintf(title, sizeof(title), "Level Editor - tyrian%d.lvl - %d level(s) - sort:%s", cur_episode, count,
		         sort_mode == SORT_PLAY_ORDER ? "play" : "arch");
		JE_outText(VGAScreen, 8, 4, title, 0, 4);

		// Column header row (dim), aligned to the same fixed columns as the
		// data rows below.
		JE_outText(VGAScreen, col_num,  14, "NUM",  0, 2);
		JE_outText(VGAScreen, col_name, 14, "NAME", 0, 2);
		JE_outText(VGAScreen, col_map,  14, "MAP",  0, 2);
		JE_outText(VGAScreen, col_shp,  14, "SHP",  0, 2);

		for (int row = 0; row < rows_visible; ++row)
		{
			int r = scroll + row;
			if (r >= count)
				break;

			int idx = display_order[r];
			const char *name = (level_title[idx][0] != '\0') ? level_title[idx] : "(unnamed)";
			int y = list_y0 + row * row_h;
			int bright = (r == sel) ? 4 : 0;

			char buf[16];
			snprintf(buf, sizeof(buf), "%d", idx);
			JE_outText(VGAScreen, col_num, y, buf, 0, bright);

			JE_outText(VGAScreen, col_name, y, name, 0, bright);

			snprintf(buf, sizeof(buf), "%c", level_summaries[idx].mapFile);
			JE_outText(VGAScreen, col_map, y, buf, 0, bright);

			snprintf(buf, sizeof(buf), "%c", level_summaries[idx].shapeFile);
			JE_outText(VGAScreen, col_shp, y, buf, 0, bright);
		}

		// Two lines: the single-line form overran the 320px width once A/C
		// were added, and JE_outText has no right-edge clip (blit_sprite_hv_
		// unsafe) so the overflow spilled onto the next scanline. The list
		// ends by y~168, leaving y=182/190 free.
		JE_outText(VGAScreen, 8, 182, "Up/Down/PgUp/PgDn select, O sort, A add, C script", 0, 0);
		JE_outText(VGAScreen, 8, 190, "Enter open, Esc quit", 0, 0);

		draw_mouse_pointer();

		JE_showVGA();
		waitUntilElapsed();

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			switch (ki.scancode)
			{
			case SDL_SCANCODE_ESCAPE:
				last_level_sel = display_order[sel];
				return -1;

			case SDL_SCANCODE_UP:
				if (sel > 0) --sel;
				break;
			case SDL_SCANCODE_DOWN:
				if (sel < count - 1) ++sel;
				break;
			case SDL_SCANCODE_PAGEUP:
				sel -= rows_visible;
				if (sel < 0) sel = 0;
				break;
			case SDL_SCANCODE_PAGEDOWN:
				sel += rows_visible;
				if (sel > count - 1) sel = count - 1;
				break;

			case SDL_SCANCODE_O:
			{
				int cur_archive_index = display_order[sel];
				sort_mode = (sort_mode == SORT_PLAY_ORDER) ? SORT_ARCHIVE_INDEX : SORT_PLAY_ORDER;
				build_display_order();
				sel = row_for_archive_index(cur_archive_index);
				break;
			}

			case SDL_SCANCODE_A:
			{
				// Add level (Phase E4): clone the currently-selected row's
				// archive record and append it to the loaded archive. This
				// only touches the IN-MEMORY archive (lvledit_add_level() is
				// purely in-memory) -- the clone is not written to
				// tyrian<ep>.lvl until the user opens it and presses S,
				// which goes through save_current_level() ->
				// lvledit_save_archive() the same as any other edit. No
				// auto-save here on purpose.
				int template_index = display_order[sel];
				int new_index = lvledit_add_level(template_index);

				if (new_index >= 0)
				{
					// The archive grew, so the whole level list (summaries,
					// play order, and the display permutation derived from
					// it) needs rebuilding from scratch -- same three calls
					// run_level_select() itself does on entry.
					build_level_summaries();
					compute_play_order();
					build_display_order();

					// count is a local snapshot of level_summary_count taken
					// once at the top of this function; it must be
					// refreshed here too or the list below still renders
					// the old (pre-add) row count.
					count = level_summary_count;

					last_level_sel = new_index;
					sel = row_for_archive_index(new_index);

					show_message("LEVEL ADDED -- OPEN IT AND PRESS S TO SAVE", 40);
				}
				else
				{
					show_message("ADD LEVEL FAILED", 30);
				}
				break;
			}

			case SDL_SCANCODE_C:
				// Open the episode-script editor (levels<ep>.dat). Per-episode,
				// not per-level, so it doesn't consume the selection; on return
				// the level list is rebuilt in case play order / titles changed.
				run_script_editor(cur_episode);
				build_level_summaries();
				compute_play_order();
				build_display_order();
				count = level_summary_count;
				sel = row_for_archive_index(last_level_sel);
				mouseClearInput();
				mouseWheelY = 0;
				break;

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
				last_level_sel = display_order[sel];
				return display_order[sel];

			default:
				break;
			}
		}

		MouseInput mi;
		while (mouseGetInput(INPUT_NO_MOTION, &mi))
		{
			if (mi.button != SDL_BUTTON_LEFT || mi.y < list_y0)
				continue;
			int row = (mi.y - list_y0) / row_h;
			if (row < 0 || row >= rows_visible)
				continue;
			int r = scroll + row;
			if (r >= count)
				continue;
			if (r == sel)   // click already-selected row = open it
			{
				last_level_sel = display_order[sel];
				return display_order[sel];
			}
			sel = r;
		}

		sel -= take_wheel_step();
		if (sel < 0) sel = 0;
		if (sel > count - 1) sel = count - 1;
	}
}

// ---------------------------------------------------------------------
// Episode select screen
// ---------------------------------------------------------------------

// Loads episode's archive as the "current" one (cur_episode/cur_lvl_filename),
// applies the fixed flight-tileset palette, and resets the level-parse state
// so a fresh run_level_select()/run_map_editor() start clean. Shared by the
// interactive lvledit_run() and the headless CLI entry points below.
static bool load_episode_archive(int episode)
{
	cur_episode = episode;
	snprintf(cur_lvl_filename, sizeof(cur_lvl_filename), "tyrian%d.lvl", episode);

	if (!lvledit_load_archive(cur_lvl_filename))
	{
		fprintf(stderr, "lvledit: failed to load %s\n", cur_lvl_filename);
		return false;
	}

	// Flight tilesets are always drawn under palette index 5 (palette.dat),
	// not palette 0. Traced via JE_loadMap()'s caller: JE_main() (tyrian2.c
	// ~878) does `JE_loadPic(VGAScreen, twoPlayerMode ? 6 : 3, false)`
	// immediately before every level's flight starts; JE_loadPic()
	// (picload.c) resolves that PCX number through pcxpal[] (pcxmast.c:
	// `{0,7,5,8,10,5,...}`) to `colors = palettes[pcxpal[PCXnumber-1]]`,
	// which for both PCX 3 (1P) and PCX 6 (2P) is pcxpal[2]==pcxpal[5]==5.
	// (JE_gammaCorrect's in-flight gamma cycle, mainint.c:1432, resets to
	// the same `palettes[pcxpal[3-1]]` for the same reason.) This is a
	// fixed engine constant, not derivable from -- or stored in -- the
	// .lvl record itself, so there's nothing level-specific to read; we
	// just match the constant the real flight loop always uses.
	set_palette(palettes[5], 0, 255);

	cur_level_index = -1;
	tileset_loaded = false;

	return true;
}

// ---------------------------------------------------------------------
// Add-episode scaffolding (Phase E6)
// ---------------------------------------------------------------------
//
// Design decision (see internal/plan/LEVEL_EDITOR_PLAN.md's E6 section for
// the full rationale): a new episode is scaffolded by CLONING episode 4's
// three data files rather than hand-authoring a minimal archive/script.
// Two engine facts force this:
//   - Per-episode data is exactly three files: tyrian<n>.lvl (levels +
//     enemy/item tables), levels<n>.dat (interlevel script), cubetxt<n>.dat
//     (story-cube text) -- JE_initEpisode() (episodes.c) composes exactly
//     these names.
//   - Episodes >= 4 read their item/enemy/ship tables from the ARCHIVE'S
//     OWN trailing blob rather than tyrian.hdt (episodes.c:76,
//     `fseek(f, lvlPos[lvlNum-1], ...)`), and tyrian4.lvl is the only
//     archive whose trailing entry is a real item-data blob (episodes 1-3's
//     trailing entry is just an EOF marker). A new episode 5 is >= 4, so it
//     MUST inherit a valid trailing blob -- only episode 4 has one to give.
// Cloning episode 4 also hands the new episode a valid, immediately
// playable levels4.dat script for free, so the scaffolded episode boots and
// plays out of the box; the author then customizes it with the tools that
// already exist (tile/event editor, add-level, the semantic script editor,
// F5 playtest). A hand-authored "one level + minimal script" archive was
// considered and rejected: it is error-prone and its correctness (shop
// menus, end sequence) can't be validated headlessly -- it needs an actual
// playthrough.

// Buffered byte-for-byte copy of one data_dir()-relative file to another,
// mirroring the .bak-creation loop in save_current_level(). Returns false
// (leaving whatever was written so far) if the source can't be opened, the
// destination can't be created, or a write comes up short.
static bool copy_data_file(const char *src_name, const char *dst_name)
{
	FILE *src = dir_fopen(data_dir(), src_name, "rb");
	if (src == NULL)
		return false;

	FILE *dst = dir_fopen(data_dir(), dst_name, "wb");
	if (dst == NULL)
	{
		fclose(src);
		return false;
	}

	bool ok = true;
	Uint8 buf[8192];
	size_t n;
	while (ok && (n = fread(buf, 1, sizeof(buf), src)) > 0)
		ok = (fwrite(buf, 1, n, dst) == n);

	fclose(dst);
	fclose(src);
	return ok;
}

// Scaffolds a new episode by cloning episode 4's three data files (the only
// episode whose .lvl carries the item-data trailing blob that episodes >= 4
// load from). new_ep must be the lowest missing slot in [1, EPISODE_MAX] and
// must not already exist. Returns true on success. Writes into data_dir().
//
// Refuses (returns false, writing nothing) if new_ep is out of range, if
// tyrian<new_ep>.lvl already exists (never clobber), or if any of episode
// 4's three source files is missing -- checked up front so a missing source
// can't leave a partial episode behind. A failure partway through the three
// copies (e.g. disk full) can still leave a partial scaffold; the caller is
// expected to treat any false return as "creation failed", not "partially
// created, try to recover".
static bool scaffold_new_episode(int new_ep)
{
	if (new_ep < 1 || new_ep > EPISODE_MAX)
		return false;

	char dst_lvl[32], dst_script[32], dst_cube[32];
	snprintf(dst_lvl, sizeof(dst_lvl), "tyrian%d.lvl", new_ep);
	snprintf(dst_script, sizeof(dst_script), "levels%d.dat", new_ep);
	snprintf(dst_cube, sizeof(dst_cube), "cubetxt%d.dat", new_ep);

	if (dir_file_exists(data_dir(), dst_lvl))
		return false;  // never clobber an existing episode

	static const char *SRC_LVL = "tyrian4.lvl";
	static const char *SRC_SCRIPT = "levels4.dat";
	static const char *SRC_CUBE = "cubetxt4.dat";

	if (!dir_file_exists(data_dir(), SRC_LVL) ||
	    !dir_file_exists(data_dir(), SRC_SCRIPT) ||
	    !dir_file_exists(data_dir(), SRC_CUBE))
		return false;  // episode 4 itself is missing -- nothing to clone

	if (!copy_data_file(SRC_LVL, dst_lvl))
		return false;
	if (!copy_data_file(SRC_SCRIPT, dst_script))
		return false;
	if (!copy_data_file(SRC_CUBE, dst_cube))
		return false;

	return true;
}

// Resolves row `row` of the episode picker (0-based: existing episodes in
// shown_eps[0..shown_count-1], then the "+ New Episode" row at index
// new_row if has_new_row). For an existing episode's row, just returns its
// episode number. For the new-episode row, scaffolds it on the spot: on
// success returns the freshly-created episode number; on failure shows a
// brief toast (show_message()) and returns 0 -- 0 is never a valid episode
// number, so the caller treats it as "stay in the picker".
static int activate_episode_row(int row, const int *shown_eps, int shown_count,
                                 bool has_new_row, int new_row, int new_ep)
{
	if (has_new_row && row == new_row)
	{
		if (scaffold_new_episode(new_ep))
			return new_ep;

		show_message("NEW EPISODE SCAFFOLD FAILED", 40);
		return 0;
	}

	if (row >= 0 && row < shown_count)
		return shown_eps[row];

	return 0;
}

// Remembers the last-selected episode across re-entries into
// run_episode_select() within one editor session, same rationale as
// last_level_sel above. Stored as an episode NUMBER (not a row index),
// since the row layout is rebuilt fresh every entry (episodes may have been
// added by the New-Episode row since the last visit).
static int last_episode_sel = 1;

// Returns the chosen episode (1-EPISODE_MAX), or -1 if the user quit the
// editor. The episode list is fully dynamic: only slots that actually have
// a tyrian<e>.lvl are shown as openable rows, and if any slot in
// [1, EPISODE_MAX] is still free, an extra "+ New Episode" row appears
// below the list (scaffold_new_episode(), above) targeting the LOWEST free
// slot. Choosing that row scaffolds the episode and returns its number
// immediately, same as choosing an existing episode -- lvledit_run() then
// calls load_episode_archive() on it exactly as it would for any other
// pick, so the freshly-scaffolded files are what gets loaded (no separate
// "re-probe" step is needed here: the next time this function is entered,
// the top-of-function scan below sees the new file on disk and lists it
// normally).
static int run_episode_select(void)
{
	mouseClearInput();
	mouseWheelY = 0;

	// Per-episode level counts, probed once per entry into this screen, for
	// display only ("(N levels)" / "(?)" on failure). This clobbers the
	// shared archive blob/cur_episode/cur_lvl_filename as a side effect, but
	// that's harmless: whichever episode the user picks gets reloaded fresh
	// by load_episode_archive() right after this function returns.
	//
	// Also determines, in the same pass, which slots exist at all (only
	// those are shown as openable rows) and the lowest free slot in
	// [1, EPISODE_MAX] (if any) for the "+ New Episode" row.
	int counts[EPISODE_MAX + 1]; // index by episode 1..EPISODE_MAX; [0] unused
	int shown_eps[EPISODE_MAX];  // episode numbers that exist, in order
	int shown_count = 0;
	int new_ep = 0;              // lowest missing slot, or 0 if none free

	for (int e = 1; e <= EPISODE_MAX; ++e)
	{
		char fname[32];
		snprintf(fname, sizeof(fname), "tyrian%d.lvl", e);

		if (dir_file_exists(data_dir(), fname))
		{
			counts[e] = lvledit_load_archive(fname) ? lvledit_level_count() : -1;
			shown_eps[shown_count++] = e;
		}
		else
		{
			counts[e] = -1;
			if (new_ep == 0)
				new_ep = e;
		}
	}

	bool has_new_row = (new_ep != 0);
	int new_row = has_new_row ? shown_count : -1;
	int row_count = shown_count + (has_new_row ? 1 : 0);

	// Translate the remembered episode number into a row index for this
	// visit's (possibly different) layout; default to row 0 if it's not
	// currently shown (defensive -- the editor never deletes an episode, so
	// this shouldn't actually happen).
	int sel = 0;
	for (int r = 0; r < shown_count; ++r)
		if (shown_eps[r] == last_episode_sel)
			sel = r;

	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		SDL_FillRect(VGAScreen, NULL, 0);

		JE_outText(VGAScreen, 8, 4, "Level Editor - select episode", 0, 4);

		for (int r = 0; r < shown_count; ++r)
		{
			int e = shown_eps[r];
			char line[64];
			if (counts[e] >= 0)
				snprintf(line, sizeof(line), "Episode %d   tyrian%d.lvl  (%d levels)", e, e, counts[e]);
			else
				snprintf(line, sizeof(line), "Episode %d   tyrian%d.lvl  (?)", e, e);

			JE_outText(VGAScreen, 16, 16 + r * 8, line, 0, r == sel ? 4 : 0);
		}

		if (has_new_row)
		{
			char line[64];
			snprintf(line, sizeof(line), "+ New Episode (episode %d)", new_ep);
			JE_outText(VGAScreen, 16, 16 + new_row * 8, line, 0, new_row == sel ? 4 : 0);
		}

		JE_outText(VGAScreen, 8, 190, "Up/Down select, Enter open/create, Esc quit editor", 0, 0);

		draw_mouse_pointer();

		JE_showVGA();
		waitUntilElapsed();

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			switch (ki.scancode)
			{
			case SDL_SCANCODE_ESCAPE:
				last_episode_sel = (shown_count > 0 && sel < shown_count) ? shown_eps[sel] : last_episode_sel;
				return -1;

			case SDL_SCANCODE_UP:
				if (sel > 0) --sel;
				break;
			case SDL_SCANCODE_DOWN:
				if (sel < row_count - 1) ++sel;
				break;

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
			{
				int chosen = activate_episode_row(sel, shown_eps, shown_count, has_new_row, new_row, new_ep);
				if (chosen != 0)
				{
					last_episode_sel = chosen;
					return chosen;
				}
				mouseClearInput();
				mouseWheelY = 0;
				break;  // scaffold failed (toast already shown) -- stay in the picker
			}

			default:
				break;
			}
		}

		MouseInput mi;
		while (mouseGetInput(INPUT_NO_MOTION, &mi))
		{
			if (mi.button != SDL_BUTTON_LEFT || mi.y < 16)
				continue;
			int r = (mi.y - 16) / 8;
			if (r < 0 || r >= row_count)
				continue;
			if (r == sel)   // click already-selected = open/create
			{
				int chosen = activate_episode_row(sel, shown_eps, shown_count, has_new_row, new_row, new_ep);
				if (chosen != 0)
				{
					last_episode_sel = chosen;
					return chosen;
				}
				mouseClearInput();
				mouseWheelY = 0;
				continue;  // scaffold failed (toast already shown) -- stay in the picker
			}
			sel = r;
		}

		sel -= take_wheel_step();
		if (sel < 0) sel = 0;
		if (sel > row_count - 1) sel = row_count - 1;
	}
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

// episode: 1-EPISODE_MAX to boot straight into that episode's level-select,
// or 0 (or any out-of-range value) to start at the in-app episode picker
// instead. EPISODE_MAX itself may not exist yet on disk -- run_episode_select()
// only offers it as an openable row once scaffold_new_episode() has created it
// (via its "+ New Episode" row), same as any other episode.
void lvledit_run(int episode)
{
	bool saved_hd_mode = hd_mode;
	hd_mode = false;  // classic 320x200 tool only, regardless of config

	// Render the episode picker under a known palette even before any
	// archive has been loaded; load_episode_archive() re-applies this same
	// palette once an episode is chosen, which is harmless.
	set_palette(palettes[5], 0, 255);

	int ep = episode;
	for (;;)
	{
		if (ep < 1 || ep > EPISODE_MAX)
		{
			ep = run_episode_select();
			if (ep < 0)
				break;  // Esc from the episode picker => quit editor
		}

		if (!load_episode_archive(ep))
		{
			ep = 0;  // failed load => back to the episode picker
			continue;
		}

		last_level_sel = 0;  // fresh episode: don't carry a stale selection

		for (;;)
		{
			int idx = run_level_select();
			if (idx < 0)
				break;  // Esc from level-select => back to episode picker

			run_map_editor(idx);
		}

		ep = 0;  // back out to the episode picker after leaving level-select
	}

	hd_mode = saved_hd_mode;
}

// ---------------------------------------------------------------------
// Shared headless setup (--edit-shot and --edit-export)
// ---------------------------------------------------------------------

// Common preamble for the headless CLI entry points below: forces classic
// mode, loads the archive via load_episode_archive() (see the comment above
// its set_palette() call for why palette 5 specifically), and parses+
// tileset-loads level_index into cur_level via load_level_for_edit(). On
// success, *out_saved_hd_mode carries the caller's
// original hd_mode (to be restored when the caller is done) and the function
// returns true. On any failure, hd_mode is already restored before
// returning false, so the caller should just propagate the failure.
static bool headless_load_for_export(int episode, int level_index, bool *out_saved_hd_mode)
{
	*out_saved_hd_mode = hd_mode;
	hd_mode = false;  // classic 320x200 tool only, regardless of config

	if (!load_episode_archive(episode))
	{
		hd_mode = *out_saved_hd_mode;
		return false;
	}

	if (!load_level_for_edit(level_index))
	{
		fprintf(stderr, "lvledit: failed to parse level %d\n", level_index);
		hd_mode = *out_saved_hd_mode;
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------
// Headless screenshot dump (--edit-shot)
// ---------------------------------------------------------------------

// Saves the screen render currently sitting in VGAScreen as
// lvledit_shot_ep<episode>_lvl<level_index>_<suffix>.bmp in the current
// directory, printing the path to stdout on success.
static void dump_current_screen(int episode, int level_index, const char *suffix)
{
	char path[80];
	snprintf(path, sizeof(path), "lvledit_shot_ep%d_lvl%d_%s.bmp", episode, level_index, suffix);

	if (save_screenshot_bmp(path))
		printf("%s\n", path);
	else
		fprintf(stderr, "lvledit: failed to save %s\n", path);
}

void lvledit_dump_screens(int episode, int level_index)
{
	bool saved_hd_mode;
	if (!headless_load_for_export(episode, level_index, &saved_hd_mode))
		return;

	const bool show_help = true;

	// One map screen per layer, each anchored at the bottom of the map (the
	// game-flow start view) -- the same state load_level_for_edit() sets up
	// for layer 0, replicated here for layers 1 and 2 (mirrors the Tab
	// layer-switch re-anchor logic in run_map_editor()).
	for (int layer = 0; layer < 3; ++layer)
	{
		active_layer = layer;
		cursor_x = 0;
		cursor_y = layer_height(layer) - 1;
		scroll_x = 0;
		scroll_y = 0;
		clamp_view();

		render_map_screen(show_help);
		JE_showVGA();

		char suffix[16];
		snprintf(suffix, sizeof(suffix), "layer%d", layer + 1);
		dump_current_screen(episode, level_index, suffix);
	}

	// Event editor, first page.
	event_sel = 0;
	event_scroll = 0;
	clamp_event_view(show_help);

	draw_event_screen(show_help);
	JE_showVGA();

	dump_current_screen(episode, level_index, "events");

	hd_mode = saved_hd_mode;
}

// ---------------------------------------------------------------------
// Headless full-map export (--edit-export)
// ---------------------------------------------------------------------

bool lvledit_export_map_cli(int episode, int level_index)
{
	bool saved_hd_mode;
	if (!headless_load_for_export(episode, level_index, &saved_hd_mode))
		return false;

	bool ok = export_full_map(episode);

	hd_mode = saved_hd_mode;
	return ok;
}

// ---------------------------------------------------------------------
// Headless add-episode scaffold self-test (--edit-addepisode-test, Phase E6)
// ---------------------------------------------------------------------

// Byte-for-byte compares two data_dir()-relative files (size, then content).
// Used below to prove the scaffolded files really are exact copies of
// episode 4's, the way scaffold_new_episode()'s copy_data_file() intends.
static bool files_identical(const char *name_a, const char *name_b)
{
	FILE *a = dir_fopen(data_dir(), name_a, "rb");
	FILE *b = dir_fopen(data_dir(), name_b, "rb");

	if (a == NULL || b == NULL)
	{
		if (a != NULL) fclose(a);
		if (b != NULL) fclose(b);
		return false;
	}

	fseek(a, 0, SEEK_END);
	fseek(b, 0, SEEK_END);
	long len_a = ftell(a);
	long len_b = ftell(b);
	fseek(a, 0, SEEK_SET);
	fseek(b, 0, SEEK_SET);

	bool same = (len_a >= 0) && (len_a == len_b);

	Uint8 buf_a[8192], buf_b[8192];
	size_t n;
	while (same && (n = fread(buf_a, 1, sizeof(buf_a), a)) > 0)
	{
		if (fread(buf_b, 1, n, b) != n || memcmp(buf_a, buf_b, n) != 0)
			same = false;
	}

	fclose(a);
	fclose(b);
	return same;
}

// Hidden --edit-addepisode-test self-test (Phase E6): proves
// scaffold_new_episode() end to end, then cleans up after itself so the
// data directory (data_dir(), e.g. the gitignored tyrian21/ for a
// --data ./tyrian21 run) is left exactly as found -- the point of this test
// is to validate the scaffold, not to actually add episode content; the user
// is expected to create their episode 5 for real through the in-app picker.
//
// No episode argument (unlike the other --edit-* self-tests): the target is
// always the lowest missing slot in [1, EPISODE_MAX], same rule the picker's
// "+ New Episode" row uses. If every slot already exists, there is nothing
// to test without risking real user content, so this prints a SKIP line and
// returns true rather than refusing to run.
bool lvledit_run_addepisode_test(void)
{
	int new_ep = 0;
	for (int e = 1; e <= EPISODE_MAX; ++e)
	{
		char fname[32];
		snprintf(fname, sizeof(fname), "tyrian%d.lvl", e);
		if (!dir_file_exists(data_dir(), fname))
		{
			new_ep = e;
			break;
		}
	}

	if (new_ep == 0)
	{
		printf("edit-addepisode: all episode slots full -- SKIP\n");
		return true;
	}

	printf("edit-addepisode: scaffolding episode %d by cloning episode 4's files\n", new_ep);

	bool all_ok = true;

	bool scaffold_ok = scaffold_new_episode(new_ep);
	printf("  scaffold_new_episode(%d): %s\n", new_ep, scaffold_ok ? "PASS" : "FAIL");
	all_ok = all_ok && scaffold_ok;

	char new_lvl[32], new_script[32], new_cube[32];
	snprintf(new_lvl, sizeof(new_lvl), "tyrian%d.lvl", new_ep);
	snprintf(new_script, sizeof(new_script), "levels%d.dat", new_ep);
	snprintf(new_cube, sizeof(new_cube), "cubetxt%d.dat", new_ep);

	bool files_exist = scaffold_ok &&
	                    dir_file_exists(data_dir(), new_lvl) &&
	                    dir_file_exists(data_dir(), new_script) &&
	                    dir_file_exists(data_dir(), new_cube);
	printf("  scaffolded files exist (%s, %s, %s): %s\n", new_lvl, new_script, new_cube,
	       files_exist ? "PASS" : "FAIL");
	all_ok = all_ok && files_exist;

	bool archive_ok = files_exist && lvledit_load_archive(new_lvl) && lvledit_level_count() > 0;
	printf("  lvledit_load_archive(%s) + level_count() > 0: %s\n", new_lvl, archive_ok ? "PASS" : "FAIL");
	all_ok = all_ok && archive_ok;

	bool identical_lvl = files_exist && files_identical(new_lvl, "tyrian4.lvl");
	printf("  %s byte-identical to tyrian4.lvl: %s\n", new_lvl, identical_lvl ? "PASS" : "FAIL");
	all_ok = all_ok && identical_lvl;

	bool identical_script = files_exist && files_identical(new_script, "levels4.dat");
	printf("  %s byte-identical to levels4.dat: %s\n", new_script, identical_script ? "PASS" : "FAIL");
	all_ok = all_ok && identical_script;

	bool identical_cube = files_exist && files_identical(new_cube, "cubetxt4.dat");
	printf("  %s byte-identical to cubetxt4.dat: %s\n", new_cube, identical_cube ? "PASS" : "FAIL");
	all_ok = all_ok && identical_cube;

	// Clean up unconditionally and best-effort: whatever scaffold_new_episode()
	// managed to write must not survive this self-test. remove() silently
	// no-ops on a path that was never created (e.g. scaffold_ok was false
	// partway through), which is fine here.
	char full_path[512];
	snprintf(full_path, sizeof(full_path), "%s/%s", data_dir(), new_lvl);
	remove(full_path);
	snprintf(full_path, sizeof(full_path), "%s/%s", data_dir(), new_script);
	remove(full_path);
	snprintf(full_path, sizeof(full_path), "%s/%s", data_dir(), new_cube);
	remove(full_path);

	printf("edit-addepisode: %s\n", all_ok ? "ALL PASS" : "FAIL");
	return all_ok;
}
