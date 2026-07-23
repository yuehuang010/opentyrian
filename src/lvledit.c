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

#include "file.h"
#include "fonthand.h"
#include "keyboard.h"
#include "nortsong.h"
#include "opentyr.h"
#include "palette.h"
#include "sprite.h"
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

static int event_sel = 0;
static int event_scroll = 0;
static int event_field = 0;  // 0..7: time, type, dat, dat2, dat3, dat4, dat5, dat6

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
		JE_outText(VGAScreen, 4, ED_HELP_Y2, "P pick [/] tile T panel U undo R redo E events S save Esc back", 0, 0);
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
				return true;
			if (ki.scancode == SDL_SCANCODE_N || ki.scancode == SDL_SCANCODE_ESCAPE)
				return false;
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

// Field ranges, matching lvledit_event's on-disk types (lvledit_io.h).
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

// Fixed pixel columns for the event table. The header row (draw_event_screen)
// and every data row (draw_event_row) draw from this single table, so they
// cannot drift apart the way the old sequential-x layout did (column x used
// to depend on the rendered width of *every preceding cell's content*, so
// no two rows lined up).
//
// Usable x is 4..316 (312px of the 320px-wide VGAScreen). TINY_FONT is a
// proportional bitmap font (JE_outText advances by sprite->width + 1 per
// glyph -- see fonthand.c), so widths below are measured via JE_textWidth()
// against real worst-case content, not guessed:
//   IDX   4 digits, event cap is LVLEDIT_MAX_EVENT (2500) -> max index 2499  => 20px
//   TIME  u16 0..65535, 5 digits                                            => 25px
//   TY    u8 event type 0..255, 3 digits                                    => 15px
//   NAME  event_type_name(): longest entries are 15 chars; measured every
//         type slot 0..89 for the true worst pixel width, not just char
//         count (proportional font, so char count alone doesn't determine
//         width) -- "ENEMY GLB ARMOR" is the widest at                      => 80px
//   DAT   dat,  s16 -32768..32767, worst case "-32768"                      => 29px
//   D2    dat2, s16 -32768..32767, same worst case                         => 29px
//   D3    dat3, s8  -128..127, worst case "-128"                            => 18px
//   D4    dat4, u8  0..255, worst case "255"                                => 15px
//   D5    dat5, s8  -128..127, worst case "-128"                            => 18px
//   D6    dat6, s8  -128..127, worst case "-128"                            => 18px
// Content sums to 267px; the remaining 45px of the 312px budget is spent as
// a flat 5px gap after each of the 9 non-final columns, which lands the
// last column's right edge exactly on x=316 -- the fit is exact, not
// comfortable, so don't add columns or widen any of these without
// re-measuring. Every header title (incl. the DAT2->D2 etc. shortening)
// already fits inside its column's w, so NAME is never truncated in
// practice; fit_name() below is just a defensive backstop.
static const struct { int x, w; const char *title; } ED_EVENT_COLS[10] =
{
	{   4, 20, "IDX"  },
	{  29, 25, "TIME" },
	{  59, 15, "TY"   },
	{  79, 80, "NAME" },
	{ 164, 29, "DAT"  },
	{ 198, 29, "D2"   },
	{ 232, 18, "D3"   },
	{ 255, 15, "D4"   },
	{ 275, 18, "D5"   },
	{ 298, 18, "D6"   },
};

// Defensive backstop for the NAME column: measurement shows no current
// event_type_name() entry exceeds the 80px column width, but if a future
// entry ever did, silently overflowing past x=316 would be worse than a
// clipped label. Trims from the end until it fits (no ellipsis -- TINY_FONT
// has no glyph reserved for one).
static void fit_name(char *buf, int max_w)
{
	size_t len = strlen(buf);
	while (len > 0 && JE_textWidth(buf, TINY_FONT) > max_w)
	{
		buf[--len] = '\0';
	}
}

