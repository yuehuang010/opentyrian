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
 * HD in-flight HUD overlay (internal/plan/REMASTER_HUD.md, phase H2.6).
 *
 * State-driven overlay, not draw interception: every present, this redraws the
 * whole sidebar/bottom-bar panel from live game state directly on top of the
 * already-composited classic 8-bit base. Any element this file doesn't cover,
 * or fails to draw, simply shows the classic pixels already there -- nothing is
 * erased or intercepted. Gated to 1P; 2P falls through to the classic panel.
 *
 * H2.6 restores the ORIGINAL PIC #3 panel art (H2.5's fully-procedural "modern
 * flat" panel deviated too far from the original game per user feedback), but
 * crisp: the panel is baked once per level palette by loading a clean PIC #3
 * into a scratch 8-bit surface and running it through the in-repo hq4x pixel-art
 * scaler (video_scale_hqNx.c) into a 4x streaming texture, instead of either the
 * earlier AI-upscaled hdpic03.dat asset (fuzzy) or H2.5's procedural fills.
 * Layout/positions stay EXACTLY where the classic art and the dynamic classic
 * draws put them.
 *
 * Geometry/colors of the dynamic bars are hand-ported from the classic draw
 * sites (see REMASTER_HUD.md) so they read as reproductions, not derivations --
 * JE_dBar3 (nortvars.c), the power bar and weapon dots (tyrian2.c),
 * draw_segmented_gauge (vga256d.c) are never called or modified; they keep
 * drawing into VGAScreenSeg for the classic fallback.
 *
 * Palette / fade sync: the dynamic bars sample the LIVE (post-fade) palette
 * (get_live_palette()), so palette fades and damage flashes track automatically.
 * The baked panel texture is always rendered from the full-brightness target
 * palette (colors[]) and dimmed uniformly via SDL_SetTextureColorMod using the
 * same dim factor, so it never pops to full brightness during a level-start/
 * -end fade. The level name (the PIC's name plate is blank; the classic code
 * draws it separately) stays an HD-font glyph synthesized against the palette
 * and cached by (glyph,hue,value); in practice level fades happen outside
 * hd_flight_active so it only ever synthesizes at full brightness (a
 * documented caching limitation, not a fade bug in normal play).
 */

#include "hd_hud.h"

#include "opentyr.h"

#include "config.h"
#include "fonthand.h"
#include "palette.h"
#include "picload.h"
#include "player.h"
#include "sprite.h"
#include "varz.h"
#include "video.h"
#include "video_scale.h"

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
static float g_dim = 1.f;  // 0..1 fade factor for the baked panel texture

// Punch-out cells: logical VGA rects whose classic pixels (icons, buttons, the
// message text) must keep showing through -- the panel draw skips them.
typedef struct { int x, y, w, h; } HdHudCell;

#define MAX_CELLS 4
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

// ---- HD-font level name -------------------------------------------------------

/* Draws a horizontal string as crisp HD glyphs at present time (green HUD
 * text), advancing by the classic TINY_FONT glyph widths + kerning exactly as
 * JE_outText does. Left-aligned only: the baked panel art's name plate is
 * blank (the classic code draws the level name itself, left-aligned, over
 * PIC #3), so no centring is needed here. */
static void draw_hud_text(int x, int y, const char *s)
{
	// HD glyph textures are cached per (glyph, hue, value) and synthesized at
	// full palette brightness, so they can't be dimmed by the texture-mod path
	// the baked panel uses. Approximate the level fade by stepping the glyph
	// brightness `value` down with g_dim instead -- quantized to 4 levels so at
	// most 4 cached variants per glyph exist (no cache thrash), which is enough
	// to keep the name from popping full-bright over a dimmed panel.
	static const Sint8 fade_value[4] = { -8, -4, 0, HUD_GREEN_VALUE };
	int fi = (int)(g_dim * 4.f);
	if (fi > 3)
		fi = 3;
	const Sint8 value = fade_value[fi];

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
			hd_hud_queue_glyph(TINY_FONT, (unsigned int)id, x, y, HUD_GREEN_HUE, value);
			x += sprite(TINY_FONT, id)->width + 1;
		}
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

