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
 * HD in-flight HUD overlay (internal/plan/REMASTER_HUD.md, phase H2.5).
 *
 * State-driven overlay, not draw interception: every present, this redraws the
 * whole sidebar/bottom-bar panel from live game state directly on top of the
 * already-composited classic 8-bit base. Any element this file doesn't cover,
 * or fails to draw, simply shows the classic pixels already there -- nothing is
 * erased or intercepted. Gated to 1P; 2P falls through to the classic panel.
 *
 * H2.5 replaces the earlier hdpic03.dat AI-upscaled panel art (rejected as too
 * smooth/fuzzy) with a fully PROCEDURAL vector panel: flat near-black fills,
 * hairline steel frames, inset wells, and crisp HD-font labels -- a clean modern
 * HUD. Layout/positions stay EXACTLY where the classic art and the dynamic
 * classic draws put them (alignment with the classic fallback and the remaining
 * punch-outs is non-negotiable), only the ornamentation changes.
 *
 * Geometry/colors of the dynamic bars are hand-ported from the classic draw
 * sites (see REMASTER_HUD.md) so they read as reproductions, not derivations --
 * JE_dBar3 (nortvars.c), the power bar and weapon dots (tyrian2.c),
 * draw_segmented_gauge (vga256d.c) are never called or modified; they keep
 * drawing into VGAScreenSeg for the classic fallback.
 *
 * Palette / fade sync: the dynamic bars sample the LIVE (post-fade) palette
 * (get_live_palette()), so palette fades and damage flashes track automatically.
 * The fixed-RGB panel/frame/well colors are scaled by a dim factor derived from
 * how faded the live palette is versus the full target palette, so they never
 * pop to full brightness during a level-start/-end fade. Labels are HD-font
 * glyphs synthesized against the palette and cached by (glyph,hue,value); in
 * practice level fades happen outside hd_flight_active so labels only ever
 * synthesize at full brightness (a documented caching limitation, not a fade
 * bug in normal play).
 */

#include "hd_hud.h"

#include "opentyr.h"

#include "config.h"
#include "fonthand.h"
#include "palette.h"
#include "player.h"
#include "sprite.h"
#include "varz.h"
#include "video.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

// Tyrian green text bank + brightness, matching the classic level-name draw
// (tyrian2.c:824: JE_outText(..., levelName, 12, 4)).
#define HUD_GREEN_HUE   12
#define HUD_GREEN_VALUE 4

// ---- Present-time context (single-threaded; set once per hd_hud_draw) --------

static SDL_Renderer *g_ren;
static const SDL_Rect *g_dst;
static float g_dim = 1.f;  // 0..1 fade factor for fixed-RGB panel elements

// Punch-out cells: logical VGA rects whose classic pixels (icons, buttons, the
// message text) must keep showing through -- the procedural fills skip them.
typedef struct { int x, y, w, h; } HdHudCell;

#define MAX_CELLS 8
static HdHudCell g_cells[MAX_CELLS];
static int g_ncells;

// ---- Coordinate mapping ------------------------------------------------------

/* Logical VGA rect (320x200) -> output window rect, via g_dst. Identical integer
 * math to the flight-sprite queue's window_rect (video.c) so HUD geometry lines
 * up pixel-for-pixel with the flight overlay and the classic scaled base. */
static SDL_Rect vga_rect_to_window(int lx, int ly, int lw, int lh)
{
	SDL_Rect r;
	r.x = g_dst->x + lx * g_dst->w / vga_width;
	r.y = g_dst->y + ly * g_dst->h / vga_height;
	r.w = g_dst->x + (lx + lw) * g_dst->w / vga_width - r.x;
	r.h = g_dst->y + (ly + lh) * g_dst->h / vga_height - r.y;
	return r;
}

// ---- Fade factor -------------------------------------------------------------

/* Derives a 0..1 dim factor from the ratio of the live (post-fade) palette's
 * brightest channel to the full target palette's brightest channel. At full
 * brightness the two match (dim == 1); during a fade both scale together, so the
 * ratio is the fade fraction; a damage flash can push the live max above the
 * target, which clamps to 1 (the panel just stays full-bright, matching the
 * flashed playfield). */
