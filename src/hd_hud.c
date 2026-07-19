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
 * erased or intercepted. Handles 1P (PIC #3) and genuine 2P (PIC #6, H4) via a
 * `mode` switch through every dynamic element; galagaMode (2P flag set but 1P-
 * style bars) is excluded and falls through to the classic panel.
 *
 * H4 adds HD icon/button bitmaps (hdoption_NN.dat, shared with the menus via the
 * same fail-once texture cache) for the sidekick icons and rear-config buttons,
 * with per-cell classic punch-out fallback when an asset is missing, plus the
 * full two-player layout (per-player shield/armor bars, 2P weapon-dot / sidekick
 * positions, the 2P option-level indicator, and the 2P level-name placement).
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
// message text) must keep showing through -- the panel draw skips them. Worst
// case (1P, every HD icon/button asset missing): 2 sidekick cells + 2 rear-config
// button cells = 4; keep headroom.
typedef struct { int x, y, w, h; } HdHudCell;

#define MAX_CELLS 6
static HdHudCell g_cells[MAX_CELLS];
static int g_ncells;

// ---- Message-bar text shadow (phase H3) --------------------------------------
//
// The bottom message window (JE_drawTextWindow / JE_outCharGlow, VGAScreenSeg
// x16..263 y188..199) is the last classic in-flight text. Instead of covering
// it with a punch-out (blocky classic pixels), the tiny hooks below mirror the
// glyphs into this shadow as the classic code draws them; hd_hud_draw() re-emits
// them as crisp HD glyphs over the baked (empty) window art every present.
//
// Cache pressure: HD glyph textures are cached per (glyph,hue,value) in a 4096-
// entry LRU (HD_FONT_CACHE_COUNT, video.c). JE_outCharGlow ramps the glow value
// over -8..9 (18 variants/glyph); the classic glyph arrays cap a message at 60
// chars. Worst realistic case: 60 chars over one bank ~= (distinct letters ~40)
// x 18 values ~= 720 distinct textures over the whole animation, and a SINGLE
// present emits at most the 29-char glow window + the ~30-char level name + a
// few playfield-HUD glyphs (< 100) -- both far under 4096, so there is no intra-
// present eviction (no dangling queue pointer) and no cross-present thrash.
// Raw glow values are therefore passed through un-quantized (full fidelity).
#define MSG_MAX_GLYPHS 60
typedef struct { bool valid; int x, y; unsigned int sprite_id; Uint8 hue; Sint8 value; } HdMsgGlyph;
static HdMsgGlyph g_msg[MSG_MAX_GLYPHS];
static int g_msg_count;   // one past the highest populated slot
static bool g_msg_active; // a message is currently shadowed

// True only while this HUD layer is the live in-flight producer targeting the
// flight surface -- the gate every message hook shares (classic text screens
// also drive JE_outCharGlow, with VGAScreen pointing elsewhere: they no-op).
static bool msg_capture_live(void)
{
	return hd_mode && hd_flight_active && VGAScreen == VGAScreenSeg;
}

void hd_hud_msg_text(int x, int y, const char *s, Uint8 hue, Sint8 value)
{
	if (!msg_capture_live())
		return;

	// Lay the string out exactly as JE_outText(screen, x, y, s, hue, value)
	// does (fonthand.c): space advances 6px, '~' toggles a +4 brightness bump,
	// other glyphs advance by TINY_FONT width+1. Static text -> no glow ramp.
	g_msg_count = 0;
	int bright = 0;

	for (int i = 0; s[i] != '\0' && g_msg_count < MSG_MAX_GLYPHS; ++i)
	{
		if (s[i] == ' ')
		{
			x += 6;
			continue;
		}
		if (s[i] == '~')
		{
			bright = (bright == 0) ? 4 : 0;
			continue;
		}

		int id = fontMap[(unsigned char)s[i]];
		if (id != -1 && sprite_exists(TINY_FONT, id))
		{
			g_msg[g_msg_count++] = (HdMsgGlyph){ true, x, y, (unsigned int)id, hue, (Sint8)(value + bright) };
			x += sprite(TINY_FONT, id)->width + 1;
		}
	}

	g_msg_active = (g_msg_count > 0);
}

void hd_hud_msg_glow_begin(void)
{
	if (!msg_capture_live())
		return;

	for (int i = 0; i < MSG_MAX_GLYPHS; ++i)
		g_msg[i].valid = false;
	g_msg_count = 0;
	g_msg_active = true; // glyphs stream in per char as the animation advances
}

void hd_hud_msg_glow_char(int slot, int x, int y, unsigned int sprite_id, Uint8 hue, Sint8 value)
{
	if (!msg_capture_live())
		return;
	if (slot < 0 || slot >= MSG_MAX_GLYPHS)
		return;

	g_msg[slot] = (HdMsgGlyph){ true, x, y, sprite_id, hue, value };
	if (slot + 1 > g_msg_count)
		g_msg_count = slot + 1;
	g_msg_active = true;
}

void hd_hud_msg_clear(void)
{
	g_msg_count = 0;
	g_msg_active = false;
}

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

/* Shield bar (JE_drawShield, varz.c:1112). In 1P (else branch, varz.c:1121):
 * one bar at y=194 from player[0].shield, plus the max-shield marker
 * (varz.c:1122-1126). In 2P (varz.c:1114-1118, the twoPlayerMode && !galagaMode
 * branch -- galaga already returned above): one bar per player at y=60+134*i
 * from roundf(shield*0.8f), and NO max-shield marker. */
static void draw_shield_bar(int mode)
{
	if (mode == 1)
	{
		for (int i = 0; i < 2; ++i)
			draw_dbar3(270, 60 + 134 * i, (int)roundf(player[i].shield * 0.8f), 144);
		return;
	}

	Player *p = &player[0];
	draw_dbar3(270, 194, (int)p->shield, 144);

	if (p->shield != p->shield_max)
	{
		int y = 193 - (int)(p->shield_max * 2);
		fill_pal(270, y, 9, 1, 68);
	}
}

/* Armor bar (JE_drawArmor, varz.c:1130); classic clamps armor to 28 as a side
 * effect before drawing -- clamp a local copy instead (no game-state mutation).
 * 1P: one bar at y=194 (varz.c:1143). 2P: one bar per player at y=60+134*i from
 * roundf(armor*0.8f) (varz.c:1136-1139). */
static void draw_armor_bar(int mode)
{
	if (mode == 1)
	{
		for (int i = 0; i < 2; ++i)
			draw_dbar3(307, 60 + 134 * i, (int)roundf(MIN(player[i].armor, 28u) * 0.8f), 224);
		return;
	}

	int armor = (int)MIN(player[0].armor, 28u);
	draw_dbar3(307, 194, armor, 224);
}

/* Weapon-power dots (tyrian2.c:1262-1281): 2px wide x 3px tall, colour 115+j.
 * 1P: x=289, y=17 (front)/38 (rear), player[0]. 2P: x=286, y=6 (front,
 * player[0])/100 (rear, player[1]) -- item_power = player[mode?i:0].weapon[i]. */
static void draw_weapon_dots(int mode)
{
	for (uint i = 0; i < 2; ++i)
	{
		uint item_power = player[mode ? i : 0].items.weapon[i].power;
		int x = mode ? 286 : 289;
		int y = (i == 0) ? (mode ? 6 : 17) : (mode ? 100 : 38);

		for (uint j = 1; j <= item_power; ++j)
		{
			fill_pal(x, y, 2, 3, (Uint8)(115 + j));
			x += 2;
		}
	}
}

/* Main power bar (tyrian2.c:1288-1300) steady-state result, x=269..276. See the
 * long note in the prior revision: draws rows [102-temp,103], row y coloured
 * 113 + min(temp,104-y)/7. Classic SKIPS this in 2P (tyrian2.c:1284 forces
 * power=900 and draws nothing), so the caller only invokes it in 1P. */
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
 * base colour 112, 2x2px segments, segment_value = max(1, ammo_max/10). Reads
 * player[mode?1:0] and hud_sidekick_y[mode], mirroring JE_drawOptions
 * (varz.c:401,422) and the per-frame refill/discharge redraws (mainint.c:4695). */
static void draw_sidekick_gauge(int mode, int i)
{
	Player *p = &player[mode ? 1 : 0];

	int ammo_max = MAX(0, p->sidekick[i].ammo_max);
	int ammo = MAX(0, p->sidekick[i].ammo);
	int segment_value = MAX(1, ammo_max / 10);
	int segments = ammo / segment_value;
	int partial = ammo % segment_value;

	int x = 284;
	int y = hud_sidekick_y[mode][i] + 13;

	for (int s = 0; s < segments; ++s)
	{
		fill_pal(x, y, 2, 2, (Uint8)(112 + 12));
		x += 2 + 1;
	}
	if (partial > 0)
		fill_pal(x, y, 2, 2, (Uint8)(112 + 12 * partial / segment_value));
}

/* 2P option-level indicator (JE_drawOptionLevel, varz.c:437-443): three 2px-wide
 * x 4px-tall vertical ticks at x=268, y=127+(t-1)*6 for t=1..3, coloured 193
 * normally and 204 (193+11) for the tick matching player[1].sidekick_level-100.
 * 1P has no such indicator (JE_drawOptionLevel no-ops when !twoPlayerMode). */
static void draw_option_level_2p(void)
{
	int lit = (int)player[1].items.sidekick_level - 100;
	for (int t = 1; t <= 3; ++t)
	{
		int y = 127 + (t - 1) * 6;
		// fill_rectangle_xy(268, y, 269, y+3) is inclusive: 2px wide, 4px tall.
		fill_pal(268, y, 2, 4, (Uint8)(193 + (lit == t ? 11 : 0)));
	}
}

/* Draws one HD OPTION_SHAPES sprite (hdoption_NN.dat) at logical (x,y) with the
 * classic sprite's logical size, on top of the baked panel art. Returns true if
 * the HD asset was available and drawn; false leaves the caller to keep the
 * classic punch-out for that cell. */
static bool draw_hd_option(int sprite_index, int x, int y)
{
	char name[32];
	snprintf(name, sizeof name, "hdoption_%02d.dat", sprite_index);

	SDL_Texture *tex = hd_hud_get_named_sprite(name, NULL, NULL);
	if (tex == NULL)
		return false;

	int w = get_sprite_width(OPTION_SHAPES, sprite_index);
	int h = get_sprite_height(OPTION_SHAPES, sprite_index);
	SDL_Rect win = vga_rect_to_window(x, y, w, h);
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	SDL_SetTextureAlphaMod(tex, 255);
	Uint8 f = (Uint8)(g_dim * 255.f);
	SDL_SetTextureColorMod(tex, f, f, f);
	SDL_RenderCopy(g_ren, tex, NULL, &win);
	return true;
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
static int g_baked_pic = -1;         // PIC number the current bake was made from

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
static bool ensure_baked_panel(int pic)
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

	if (g_have_baked && g_baked_pic == pic && memcmp(g_baked_colors, colors, sizeof(colors)) == 0)
		return true; // still current (same PIC + palette), nothing to do

	// JE_loadPic(..., false) writes raw indices into the scratch surface and,
	// as an unconditional side effect (storepal only gates set_palette()),
	// overwrites the `colors` global with PIC #3's own native palette bank --
	// save/restore around it so this present-time rebake can never leave
	// `colors` (the stable per-level target every other HD-HUD calculation
	// reads) permanently clobbered.
	Palette saved_colors;
	memcpy(saved_colors, colors, sizeof(colors));
	JE_loadPic(g_bake_scratch, pic, false); // PIC #3 (1P) / #6 (2P) (tyrian2.c:817)
	memcpy(colors, saved_colors, sizeof(colors));

	// The PIC's playfield region (x<264, y<184) is index-0 black -- the 264/184
	// panel split is engine-wide (tyrian2.c playfield composite), same for #3 and
	// #6. hq4x's 3x3
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
	g_baked_pic = pic;
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
	// galagaMode sets twoPlayerMode but draws 1P-style bars against the #6 panel
	// (JE_drawShield's else branch, varz.c:1119) -- not worth a third layout
	// matrix, so keep it fully classic (draw nothing; the classic panel shows).
	// Genuine 2P (twoPlayerMode && !galagaMode) is handled below via `mode`.
	if (galagaMode)
		return;

	const int mode = twoPlayerMode ? 1 : 0; // 0 = 1P (PIC #3), 1 = 2P (PIC #6)
	const int pic = mode ? 6 : 3;

	g_ren = renderer;
	g_dst = dst_rect;
	g_dim = compute_dim();

	if (!ensure_baked_panel(pic))
		return; // asset/allocation unavailable: draw nothing, classic base shows

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
	SDL_SetTextureBlendMode(g_bake_tex, SDL_BLENDMODE_BLEND);
	SDL_SetTextureAlphaMod(g_bake_tex, 255);
	Uint8 f = (Uint8)(g_dim * 255.f);
	SDL_SetTextureColorMod(g_bake_tex, f, f, f);

	// Sidekick icons: try the HD bitmap per cell; on success drop the punch-out
	// (draw a black cell fill + the crisp HD icon on the baked art below), on any
	// failure keep the punch-out so the classic icon shows through. The vector
	// gauge always draws ON TOP afterwards. Icon = OPTION_SHAPES[icongr-1]
	// (JE_drawOptions, varz.c:425-426); this_player = player[mode?1:0].
	g_ncells = 0;
	bool icon_hd[2] = { false, false };
	int  icon_idx[2] = { -1, -1 };
	Player *this_player = &player[mode ? 1 : 0];
	for (int i = 0; i < 2; ++i)
	{
		int y = hud_sidekick_y[mode][i];
		int icongr = options[this_player->items.sidekick[i]].icongr;
		if (icongr > 0)
			icon_idx[i] = icongr - 1;

		// Probe the HD asset; success suppresses the punch-out (the icon is drawn
		// on top of the baked art below). The load is cached, so this probe and
		// the later draw_hd_option() share one parse.
		bool ok = false;
		if (icon_idx[i] >= 0)
		{
			char name[32];
			snprintf(name, sizeof name, "hdoption_%02d.dat", icon_idx[i]);
			ok = (hd_hud_get_named_sprite(name, NULL, NULL) != NULL);
		}
		icon_hd[i] = ok;
		if (!ok)
			// 29x16 erase-rect footprint (JE_drawOptions, varz.c:424).
			g_cells[g_ncells++] = (HdHudCell){ 284, y, 29, 16 };
	}

	// Rear-config buttons (JE_drawPortConfigButtons, mainint.c:210-224): 1P ONLY
	// (it returns for 2P). Left button at (285,44), right at (302,44); which one
	// shows the "lit" sprite (18) vs "unlit" (19) depends on player[0].weapon_mode
	// (==1: left lit / right unlit; else: left unlit / right lit). Try HD per
	// button; punch out only the buttons whose HD asset is missing.
	bool btn_hd[2] = { false, false };
	int  btn_sprite[2] = { -1, -1 };
	const int btn_x[2] = { 285, 302 };
	if (mode == 0)
	{
		int lit = 18, unlit = 19;
		if (player[0].weapon_mode == 1)
			{ btn_sprite[0] = lit;   btn_sprite[1] = unlit; }
		else
			{ btn_sprite[0] = unlit; btn_sprite[1] = lit;   }

		for (int b = 0; b < 2; ++b)
		{
			char name[32];
			snprintf(name, sizeof name, "hdoption_%02d.dat", btn_sprite[b]);
			btn_hd[b] = (hd_hud_get_named_sprite(name, NULL, NULL) != NULL);
			if (!btn_hd[b])
			{
				int w = get_sprite_width(OPTION_SHAPES, btn_sprite[b]);
				int h = get_sprite_height(OPTION_SHAPES, btn_sprite[b]);
				g_cells[g_ncells++] = (HdHudCell){ btn_x[b], 44, w, h };
			}
		}
	}

	// Message-bar interior (mainint.c:99-109): NO punch-out (phase H3). The
	// baked empty-window art draws here and the message-text shadow (populated
	// by the hd_hud_msg_* hooks) re-emits as crisp HD glyphs on top -- see the
	// re-emit at the end of this function. The writer audit (REMASTER_HUD.md H3)
	// confirmed JE_drawTextWindow / JE_outCharGlow / the two erase blits are the
	// only in-flight writers to this region, and all are hooked.

	assert(g_ncells <= MAX_CELLS);

	// The two panel strips (sidebar + bottom bar), from the baked hq4x art.
	draw_panel_strip(264, 0, 56, 200);
	draw_panel_strip(0, 184, 264, 16);

	// HD icons/buttons on top of the baked art (only the cells that loaded).
	for (int i = 0; i < 2; ++i)
		if (icon_hd[i])
		{
			int y = hud_sidekick_y[mode][i];
			fill_pal(284, y, 29, 16, 0); // classic black erase (varz.c:424)
			draw_hd_option(icon_idx[i], 284, y);
		}
	for (int b = 0; b < 2; ++b)
		if (btn_hd[b])
			draw_hd_option(btn_sprite[b], btn_x[b], 44);

	draw_shield_bar(mode);
	draw_armor_bar(mode);
	if (mode == 0)
		draw_power_bar(); // classic skips the power bar in 2P (tyrian2.c:1284)
	draw_weapon_dots(mode);
	draw_sidekick_gauge(mode, 0);
	draw_sidekick_gauge(mode, 1);
	if (mode == 1)
		draw_option_level_2p(); // 2P-only option-level indicator (varz.c:437)

	// Level name on the plate (blank in the baked art; the classic code draws it
	// separately over VGAScreen too, at (268, 118) in 1P / (268, 76) in 2P
	// -- tyrian2.c:825).
	draw_hud_text(268, mode ? 76 : 118, levelName);

	// Message-bar text: re-emit the shadowed glyphs (populated by the
	// hd_hud_msg_* hooks at the classic draw sites) as crisp HD glyphs over the
	// baked window art. Each glyph carries its live hue (bank: 0 for the static
	// window text, 7 warningRed / 15 useLastBank / 14 for the glow) and value
	// (glow ramp), so red warnings render red and the glow animates.
	if (g_msg_active)
	{
		for (int i = 0; i < g_msg_count; ++i)
		{
			const HdMsgGlyph *m = &g_msg[i];
			if (m->valid && sprite_exists(TINY_FONT, m->sprite_id))
				hd_hud_queue_glyph(TINY_FONT, m->sprite_id, m->x, m->y, m->hue, m->value);
		}
	}
}
