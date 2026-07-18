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
/** @file hd_hud.c
 * HD in-flight HUD overlay (internal/plan/REMASTER_HUD.md, phases H0-H2).
 *
 * State-driven overlay, not draw interception: every present, this redraws
 * the whole sidebar/bottom-bar panel from live game state directly on top of
 * the already-composited classic 8-bit base (so any element this file
 * doesn't cover, or fails to draw, simply shows the classic pixels that are
 * already there -- nothing is erased or intercepted). Gated to 1P and to the
 * hdpic03.dat panel texture actually being available; either failing means
 * hd_hud_draw() is a no-op.
 *
 * Geometry/colors here are hand-ported from the classic draw sites (see
 * REMASTER_HUD.md's file:line list) so they read as *reproductions*, not
 * derivations -- JE_dBar3 (nortvars.c), the power bar and weapon dots
 * (tyrian2.c), draw_segmented_gauge (vga256d.c) are never called or
 * modified; they keep drawing into VGAScreenSeg for the classic fallback.
 */

#include "hd_hud.h"

#include "opentyr.h"

#include "config.h"
#include "palette.h"
#include "player.h"
#include "sprite.h"
#include "varz.h"
#include "video.h"

#include <assert.h>
#include <stdbool.h>

/* Panel art (PIC #3, 1P) resolution, queried once from the cached texture and
 * assumed to stay constant thereafter (the fail-once cache in video.c never
 * reloads a texture once it has loaded). 0 means "not yet resolved". */
static int panel_tex_w = 0, panel_tex_h = 0;
static bool panel_tex_query_failed = false;

/* Resolves (and caches) the HD panel texture + its pixel size. Returns false
 * -- draw nothing -- if the asset is unavailable or malformed, mirroring the
 * fail-once behaviour of every other HD asset cache in this codebase. */
static bool ensure_panel_tex(SDL_Texture **out_tex)
{
	if (panel_tex_query_failed)
		return false;

	SDL_Texture *tex = hd_hud_get_panel_texture(3); // PIC #3 == 1P panel (tyrian2.c:816)
	if (tex == NULL)
	{
		panel_tex_query_failed = true;
		return false;
	}

	if (panel_tex_w == 0 || panel_tex_h == 0)
	{
		int w = 0, h = 0;
		if (SDL_QueryTexture(tex, NULL, NULL, &w, &h) != 0 || w <= 0 || h <= 0)
		{
			panel_tex_query_failed = true;
			return false;
		}
		panel_tex_w = w;
		panel_tex_h = h;
	}

	*out_tex = tex;
	return true;
}

/* Logical VGA rect (320x200) -> output window rect, via dst_rect. Identical
 * integer math to the flight-sprite queue's window_rect (video.c:2255-2259)
 * so HUD geometry lines up pixel-for-pixel with the flight-sprite overlay
 * and the classic scaled base underneath. */
static SDL_Rect vga_rect_to_window(const SDL_Rect *dst_rect, int lx, int ly, int lw, int lh)
{
	SDL_Rect r;
	r.x = dst_rect->x + lx * dst_rect->w / vga_width;
	r.y = dst_rect->y + ly * dst_rect->h / vga_height;
	r.w = dst_rect->x + (lx + lw) * dst_rect->w / vga_width - r.x;
	r.h = dst_rect->y + (ly + lh) * dst_rect->h / vga_height - r.y;
	return r;
}

/* Logical VGA rect -> source rect within the panel texture. The asset need
 * not be a fixed multiple of 320x200 (read the header, don't assume a scale
 * factor), so this scales by the texture's actual queried size. */
static SDL_Rect vga_rect_to_tex_src(int lx, int ly, int lw, int lh)
{
	SDL_Rect r;
	r.x = lx * panel_tex_w / vga_width;
	r.y = ly * panel_tex_h / vga_height;
	r.w = (lx + lw) * panel_tex_w / vga_width - r.x;
	r.h = (ly + lh) * panel_tex_h / vga_height - r.y;
	return r;
}