static float compute_dim(void)
{
	const SDL_Color *live = get_live_palette();

	int live_max = 0, full_max = 0;
	for (int i = 0; i < 256; ++i)
	{
		int lm = MAX(live[i].r, MAX(live[i].g, live[i].b));
		int fm = MAX(colors[i].r, MAX(colors[i].g, colors[i].b));
		if (lm > live_max) live_max = lm;
		if (fm > full_max) full_max = fm;
	}

	if (full_max <= 0)
		return 0.f;

	float d = (float)live_max / (float)full_max;
	return d < 0.f ? 0.f : d > 1.f ? 1.f : d;
}

// ---- Cell-aware flat fill ----------------------------------------------------

static bool cell_covers(int mx, int my)
{
	for (int i = 0; i < g_ncells; ++i)
		if (mx >= g_cells[i].x && mx < g_cells[i].x + g_cells[i].w &&
		    my >= g_cells[i].y && my < g_cells[i].y + g_cells[i].h)
			return true;
	return false;
}

/* Fills the VGA rect (x,y,w,h) with a dimmed RGB colour, skipping the punch-out
 * cells. Rect subtraction via coordinate compression: cell y-edges split the
 * rect into bands, each band's active cells' x-edges split it into spans, and a
 * span is drawn iff its midpoint isn't inside any cell. Cheap and correct for
 * the small, non-overlapping cell counts here. */
static void fill_rgb(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b)
{
	if (w <= 0 || h <= 0)
		return;

	SDL_SetRenderDrawColor(g_ren, (Uint8)(r * g_dim), (Uint8)(g * g_dim), (Uint8)(b * g_dim), 255);

	int ys[2 + 2 * MAX_CELLS];
	int ny = 0;
	ys[ny++] = y;
	ys[ny++] = y + h;
	for (int i = 0; i < g_ncells; ++i)
	{
		int y0 = MAX(g_cells[i].y, y);
		int y1 = MIN(g_cells[i].y + g_cells[i].h, y + h);
		if (y0 < y1) { ys[ny++] = y0; ys[ny++] = y1; }
	}
	for (int i = 1; i < ny; ++i) { int v = ys[i], j = i - 1; while (j >= 0 && ys[j] > v) { ys[j+1] = ys[j]; --j; } ys[j+1] = v; }

	for (int yi = 0; yi + 1 < ny; ++yi)
	{
		int y0 = ys[yi], y1 = ys[yi + 1];
		if (y1 <= y0)
			continue;
		int mid_y = y0 + (y1 - y0) / 2;

		int xs[2 + 2 * MAX_CELLS];
		int nx = 0;
		xs[nx++] = x;
		xs[nx++] = x + w;
		for (int i = 0; i < g_ncells; ++i)
		{
			if (mid_y < g_cells[i].y || mid_y >= g_cells[i].y + g_cells[i].h)
				continue;
			int x0 = MAX(g_cells[i].x, x);
			int x1 = MIN(g_cells[i].x + g_cells[i].w, x + w);
			if (x0 < x1) { xs[nx++] = x0; xs[nx++] = x1; }
		}
		for (int i = 1; i < nx; ++i) { int v = xs[i], j = i - 1; while (j >= 0 && xs[j] > v) { xs[j+1] = xs[j]; --j; } xs[j+1] = v; }

		for (int xi = 0; xi + 1 < nx; ++xi)
		{
			int x0 = xs[xi], x1 = xs[xi + 1];
			if (x1 <= x0)
				continue;
			int mid_x = x0 + (x1 - x0) / 2;
			if (cell_covers(mid_x, mid_y))
				continue;

			SDL_Rect win = vga_rect_to_window(x0, y0, x1 - x0, y1 - y0);
			SDL_RenderFillRect(g_ren, &win);
		}
	}
}

// ---- Palette-sampled fill (for the dynamic bars: fades with the live palette)-

static void fill_pal(int x, int y, int w, int h, Uint8 color_index)
{
	if (w <= 0 || h <= 0)
		return;

	SDL_Rect win = vga_rect_to_window(x, y, w, h);
	SDL_Color c = get_live_palette()[color_index];
	SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, 255);
	SDL_RenderFillRect(g_ren, &win);
}

// ---- Panel design language ---------------------------------------------------