// ---- Baked panel art (hq4x upscale of a clean PIC #3) -------------------------

// hq4x_32() always upscales the full 320x200 VGA surface by exactly 4x
// (video_scale_hqNx.c hardcodes vga_width/vga_height, not the surface's own
// w/h), so the bake texture size and the VGA->texture-source scale factor are
// both compile-time constants -- no need to query the texture back.
#define BAKE_TEX_W (4 * vga_width)
#define BAKE_TEX_H (4 * vga_height)

static SDL_Surface *g_bake_scratch;  // 320x200 8bpp scratch for JE_loadPic
static SDL_Texture *g_bake_tex;      // BAKE_TEX_W x BAKE_TEX_H hq4x result
static bool g_bake_setup_failed;     // surface/texture allocation failed once

static bool g_have_baked;            // g_bake_tex holds a valid bake
static SDL_Color g_baked_colors[256]; // colors[] snapshot the bake was made from

/* Mirrors palette.c's static rgb_to_yuv() (not exported): hq4x_32 needs both
 * rgb_palette[] (pixel colour) and yuv_palette[] (its edge-detection metric)
 * refilled consistently from the same source palette, or the antialiasing
 * pattern it picks would be judged against stale colours. */
static Uint32 hud_rgb_to_yuv(int r, int g, int b)
{
	int y = (r + g + b) >> 2,
	    u = 128 + ((r - b) >> 2),
	    v = 128 + ((-r + 2 * g - b) >> 3);
	return ((Uint32)y << 16) + ((Uint32)u << 8) + (Uint32)v;
}

/* Lazily creates the scratch surface + bake texture, then (re)bakes iff
 * colors[] has changed since the last bake (a level load/change, or the very
 * first draw). Returns false -- draw nothing, classic panel shows through --
 * on any allocation failure, fail-once like every other HD asset cache. */
static bool ensure_baked_panel(void)
{
	if (g_bake_setup_failed)
		return false;

	if (g_bake_scratch == NULL)
	{
		// Mirrors how video.c creates the 8-bit VGAScreen family of surfaces.
		g_bake_scratch = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
		if (g_bake_scratch == NULL)
		{
			g_bake_setup_failed = true;
			return false;
		}
	}

	if (g_bake_tex == NULL)
	{
		g_bake_tex = SDL_CreateTexture(g_ren, main_window_tex_format->format,
			SDL_TEXTUREACCESS_STREAMING, BAKE_TEX_W, BAKE_TEX_H);
		if (g_bake_tex == NULL)
		{
			g_bake_setup_failed = true;
			return false;
		}
	}

	if (g_have_baked && memcmp(g_baked_colors, colors, sizeof(colors)) == 0)
		return true; // still current, nothing to do

	// JE_loadPic(..., false) writes raw indices into the scratch surface and,
	// as an unconditional side effect (storepal only gates set_palette()),
	// overwrites the `colors` global with PIC #3's own native palette bank --
	// save/restore around it so this present-time rebake can never leave
	// `colors` (the stable per-level target every other HD-HUD calculation
	// reads) permanently clobbered.
	Palette saved_colors;
	memcpy(saved_colors, colors, sizeof(colors));
	JE_loadPic(g_bake_scratch, 3, false); // PIC #3 == 1P panel (tyrian2.c:816)
	memcpy(colors, saved_colors, sizeof(colors));

	// PIC #3's playfield region (x<264, y<184) is index-0 black. hq4x's 3x3
	// kernel would edge-round the panel border against that black, baking a
	// dark rim into the art along x=264 and y=184 (in classic, that border
	// abuts live tiles, not black). Pad the playfield side by replicating the
	// panel's first row/column outward a few pixels so the filter sees
	// continuation; the padded pixels themselves are never sampled (the strips
	// only draw x>=264 / y>=184).
	{
		Uint8 *px = (Uint8 *)g_bake_scratch->pixels;
		const int pitch = g_bake_scratch->pitch;
		for (int y = 181; y <= 183; ++y)  // strip-top row upward
			memcpy(px + y * pitch, px + 184 * pitch, 264);
		for (int y = 0; y < 184; ++y)     // sidebar-left column outward
			memset(px + y * pitch + 261, px[y * pitch + 264], 3);
	}

	// hq4x_32 samples the global live rgb_palette[]/yuv_palette[]; swap in the
	// full-brightness target palette (colors[]) for the duration of the bake
	// (dimming is applied uniformly afterwards via texture colour mod, not
	// baked in), then restore whatever the live present-time values were.
	Uint32 saved_rgb[256], saved_yuv[256];
	memcpy(saved_rgb, rgb_palette, sizeof(rgb_palette));
	memcpy(saved_yuv, yuv_palette, sizeof(yuv_palette));

	for (int i = 0; i < 256; ++i)
	{
		rgb_palette[i] = SDL_MapRGB(main_window_tex_format, colors[i].r, colors[i].g, colors[i].b);
		yuv_palette[i] = hud_rgb_to_yuv(colors[i].r, colors[i].g, colors[i].b);
	}

	hq4x_32(g_bake_scratch, g_bake_tex);

	memcpy(rgb_palette, saved_rgb, sizeof(rgb_palette));
	memcpy(yuv_palette, saved_yuv, sizeof(yuv_palette));

	memcpy(g_baked_colors, colors, sizeof(colors));
	g_have_baked = true;
	return true;
}