static void fill_vga_rect(SDL_Renderer *renderer, const SDL_Rect *dst_rect, int x, int y, int w, int h, Uint8 color_index)
{
	if (w <= 0 || h <= 0)
		return;

	SDL_Rect win = vga_rect_to_window(dst_rect, x, y, w, h);
	SDL_Color c = colors[color_index];
	SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
	SDL_RenderFillRect(renderer, &win);
}

static void draw_panel_span(SDL_Renderer *renderer, const SDL_Rect *dst_rect, SDL_Texture *tex, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;

	SDL_Rect dst = vga_rect_to_window(dst_rect, x, y, w, h);
	SDL_Rect src = vga_rect_to_tex_src(x, y, w, h);
	SDL_RenderCopy(renderer, tex, &src, &dst);
}

/* A punch-out hole: a logical VGA rect whose classic pixels (already present
 * in the base image blitted earlier in scale_and_flip) must keep showing
 * through instead of the HD panel art. */
typedef struct
{
	int x, y, w, h;
}
HdHudCell;

#define MAX_CELLS 8

/* Draws the VGA rect (strip_x,strip_y,strip_w,strip_h) from `tex`, skipping
 * `cells`. Rect subtraction via coordinate-compression: the cells' y-edges
 * split the strip into horizontal bands, each band's active cells' x-edges
 * split it into spans, and a span is drawn iff its midpoint isn't inside any
 * cell. Cheap and correct for the small, non-overlapping cell counts here;
 * not a general polygon clipper. */
static void draw_strip_minus_cells(SDL_Renderer *renderer, const SDL_Rect *dst_rect, SDL_Texture *tex,
	int strip_x, int strip_y, int strip_w, int strip_h, const HdHudCell *cells, int cell_count)
{
	int ys[2 + 2 * MAX_CELLS];
	int ny = 0;
	ys[ny++] = strip_y;
	ys[ny++] = strip_y + strip_h;

	for (int i = 0; i < cell_count; ++i)
	{
		int y0 = MAX(cells[i].y, strip_y);
		int y1 = MIN(cells[i].y + cells[i].h, strip_y + strip_h);
		if (y0 < y1)
		{
			ys[ny++] = y0;
			ys[ny++] = y1;
		}
	}

	for (int i = 1; i < ny; ++i) // insertion sort, ny is tiny
	{
		int v = ys[i], j = i - 1;
		while (j >= 0 && ys[j] > v) { ys[j + 1] = ys[j]; --j; }
		ys[j + 1] = v;
	}

	for (int yi = 0; yi + 1 < ny; ++yi)
	{
		int y0 = ys[yi], y1 = ys[yi + 1];
		if (y1 <= y0)
			continue;
		int mid_y = y0 + (y1 - y0) / 2;

		int xs[2 + 2 * MAX_CELLS];
		int nx = 0;
		xs[nx++] = strip_x;
		xs[nx++] = strip_x + strip_w;

		for (int i = 0; i < cell_count; ++i)
		{
			if (mid_y < cells[i].y || mid_y >= cells[i].y + cells[i].h)
				continue;
			int x0 = MAX(cells[i].x, strip_x);
			int x1 = MIN(cells[i].x + cells[i].w, strip_x + strip_w);
			if (x0 < x1)
			{
				xs[nx++] = x0;
				xs[nx++] = x1;
			}
		}

		for (int i = 1; i < nx; ++i)
		{
			int v = xs[i], j = i - 1;
			while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; --j; }
			xs[j + 1] = v;
		}

		for (int xi = 0; xi + 1 < nx; ++xi)
		{
			int x0 = xs[xi], x1 = xs[xi + 1];
			if (x1 <= x0)
				continue;
			int mid_x = x0 + (x1 - x0) / 2;

			bool covered = false;
			for (int i = 0; i < cell_count; ++i)
			{
				if (mid_x >= cells[i].x && mid_x < cells[i].x + cells[i].w &&
				    mid_y >= cells[i].y && mid_y < cells[i].y + cells[i].h)
				{
					covered = true;
					break;
				}
			}

			if (!covered)
				draw_panel_span(renderer, dst_rect, tex, x0, y0, x1 - x0, y1 - y0);
		}
	}
}