// Flat, dark, crisp. All values pre-dim; fill_rgb scales by g_dim.
#define PANEL_R 0x12
#define PANEL_G 0x14
#define PANEL_B 0x18   // base near-black #121418
#define BAND_R  0x18
#define BAND_G  0x1b
#define BAND_B  0x21   // subtly lighter upper two-tone band
#define WELL_R  0x0a
#define WELL_G  0x0c
#define WELL_B  0x10   // inset darker well #0a0c10
#define FRAME_R 0x3c
#define FRAME_G 0x43
#define FRAME_B 0x50   // desaturated steel hairline #3c4350
#define BEVEL_R 0x5a
#define BEVEL_G 0x64
#define BEVEL_B 0x74   // brighter top edge for a subtle bevel

static void draw_hline(int x, int y, int w, Uint8 r, Uint8 g, Uint8 b)
{
	SDL_Rect win = vga_rect_to_window(x, y, w, 1);
	win.h = 1;  // exactly one output pixel tall (hairline)
	SDL_SetRenderDrawColor(g_ren, (Uint8)(r * g_dim), (Uint8)(g * g_dim), (Uint8)(b * g_dim), 255);
	SDL_RenderFillRect(g_ren, &win);
}

static void draw_vline(int x, int y, int h, Uint8 r, Uint8 g, Uint8 b)
{
	SDL_Rect win = vga_rect_to_window(x, y, 1, h);
	win.w = 1;
	SDL_SetRenderDrawColor(g_ren, (Uint8)(r * g_dim), (Uint8)(g * g_dim), (Uint8)(b * g_dim), 255);
	SDL_RenderFillRect(g_ren, &win);
}

/* Hairline steel frame around a logical VGA box, with a brighter top edge for a
 * subtle bevel. Drawn as four 1-output-pixel lines (crisp, resolution-native). */
static void frame_box(int x, int y, int w, int h)
{
	draw_hline(x, y, w, BEVEL_R, BEVEL_G, BEVEL_B);          // top (bevel)
	draw_hline(x, y + h - 1, w, FRAME_R, FRAME_G, FRAME_B);  // bottom
	draw_vline(x, y, h, FRAME_R, FRAME_G, FRAME_B);          // left
	draw_vline(x + w - 1, y, h, FRAME_R, FRAME_G, FRAME_B);  // right
}

/* An inset "screen": dark well fill (skipping punch-out cells) + steel frame. */
static void draw_screen(int x, int y, int w, int h)
{
	fill_rgb(x, y, w, h, WELL_R, WELL_G, WELL_B);
	frame_box(x, y, w, h);
}

// ---- HD-font labels ----------------------------------------------------------

static int hud_text_width(const char *s)
{
	int x = 0;
	for (int i = 0; s[i] != '\0'; ++i)
	{
		int id = fontMap[(unsigned char)s[i]];
		if (s[i] == ' ')
			x += 6;
		else if (id != -1 && sprite_exists(TINY_FONT, id))
			x += sprite(TINY_FONT, id)->width + 1;
	}
	return x;
}

/* Draws a horizontal string as crisp HD glyphs at present time (green HUD text),
 * advancing by the classic TINY_FONT glyph widths + kerning exactly as JE_outText
 * does. `centered` centres the string on x. */
static void draw_hud_text(int x, int y, const char *s, bool centered)
{
	if (centered)
		x -= hud_text_width(s) / 2;

	for (int i = 0; s[i] != '\0'; ++i)
	{
		int id = fontMap[(unsigned char)s[i]];
		if (s[i] == ' ')
		{
			x += 6;
			continue;
		}
		if (id != -1 && sprite_exists(TINY_FONT, id))
		{
			hd_hud_queue_glyph(TINY_FONT, (unsigned int)id, x, y, HUD_GREEN_HUE, HUD_GREEN_VALUE);
			x += sprite(TINY_FONT, id)->width + 1;
		}
	}
}

/* Vertical stacked label: one glyph per row (SHIELD/ARMOR in the classic art). */
static void draw_hud_text_vertical(int x, int y, int pitch, const char *s)
{
	for (int i = 0; s[i] != '\0'; ++i)
	{
		int id = fontMap[(unsigned char)s[i]];
		if (id != -1 && sprite_exists(TINY_FONT, id))
			hd_hud_queue_glyph(TINY_FONT, (unsigned int)id, x, y, HUD_GREEN_HUE, HUD_GREEN_VALUE);
		y += pitch;
	}
}