// Draws one event row, highlighting the field cursor (if this is the
// selected row) by redrawing just that field's text in a brighter color.
// Numeric columns are right-aligned within their column box (x + w -
// textwidth); NAME is left-aligned. The cursor highlight rectangle snaps to
// the column box (col.x-1 .. col.x+col.w), not to the text's rendered
// width, so it stays put as the value's digit count changes.
static void draw_event_row(int index, int y, bool selected)
{
	const lvledit_event *ev = &cur_level.event[index];

	char prefix[8], f_time[8], f_type[6], name_buf[20], f_dat[8], f_dat2[8], f_dat3[6], f_dat4[6], f_dat5[6], f_dat6[6];

	snprintf(prefix, sizeof(prefix), "%d", index);
	snprintf(f_time, sizeof(f_time), "%u", ev->time);
	snprintf(f_type, sizeof(f_type), "%u", ev->type);
	snprintf(name_buf, sizeof(name_buf), "%s", event_type_name(ev->type));
	snprintf(f_dat, sizeof(f_dat), "%d", ev->dat);
	snprintf(f_dat2, sizeof(f_dat2), "%d", ev->dat2);
	snprintf(f_dat3, sizeof(f_dat3), "%d", ev->dat3);
	snprintf(f_dat4, sizeof(f_dat4), "%u", ev->dat4);
	snprintf(f_dat5, sizeof(f_dat5), "%d", ev->dat5);
	snprintf(f_dat6, sizeof(f_dat6), "%d", ev->dat6);

	// parts[i]/field[i] line up 1:1 with ED_EVENT_COLS[i]. field[i] is the
	// editable field index (0-7), or -1 for non-editable columns (IDX
	// prefix, NAME label).
	const char *parts[] = { prefix, f_time, f_type, name_buf, f_dat, f_dat2, f_dat3, f_dat4, f_dat5, f_dat6 };
	const int   field[] = { -1,     0,      1,      -1,       2,     3,      4,      5,      6,      7      };
	const bool  left_align[] = { false, false, false, true, false, false, false, false, false, false };

	fit_name(name_buf, ED_EVENT_COLS[3].w);

	int base_bright = selected ? 4 : 0;

	for (size_t i = 0; i < COUNTOF(parts); ++i)
	{
		const int col_x = ED_EVENT_COLS[i].x;
		const int col_w = ED_EVENT_COLS[i].w;
		bool is_cursor_field = selected && field[i] == event_field;

		int text_w = JE_textWidth(parts[i], TINY_FONT);
		int x = left_align[i] ? col_x : col_x + col_w - text_w;

		if (is_cursor_field)
		{
			JE_rectangle(VGAScreen, col_x - 1, y - 1, col_x + col_w, y + 7, 15);
			JE_outText(VGAScreen, x, y, parts[i], 0, 8);
		}
		else
		{
			JE_outText(VGAScreen, x, y, parts[i], 0, base_bright);
		}
	}
}

static void clamp_event_view(void)
{
	int count = cur_level.event_count;

	if (count == 0)
	{
		event_sel = 0;
		event_scroll = 0;
		return;
	}

	if (event_sel < 0) event_sel = 0;
	if (event_sel >= count) event_sel = count - 1;

	if (event_sel < event_scroll) event_scroll = event_sel;
	if (event_sel >= event_scroll + ED_EVENT_ROWS_VISIBLE) event_scroll = event_sel - ED_EVENT_ROWS_VISIBLE + 1;

	int max_scroll = count > ED_EVENT_ROWS_VISIBLE ? count - ED_EVENT_ROWS_VISIBLE : 0;
	if (event_scroll > max_scroll) event_scroll = max_scroll;
	if (event_scroll < 0) event_scroll = 0;
}