/* Logical VGA rect -> source rect within the bake texture: an exact 4x scale
 * (see BAKE_TEX_W/H above), not a proportional query-based scale. */
static SDL_Rect vga_rect_to_bake_src(int lx, int ly, int lw, int lh)
{
	SDL_Rect r = { lx * 4, ly * 4, lw * 4, lh * 4 };
	return r;
}

/* Draws the VGA strip rect (x,y,w,h) from the baked texture, skipping the
 * punch-out cells. Rect subtraction via coordinate compression: cell y-edges
 * split the strip into horizontal bands, each band's active cells' x-edges
 * split it into spans, and a span is drawn (as a textured RenderCopy) iff its
 * midpoint isn't inside any cell. Cheap and correct for the small,
 * non-overlapping cell counts here; not a general polygon clipper. */
static void draw_panel_strip(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;

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

			bool covered = false;
			for (int i = 0; i < g_ncells; ++i)
				if (mid_x >= g_cells[i].x && mid_x < g_cells[i].x + g_cells[i].w &&
				    mid_y >= g_cells[i].y && mid_y < g_cells[i].y + g_cells[i].h)
				{
					covered = true;
					break;
				}
			if (covered)
				continue;

			SDL_Rect win = vga_rect_to_window(x0, y0, x1 - x0, y1 - y0);
			SDL_Rect src = vga_rect_to_bake_src(x0, y0, x1 - x0, y1 - y0);
			SDL_RenderCopy(g_ren, g_bake_tex, &src, &win);
		}
	}
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

	if (!ensure_baked_panel())
		return; // asset/allocation unavailable: draw nothing, classic base shows

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
	SDL_SetTextureBlendMode(g_bake_tex, SDL_BLENDMODE_BLEND);
	SDL_SetTextureAlphaMod(g_bake_tex, 255);
	Uint8 f = (Uint8)(g_dim * 255.f);
	SDL_SetTextureColorMod(g_bake_tex, f, f, f);

	// Punch-out cells: elements whose classic pixels must keep showing through
	// the baked panel art (the textured strip draw skips these rects).
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

	// The two panel strips (sidebar + bottom bar), from the baked hq4x art.
	draw_panel_strip(264, 0, 56, 200);
	draw_panel_strip(0, 184, 264, 16);

	draw_shield_bar();
	draw_armor_bar();
	draw_power_bar();
	draw_weapon_dots();
	draw_sidekick_gauge(0);
	draw_sidekick_gauge(1);

	// Level name on the plate (blank in the baked art; the classic code draws
	// it separately over VGAScreen too, at the same coordinates).
	draw_hud_text(268, 118, levelName);
}