// ---- Dynamic vector elements (classic geometry & colour ramps) ---------------

/* Reproduces JE_dBar3 (src/nortvars.c:31): num+1 segments, each 9px wide x 2px
 * tall (rows y-1,y); colour starts base_col+2, stepping up by 1 every 2 segments
 * except zWait starts at 2 so the first 3 segments share base_col+2; y steps -2
 * per segment (the bar grows upward). */
static void draw_dbar3(int x, int y, int num, Uint8 base_col)
{
	if (num < 0)
		return;

	int col = base_col + 2;
	int zWait = 2;

	for (int z = 0; z <= num; ++z)
	{
		fill_pal(x, y - 1, 9, 2, (Uint8)col);

		if (zWait > 0)
			--zWait;
		else { ++col; zWait = 1; }
		y -= 2;
	}
}

/* Shield bar (JE_drawShield, varz.c:1112) + max-shield marker (varz.c:1126). */
static void draw_shield_bar(void)
{
	Player *p = &player[0];

	draw_dbar3(270, 194, (int)p->shield, 144);

	if (p->shield != p->shield_max)
	{
		int y = 193 - (int)(p->shield_max * 2);
		fill_pal(270, y, 9, 1, 68);
	}
}

/* Armor bar (JE_drawArmor, varz.c:1130); classic clamps armor to 28 as a side
 * effect before drawing -- clamp a local copy instead (no game-state mutation). */
static void draw_armor_bar(void)
{
	int armor = (int)MIN(player[0].armor, 28u);
	draw_dbar3(307, 194, armor, 224);
}

/* Weapon-power dots (tyrian2.c:1256-1276): 2px wide x 3px tall, colour 115+j. */
static void draw_weapon_dots(void)
{
	for (uint i = 0; i < 2; ++i)
	{
		uint item_power = player[0].items.weapon[i].power;
		int x = 289;
		int y = (i == 0) ? 17 : 38;

		for (uint j = 1; j <= item_power; ++j)
		{
			fill_pal(x, y, 2, 3, (Uint8)(115 + j));
			x += 2;
		}
	}
}

/* Main power bar (tyrian2.c:1288-1300) steady-state result, x=269..276. See the
 * long note in the prior revision: draws rows [102-temp,103], row y coloured
 * 113 + min(temp,104-y)/7. */
static void draw_power_bar(void)
{
	int temp = (int)power / 10;
	if (temp > 90)
		temp = 90;
	if (temp < 1)
		return;

	for (int y = 102 - temp; y <= 103; ++y)
		fill_pal(269, y, 8, 1, (Uint8)(113 + MIN(temp, 104 - y) / 7));
}

/* Sidekick ammo gauge (draw_segmented_gauge, vga256d.c:158) at (284,y+13),
 * base colour 112, 2x2px segments, segment_value = max(1, ammo_max/10). */
static void draw_sidekick_gauge(int i)
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
		fill_pal(x, y, 2, 2, (Uint8)(112 + 12));
		x += 2 + 1;
	}
	if (partial > 0)
		fill_pal(x, y, 2, 2, (Uint8)(112 + 12 * partial / segment_value));
}

// ---- Procedural panel --------------------------------------------------------

/* Backgrounds: flat base fill for both strips, a subtle upper two-tone band, and
 * the inset wells. Everything respects the punch-out cells via fill_rgb. */