static void draw_event_screen(bool show_help)
{
	SDL_FillRect(VGAScreen, NULL, 0);

	char title[64];
	snprintf(title, sizeof(title), "Event Editor - Lvl %d", cur_level_index);
	JE_outText(VGAScreen, 4, 4, title, 0, 4);

	// Same ED_EVENT_COLS table the data rows draw from (draw_event_row), so
	// the header can't drift out of alignment with the columns below it.
	for (size_t i = 0; i < COUNTOF(ED_EVENT_COLS); ++i)
		JE_outText(VGAScreen, ED_EVENT_COLS[i].x, 13, ED_EVENT_COLS[i].title, 0, 2);

	int count = cur_level.event_count;

	if (count == 0)
	{
		JE_outText(VGAScreen, 4, ED_EVENT_ROW_Y0, "(no events -- press I to insert one)", 0, 2);
	}
	else
	{
		for (int row = 0; row < ED_EVENT_ROWS_VISIBLE; ++row)
		{
			int idx = event_scroll + row;
			if (idx >= count)
				break;

			draw_event_row(idx, ED_EVENT_ROW_Y0 + row * ED_EVENT_ROW_H, idx == event_sel);
		}
	}

	char status[96];
	snprintf(status, sizeof(status), "Event %d/%d (cap %d)%s", count > 0 ? event_sel : -1, count, LVLEDIT_MAX_EVENT,
	         level_dirty ? "  *unsaved*" : "");
	JE_outText(VGAScreen, 4, ED_EVENT_STATUS_Y, status, 0, 2);

	if (entering_number)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "Enter value: %s_", number_entry_buf);
		JE_outText(VGAScreen, 4, ED_EVENT_HELP_Y1, buf, 0, 4);
	}
	else if (show_help)
	{
		JE_outText(VGAScreen, 4, ED_EVENT_HELP_Y1, "Up/Dn/PgUp/PgDn/Home/End row L/R field +/- (Shift=10) adj", 0, 0);
		JE_outText(VGAScreen, 4, ED_EVENT_HELP_Y2, "Enter val I ins D del R sort U undo Y redo S save Esc back", 0, 0);
	}
}