/* Reproduces JE_dBar3 (src/nortvars.c:31): num+1 segments, each a 9px-wide x
 * 2px-tall (rows y-1,y) rect; color starts base_col+2 and steps up by 1 every
 * 2 segments -- except zWait starts at 2 (not 1), so the first *3* segments
 * share base_col+2. y steps by -2/segment (the bar grows upward). Not called
 * for negative num (nothing to draw). */
static void draw_dbar3(SDL_Renderer *renderer, const SDL_Rect *dst_rect, int x, int y, int num, Uint8 base_col)
{
	if (num < 0)
		return;

	int col = base_col + 2;
	int zWait = 2;

	for (int z = 0; z <= num; ++z)
	{
		fill_vga_rect(renderer, dst_rect, x, y - 1, 9, 2, (Uint8)col);

		if (zWait > 0)
			--zWait;
		else
		{
			++col;
			zWait = 1;
		}
		y -= 2;
	}
}

/* Shield bar (JE_drawShield, varz.c:1112) + max-shield marker (varz.c:1126),
 * 1P-only geometry (270,194). */
static void draw_shield_bar(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
	Player *p = &player[0];

	draw_dbar3(renderer, dst_rect, 270, 194, (int)p->shield, 144);

	if (p->shield != p->shield_max)
	{
		int y = 193 - (int)(p->shield_max * 2);
		fill_vga_rect(renderer, dst_rect, 270, y, 9, 1, 68); // JE_rectangle with b==d: a single-row line
	}
}

/* Armor bar (JE_drawArmor, varz.c:1130), 1P-only geometry (307,194). Classic
 * JE_drawArmor clamps player[].armor to 28 as a mutating side effect before
 * drawing; this overlay must not mutate game state, so it clamps a local
 * copy instead -- same visual result, no side effect. */
static void draw_armor_bar(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
	int armor = (int)MIN(player[0].armor, 28u);
	draw_dbar3(renderer, dst_rect, 307, 194, armor, 224);
}

/* Weapon-power dots (tyrian2.c:1256-1276), 1P-only geometry (x=289). Each dot
 * is a 2px-wide x 3px-tall rect, color 115+j (j is 1-based). */
static void draw_weapon_dots(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
	for (uint i = 0; i < 2; ++i)
	{
		uint item_power = player[0].items.weapon[i].power;
		int x = 289;
		int y = (i == 0) ? 17 : 38;

		for (uint j = 1; j <= item_power; ++j)
		{
			fill_vga_rect(renderer, dst_rect, x, y, 2, 3, (Uint8)(115 + j));
			x += 2;
		}
	}
}

/* Main power bar (tyrian2.c:1288-1300), 1P-only geometry (x=269..276). The
 * classic draw is incremental and history-dependent: each grow tick fills
 * rows 102-temp .. 103-lastPower with that tick's color (113 + temp/7), so
 * under the common one-unit-per-tick climb the bar occupies rows
 * [102-temp, 103] and row y was last painted at temp' == min(temp, 104-y),
 * giving color 113 + min(temp, 104-y)/7. This overlay has no history to
 * replay -- it redraws fresh from the live `power` global every present --
 * so it draws exactly that steady-state result. It diverges from the exact
 * historical residue only transiently: a multi-bracket jump in one tick, or
 * a shrink (whose classic erase also clears rows 102-temp..103-temp, leaving
 * a bar 2 rows shorter until the next grow repaints them). */
static void draw_power_bar(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
	int temp = (int)power / 10;
	if (temp > 90)
		temp = 90;
	if (temp < 1)
		return;

	for (int y = 102 - temp; y <= 103; ++y)
		fill_vga_rect(renderer, dst_rect, 269, y, 8, 1, (Uint8)(113 + MIN(temp, 104 - y) / 7));
}

/* Sidekick ammo gauge (draw_segmented_gauge semantics, vga256d.c:158), called
 * from JE_drawOptions (varz.c:422) at (284, hud_sidekick_y[0][i]+13), base
 * color 112, 2x2px segments, segment_value = max(1, ammo_max/10). */