static void draw_panel_background(void)
{
	const int sk_y0 = hud_sidekick_y[0][0], sk_y1 = hud_sidekick_y[0][1];

	// Base fill: sidebar [264,0,56,200] and bottom strip [0,184,264,16].
	fill_rgb(264, 0, 56, 200, PANEL_R, PANEL_G, PANEL_B);
	fill_rgb(0, 184, 264, 16, PANEL_R, PANEL_G, PANEL_B);

	// Subtle two-tone: a lighter band across the upper sidebar (large flat rect,
	// not a smooth dither).
	fill_rgb(264, 0, 56, 110, BAND_R, BAND_G, BAND_B);

	// Inset wells (dark) behind the bars and label plates.
	fill_rgb(267, 10, 12, 96, WELL_R, WELL_G, WELL_B);          // power bar well

	// Gun / mode / sidekick "screens": dark interior + steel frame.
	draw_screen(280, 1, 39, 21);          // FRONT GUN box (dots at y17 inside)
	draw_screen(280, 22, 39, 20);         // REAR GUN box (dots at y38 inside)
	draw_screen(280, 43, 39, 21);         // MODE box (buttons y44 + MODE label inside)
	frame_box(280, 62, 39, 19);           // sidekick 1 (icon+gauge punch-out inside)
	frame_box(280, 81, 39, 19);           // sidekick 2
	frame_box(267, 9, 12, 98);            // power-bar well frame

	draw_screen(265, 114, 54, 14);        // level-name plate

	// Bottom: shield/armor wells + the vertical-label plate between them.
	draw_screen(266, 132, 14, 66);        // shield well
	draw_screen(281, 132, 24, 66);        // SHIELD/ARMOR label plate
	draw_screen(305, 132, 14, 66);        // armor well

	// Bottom message bar frame (interior is a punch-out for the classic message).
	frame_box(14, 185, 250, 14);

	(void)sk_y0; (void)sk_y1;
}

static void draw_panel_labels(void)
{
	// Upper sidebar labels, two lines each above their dot wells (pitch 8 keeps
	// the two lines from touching; the second line sits just above the dots).
	draw_hud_text(284, 2, "FRONT", false);
	draw_hud_text(284, 10, "GUN", false);
	draw_hud_text(284, 23, "REAR", false);
	draw_hud_text(284, 31, "GUN", false);
	draw_hud_text(284, 54, "MODE", false);

	// Vertical SHIELD / ARMOR between/beside their bars (one glyph per row).
	draw_hud_text_vertical(285, 137, 7, "SHIELD");
	draw_hud_text_vertical(297, 137, 7, "ARMOR");

	// Level name on the plate (was a classic punch-out; now HD-drawn).
	draw_hud_text(268, 118, levelName, false);
}

void hd_hud_draw(SDL_Renderer *renderer, const SDL_Rect *dst_rect)
{
	// H4 (later): two-player uses a different panel and layout -- out of scope.
	// The classic panel keeps drawing underneath, so this simply falls back.
	if (twoPlayerMode)
		return;

	g_ren = renderer;
	g_dst = dst_rect;
	g_dim = compute_dim();

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

	// Punch-out cells: elements whose classic pixels must keep showing through
	// the procedural panel (the fills skip these rects).
	const int sk_y0 = hud_sidekick_y[0][0], sk_y1 = hud_sidekick_y[0][1];
	g_ncells = 0;

	// Sidekick icons: fixed 29x16 erase-rect footprint (JE_drawOptions,
	// varz.c:422). The vector gauge is drawn ON TOP; the icon bitmap shows.
	g_cells[g_ncells++] = (HdHudCell){ 284, sk_y0, 29, 16 };
	g_cells[g_ncells++] = (HdHudCell){ 284, sk_y1, 29, 16 };

	// Rear-config buttons (mainint.c:202): OPTION_SHAPES 18/19 at (285,44),
	// (302,44). Sized from the live sprite data so a data change can't create a
	// mismatched hole.
	{
		int w18 = get_sprite_width(OPTION_SHAPES, 18), h18 = get_sprite_height(OPTION_SHAPES, 18);
		int w19 = get_sprite_width(OPTION_SHAPES, 19), h19 = get_sprite_height(OPTION_SHAPES, 19);
		int w = MAX(w18, w19), h = MAX(h18, h19);
		int left = MIN(285, 302), right = MAX(285 + w, 302 + w);
		g_cells[g_ncells++] = (HdHudCell){ left, 44, right - left, h };
	}

	// Message bar interior (mainint.c:99-109): classic text at (16,190),
	// punched out until H3 draws HD text here.
	g_cells[g_ncells++] = (HdHudCell){ 16, 188, 263 - 16 + 1, 200 - 188 };

	assert(g_ncells <= MAX_CELLS);

	draw_panel_background();

	draw_shield_bar();
	draw_armor_bar();
	draw_power_bar();
	draw_weapon_dots();
	draw_sidekick_gauge(0);
	draw_sidekick_gauge(1);

	draw_panel_labels();
}