// Applies the pending numeric-entry buffer to the selected event's current
// field, clamped to that field's real range. No-op if the buffer is empty
// (bare Enter, or everything got backspaced away).
static void commit_number_entry(void)
{
	if (number_entry_len > 0 && cur_level.event_count > 0)
	{
		long v = strtol(number_entry_buf, NULL, 10);
		lvledit_event *ev = &cur_level.event[event_sel];
		long before = event_field_get(ev, event_field);

		// Pre-clamp so we can tell (before mutating anything) whether this
		// will actually change the field -- mirrors event_field_set()'s own
		// clamping, so a no-op entry (e.g. re-entering the same value, or a
		// value that clamps back to `before`) pushes no undo step.
		long lo, hi;
		event_field_range(event_field, &lo, &hi);
		if (v < lo) v = lo;
		if (v > hi) v = hi;

		if (v != before)
		{
			undo_push();
			event_field_set(ev, event_field, v);
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

	clamp_event_view();

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
				event_sel -= ED_EVENT_ROWS_VISIBLE;
				break;
			case SDL_SCANCODE_PAGEDOWN:
				event_sel += ED_EVENT_ROWS_VISIBLE;
				break;
			case SDL_SCANCODE_HOME:
				event_sel = 0;
				break;
			case SDL_SCANCODE_END:
				event_sel = cur_level.event_count - 1;
				break;

			case SDL_SCANCODE_LEFT:
				event_field = (event_field + 7) % 8;
				break;
			case SDL_SCANCODE_RIGHT:
				event_field = (event_field + 1) % 8;
				break;

			case SDL_SCANCODE_EQUALS:
			case SDL_SCANCODE_KP_PLUS:
				if (cur_level.event_count > 0)
				{
					lvledit_event *ev = &cur_level.event[event_sel];
					long before = event_field_get(ev, event_field);

					long lo, hi;
					event_field_range(event_field, &lo, &hi);
					long want = before + (fast ? 10 : 1);
					if (want < lo) want = lo;
					if (want > hi) want = hi;

					if (want != before)
					{
						undo_push();
						event_field_set(ev, event_field, want);
						level_dirty = true;
					}
				}
				break;

			case SDL_SCANCODE_MINUS:
			case SDL_SCANCODE_KP_MINUS:
				if (cur_level.event_count > 0)
				{
					lvledit_event *ev = &cur_level.event[event_sel];
					long before = event_field_get(ev, event_field);

					long lo, hi;
					event_field_range(event_field, &lo, &hi);
					long want = before - (fast ? 10 : 1);
					if (want < lo) want = lo;
					if (want > hi) want = hi;

					if (want != before)
					{
						undo_push();
						event_field_set(ev, event_field, want);
						level_dirty = true;
					}
				}
				break;

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

			case SDL_SCANCODE_U:
				if (undo_apply())
				{
					level_dirty = true;
					clamp_event_view();
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
					clamp_event_view();
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

		clamp_event_view();

		draw_event_screen(show_help);
		maybe_save_screenshot();

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

static void run_map_editor(int level_index)
{
	if (!load_level_for_edit(level_index))
	{
		fprintf(stderr, "lvledit: failed to parse level %d\n", level_index);
		return;
	}

	bool show_help = true;

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

		clamp_view();

		render_map_screen(show_help);
		maybe_save_screenshot();

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

// Returns the chosen level ARCHIVE INDEX, or -1 if the user quit the editor.
// The list itself is rendered/navigated via display_order[] (row -> archive
// index) so it can be shown in play order or archive order, but every
// record is always identified by -- and this always returns -- its real
// archive index; sorting is display-only.
static int run_level_select(void)
{
	build_level_summaries();

	int count = level_summary_count;
	if (count == 0)
		return -1;

	compute_play_order();
	build_display_order();

	int sel = row_for_archive_index(last_level_sel);
	int scroll = 0;
	const int rows_visible = 19;
	const int row_h = 8;

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

		for (int row = 0; row < rows_visible; ++row)
		{
			int r = scroll + row;
			if (r >= count)
				break;

			int idx = display_order[r];
			const char *title = (level_title[idx][0] != '\0') ? level_title[idx] : "(unnamed)";

			char line[64];
			snprintf(line, sizeof(line), "%2d  %-9s  map=%c shp=%c", idx, title,
			         level_summaries[idx].mapFile, level_summaries[idx].shapeFile);

			JE_outText(VGAScreen, 16, 16 + row * row_h, line, 0, r == sel ? 4 : 0);
		}

		JE_outText(VGAScreen, 8, 190, "Up/Down/PgUp/PgDn select, O sort, Enter open, Esc quit", 0, 0);

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

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
				last_level_sel = display_order[sel];
				return display_order[sel];

			default:
				break;
			}
		}
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

// Remembers the last-selected row across re-entries into run_episode_select()
// within one editor session, same rationale as last_level_sel above.
static int last_episode_sel = 1;

// Returns the chosen episode (1-4), or -1 if the user quit the editor.
static int run_episode_select(void)
{
	// Per-episode level counts, probed once per entry into this screen, for
	// display only ("(N levels)" / "(?)" on failure). This clobbers the
	// shared archive blob/cur_episode/cur_lvl_filename as a side effect, but
	// that's harmless: whichever episode the user picks gets reloaded fresh
	// by load_episode_archive() right after this function returns.
	int counts[5]; // index by episode 1..4; counts[0] unused
	for (int e = 1; e <= 4; ++e)
	{
		char fname[32];
		snprintf(fname, sizeof(fname), "tyrian%d.lvl", e);
		counts[e] = lvledit_load_archive(fname) ? lvledit_level_count() : -1;
	}

	int sel = last_episode_sel;
	if (sel < 1) sel = 1;
	if (sel > 4) sel = 4;

	for (;;)
	{
		setFrameCount(1);
		handleSdlEvents();

		SDL_FillRect(VGAScreen, NULL, 0);

		JE_outText(VGAScreen, 8, 4, "Level Editor - select episode", 0, 4);

		for (int e = 1; e <= 4; ++e)
		{
			char line[64];
			if (counts[e] >= 0)
				snprintf(line, sizeof(line), "Episode %d   tyrian%d.lvl  (%d levels)", e, e, counts[e]);
			else
				snprintf(line, sizeof(line), "Episode %d   tyrian%d.lvl  (?)", e, e);

			JE_outText(VGAScreen, 16, 16 + (e - 1) * 8, line, 0, e == sel ? 4 : 0);
		}

		JE_outText(VGAScreen, 8, 190, "Up/Down select, Enter open, Esc quit editor", 0, 0);

		JE_showVGA();
		waitUntilElapsed();

		KeyboardInput ki;
		while (keyboardGetInput(&ki))
		{
			switch (ki.scancode)
			{
			case SDL_SCANCODE_ESCAPE:
				last_episode_sel = sel;
				return -1;

			case SDL_SCANCODE_UP:
				if (sel > 1) --sel;
				break;
			case SDL_SCANCODE_DOWN:
				if (sel < 4) ++sel;
				break;

			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_KP_ENTER:
				last_episode_sel = sel;
				return sel;

			default:
				break;
			}
		}
	}
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

// episode: 1-4 to boot straight into that episode's level-select, or 0 (or
// any out-of-range value) to start at the in-app episode picker instead.
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
		if (ep < 1 || ep > 4)
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
	clamp_event_view();

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