static void draw_sidekick_gauge(SDL_Renderer *renderer, const SDL_Rect *dst_rect, int i)
{
	Player *p = &player[0];

	int ammo_max = MAX(0, p->sidekick[i].ammo_max);
	int ammo = MAX(0, p->sidekick[i].ammo);
	int segment_value = MAX(1, ammo_max / 10);
	int segments = ammo / segment_value;
	int partial = ammo % segment_value;

	int x = 284;
	int y = hud_sidekick_y[0][i] + 13;

	for (int s = 0; s < segments; ++s)
	{
		fill_vga_rect(renderer, dst_rect, x, y, 2, 2, (Uint8)(112 + 12));
		x += 2 + 1;
	}
	if (partial > 0)
		fill_vga_rect(renderer, dst_rect, x, y, 2, 2, (Uint8)(112 + 12 * partial / segment_value));
}

void hd_hud_draw(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
	// H4 (later): two-player uses a different panel (hdpic06.dat) and layout
	// (JE_drawOptionLevel's x=268 geometry etc.) -- out of scope for H0-H2.
	if (twoPlayerMode)
		return;

	SDL_Texture *tex;
	if (!ensure_panel_tex(&tex))
		return; // asset unavailable: draw nothing, classic base already shows

	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	SDL_SetTextureColorMod(tex, 255, 255, 255);
	SDL_SetTextureAlphaMod(tex, 255);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

	// Punch-out cells: elements whose classic pixels (icons, buttons, text)
	// must keep showing through the HD panel art. The bars/dots/gauges are
	// *not* holes here -- H2 draws them vectorized on top of the HD
	// background below, instead of leaving them as classic punch-outs.
	const int sk_y0 = hud_sidekick_y[0][0], sk_y1 = hud_sidekick_y[0][1];

	HdHudCell cells[MAX_CELLS];
	int n = 0;

	// Sidekick icon cells: fixed 29x16 erase-rect footprint from
	// JE_drawOptions (varz.c:422: fill_rectangle_xy(284,y,284+28,y+15,0)),
	// independent of the actual icon sprite's size.
	cells[n++] = (HdHudCell){ 284, sk_y0, 29, 16 };
	cells[n++] = (HdHudCell){ 284, sk_y1, 29, 16 };

	// Rear-config buttons (mainint.c:202-208): OPTION_SHAPES 18/19 at
	// (285,44) and (302,44). Sized from the live sprite data rather than an
	// assumed constant so a data change can't silently create a mismatched
	// hole.
	{
		int w18 = get_sprite_width(OPTION_SHAPES, 18), h18 = get_sprite_height(OPTION_SHAPES, 18);
		int w19 = get_sprite_width(OPTION_SHAPES, 19), h19 = get_sprite_height(OPTION_SHAPES, 19);
		int w = MAX(w18, w19), h = MAX(h18, h19);
		int left = MIN(285, 302), right = MAX(285 + w, 302 + w);
		cells[n++] = (HdHudCell){ left, 44, right - left, h };
	}

	// Message bar (mainint.c:99-109): JE_drawTextWindow / JE_outCharGlow at
	// (16,189)-(20,190); punched out until H3 draws HD text here.
	cells[n++] = (HdHudCell){ 16, 188, 263 - 16 + 1, 200 - 188 };

	// Level-name block (tyrian2.c:824): JE_outText(268, 118, levelName, ...)
	// for 1P. levelName's rendered width is state-dependent (up to 10
	// chars); punched out generously across the full sidebar width rather
	// than measuring text, since it stays a punch-out through H3 anyway.
	cells[n++] = (HdHudCell){ 264, 116, 320 - 264, 128 - 116 };

	assert(n <= MAX_CELLS);

	// Sidebar strip: VGA x in [264,320), y in [0,200).
	draw_strip_minus_cells(renderer, dst_rect, tex, 264, 0, 320 - 264, 200, cells, n);
	// Bottom bar strip: VGA y in [184,200), x in [0,264). Disjoint from the
	// sidebar strip (that one already covers x >= 264 for every y).
	draw_strip_minus_cells(renderer, dst_rect, tex, 0, 184, 264, 200 - 184, cells, n);

	draw_shield_bar(renderer, dst_rect);
	draw_armor_bar(renderer, dst_rect);
	draw_power_bar(renderer, dst_rect);
	draw_weapon_dots(renderer, dst_rect);
	draw_sidekick_gauge(renderer, dst_rect, 0);
	draw_sidekick_gauge(renderer, dst_rect, 1);
}
