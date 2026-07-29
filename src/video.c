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
/** @file video.c
 * SDL2 window/renderer setup and the main VGAScreen present path, plus the HD-remaster overlay
 * layer (backdrops, sprite/flight display list, tile atlases, HD font emission).
 *
 * Entry points: init_video(), JE_showVGA(), hd_set_backdrop(), hd_flight_set(), load_hd_sheet_frame().
 */

#include "video.h"

#include "file.h"
#include "hd_hud.h"
#include "keyboard.h"
#include "opentyr.h"
#include "palette.h"
#include "sprite.h"
#include "video_scale.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const scaling_mode_names[ScalingMode_MAX] = {
	"Center",
	"Integer",
	"Fit 8:5",
	"Fit 4:3",
};

int fullscreen_display;
ScalingMode scaling_mode = SCALE_INTEGER;

int window_width = 0, window_height = 0; // saved free (windowed) window size; 0 = unset
static SDL_Rect last_output_rect = { 0, 0, vga_width, vga_height };

bool hd_mode = true;
bool hd_backdrop_active = false;
int hd_backdrop_id = 0;
int hd_backdrop_fade = 0;

bool crt_mode = false;

bool hd_flight_active = false;
bool hd_font_flight_hud = false;

static SDL_Texture *crt_scanline_tex = NULL;
static SDL_Texture *crt_vignette_tex = NULL;
static int crt_built_w = 0;
static int crt_built_h = 0;

#define HD_BACKDROP_COUNT 13 // matches PCX_NUM (src/pcxmast.h); PIC numbers are 1-based
static SDL_Texture *hd_backdrop_tex[HD_BACKDROP_COUNT + 1];        // [0] unused
static bool hd_backdrop_load_failed[HD_BACKDROP_COUNT + 1];

// The texture scale_and_flip's backdrop branch actually draws this frame, regardless
// of whether it was activated via the numeric PIC path (hd_set_backdrop) or the
// filename-keyed asset path (hd_set_backdrop_asset). Kept in lockstep with
// hd_backdrop_active by both setters; hd_clear_backdrop only needs to flip the flag.
static SDL_Texture *hd_backdrop_cur_tex = NULL;

// Cache of loaded full-screen HD backdrop assets (e.g. hdpcx_tyrset.dat), keyed by
// short asset name ("tyrset"). Mirrors load_hd_sprite's cache-by-filename pattern.
#define HD_BACKDROP_ASSET_CACHE_COUNT 8
static char hd_backdrop_asset_cache_name[HD_BACKDROP_ASSET_CACHE_COUNT][64];
static SDL_Texture *hd_backdrop_asset_cache_tex[HD_BACKDROP_ASSET_CACHE_COUNT];
static bool hd_backdrop_asset_cache_load_failed[HD_BACKDROP_ASSET_CACHE_COUNT];

// Cache of loaded HD sprite overlay textures (e.g. hdplanet_146.dat), keyed by asset filename.
#define HD_SPRITE_CACHE_COUNT 16
static char hd_sprite_cache_name[HD_SPRITE_CACHE_COUNT][64];
static SDL_Texture *hd_sprite_cache_tex[HD_SPRITE_CACHE_COUNT];
static bool hd_sprite_cache_load_failed[HD_SPRITE_CACHE_COUNT];

// Per-frame queue of HD sprite overlays to draw this frame, in logical VGA-space rects.
// Rebuilt every frame by the call sites (immediate-mode pattern); drained by scale_and_flip().
#define HD_SPRITE_QUEUE_COUNT 16
typedef struct
{
	SDL_Texture *tex;
	int lx, ly, lw, lh;
} HDSpriteQueueEntry;
static HDSpriteQueueEntry hd_sprite_queue[HD_SPRITE_QUEUE_COUNT];
static int hd_sprite_queue_count = 0;
static bool hd_sprite_queue_overflow_warned = false;

// ---- HD in-flight sprite compositor (Track B) ----
// Per-frame HD sheet-frame texture cache, keyed by (sheet id, frame index).
// Mirrors load_hd_sprite's "load once, remember failure, never retry" pattern so
// a missing frame file is not re-opened every render.
// 6 static-identity sheets (HD_SHEET_SHEET8..HD_SHEET_EXPLOSION) + 31 dynamic
// per-level enemy banks (HD_SHEET_ENEMY_FIRST..+HD_SHEET_ENEMY_COUNT-1), sized
// with headroom for future static sheets (see sprite.h).
#define HD_FLIGHT_SHEET_COUNT 48
#define HD_FLIGHT_FRAME_MAX 512
static SDL_Texture *hd_sheet_frame_tex[HD_FLIGHT_SHEET_COUNT][HD_FLIGHT_FRAME_MAX];
static bool hd_sheet_frame_load_failed[HD_FLIGHT_SHEET_COUNT][HD_FLIGHT_FRAME_MAX];

// Per-frame queue of HD flight-sprite overlays, in logical VGA-space rects (with
// the playfield x-offset already applied by the caller). Rebuilt every presented
// frame from the interp display list; drained by scale_and_flip().
#define HD_FLIGHT_QUEUE_COUNT 512
typedef struct
{
	SDL_Texture *tex;
	SDL_Rect src;        // source rect within the texture
	int lx, ly, lw, lh;  // logical position/size in the 320x200 VGA space
	SDL_BlendMode blendmode;
	Uint8 r, g, b, a;    // colour / alpha mod
} HDFlightQueueEntry;
static HDFlightQueueEntry hd_flight_queue[HD_FLIGHT_QUEUE_COUNT];
static int hd_flight_queue_count = 0;
static bool hd_flight_queue_overflow_warned = false;

SDL_Surface *VGAScreen, *VGAScreenSeg;
SDL_Surface *VGAScreen2;
SDL_Surface *game_screen;

SDL_Window *main_window = NULL;
static SDL_Renderer *main_window_renderer = NULL;
SDL_PixelFormat *main_window_tex_format = NULL;
static SDL_Texture *main_window_texture = NULL;

static ScalerFunction scaler_function;

static void init_renderer(void);
static void deinit_renderer(void);
static void init_texture(void);
static void deinit_texture(void);

static int window_get_display_index(void);
static void window_center_in_display(int display_index);
static void calc_dst_render_rect(SDL_Surface *src_surface, SDL_Rect *dst_rect);
static void classic_scale_base(SDL_Surface *src_surface, SDL_Rect *dst_rect);
static void scale_and_flip(SDL_Surface *);
static SDL_Texture *load_hdpx_texture(const char *filename);
static bool load_hd_backdrop(int pic_num);
static SDL_Texture *load_hd_sprite(const char *asset_name);
static SDL_Texture *load_hd_backdrop_asset(const char *name);
static void deinit_crt_overlay(void);
static bool build_crt_overlay(int out_w, int out_h);
static void apply_crt_overlay(const SDL_Rect *dst_rect);

void init_video(void)
{
	if (SDL_WasInit(SDL_INIT_VIDEO))
		return;

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) == -1)
	{
		fprintf(stderr, "error: failed to initialize SDL video: %s\n", SDL_GetError());
		exit(1);
	}

	// Create the software surfaces that the game renders to. These are all 320x200x8 regardless
	// of the window size or monitor resolution.
	VGAScreen = VGAScreenSeg = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	VGAScreen2 = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	game_screen = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);

	// The game code writes to surface->pixels directly without locking, so make sure that we
	// indeed created software surfaces that support this.
	assert(!SDL_MUSTLOCK(VGAScreen));
	assert(!SDL_MUSTLOCK(VGAScreen2));
	assert(!SDL_MUSTLOCK(game_screen));

	JE_clr256(VGAScreen);

	// Create the window with a temporary initial size, hidden until we set up the
	// scaler and find the true window size
	main_window = SDL_CreateWindow("OpenTyrian",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		vga_width, vga_height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);

	if (main_window == NULL)
	{
		fprintf(stderr, "error: failed to create window: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	reinit_fullscreen(fullscreen_display);
	init_renderer();
	init_texture();
	init_scaler(scaler);

	// init_scaler() just snapped the window to the scaler's fixed size; if the
	// user had previously resized the window and we saved that size, restore
	// it now instead (free scaling, Phase 5).
	if (fullscreen_display == -1 &&
	    window_width >= scalers[scaler].width && window_height >= scalers[scaler].height)
	{
		SDL_SetWindowSize(main_window, window_width, window_height);
		window_center_in_display(window_get_display_index());
	}

	SDL_ShowWindow(main_window);

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderPresent(main_window_renderer);
}

void deinit_video(void)
{
	// Capture the final free-form window size in case the last resize wasn't
	// otherwise observed (e.g. the window closed mid-drag), so it can be
	// persisted by saveConfiguration(), which runs after this.
	if (fullscreen_display == -1)
	{
		SDL_GetWindowSize(main_window, &window_width, &window_height);
	}

	deinit_texture();
	deinit_renderer();

	SDL_DestroyWindow(main_window);

	SDL_FreeSurface(VGAScreenSeg);
	SDL_FreeSurface(VGAScreen2);
	SDL_FreeSurface(game_screen);

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void init_renderer(void)
{
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

	main_window_renderer = SDL_CreateRenderer(main_window, -1, 0);

	if (main_window_renderer == NULL)
	{
		fprintf(stderr, "error: failed to create renderer: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
}

static void deinit_renderer(void)
{
	deinit_crt_overlay();

	if (main_window_renderer != NULL)
	{
		SDL_DestroyRenderer(main_window_renderer);
		main_window_renderer = NULL;
	}
}

static void init_texture(void)
{
	assert(main_window_renderer != NULL);

	int bpp = 32; // TODOSDL2
	Uint32 format = bpp == 32 ? SDL_PIXELFORMAT_RGB888 : SDL_PIXELFORMAT_RGB565;
	int scaler_w = scalers[scaler].width;
	int scaler_h = scalers[scaler].height;

	main_window_tex_format = SDL_AllocFormat(format);

	main_window_texture = SDL_CreateTexture(main_window_renderer, format, SDL_TEXTUREACCESS_STREAMING, scaler_w, scaler_h);

	if (main_window_texture == NULL)
	{
		fprintf(stderr, "error: failed to create scaler texture %dx%dx%s: %s\n", scaler_w, scaler_h, SDL_GetPixelFormatName(format), SDL_GetError());
		exit(EXIT_FAILURE);
	}
}

static void deinit_texture(void)
{
	if (main_window_texture != NULL)
	{
		SDL_DestroyTexture(main_window_texture);
		main_window_texture = NULL;
	}

	if (main_window_tex_format != NULL)
	{
		SDL_FreeFormat(main_window_tex_format);
		main_window_tex_format = NULL;
	}
}

static int window_get_display_index(void)
{
	return SDL_GetWindowDisplayIndex(main_window);
}

static void window_center_in_display(int display_index)
{
	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	SDL_Rect bounds;
	SDL_GetDisplayBounds(display_index, &bounds);

	SDL_SetWindowPosition(main_window, bounds.x + (bounds.w - win_w) / 2, bounds.y + (bounds.h - win_h) / 2);
}

void reinit_fullscreen(int new_display)
{
	fullscreen_display = new_display;

	if (fullscreen_display >= SDL_GetNumVideoDisplays())
	{
		fullscreen_display = 0;
	}

	SDL_SetWindowFullscreen(main_window, SDL_FALSE);

	// When returning to windowed mode, restore the user's last free-form
	// window size (Phase 5) rather than snapping back to the scaler's fixed
	// size; fall back to the scaler size if no valid saved size exists.
	int w = scalers[scaler].width;
	int h = scalers[scaler].height;
	if (fullscreen_display == -1 && window_width >= w && window_height >= h)
	{
		w = window_width;
		h = window_height;
	}
	SDL_SetWindowSize(main_window, w, h);

	if (fullscreen_display == -1)
	{
		window_center_in_display(window_get_display_index());
	}
	else
	{
		window_center_in_display(fullscreen_display);

		if (SDL_SetWindowFullscreen(main_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		{
			reinit_fullscreen(-1);
			return;
		}
	}
}

void video_on_win_resize(void)
{
	int w, h;
	int scaler_w, scaler_h;

	// Tell video to reinit if the window was manually resized by the user.
	// Also enforce a minimum size on the window.

	SDL_GetWindowSize(main_window, &w, &h);
	scaler_w = scalers[scaler].width;
	scaler_h = scalers[scaler].height;

	if (w < scaler_w || h < scaler_h)
	{
		w = w < scaler_w ? scaler_w : w;
		h = h < scaler_h ? scaler_h : h;

		SDL_SetWindowSize(main_window, w, h);
	}

	// Persist the free-form size (Phase 5) so it can be restored on the next
	// launch or when returning from fullscreen. Fullscreen-desktop sizing
	// reflects the display resolution, not a size the user actually chose, so
	// don't record it as the "free" size.
	if (fullscreen_display == -1)
	{
		window_width = w;
		window_height = h;
	}
}

void toggle_fullscreen(void)
{
	if (fullscreen_display != -1)
		reinit_fullscreen(-1);
	else
		reinit_fullscreen(SDL_GetWindowDisplayIndex(main_window));
}

bool init_scaler(unsigned int new_scaler)
{
	int w = scalers[new_scaler].width,
	    h = scalers[new_scaler].height;
	int bpp = main_window_tex_format->BitsPerPixel; // TODOSDL2

	scaler = new_scaler;

	deinit_texture();
	init_texture();

	if (fullscreen_display == -1)
	{
		// Changing scalers, when not in fullscreen mode, forces the window
		// to resize to exactly match the scaler's output dimensions.
		SDL_SetWindowSize(main_window, w, h);
		window_center_in_display(window_get_display_index());
	}

	switch (bpp)
	{
	case 32:
		scaler_function = scalers[scaler].scaler32;
		break;
	case 16:
		scaler_function = scalers[scaler].scaler16;
		break;
	default:
		scaler_function = NULL;
		break;
	}

	if (scaler_function == NULL)
	{
		assert(false);
		return false;
	}

	return true;
}

bool set_scaling_mode_by_name(const char *name)
{
	for (int i = 0; i < ScalingMode_MAX; ++i)
	{
		 if (strcmp(name, scaling_mode_names[i]) == 0)
		 {
			 scaling_mode = i;
			 return true;
		 }
	}
	return false;
}

void JE_clr256(SDL_Surface *screen)
{
	SDL_FillRect(screen, NULL, 0);
}

void JE_showVGA(void) 
{ 
	scale_and_flip(VGAScreen); 
}

static void calc_dst_render_rect(SDL_Surface *const src_surface, SDL_Rect *const dst_rect)
{
	// Decides how the logical output texture (after software scaling applied) will fit
	// in the window.

	int win_w, win_h;
	SDL_GetRendererOutputSize(main_window_renderer, &win_w, &win_h);

	int maxh_width, maxw_height;

	switch (scaling_mode)
	{
	case SCALE_CENTER:
		SDL_QueryTexture(main_window_texture, NULL, NULL, &dst_rect->w, &dst_rect->h);
		break;
	case SCALE_INTEGER:
		dst_rect->w = src_surface->w;
		dst_rect->h = src_surface->h;
		while (dst_rect->w + src_surface->w <= win_w && dst_rect->h + src_surface->h <= win_h)
		{
			dst_rect->w += src_surface->w;
			dst_rect->h += src_surface->h;
		}
		break;
	case SCALE_ASPECT_8_5:
		maxh_width = win_h * (8.f / 5.f);
		maxw_height = win_w * (5.f / 8.f);

		if (maxh_width > win_w)
		{
			dst_rect->w = win_w;
			dst_rect->h = maxw_height;
		}
		else
		{
			dst_rect->w = maxh_width;
			dst_rect->h = win_h;
		}
		break;
	case SCALE_ASPECT_4_3:
		maxh_width = win_h * (4.f / 3.f);
		maxw_height = win_w * (3.f / 4.f);

		if (maxh_width > win_w)
		{
			dst_rect->w = win_w;
			dst_rect->h = maxw_height;
		}
		else
		{
			dst_rect->w = maxh_width;
			dst_rect->h = win_h;
		}
		break;
	case ScalingMode_MAX:
		assert(false);
		break;
	}

	dst_rect->x = (win_w - dst_rect->w) / 2;
	dst_rect->y = (win_h - dst_rect->h) / 2;
}

/**
 * Shared HDPX loader: reads a "HDPX" magic + u32 width/height + raw RGBA32 pixel
 * blob from `filename` in the data directory and uploads it into a new static
 * texture. Returns the texture, or NULL if the file is missing/malformed. Used
 * by load_hd_backdrop, load_hd_sprite, and load_hd_backdrop_asset so the HDPX
 * parsing itself lives in exactly one place.
 */
/**
 * Reads a "HDPX" magic + u32 width/height + raw RGBA32 pixel blob from `filename`
 * in the data directory into a freshly malloc'd buffer (caller frees). Returns the
 * pixel buffer and writes the dimensions to out_width/out_height, or NULL if the
 * file is missing/malformed. Shared by the static-texture loader (load_hdpx_texture)
 * and the streaming ending-cutscene loader (hd_anim_show_frame) so the HDPX parsing
 * lives in exactly one place.
 */
static Uint8 *load_hdpx_pixels(const char *filename, Uint32 *out_width, Uint32 *out_height)
{
	Uint8 *pixels = NULL;

	FILE *f = dir_fopen(data_dir(), filename, "rb");
	if (f != NULL)
	{
		Uint8 magic[4];
		Uint8 dims[8];

		if (fread(magic, 1, 4, f) == 4 &&
		    memcmp(magic, "HDPX", 4) == 0 &&
		    fread(dims, 1, 8, f) == 8)
		{
			Uint32 width, height;
			memcpy(&width, &dims[0], 4);
			memcpy(&height, &dims[4], 4);
			width = SDL_SwapLE32(width);
			height = SDL_SwapLE32(height);

			if (width > 0 && height > 0 && width <= 8192 && height <= 8192)
			{
				size_t pixel_bytes = (size_t)width * height * 4;
				Uint8 *buf = malloc(pixel_bytes);

				if (buf != NULL && fread(buf, 1, pixel_bytes, f) == pixel_bytes)
				{
					pixels = buf;
					*out_width = width;
					*out_height = height;
				}
				else
				{
					free(buf);
				}
			}
		}

		fclose(f);
	}

	return pixels;
}

static SDL_Texture *load_hdpx_texture(const char *filename)
{
	SDL_Texture *tex = NULL;

	Uint32 width, height;
	Uint8 *pixels = load_hdpx_pixels(filename, &width, &height);
	if (pixels != NULL)
	{
		tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_STATIC, (int)width, (int)height);

		if (tex != NULL)
		{
			if (SDL_UpdateTexture(tex, NULL, pixels, (int)(width * 4)) == 0)
			{
				SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
			}
			else
			{
				SDL_DestroyTexture(tex);
				tex = NULL;
			}
		}

		free(pixels);
	}

	return tex;
}

/**
 * Lazily loads the HD backdrop asset ("hdpicNN.dat") for the given 1-based PIC
 * number into hd_backdrop_tex[pic_num]. Returns true if the texture is ready
 * to use. Missing/malformed assets degrade gracefully (HD compositing is
 * simply skipped) rather than crashing; the warning is only ever printed once
 * per PIC.
 */
static bool load_hd_backdrop(int pic_num)
{
	if (pic_num < 1 || pic_num > HD_BACKDROP_COUNT)
		return false;

	if (hd_backdrop_tex[pic_num] != NULL)
		return true;

	if (hd_backdrop_load_failed[pic_num])
		return false;

	char name[16];
	snprintf(name, sizeof name, "hdpic%02d.dat", pic_num);

	SDL_Texture *tex = load_hdpx_texture(name);
	if (tex == NULL)
	{
		fprintf(stderr, "warning: HD backdrop hdpic%02d.dat unavailable; falling back\n", pic_num);
		hd_backdrop_load_failed[pic_num] = true;
		return false;
	}

	hd_backdrop_tex[pic_num] = tex;
	return true;
}

bool hd_set_backdrop(int pic_num)
{
	// Only begin HD compositing if the HD asset actually loads. This lets callers
	// gate the destructive JE_clr256() on the return value, so a missing/broken HD
	// asset falls back to the classic backdrop pixels rather than a wiped black one.
	if (!load_hd_backdrop(pic_num))
	{
		hd_backdrop_active = false;
		return false;
	}

	hd_backdrop_id = pic_num;
	hd_backdrop_cur_tex = hd_backdrop_tex[pic_num];
	hd_backdrop_active = true;
	hd_backdrop_fade = 0;
	return true;
}

/**
 * Lazily loads and caches the full-screen HD backdrop asset ("hdpcx_<name>.dat")
 * for the given short asset name (e.g. "tyrset" -> "hdpcx_tyrset.dat"). Returns
 * the cached texture, or NULL if the asset is missing/malformed; a load failure
 * is only ever reported (and retried) once per distinct asset name.
 */
static SDL_Texture *load_hd_backdrop_asset(const char *name)
{
	int slot = -1;
	for (int i = 0; i < HD_BACKDROP_ASSET_CACHE_COUNT; ++i)
	{
		if (hd_backdrop_asset_cache_name[i][0] != '\0' && strcmp(hd_backdrop_asset_cache_name[i], name) == 0)
		{
			if (hd_backdrop_asset_cache_load_failed[i])
				return NULL;
			return hd_backdrop_asset_cache_tex[i];
		}
		if (slot < 0 && hd_backdrop_asset_cache_name[i][0] == '\0')
			slot = i;
	}

	if (slot < 0)
	{
		fprintf(stderr, "warning: HD backdrop asset cache full; cannot load %s\n", name);
		return NULL;
	}

	snprintf(hd_backdrop_asset_cache_name[slot], sizeof hd_backdrop_asset_cache_name[slot], "%s", name);

	char filename[64];
	snprintf(filename, sizeof filename, "hdpcx_%s.dat", name);

	SDL_Texture *tex = load_hdpx_texture(filename);
	if (tex == NULL)
	{
		fprintf(stderr, "warning: HD backdrop asset %s unavailable; falling back\n", filename);
		hd_backdrop_asset_cache_load_failed[slot] = true;
		return NULL;
	}

	hd_backdrop_asset_cache_tex[slot] = tex;
	return tex;
}

bool hd_set_backdrop_asset(const char *name)
{
	// Mirrors hd_set_backdrop(int): only begin HD compositing if the asset actually
	// loads, so callers can gate the destructive JE_clr256() on the return value.
	SDL_Texture *tex = load_hd_backdrop_asset(name);
	if (tex == NULL)
	{
		hd_backdrop_active = false;
		return false;
	}

	hd_backdrop_cur_tex = tex;
	hd_backdrop_active = true;
	hd_backdrop_fade = 0;
	return true;
}

void hd_clear_backdrop(void)
{
	hd_backdrop_active = false;
}

// --- HD ending-cutscene streaming compositor ---
// The ending anm (tyrend.anm) has 111 HD frames, which would thrash the 8-slot
// fail-once asset cache used by hd_set_backdrop_asset. Instead we keep one
// persistent STREAMING texture, created lazily from the first frame's dimensions
// and SDL_UpdateTexture'd in place per frame, then point the existing HD backdrop
// draw path (hd_backdrop_cur_tex / hd_backdrop_active in scale_and_flip) at it.
static SDL_Texture *hd_anim_tex = NULL;
static int hd_anim_tex_w = 0, hd_anim_tex_h = 0;

void hd_anim_begin(void)
{
	// The streaming texture is created lazily on the first frame (once its
	// dimensions are known); here we only arm brightness. The ending anm plays
	// at full brightness -- unlike hd_set_backdrop, there is no HD fade-in.
	hd_backdrop_fade = 255;
}

bool hd_anim_show_frame(const char *basename, int frame_index)
{
	char filename[64];
	snprintf(filename, sizeof filename, "hd%s_%04d.dat", basename, frame_index);

	Uint32 width, height;
	Uint8 *pixels = load_hdpx_pixels(filename, &width, &height);
	if (pixels == NULL)
		return false;  // leave whatever was showing -> classic 320x200 fallback

	bool ok = false;

	// Create the persistent streaming texture once, on the first frame that loads.
	if (hd_anim_tex == NULL)
	{
		hd_anim_tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
			SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
		if (hd_anim_tex != NULL)
		{
			SDL_SetTextureBlendMode(hd_anim_tex, SDL_BLENDMODE_BLEND);
			hd_anim_tex_w = (int)width;
			hd_anim_tex_h = (int)height;
		}
	}

	if (hd_anim_tex != NULL &&
	    (int)width == hd_anim_tex_w && (int)height == hd_anim_tex_h &&
	    SDL_UpdateTexture(hd_anim_tex, NULL, pixels, (int)(width * 4)) == 0)
	{
		hd_backdrop_cur_tex = hd_anim_tex;
		hd_backdrop_active = true;
		ok = true;
	}

	free(pixels);
	return ok;
}

void hd_anim_end(void)
{
	// Stop compositing and don't leave a stale HD backdrop for the next screen.
	hd_clear_backdrop();
	hd_backdrop_cur_tex = NULL;

	if (hd_anim_tex != NULL)
	{
		SDL_DestroyTexture(hd_anim_tex);
		hd_anim_tex = NULL;
	}
	hd_anim_tex_w = hd_anim_tex_h = 0;
}

/**
 * Lazily loads and caches the HD sprite overlay asset with the given filename
 * (e.g. "hdplanet_146.dat") from the data directory. Returns the cached texture,
 * or NULL if the asset is missing/malformed; a load failure is only ever
 * reported (and retried) once per distinct asset name.
 */
static SDL_Texture *load_hd_sprite(const char *asset_name)
{
	int slot = -1;
	for (int i = 0; i < HD_SPRITE_CACHE_COUNT; ++i)
	{
		if (hd_sprite_cache_name[i][0] != '\0' && strcmp(hd_sprite_cache_name[i], asset_name) == 0)
		{
			if (hd_sprite_cache_load_failed[i])
				return NULL;
			return hd_sprite_cache_tex[i];
		}
		if (slot < 0 && hd_sprite_cache_name[i][0] == '\0')
			slot = i;
	}

	if (slot < 0)
	{
		fprintf(stderr, "warning: HD sprite cache full; cannot load %s\n", asset_name);
		return NULL;
	}

	snprintf(hd_sprite_cache_name[slot], sizeof hd_sprite_cache_name[slot], "%s", asset_name);

	SDL_Texture *tex = load_hdpx_texture(asset_name);
	if (tex == NULL)
	{
		fprintf(stderr, "warning: HD sprite %s unavailable; falling back\n", asset_name);
		hd_sprite_cache_load_failed[slot] = true;
		return NULL;
	}

	hd_sprite_cache_tex[slot] = tex;
	return tex;
}

bool hd_set_sprite(const char *asset_name, int lx, int ly, int lw, int lh)
{
	// Only claim the sprite (suppressing the caller's classic blit) when it will
	// actually be composited: scale_and_flip only draws the overlay queue on the
	// HD-backdrop-active path. Without the hd_backdrop_active check, a present HD
	// sprite asset paired with a missing/failed backdrop would make the caller skip
	// the classic blit while the overlay is silently dropped -- the sprite vanishes.
	if (!hd_mode || !hd_backdrop_active)
		return false;

	SDL_Texture *tex = load_hd_sprite(asset_name);
	if (tex == NULL)
		return false;

	if (hd_sprite_queue_count >= HD_SPRITE_QUEUE_COUNT)
	{
		if (!hd_sprite_queue_overflow_warned)
		{
			fprintf(stderr, "warning: HD sprite overlay queue full; dropping %s\n", asset_name);
			hd_sprite_queue_overflow_warned = true;
		}
		return false;
	}

	HDSpriteQueueEntry *entry = &hd_sprite_queue[hd_sprite_queue_count++];
	entry->tex = tex;
	entry->lx = lx;
	entry->ly = ly;
	entry->lw = lw;
	entry->lh = lh;

	return true;
}

void hd_clear_sprites(void)
{
	hd_sprite_queue_count = 0;
}

SDL_Texture *hd_hud_get_named_sprite(const char *asset_name, int *out_w, int *out_h)
{
	// Shares load_hd_sprite's fail-once, load-once texture cache (no duplicate
	// parsing) so the HD HUD panel's icons/buttons reuse the same textures the
	// menus load via hd_set_sprite. Present-time lookup for the flight HUD; the
	// caller (hd_hud_draw) already runs only under hd_mode && hd_flight_active.
	if (!hd_mode)
		return NULL;

	SDL_Texture *tex = load_hd_sprite(asset_name);
	if (tex == NULL)
		return NULL;

	if (out_w != NULL || out_h != NULL)
		SDL_QueryTexture(tex, NULL, NULL, out_w, out_h);

	return tex;
}

// Immediate-mode HD mouse cursor overlay: a single slot (only one cursor ever
// exists) holding the four shopSpriteSheet sub-sprite textures of the classic
// 2x2 cursor blit (index, index+1, index+19, index+20 -- blit_sprite2x2's
// layout) plus the cursor's logical VGA top-left. Re-armed by
// JE_mouseStart(Filter) each frame it draws the classic cursor and drained by
// scale_and_flip(); if the cursor isn't re-emitted before the next present
// (JE_mouseReplace erased it and nothing re-showed it), nothing is drawn --
// mirroring the classic erase/redraw cycle.
static SDL_Texture *hd_cursor_tex[4];
static int hd_cursor_lx, hd_cursor_ly;
static bool hd_cursor_valid = false;

void hd_set_cursor(int sprite_index, int lx, int ly)
{
	if (!hd_mode)
		return;

	const int sub_index[4] = { sprite_index, sprite_index + 1, sprite_index + 19, sprite_index + 20 };

	SDL_Texture *tex[4];
	for (int i = 0; i < 4; ++i)
	{
		// Engine Sprite2 indices are 1-based (blit_sprite2 reads entry
		// [index - 1]); the hdcomp files are numbered by 0-based data entry,
		// same as interp.c's frame = index - 1 conversion.
		char name[32];
		snprintf(name, sizeof name, "hdcomp_shop_%02d.dat", sub_index[i] - 1);
		tex[i] = load_hd_sprite(name);

		// All four quadrants or nothing: a partially-HD cursor would render
		// with holes, so any missing sub-sprite keeps the classic cursor.
		if (tex[i] == NULL)
			return;
	}

	memcpy(hd_cursor_tex, tex, sizeof hd_cursor_tex);
	hd_cursor_lx = lx;
	hd_cursor_ly = ly;
	hd_cursor_valid = true;
}

/**
 * Lazily loads and caches one HD sheet frame ("hdcomp_<stem>_NN.dat") for the
 * given (sheet id, frame index). Returns the cached texture, or NULL if the asset
 * is missing/malformed. Mirrors load_hd_sprite: a load failure is only ever
 * reported (and retried) once per distinct (sheet, index).
 */
SDL_Texture *load_hd_sheet_frame(int sheet_id, int index)
{
	if (sheet_id < 0 || sheet_id >= HD_FLIGHT_SHEET_COUNT || index < 0 || index >= HD_FLIGHT_FRAME_MAX)
		return NULL;

	if (hd_sheet_frame_tex[sheet_id][index] != NULL)
		return hd_sheet_frame_tex[sheet_id][index];

	if (hd_sheet_frame_load_failed[sheet_id][index])
		return NULL;

	const char *stem = hd_sheet_stem(sheet_id);
	if (stem == NULL)
	{
		hd_sheet_frame_load_failed[sheet_id][index] = true;
		return NULL;
	}

	char name[32];
	snprintf(name, sizeof name, "hdcomp_%s_%02d.dat", stem, index);

	bool ok = false;
	SDL_Texture *tex = NULL;

	FILE *f = dir_fopen(data_dir(), name, "rb");
	if (f != NULL)
	{
		Uint8 magic[4];
		Uint8 dims[8];
		Uint8 *pixels = NULL;

		if (fread(magic, 1, 4, f) == 4 &&
		    memcmp(magic, "HDPX", 4) == 0 &&
		    fread(dims, 1, 8, f) == 8)
		{
			Uint32 width, height;
			memcpy(&width, &dims[0], 4);
			memcpy(&height, &dims[4], 4);
			width = SDL_SwapLE32(width);
			height = SDL_SwapLE32(height);

			if (width > 0 && height > 0 && width <= 8192 && height <= 8192)
			{
				size_t pixel_bytes = (size_t)width * height * 4;
				pixels = malloc(pixel_bytes);

				if (pixels != NULL && fread(pixels, 1, pixel_bytes, f) == pixel_bytes)
				{
					tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
						SDL_TEXTUREACCESS_STATIC, (int)width, (int)height);

					if (tex != NULL)
					{
						if (SDL_UpdateTexture(tex, NULL, pixels, (int)(width * 4)) == 0)
						{
							SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
							ok = true;
						}
						else
						{
							SDL_DestroyTexture(tex);
							tex = NULL;
						}
					}
				}
			}
		}

		free(pixels);
		fclose(f);
	}

	if (!ok)
	{
		fprintf(stderr, "warning: HD sheet frame %s unavailable; falling back\n", name);
		hd_sheet_frame_load_failed[sheet_id][index] = true;
		return NULL;
	}

	hd_sheet_frame_tex[sheet_id][index] = tex;
	return tex;
}

bool hd_flight_lookup(int sheet_id, int index)
{
	return load_hd_sheet_frame(sheet_id, index) != NULL;
}

// ---- HD level-tileset atlases (Phase S3) ----
//
// One HDPX grid atlas per (bank, palette): 20 columns x 30 rows of 96x112 cells
// (1920x3360), covering the 600 bank-z tile slots. Same "load once, remember
// failure, never retry" caching as load_hd_sheet_frame so a missing atlas isn't
// re-opened every frame. 5 banks (shape chars ')','w','x','y','z') x 23 palettes.
#define HD_TILE_BANKS 5
#define HD_TILE_ATLAS_COLS 20
#define HD_TILE_CELL_W 96
#define HD_TILE_CELL_H 112
static const char hd_tile_bank_char[HD_TILE_BANKS] = { ')', 'w', 'x', 'y', 'z' };
static SDL_Texture *hd_tile_atlas_tex[HD_TILE_BANKS][PALETTE_COUNT];
static bool hd_tile_atlas_load_failed[HD_TILE_BANKS][PALETTE_COUNT];

SDL_Texture *load_hd_tile_atlas(int bank, int palette)
{
	if (bank < 0 || bank >= HD_TILE_BANKS || palette < 0 || palette >= PALETTE_COUNT)
		return NULL;

	if (hd_tile_atlas_tex[bank][palette] != NULL)
		return hd_tile_atlas_tex[bank][palette];

	if (hd_tile_atlas_load_failed[bank][palette])
		return NULL;

	char name[32];
	snprintf(name, sizeof name, "hdtile_%c_p%02d.dat", hd_tile_bank_char[bank], palette);

	SDL_Texture *tex = load_hdpx_texture(name);
	if (tex == NULL)
	{
		hd_tile_atlas_load_failed[bank][palette] = true;
		return NULL;
	}

	hd_tile_atlas_tex[bank][palette] = tex;
	return tex;
}

void hd_tile_atlas_src(int z, SDL_Rect *src)
{
	src->x = (z % HD_TILE_ATLAS_COLS) * HD_TILE_CELL_W;
	src->y = (z / HD_TILE_ATLAS_COLS) * HD_TILE_CELL_H;
	src->w = HD_TILE_CELL_W;
	src->h = HD_TILE_CELL_H;
}

// Which base palette (0..PALETTE_COUNT-1) is currently live, or -1 if colors[]
// doesn't exactly match any base palette (a filter tint, an in-progress fade, or
// a custom/interlude palette) -- in which case HD tiles are skipped for the frame
// and the classic 8-bit background carries the correct (tinted) look.
int current_palette_index(void)
{
	for (int p = 0; p < PALETTE_COUNT; ++p)
	{
		if (memcmp(colors, palettes[p], sizeof(Palette)) == 0)
			return p;
	}
	return -1;
}

// ---- _filter (hue-band) recoloring parity: on-demand recolored-frame cache ----
//
// blit_sprite2_filter (src/sprite.c) draws `*pixels = filter | (*data & 0x0f)`:
// the *raw* filter byte is OR'd with the sprite data's low nibble (0..15,
// "brightness"), verbatim -- not a clean high-nibble/low-nibble split. Observed
// filter bytes confirm this: 0x70 (Magneto RePulse) is high-nibble-only, but
// 0x09 (iced/frozen tint, tyrian2.c:375) and the event-type-27 "Enemy Global
// AccelRev" values (tyrian2.c:4714, raw eventdat3 in 1..16, unshifted) have low
// bits set too. `hd_filter_manifest.json` documents the same generic formula:
// output = live_palette[filter | brightness_nibble]. So synthesis below uses
// `filter | k` for k in 0..15 literally, which reproduces the engine's output
// for every filter byte actually seen (clean bands included, since OR with a
// zero low nibble degenerates to the simple case).

// Raw brightness-map pixels (RGBA, R=G=B=brightness, A=coverage), loaded from
// "hdcompb_<stem>_NN.dat" and cached per (sheet, index) -- same "load once,
// remember failure, never retry" pattern as load_hd_sheet_frame. Only the enemy
// sheets have this asset (blit_sprite2_filter is only ever called from
// blit_enemy(), tyrian2.c); any other sheet id fails to open the file once and
// is cheaply remembered as unavailable. Fixed 48x56 (12x14 base tile * 4x scale,
// matching the hdcomp_* colour assets); any other size is treated as malformed.
#define HD_FILTER_BRIGHTNESS_W 48
#define HD_FILTER_BRIGHTNESS_H 56
static Uint8 *hd_sheet_frame_brightness[HD_FLIGHT_SHEET_COUNT][HD_FLIGHT_FRAME_MAX];
static bool hd_sheet_frame_brightness_load_failed[HD_FLIGHT_SHEET_COUNT][HD_FLIGHT_FRAME_MAX];

static const Uint8 *load_hd_sheet_frame_brightness(int sheet_id, int index)
{
	if (sheet_id < 0 || sheet_id >= HD_FLIGHT_SHEET_COUNT || index < 0 || index >= HD_FLIGHT_FRAME_MAX)
		return NULL;

	if (hd_sheet_frame_brightness[sheet_id][index] != NULL)
		return hd_sheet_frame_brightness[sheet_id][index];

	if (hd_sheet_frame_brightness_load_failed[sheet_id][index])
		return NULL;

	const char *stem = hd_sheet_stem(sheet_id);
	if (stem == NULL)
	{
		hd_sheet_frame_brightness_load_failed[sheet_id][index] = true;
		return NULL;
	}

	char name[32];
	snprintf(name, sizeof name, "hdcompb_%s_%02d.dat", stem, index);

	bool ok = false;
	Uint8 *pixels = NULL;

	FILE *f = dir_fopen(data_dir(), name, "rb");
	if (f != NULL)
	{
		Uint8 magic[4];
		Uint8 dims[8];

		if (fread(magic, 1, 4, f) == 4 &&
		    memcmp(magic, "HDPX", 4) == 0 &&
		    fread(dims, 1, 8, f) == 8)
		{
			Uint32 width, height;
			memcpy(&width, &dims[0], 4);
			memcpy(&height, &dims[4], 4);
			width = SDL_SwapLE32(width);
			height = SDL_SwapLE32(height);

			if (width == HD_FILTER_BRIGHTNESS_W && height == HD_FILTER_BRIGHTNESS_H)
			{
				size_t pixel_bytes = (size_t)width * height * 4;
				pixels = malloc(pixel_bytes);

				if (pixels != NULL && fread(pixels, 1, pixel_bytes, f) == pixel_bytes)
					ok = true;
			}
		}

		fclose(f);
	}

	if (!ok)
	{
		free(pixels);
		fprintf(stderr, "warning: HD brightness map %s unavailable; falling back\n", name);
		hd_sheet_frame_brightness_load_failed[sheet_id][index] = true;
		return NULL;
	}

	hd_sheet_frame_brightness[sheet_id][index] = pixels;
	return pixels;
}

// Synthesized (sheet, index, filter) recolor textures. LRU-capped; the working
// set of distinct (index, filter) pairs on screen at once is small (tens). MUST
// be >= HD_FLIGHT_QUEUE_COUNT: these textures are handed to the flight queue
// during queue-build and not drawn until scale_and_flip, so evicting (and
// destroying) an entry synthesized earlier in the SAME frame would leave a
// dangling pointer in the queue. Sizing the cache >= the queue guarantees no
// intra-frame eviction (distinct filtered entries this frame <= queue capacity).
#define HD_FILTERED_CACHE_COUNT HD_FLIGHT_QUEUE_COUNT
typedef struct
{
	bool used;
	int sheet_id;
	int index;
	Uint8 filter;
	SDL_Texture *tex;
	Uint32 last_touch;
} HDFilteredCacheEntry;
static HDFilteredCacheEntry hd_filtered_cache[HD_FILTERED_CACHE_COUNT];
static Uint32 hd_filtered_cache_clock = 0;

SDL_Texture *load_hd_sheet_frame_filtered(int sheet_id, int index, Uint8 filter)
{
	if (sheet_id < 0 || sheet_id >= HD_FLIGHT_SHEET_COUNT || index < 0 || index >= HD_FLIGHT_FRAME_MAX)
		return NULL;

	++hd_filtered_cache_clock;

	for (int i = 0; i < HD_FILTERED_CACHE_COUNT; ++i)
	{
		HDFilteredCacheEntry *e = &hd_filtered_cache[i];
		if (e->used && e->sheet_id == sheet_id && e->index == index && e->filter == filter)
		{
			e->last_touch = hd_filtered_cache_clock;
			return e->tex;
		}
	}

	const Uint8 *brightness = load_hd_sheet_frame_brightness(sheet_id, index);
	if (brightness == NULL)
		return NULL;

	// Palette source: the live VGA palette (colors[256]), sampled at synthesis
	// time. Enemies use the stable level palette throughout flight, so this is
	// exact almost always; the one caveat is an enemy hit/recolored mid-fade
	// (level transition, death sequence) could show the pre/post-fade tint for
	// one synth instead of the exact interpolated fade value. Accepted per the
	// design doc rather than wiring a palette-generation cache-invalidation hook.
	SDL_Color shade[16];
	for (int k = 0; k < 16; ++k)
		shade[k] = colors[(Uint8)(filter | k)];

	static Uint8 rgba[HD_FILTER_BRIGHTNESS_W * HD_FILTER_BRIGHTNESS_H * 4];
	for (int p = 0; p < HD_FILTER_BRIGHTNESS_W * HD_FILTER_BRIGHTNESS_H; ++p)
	{
		Uint8 a = brightness[p * 4 + 3];
		if (a == 0)
		{
			memset(&rgba[p * 4], 0, 4);
			continue;
		}

		Uint8 r_chan = brightness[p * 4 + 0]; // R = G = B = brightness (see manifest)
		float pos = (r_chan / 255.0f) * 15.0f;
		int lo = (int)pos;
		if (lo > 14)
			lo = 14;
		int hi = lo + 1;
		float t = pos - (float)lo;

		rgba[p * 4 + 0] = (Uint8)lrintf(shade[lo].r + (shade[hi].r - shade[lo].r) * t);
		rgba[p * 4 + 1] = (Uint8)lrintf(shade[lo].g + (shade[hi].g - shade[lo].g) * t);
		rgba[p * 4 + 2] = (Uint8)lrintf(shade[lo].b + (shade[hi].b - shade[lo].b) * t);
		rgba[p * 4 + 3] = a;
	}

	SDL_Texture *tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, HD_FILTER_BRIGHTNESS_W, HD_FILTER_BRIGHTNESS_H);
	if (tex == NULL)
		return NULL;

	if (SDL_UpdateTexture(tex, NULL, rgba, HD_FILTER_BRIGHTNESS_W * 4) != 0)
	{
		SDL_DestroyTexture(tex);
		return NULL;
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

	int slot = -1;
	for (int i = 0; i < HD_FILTERED_CACHE_COUNT; ++i)
	{
		if (!hd_filtered_cache[i].used)
		{
			slot = i;
			break;
		}
	}
	if (slot < 0)
	{
		Uint32 oldest = 0xffffffffu;
		for (int i = 0; i < HD_FILTERED_CACHE_COUNT; ++i)
		{
			if (hd_filtered_cache[i].last_touch < oldest)
			{
				oldest = hd_filtered_cache[i].last_touch;
				slot = i;
			}
		}
		SDL_DestroyTexture(hd_filtered_cache[slot].tex);
	}

	hd_filtered_cache[slot].used = true;
	hd_filtered_cache[slot].sheet_id = sheet_id;
	hd_filtered_cache[slot].index = index;
	hd_filtered_cache[slot].filter = filter;
	hd_filtered_cache[slot].tex = tex;
	hd_filtered_cache[slot].last_touch = hd_filtered_cache_clock;

	return tex;
}

void hd_flight_begin(void)
{
	hd_flight_queue_count = 0;
}

void hd_flight_set(SDL_Texture *tex, SDL_Rect src, int lx, int ly, int lw, int lh, SDL_BlendMode blendmode, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	if (tex == NULL)
		return;

	if (hd_flight_queue_count >= HD_FLIGHT_QUEUE_COUNT)
	{
		if (!hd_flight_queue_overflow_warned)
		{
			fprintf(stderr, "warning: HD flight sprite queue full; dropping sprites\n");
			hd_flight_queue_overflow_warned = true;
		}
		return;
	}

	HDFlightQueueEntry *entry = &hd_flight_queue[hd_flight_queue_count++];
	entry->tex = tex;
	entry->src = src;
	entry->lx = lx;
	entry->ly = ly;
	entry->lw = lw;
	entry->lh = lh;
	entry->blendmode = blendmode;
	entry->r = r;
	entry->g = g;
	entry->b = b;
	entry->a = a;
}

void hd_flight_clear(void)
{
	hd_flight_queue_count = 0;
}

// ---- HD font glyph synthesis (Track C: HD text) ----
//
// Font glyphs are runtime-recolored the same way flight sprites are: each classic
// draw call (src/fonthand.c, src/font.c) passes a hue/colorbank band plus a signed
// brightness "value" into one of blit_sprite_hv / _hv_unsafe / _hv_blend / _dark
// (src/sprite.c), which compute the destination palette index per pixel from the
// glyph byte's low nibble (a 0..15 brightness level). We reproduce that math
// against the live palette (colors[]) starting from a grayscale brightness map
// (hdfont_<stem>_NN.dat: R = brightness*17, A = coverage; 4x), synthesizing one
// RGBA texture per distinct (table, index, hue, value, mode) and caching it LRU.
// Any failure returns NULL, so the caller keeps the classic glyph.
//
// The assets are produced by tools/hd_vectorize_font.py, which VECTOR-TRACES the
// original glyphs: alpha is the sub-pixel coverage of the traced silhouette, and
// R=G=B is the per-pixel source shade nibble, edge-extended before upsampling. (It
// replaced the earlier xBRZ upscale of tools/hd_extract_font.py in place -- same
// filenames, same HDPX layout, same 4x scale. Check hd_font_manifest.json's
// "format" field to see which generation a data dir actually carries.)

#define HD_FONT_TABLE_COUNT 3          // FONT_SHAPES, SMALL_FONT_SHAPES, TINY_FONT
#define HD_FONT_GLYPH_MAX   128        // TINY_FONT has 127 glyphs

// Asset stems keyed by sprite-table index (FONT_SHAPES=0, SMALL_FONT_SHAPES=1,
// TINY_FONT=2); confirmed against tyrian21/hd_font_manifest.json prefixes.
static const char *const hd_font_stems[HD_FONT_TABLE_COUNT] = { "font", "small", "tiny" };

typedef struct { Uint8 *pixels; int w, h; } HDFontBrightness;
static HDFontBrightness hd_font_brightness[HD_FONT_TABLE_COUNT][HD_FONT_GLYPH_MAX];
static bool hd_font_brightness_load_failed[HD_FONT_TABLE_COUNT][HD_FONT_GLYPH_MAX];

// Lazily loads and caches one glyph's raw brightness-map pixels ("hdfont_<stem>_NN.dat").
// Fail-once, never-retry (mirrors load_hd_sheet_frame_brightness).
static const HDFontBrightness *load_hd_font_brightness(unsigned int table, unsigned int index)
{
	if (table >= HD_FONT_TABLE_COUNT || index >= HD_FONT_GLYPH_MAX)
		return NULL;

	HDFontBrightness *slot = &hd_font_brightness[table][index];
	if (slot->pixels != NULL)
		return slot;
	if (hd_font_brightness_load_failed[table][index])
		return NULL;

	char name[32];
	snprintf(name, sizeof name, "hdfont_%s_%02u.dat", hd_font_stems[table], index);

	bool ok = false;
	Uint8 *pixels = NULL;
	int gw = 0, gh = 0;

	FILE *f = dir_fopen(data_dir(), name, "rb");
	if (f != NULL)
	{
		Uint8 magic[4];
		Uint8 dims[8];

		if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "HDPX", 4) == 0 &&
		    fread(dims, 1, 8, f) == 8)
		{
			Uint32 width, height;
			memcpy(&width, &dims[0], 4);
			memcpy(&height, &dims[4], 4);
			width = SDL_SwapLE32(width);
			height = SDL_SwapLE32(height);

			if (width > 0 && height > 0 && width <= 1024 && height <= 1024)
			{
				size_t pixel_bytes = (size_t)width * height * 4;
				pixels = malloc(pixel_bytes);
				if (pixels != NULL && fread(pixels, 1, pixel_bytes, f) == pixel_bytes)
				{
					gw = (int)width;
					gh = (int)height;
					ok = true;
				}
			}
		}

		fclose(f);
	}

	if (!ok)
	{
		free(pixels);
		fprintf(stderr, "warning: HD font glyph %s unavailable; falling back\n", name);
		hd_font_brightness_load_failed[table][index] = true;
		return NULL;
	}

	slot->pixels = pixels;
	slot->w = gw;
	slot->h = gh;
	return slot;
}

// Synthesized glyph textures, keyed (table, index, hue, value, mode). LRU-capped.
// As with the filtered flight cache, the cache MUST be >= the font queue: a glyph
// texture is handed to the queue during draw-emit and not drawn until the next
// scale_and_flip(), so evicting one synthesized earlier in the SAME frame would
// dangle a queue pointer. Sizing cache == queue guarantees no intra-frame eviction
// (distinct glyphs emitted this frame <= queue capacity).
#define HD_FONT_QUEUE_COUNT 4096
#define HD_FONT_CACHE_COUNT HD_FONT_QUEUE_COUNT

typedef struct
{
	bool used;
	unsigned int table, index;
	Uint8 hue;
	Sint8 value;
	int mode;
	// Shadow spec baked into the SAME texture (see synth_hd_font_combined). A drop
	// shadow / 4-way outline is the same glyph offset by (sdx,sdy) logical px; when
	// shadow_mode < 0 the entry is a plain (unshadowed) glyph. ox/oy are the logical
	// offset from the glyph's draw position to the combined texture's top-left (0,0
	// for a drop shadow; negative for a full outline that extends up/left).
	int shadow_mode;
	int sdx, sdy;
	bool full;
	// 4-corner recolored outline baked into the same texture (see
	// synth_hd_font_outlined); sdx == sdy == the outline distance when set.
	bool outline;
	Sint8 outline_value;
	int ox, oy;
	SDL_Texture *tex;
	int w, h;
	Uint32 last_touch;
} HDFontCacheEntry;
static HDFontCacheEntry hd_font_cache[HD_FONT_CACHE_COUNT];
static Uint32 hd_font_cache_clock = 0;

// Reproduce blit_sprite_hv's clamp/wrap of (nibble + value) into a 0..15 shade:
// underflow (as Uint8, >= 0x1f) wraps to 0, mild overflow (0x10..0x1e) pins to 15.
// (blit_sprite_hv_unsafe has no clamp, but for every real font draw the nibble+value
// range never overflows/underflows, so this is byte-identical to the unsafe path too.)
static Uint8 hd_font_hv_shade(int nibble, Sint8 value)
{
	Uint8 temp = (Uint8)((nibble & 0x0f) + value);
	if (temp > 0xf)
		temp = (temp >= 0x1f) ? 0x0 : 0xf;
	return temp;
}

static SDL_Texture *synth_hd_font_glyph(unsigned int table, unsigned int index, Uint8 hue, Sint8 value, int mode, int *out_w, int *out_h)
{
	const HDFontBrightness *b = load_hd_font_brightness(table, index);
	if (b == NULL)
		return NULL;

	// Precompute the 16 recolored shades for the recolor (HV / HV_BLEND) modes.
	SDL_Color shade[16];
	if (mode == HD_FONT_MODE_HV || mode == HD_FONT_MODE_HV_BLEND)
	{
		Uint8 hue_hi = (Uint8)(hue << 4);
		for (int k = 0; k < 16; ++k)
			shade[k] = colors[(Uint8)(hue_hi | hd_font_hv_shade(k, value))];
	}

	size_t count = (size_t)b->w * b->h;
	Uint8 *rgba = malloc(count * 4);
	if (rgba == NULL)
		return NULL;

	for (size_t p = 0; p < count; ++p)
	{
		Uint8 cov = b->pixels[p * 4 + 3];  // coverage / anti-aliased alpha
		if (cov == 0)
		{
			memset(&rgba[p * 4], 0, 4);
			continue;
		}

		Uint8 rr, gg, bb, aa;
		if (mode == HD_FONT_MODE_DARK)
		{
			rr = gg = bb = 0;
			aa = (Uint8)(cov / 2);   // ~50% black shadow (blit_sprite_dark darken)
		}
		else if (mode == HD_FONT_MODE_BLACK)
		{
			rr = gg = bb = 0;
			aa = cov;                // solid black glyph (blit_sprite_dark black)
		}
		else
		{
			// Smooth brightness from R; interpolate between the two nearest shades.
			// At an integer-brightness pixel (R = brightness*17) pos is exact, t == 0,
			// and the result equals colors[(hue<<4) | clamp(brightness+value)] -- i.e.
			// parity-exact with the 8-bit blit.
			Uint8 rchan = b->pixels[p * 4 + 0];  // R = G = B = brightness
			float pos = (rchan / 255.0f) * 15.0f;
			int lo = (int)pos;
			if (lo > 14)
				lo = 14;
			int hi = lo + 1;
			float t = pos - (float)lo;
			rr = (Uint8)lrintf(shade[lo].r + (shade[hi].r - shade[lo].r) * t);
			gg = (Uint8)lrintf(shade[lo].g + (shade[hi].g - shade[lo].g) * t);
			bb = (Uint8)lrintf(shade[lo].b + (shade[hi].b - shade[lo].b) * t);
			aa = (mode == HD_FONT_MODE_HV_BLEND) ? (Uint8)(cov / 2) : cov;
		}

		rgba[p * 4 + 0] = rr;
		rgba[p * 4 + 1] = gg;
		rgba[p * 4 + 2] = bb;
		rgba[p * 4 + 3] = aa;
	}

	SDL_Texture *tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, b->w, b->h);
	if (tex == NULL)
	{
		free(rgba);
		return NULL;
	}
	if (SDL_UpdateTexture(tex, NULL, rgba, b->w * 4) != 0)
	{
		SDL_DestroyTexture(tex);
		free(rgba);
		return NULL;
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	free(rgba);

	*out_w = b->w;
	*out_h = b->h;
	return tex;
}

// Synthesize ONE truecolor texture containing the main glyph composited OVER its
// own drop shadow / outline, with the shadow KNOCKED OUT under the glyph. This is
// the HD analogue of the classic two-pass "shadow blit, then opaque glyph blit":
// there the opaque 8-bit glyph fully overwrites the shadow beneath it, so the
// shadow only survives in its offset halo. In HD the glyph is anti-aliased (alpha =
// coverage), so drawing an AA glyph as a separate translucent quad OVER a separate
// black-shadow quad lets the shadow bleed through every partially-covered glyph
// edge -- darkening dense descenders ('g','p','q','y','j'). Baking both into one
// texture and reducing the shadow's alpha by (1 - glyph_coverage) restores the
// classic look: the glyph's AA edges blend against the BACKGROUND, not the shadow.
//
// The shadow is the SAME glyph index offset by (sdx,sdy) logical px (full => a
// 4-way {(0,-d),(+d,0),(0,+d),(-d,0)} outline). The canvas is grown to bound the
// glyph (placed so its origin maps to the caller's draw position) plus every shadow
// copy; *out_ox/*out_oy give the logical shift from the draw position to the
// canvas top-left (0,0 for a drop shadow, negative for an outline).
static SDL_Texture *synth_hd_font_combined(
	unsigned int table, unsigned int index, Uint8 hue, Sint8 value, int glyph_mode,
	int shadow_mode, int sdx, int sdy, bool full,
	int *out_w, int *out_h, int *out_ox, int *out_oy)
{
	const HDFontBrightness *b = load_hd_font_brightness(table, index);
	if (b == NULL)
		return NULL;

	const int bw = b->w, bh = b->h;

	// Precompute the 16 recolored shades for the glyph's HV / HV_BLEND recolor.
	SDL_Color shade[16];
	if (glyph_mode == HD_FONT_MODE_HV || glyph_mode == HD_FONT_MODE_HV_BLEND)
	{
		Uint8 hue_hi = (Uint8)(hue << 4);
		for (int k = 0; k < 16; ++k)
			shade[k] = colors[(Uint8)(hue_hi | hd_font_hv_shade(k, value))];
	}

	// Shadow copies, in HD px (assets are 4x, so 1 logical px == 4 HD px).
	int offx[4], offy[4], noff;
	const int dxh = sdx * 4, dyh = sdy * 4;
	if (full)
	{
		offx[0] = 0;    offy[0] = -dyh;
		offx[1] = dxh;  offy[1] = 0;
		offx[2] = 0;    offy[2] = dyh;
		offx[3] = -dxh; offy[3] = 0;
		noff = 4;
	}
	else
	{
		offx[0] = dxh; offy[0] = dyh;
		noff = 1;
	}

	// Canvas bounds = union of the glyph (at 0,0) and every shadow copy.
	int x0 = 0, x1 = bw, y0 = 0, y1 = bh;
	for (int i = 0; i < noff; ++i)
	{
		if (offx[i] < x0)      x0 = offx[i];
		if (offx[i] + bw > x1) x1 = offx[i] + bw;
		if (offy[i] < y0)      y0 = offy[i];
		if (offy[i] + bh > y1) y1 = offy[i] + bh;
	}
	const int W = x1 - x0, H = y1 - y0;
	const int gx = -x0, gy = -y0;  // glyph placement within the canvas

	Uint8 *rgba = calloc((size_t)W * H, 4);
	if (rgba == NULL)
		return NULL;

	for (int Y = 0; Y < H; ++Y)
	{
		for (int X = 0; X < W; ++X)
		{
			// --- glyph sample (top layer) ---
			float ag = 0.f, Gr = 0.f, Gg = 0.f, Gb = 0.f;
			int sxg = X - gx, syg = Y - gy;
			if (sxg >= 0 && sxg < bw && syg >= 0 && syg < bh)
			{
				size_t sp = (size_t)syg * bw + sxg;
				Uint8 cov = b->pixels[sp * 4 + 3];
				if (cov != 0)
				{
					if (glyph_mode == HD_FONT_MODE_DARK)
						ag = (cov / 2) / 255.f;              // black, ~50%
					else if (glyph_mode == HD_FONT_MODE_BLACK)
						ag = cov / 255.f;                    // black, solid
					else
					{
						Uint8 rchan = b->pixels[sp * 4 + 0];
						float pos = (rchan / 255.0f) * 15.0f;
						int lo = (int)pos;
						if (lo > 14)
							lo = 14;
						int hi = lo + 1;
						float t = pos - (float)lo;
						Gr = shade[lo].r + (shade[hi].r - shade[lo].r) * t;
						Gg = shade[lo].g + (shade[hi].g - shade[lo].g) * t;
						Gb = shade[lo].b + (shade[hi].b - shade[lo].b) * t;
						ag = (glyph_mode == HD_FONT_MODE_HV_BLEND) ? (cov / 2) / 255.f : cov / 255.f;
					}
				}
			}

			// --- shadow sample (bottom layer): black, union of the copies ---
			float as = 0.f;
			for (int i = 0; i < noff; ++i)
			{
				int sxs = X - (offx[i] - x0), sys = Y - (offy[i] - y0);
				if (sxs >= 0 && sxs < bw && sys >= 0 && sys < bh)
				{
					size_t sp = (size_t)sys * bw + sxs;
					Uint8 cov = b->pixels[sp * 4 + 3];
					if (cov != 0)
					{
						float a = (shadow_mode == HD_FONT_MODE_BLACK) ? cov / 255.f : (cov / 2) / 255.f;
						if (a > as)
							as = a;  // opaque black -> union is max coverage
					}
				}
			}

			// --- knockout + composite (glyph OVER shadow, premultiplied) ---
			// Knock the shadow out under the glyph (as_eff = as*(1-ag)) so the glyph's
			// anti-aliased edge blends toward the GLYPH color instead of being pulled
			// to black -- but apply that knockout to the COLOR MIX ONLY. Coverage must
			// stay the full Porter-Duff "over" alpha, A = ag + as_eff: deflating it by
			// a second (1-ag) made the glyph/outline seam translucent (up to 25% at a
			// half-covered edge pixel), so a light background showed through it as a
			// hairline gap in the outline.
			// Where the glyph is absent (ag==0) A==as and color==black -> the shadow
			// halo is byte-identical to the old separate shadow quad. Where the shadow
			// is absent (as==0) A==ag==A_mix and color==G -> byte-identical to a plain
			// glyph. Only in the overlap does the shadow's pull-to-black relax.
			float as_eff = as * (1.f - ag);
			float A = ag + as_eff;                    // true coverage
			float A_mix = ag + as_eff * (1.f - ag);   // knockout-weighted color mix
			size_t dp = ((size_t)Y * W + X) * 4;
			if (A <= 0.f)
				continue;  // calloc already zeroed
			// ag <= A_mix, so the unpremultiplied color can never exceed G.
			rgba[dp + 0] = (Uint8)lrintf(Gr * ag / A_mix);
			rgba[dp + 1] = (Uint8)lrintf(Gg * ag / A_mix);
			rgba[dp + 2] = (Uint8)lrintf(Gb * ag / A_mix);
			rgba[dp + 3] = (Uint8)lrintf(A * 255.f);
		}
	}

	SDL_Texture *tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, W, H);
	if (tex == NULL)
	{
		free(rgba);
		return NULL;
	}
	if (SDL_UpdateTexture(tex, NULL, rgba, W * 4) != 0)
	{
		SDL_DestroyTexture(tex);
		free(rgba);
		return NULL;
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	free(rgba);

	*out_w = W;
	*out_h = H;
	*out_ox = x0 / 4;
	*out_oy = y0 / 4;
	return tex;
}

// Bilinear sample of the outline union (nonzero == covered) at continuous classic
// coordinates, treating cells as unit squares centred on integer coordinates.
static float hd_outline_sample(const Uint8 *cov, int w, int h, float u, float v)
{
	const int x0 = (int)floorf(u), y0 = (int)floorf(v);
	const float fx = u - (float)x0, fy = v - (float)y0;

	float acc = 0.f;
	for (int dy = 0; dy <= 1; ++dy)
	{
		for (int dx = 0; dx <= 1; ++dx)
		{
			const int x = x0 + dx, y = y0 + dy;
			if (x < 0 || y < 0 || x >= w || y >= h || cov[(size_t)y * w + x] == 0)
				continue;
			acc += (dx ? fx : 1.f - fx) * (dy ? fy : 1.f - fy);
		}
	}
	return acc;
}

// Outline shade for an HD pixel: the union cell it lands in, or -- where the
// rounding has pulled coverage in from a neighbour -- the nearest covered cell.
static Uint8 hd_outline_shade(const Uint8 *cov, int w, int h, int cx, int cy)
{
	static const int nx[8] = { 1, -1,  0,  0,  1, -1,  1, -1 };
	static const int ny[8] = { 0,  0,  1, -1,  1, -1, -1,  1 };

	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	if (cx >= w) cx = w - 1;
	if (cy >= h) cy = h - 1;

	if (cov[(size_t)cy * w + cx] != 0)
		return cov[(size_t)cy * w + cx] & 0x0f;

	for (int i = 0; i < 8; ++i)
	{
		const int x = cx + nx[i], y = cy + ny[i];
		if (x >= 0 && y >= 0 && x < w && y < h && cov[(size_t)y * w + x] != 0)
			return cov[(size_t)y * w + x] & 0x0f;
	}
	return 0;
}

// Synthesize ONE truecolor texture containing the glyph over a recolored 4-corner
// outline -- the classic {(-d,-d),(+d,+d),(+d,-d),(-d,+d)} blit_sprite_hv passes of
// the title menu (src/tyrian2.c).
//
// The outline's SHAPE is unioned at CLASSIC resolution, from the sprite's exact
// mask, and only then upscaled. That ordering is the whole fix: the classic passes
// are opaque hard-edged copies, so their union leaves the 1px background channels
// between and inside letters fully open. Unioning the anti-aliased HD asset instead
// (which is what drawing the four copies as separate HD quads does) both fattens
// each copy by ~half a logical pixel and, because the traced silhouette differs
// from the hard classic mask by ~7% of glyph area at the 50% threshold, closes
// those channels except for a ragged sliver -- read on screen as a hairline gap in
// the outline. Later offsets overwrite earlier ones, matching the classic pass order.
//
// The upscale is bilinear-with-a-sharpened-ramp rather than nearest, which restores
// the rounded corners and soft edges of the HD look: at a straight edge the 50%
// crossing sits exactly on the classic pixel boundary (so the outline neither
// fattens nor thins), while convex corners round off and the ramp anti-aliases.
//
// The glyph itself stays the smooth HD asset, composited on top with the shadow
// knockout of synth_hd_font_combined (outline knocked out under the glyph for the
// color mix, full coverage kept for alpha).
#define HD_OUTLINE_SHARPNESS 3.0f

static SDL_Texture *synth_hd_font_outlined(
	unsigned int table, unsigned int index, Uint8 hue, Sint8 value, Sint8 outline_value, int dist,
	int *out_w, int *out_h, int *out_ox, int *out_oy)
{
	const HDFontBrightness *b = load_hd_font_brightness(table, index);
	if (b == NULL || !sprite_exists(table, index))
		return NULL;

	const int cw = sprite(table, index)->width, ch = sprite(table, index)->height;
	const int bw = b->w, bh = b->h;

	// The outline rasterizer maps HD px -> classic px by /4, so the asset must be
	// the exact 4x upscale of the sprite. Anything else -> classic fallback.
	if (dist < 1 || cw <= 0 || ch <= 0 || bw != cw * 4 || bh != ch * 4)
		return NULL;

	Uint8 *mask = malloc((size_t)cw * ch);
	if (mask == NULL)
		return NULL;
	if (!sprite_shade_mask(table, index, mask))
	{
		free(mask);
		return NULL;
	}

	SDL_Color shade[16], outline_shade[16];
	const Uint8 hue_hi = (Uint8)(hue << 4);
	for (int k = 0; k < 16; ++k)
	{
		shade[k] = colors[(Uint8)(hue_hi | hd_font_hv_shade(k, value))];
		outline_shade[k] = colors[(Uint8)(hue_hi | hd_font_hv_shade(k, outline_value))];
	}

	// Union the four copies at CLASSIC resolution, in classic pass order (later
	// copies overwrite earlier ones where they overlap).
	const int ow = cw + 2 * dist, oh = ch + 2 * dist;

	Uint8 *outline = calloc((size_t)ow * oh, 1);
	if (outline == NULL)
	{
		free(mask);
		return NULL;
	}

	static const int offx[4] = { -1,  1,  1, -1 };
	static const int offy[4] = { -1,  1, -1,  1 };
	for (int i = 0; i < 4; ++i)
	{
		for (int y = 0; y < ch; ++y)
		{
			for (int x = 0; x < cw; ++x)
			{
				const Uint8 m = mask[(size_t)y * cw + x];
				if (m == 0)
					continue;
				const int ox = x + offx[i] * dist + dist, oy = y + offy[i] * dist + dist;
				outline[(size_t)oy * ow + ox] = m;
			}
		}
	}

	free(mask);

	const int d = dist * 4;
	const int W = bw + 2 * d, H = bh + 2 * d;
	const int gx = d, gy = d;  // glyph placement within the canvas

	Uint8 *rgba = calloc((size_t)W * H, 4);
	if (rgba == NULL)
	{
		free(outline);
		return NULL;
	}

	for (int Y = 0; Y < H; ++Y)
	{
		for (int X = 0; X < W; ++X)
		{
			// --- glyph sample (top layer), anti-aliased HD asset ---
			float ag = 0.f, Gr = 0.f, Gg = 0.f, Gb = 0.f;
			const int sxg = X - gx, syg = Y - gy;
			if (sxg >= 0 && sxg < bw && syg >= 0 && syg < bh)
			{
				const size_t sp = (size_t)syg * bw + sxg;
				const Uint8 cov = b->pixels[sp * 4 + 3];
				if (cov != 0)
				{
					const Uint8 rchan = b->pixels[sp * 4 + 0];
					float pos = (rchan / 255.0f) * 15.0f;
					int lo = (int)pos;
					if (lo > 14)
						lo = 14;
					const int hi = lo + 1;
					const float t = pos - (float)lo;
					Gr = shade[lo].r + (shade[hi].r - shade[lo].r) * t;
					Gg = shade[lo].g + (shade[hi].g - shade[lo].g) * t;
					Gb = shade[lo].b + (shade[hi].b - shade[lo].b) * t;
					ag = cov / 255.f;
				}
			}

			// --- outline sample (bottom layer): the classic-resolution union,
			// upscaled with a sharpened bilinear ramp (rounded corners, AA edges) ---
			const float u = (X + 0.5f) / 4.f - 0.5f, v = (Y + 0.5f) / 4.f - 0.5f;
			float as = (hd_outline_sample(outline, ow, oh, u, v) - 0.5f) * HD_OUTLINE_SHARPNESS + 0.5f;
			if (as < 0.f)
				as = 0.f;
			else if (as > 1.f)
				as = 1.f;

			float Or = 0.f, Og = 0.f, Ob = 0.f;
			if (as > 0.f)
			{
				const SDL_Color c = outline_shade[hd_outline_shade(outline, ow, oh, X / 4, Y / 4)];
				Or = c.r;
				Og = c.g;
				Ob = c.b;
			}

			// Knockout + composite, as in synth_hd_font_combined: the outline is
			// knocked out under the glyph for the COLOR mix only, so the glyph's
			// anti-aliased edge blends toward the glyph color rather than being
			// pulled into the outline color, while alpha keeps full coverage.
			const float as_eff = as * (1.f - ag);
			const float A = ag + as_eff;
			const float A_mix = ag + as_eff * (1.f - ag);
			if (A <= 0.f)
				continue;  // calloc already zeroed
			const size_t dp = ((size_t)Y * W + X) * 4;
			rgba[dp + 0] = (Uint8)lrintf((Gr * ag + Or * as_eff) / A_mix);
			rgba[dp + 1] = (Uint8)lrintf((Gg * ag + Og * as_eff) / A_mix);
			rgba[dp + 2] = (Uint8)lrintf((Gb * ag + Ob * as_eff) / A_mix);
			rgba[dp + 3] = (Uint8)lrintf(A * 255.f);
		}
	}

	free(outline);

	SDL_Texture *tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, W, H);
	if (tex == NULL)
	{
		free(rgba);
		return NULL;
	}
	if (SDL_UpdateTexture(tex, NULL, rgba, W * 4) != 0)
	{
		SDL_DestroyTexture(tex);
		free(rgba);
		return NULL;
	}
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	free(rgba);

	*out_w = W;
	*out_h = H;
	*out_ox = -dist;
	*out_oy = -dist;
	return tex;
}

static SDL_Texture *load_hd_font_glyph(unsigned int table, unsigned int index, Uint8 hue, Sint8 value, int mode,
	int shadow_mode, int sdx, int sdy, bool full, bool outline, Sint8 outline_value,
	int *out_w, int *out_h, int *out_ox, int *out_oy)
{
	// DARK / BLACK glyphs ignore hue+value; normalize the key so they dedupe.
	if (mode == HD_FONT_MODE_DARK || mode == HD_FONT_MODE_BLACK)
	{
		hue = 0;
		value = 0;
	}
	// Normalize the shadow spec for plain glyphs so they dedupe regardless of the
	// (unused) offset arguments. An outlined glyph keeps sdx/sdy as its distance.
	if (shadow_mode < 0 && !outline)
	{
		shadow_mode = -1;
		sdx = sdy = 0;
		full = false;
	}
	if (!outline)
		outline_value = 0;

	++hd_font_cache_clock;

	for (int i = 0; i < HD_FONT_CACHE_COUNT; ++i)
	{
		HDFontCacheEntry *e = &hd_font_cache[i];
		if (e->used && e->table == table && e->index == index &&
		    e->hue == hue && e->value == value && e->mode == mode &&
		    e->shadow_mode == shadow_mode && e->sdx == sdx && e->sdy == sdy && e->full == full &&
		    e->outline == outline && e->outline_value == outline_value)
		{
			e->last_touch = hd_font_cache_clock;
			*out_w = e->w;
			*out_h = e->h;
			*out_ox = e->ox;
			*out_oy = e->oy;
			return e->tex;
		}
	}

	int gw = 0, gh = 0, ox = 0, oy = 0;
	SDL_Texture *tex = outline
		? synth_hd_font_outlined(table, index, hue, value, outline_value, sdx, &gw, &gh, &ox, &oy)
		: (shadow_mode < 0)
			? synth_hd_font_glyph(table, index, hue, value, mode, &gw, &gh)
			: synth_hd_font_combined(table, index, hue, value, mode, shadow_mode, sdx, sdy, full, &gw, &gh, &ox, &oy);
	if (tex == NULL)
		return NULL;

	int slot = -1;
	for (int i = 0; i < HD_FONT_CACHE_COUNT; ++i)
	{
		if (!hd_font_cache[i].used)
		{
			slot = i;
			break;
		}
	}
	if (slot < 0)
	{
		Uint32 oldest = 0xffffffffu;
		for (int i = 0; i < HD_FONT_CACHE_COUNT; ++i)
		{
			if (hd_font_cache[i].last_touch < oldest)
			{
				oldest = hd_font_cache[i].last_touch;
				slot = i;
			}
		}
		SDL_DestroyTexture(hd_font_cache[slot].tex);
	}

	hd_font_cache[slot].used = true;
	hd_font_cache[slot].table = table;
	hd_font_cache[slot].index = index;
	hd_font_cache[slot].hue = hue;
	hd_font_cache[slot].value = value;
	hd_font_cache[slot].mode = mode;
	hd_font_cache[slot].shadow_mode = shadow_mode;
	hd_font_cache[slot].sdx = sdx;
	hd_font_cache[slot].sdy = sdy;
	hd_font_cache[slot].full = full;
	hd_font_cache[slot].outline = outline;
	hd_font_cache[slot].outline_value = outline_value;
	hd_font_cache[slot].ox = ox;
	hd_font_cache[slot].oy = oy;
	hd_font_cache[slot].tex = tex;
	hd_font_cache[slot].w = gw;
	hd_font_cache[slot].h = gh;
	hd_font_cache[slot].last_touch = hd_font_cache_clock;

	*out_w = gw;
	*out_h = gh;
	*out_ox = ox;
	*out_oy = oy;
	return tex;
}

// Per-present queue of HD glyphs, in absolute logical VGA-space rects. Rebuilt
// every present by the font choke points (immediate mode); drained (as the topmost
// layer) by scale_and_flip() in all three present branches.
typedef struct { SDL_Texture *tex; int lx, ly, lw, lh; } HDFontQueueEntry;
static HDFontQueueEntry hd_font_queue[HD_FONT_QUEUE_COUNT];
static int hd_font_queue_count = 0;
static bool hd_font_queue_overflow_warned = false;

bool hd_font_force_classic = false;

// True iff hd_font_emit* will claim glyphs for this surface right now. Callers that
// draw shadowed text as separate classic passes (JE_textShade, drawFontHvShadow)
// use this to keep the exact classic multi-pass ordering when HD is inactive, and
// switch to the single-quad combined path only when it is active.
bool hd_font_active(SDL_Surface *screen)
{
	if (!hd_mode || hd_font_force_classic)
		return false;
	// Only claim glyphs drawn directly to the surface that scale_and_flip presents
	// verbatim in full-screen VGA coordinates (VGAScreenSeg). Text drawn to a
	// background buffer (VGAScreen2) or the playfield buffer (game_screen) reaches
	// the screen via a memcpy / a shifted+clipped composite that a full-screen HD
	// quad can't reproduce, so those keep the classic path.
	if (screen != VGAScreenSeg)
		return false;

	// In the real-time flight loop the immediate-mode HD queue is rebuilt and
	// drained every present, so it can only hold text that is re-emitted every
	// frame. Claim in-flight text ONLY when a caller explicitly marks it as
	// per-frame HUD (hd_font_flight_hud) -- e.g. JE_inGameDisplays. In-flight text
	// that is drawn once and held (item-pickup messages, the level-start HUD) or
	// drawn by a nested pause/menu present loop stays classic so it never vanishes.
	if (hd_flight_active && !hd_font_flight_hud)
		return false;

	return true;
}

// Queues one HD glyph texture (already synthesized). Returns false if the glyph is
// out of range, the asset is missing, or the queue is full.
static bool hd_font_queue_glyph(unsigned int table, unsigned int index, int lx, int ly, HDFontMode mode,
	Uint8 hue, Sint8 value, int shadow_mode, int sdx, int sdy, bool full, bool outline, Sint8 outline_value)
{
	if (table >= HD_FONT_TABLE_COUNT || index >= HD_FONT_GLYPH_MAX)
		return false;

	int gw = 0, gh = 0, ox = 0, oy = 0;
	SDL_Texture *tex = load_hd_font_glyph(table, index, hue, value, (int)mode, shadow_mode, sdx, sdy, full,
		outline, outline_value, &gw, &gh, &ox, &oy);
	if (tex == NULL)
		return false;

	if (hd_font_queue_count >= HD_FONT_QUEUE_COUNT)
	{
		if (!hd_font_queue_overflow_warned)
		{
			fprintf(stderr, "warning: HD font queue full; falling back to classic glyphs\n");
			hd_font_queue_overflow_warned = true;
		}
		return false;
	}

	HDFontQueueEntry *e = &hd_font_queue[hd_font_queue_count++];
	e->tex = tex;
	e->lx = lx + ox;  // combined textures may extend up/left of the draw position
	e->ly = ly + oy;
	e->lw = gw / 4;  // HD assets are 4x; logical footprint == classic glyph size
	e->lh = gh / 4;
	return true;
}

// Every glyph's HD asset carries true anti-aliased coverage -- the vector trace
// derives alpha from a supersampled fill of the presence mask, so the flat
// straight-stroke shapes (e.g. '4') get the same soft convex corners as their
// gradient neighbours without the per-glyph corner-softening the older xBRZ
// extractor needed. The tiny font can therefore use the HD path uniformly (no
// per-glyph brightness disparity).
bool hd_font_emit(SDL_Surface *screen, unsigned int table, unsigned int index, int lx, int ly, HDFontMode mode, Uint8 hue, Sint8 value)
{
	if (!hd_font_active(screen))
		return false;

	return hd_font_queue_glyph(table, index, lx, ly, mode, hue, value, -1, 0, 0, false, false, 0);
}

// Emit a glyph composited with its own drop shadow / outline as a SINGLE quad (see
// synth_hd_font_combined). shadow_mode is HD_FONT_MODE_DARK or HD_FONT_MODE_BLACK;
// lx,ly is the MAIN glyph's draw position (the combined texture places itself
// relative to that). Returns false -> caller must draw the classic shadow+glyph.
bool hd_font_emit_shadowed(SDL_Surface *screen, unsigned int table, unsigned int index, int lx, int ly,
	HDFontMode glyph_mode, Uint8 hue, Sint8 value, HDFontMode shadow_mode, int sdx, int sdy, bool full)
{
	if (!hd_font_active(screen))
		return false;

	return hd_font_queue_glyph(table, index, lx, ly, glyph_mode, hue, value, (int)shadow_mode, sdx, sdy, full, false, 0);
}

// Emit a glyph composited with a recolored 4-corner outline as a SINGLE quad (see
// synth_hd_font_outlined). lx,ly is the MAIN glyph's draw position. Returns false
// -> caller must draw the classic outline + glyph passes.
bool hd_font_emit_outlined(SDL_Surface *screen, unsigned int table, unsigned int index, int lx, int ly,
	Uint8 hue, Sint8 value, Sint8 outline_value, int dist)
{
	if (!hd_font_active(screen))
		return false;

	return hd_font_queue_glyph(table, index, lx, ly, HD_FONT_MODE_HV, hue, value, -1, dist, dist, false,
		true, outline_value);
}

// Present-time HD glyph hook for the procedural HD HUD panel (src/hd_hud.c). The
// panel is drawn from scale_and_flip() at present time, above the flight sprites
// and below draw_hd_font_queue(), so glyphs queued here render this same present.
// This deliberately bypasses hd_font_active() (which would decline in-flight
// unless hd_font_flight_hud is set): the HUD panel is its own present-time
// producer, re-emitted every present just like the flight-HUD text, so the
// immediate-mode queue holds it correctly. It stays a NARROW hook (opaque HV
// recolor, no shadow) and never weakens the classic-path guards.
bool hd_hud_queue_glyph(unsigned int table, unsigned int index, int lx, int ly, Uint8 hue, Sint8 value)
{
	if (!hd_mode)
		return false;

	return hd_font_queue_glyph(table, index, lx, ly, HD_FONT_MODE_HV, hue, value, -1, 0, 0, false, false, 0);
}

// Draws (and drains) the HD font queue as the topmost UI layer, mapping each
// glyph's absolute logical VGA rect into the window exactly as the other queues do.
static void draw_hd_font_queue(const SDL_Rect *dst_rect)
{
	for (int i = 0; i < hd_font_queue_count; ++i)
	{
		HDFontQueueEntry *e = &hd_font_queue[i];

		SDL_Rect window_rect;
		window_rect.x = dst_rect->x + e->lx * dst_rect->w / vga_width;
		window_rect.y = dst_rect->y + e->ly * dst_rect->h / vga_height;
		window_rect.w = e->lw * dst_rect->w / vga_width;
		window_rect.h = e->lh * dst_rect->h / vga_height;

		SDL_RenderCopy(main_window_renderer, e->tex, NULL, &window_rect);
	}
	hd_font_queue_count = 0;
}

// Draws (and drains) the HD mouse cursor above the HD text -- the true topmost
// layer, matching the classic z-order where the cursor is blitted last. Each of
// the four quadrants maps its own logical edges into the window (rather than
// scaling a fixed 12x14 extent) so adjacent quads share edges without rounding
// seams; the render clip keeps an edge-clamped cursor inside the game area
// (the classic blit clips at the VGA edges, the HD quad must not spill into
// the letterbox bars).
static void draw_hd_cursor(const SDL_Rect *dst_rect)
{
	if (!hd_cursor_valid)
		return;
	hd_cursor_valid = false;

	static const int off_x[4] = { 0, 12, 0, 12 };
	static const int off_y[4] = { 0, 0, 14, 14 };

	SDL_RenderSetClipRect(main_window_renderer, dst_rect);
	for (int i = 0; i < 4; ++i)
	{
		const int lx = hd_cursor_lx + off_x[i];
		const int ly = hd_cursor_ly + off_y[i];

		SDL_Rect window_rect;
		window_rect.x = dst_rect->x + lx * dst_rect->w / vga_width;
		window_rect.y = dst_rect->y + ly * dst_rect->h / vga_height;
		window_rect.w = dst_rect->x + (lx + 12) * dst_rect->w / vga_width - window_rect.x;
		window_rect.h = dst_rect->y + (ly + 14) * dst_rect->h / vga_height - window_rect.y;

		SDL_RenderCopy(main_window_renderer, hd_cursor_tex[i], NULL, &window_rect);
	}
	SDL_RenderSetClipRect(main_window_renderer, NULL);
}

static void deinit_crt_overlay(void)
{
	if (crt_scanline_tex != NULL)
	{
		SDL_DestroyTexture(crt_scanline_tex);
		crt_scanline_tex = NULL;
	}
	if (crt_vignette_tex != NULL)
	{
		SDL_DestroyTexture(crt_vignette_tex);
		crt_vignette_tex = NULL;
	}
	crt_built_w = crt_built_h = 0;
}

/**
 * Builds the scanline and vignette overlay textures for the given renderer output
 * size (in pixels). Returns false (leaving the overlay textures untouched/absent)
 * if either texture fails to build, so the caller can just skip the overlay.
 */
static bool build_crt_overlay(int out_w, int out_h)
{
	deinit_crt_overlay();

	if (out_w <= 0 || out_h <= 0)
		return false;

	// --- Scanlines: one dark row per ~2 logical (400-line-doubled) output rows ---
	int period = out_h / 400;
	if (period < 2)
		period = 2;

	// SDL_PIXELFORMAT_RGBA32 is endian-normalized: bytes in memory are always R,G,B,A.
	Uint8 *scanline_pixels = malloc((size_t)out_w * (size_t)out_h * 4);
	if (scanline_pixels == NULL)
		return false;

	for (int y = 0; y < out_h; ++y)
	{
		bool dark_row = (y % period) < (period / 2);
		Uint8 alpha = dark_row ? 60 : 0;
		Uint8 *row = scanline_pixels + (size_t)y * out_w * 4;
		for (int x = 0; x < out_w; ++x)
		{
			row[x * 4 + 0] = 0;
			row[x * 4 + 1] = 0;
			row[x * 4 + 2] = 0;
			row[x * 4 + 3] = alpha;
		}
	}

	crt_scanline_tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, out_w, out_h);
	if (crt_scanline_tex != NULL)
	{
		if (SDL_UpdateTexture(crt_scanline_tex, NULL, scanline_pixels, out_w * 4) != 0)
		{
			SDL_DestroyTexture(crt_scanline_tex);
			crt_scanline_tex = NULL;
		}
		else
		{
			SDL_SetTextureBlendMode(crt_scanline_tex, SDL_BLENDMODE_BLEND);
		}
	}
	free(scanline_pixels);

	if (crt_scanline_tex == NULL)
	{
		deinit_crt_overlay();
		return false;
	}

	// --- Vignette: small radial-darkening texture, stretched over the output ---
	const int vig_size = 256;
	Uint8 *vignette_pixels = malloc((size_t)vig_size * (size_t)vig_size * 4);
	if (vignette_pixels == NULL)
	{
		deinit_crt_overlay();
		return false;
	}

	const float center = (vig_size - 1) / 2.0f;
	const float max_dist = sqrtf(center * center + center * center);
	const Uint8 max_alpha = 90;

	for (int y = 0; y < vig_size; ++y)
	{
		Uint8 *row = vignette_pixels + (size_t)y * vig_size * 4;
		for (int x = 0; x < vig_size; ++x)
		{
			float dx = x - center;
			float dy = y - center;
			float dist = sqrtf(dx * dx + dy * dy) / max_dist; // 0 at center, 1 at corners
			if (dist > 1.0f)
				dist = 1.0f;
			Uint8 alpha = (Uint8)(dist * dist * max_alpha);
			row[x * 4 + 0] = 0;
			row[x * 4 + 1] = 0;
			row[x * 4 + 2] = 0;
			row[x * 4 + 3] = alpha;
		}
	}

	crt_vignette_tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
		SDL_TEXTUREACCESS_STATIC, vig_size, vig_size);
	if (crt_vignette_tex != NULL)
	{
		if (SDL_UpdateTexture(crt_vignette_tex, NULL, vignette_pixels, vig_size * 4) != 0)
		{
			SDL_DestroyTexture(crt_vignette_tex);
			crt_vignette_tex = NULL;
		}
		else
		{
			SDL_SetTextureBlendMode(crt_vignette_tex, SDL_BLENDMODE_BLEND);
		}
	}
	free(vignette_pixels);

	if (crt_vignette_tex == NULL)
	{
		deinit_crt_overlay();
		return false;
	}

	crt_built_w = out_w;
	crt_built_h = out_h;
	return true;
}

/**
 * If crt_mode is enabled, draws the cached scanline + vignette overlays over
 * dst_rect. Rebuilds the cached textures if the renderer output size changed
 * since they were last built. Does nothing (and never fails loudly) if crt_mode
 * is off or the overlay textures can't be built.
 */
static void apply_crt_overlay(const SDL_Rect *dst_rect)
{
	if (!crt_mode)
		return;

	int out_w, out_h;
	SDL_GetRendererOutputSize(main_window_renderer, &out_w, &out_h);

	if (out_w != crt_built_w || out_h != crt_built_h)
	{
		if (!build_crt_overlay(out_w, out_h))
			return;
	}

	if (crt_scanline_tex == NULL || crt_vignette_tex == NULL)
		return;

	SDL_RenderCopy(main_window_renderer, crt_scanline_tex, NULL, dst_rect);
	SDL_RenderCopy(main_window_renderer, crt_vignette_tex, NULL, dst_rect);
}

/**
 * The classic present base: software-scale the 8-bit src_surface into
 * main_window_texture and blit it to the window at the computed destination rect
 * (returned in *dst_rect). This is exactly the head of the classic scale-and-flip
 * tail, factored out so the HD flight branch composites on an identical base --
 * making empty-queue HD output byte-identical to classic by construction. Does NOT
 * apply the CRT overlay or present; the caller owns those (ordering differs).
 */
static void classic_scale_base(SDL_Surface *src_surface, SDL_Rect *dst_rect)
{
	assert(scaler_function != NULL);
	scaler_function(src_surface, main_window_texture);

	calc_dst_render_rect(src_surface, dst_rect);

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderCopy(main_window_renderer, main_window_texture, NULL, dst_rect);
}

// Debug-only headless screenshot hook. When OT_SHOTDIR is set, reads the
// composited backbuffer back (HD font/sprite overlays included) just before each
// present and saves BMPs. OT_SHOTEVERY=N captures every Nth present (default 60);
// OT_SHOTQUIT=M exit(0) after present M. No-op unless OT_SHOTDIR is set, so it is
// inert in normal play.
static void maybe_dump_frame(void)
{
	static bool inited = false;
	static const char *dir = NULL;
	static int every = 60, quitat = 0;
	static long present_no = 0;

	if (!inited)
	{
		inited = true;
		dir = getenv("OT_SHOTDIR");
		const char *e = getenv("OT_SHOTEVERY");
		if (e != NULL && atoi(e) > 0)
			every = atoi(e);
		const char *q = getenv("OT_SHOTQUIT");
		if (q != NULL)
			quitat = atoi(q);
	}

	// Debug-only scripted input: OT_KEYS="F:name,F:name,..." pushes a keydown+keyup
	// for `name` at present frame F, to drive menus headlessly (e.g. reach the
	// save/load screen). Names: up/down/left/right/return/space/escape/y/n.
	const char *script = getenv("OT_KEYS");
	if (script != NULL)
	{
		const char *p = script;
		while (*p != '\0')
		{
			long f = strtol(p, (char **)&p, 10);
			if (*p == ':')
				++p;
			char name[16];
			int n = 0;
			while (*p != '\0' && *p != ',' && n < 15)
				name[n++] = *p++;
			name[n] = '\0';
			if (*p == ',')
				++p;
			if (f == present_no + 1)  // fire on the upcoming frame
			{
				SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
				if (!strcmp(name, "up")) sc = SDL_SCANCODE_UP;
				else if (!strcmp(name, "down")) sc = SDL_SCANCODE_DOWN;
				else if (!strcmp(name, "left")) sc = SDL_SCANCODE_LEFT;
				else if (!strcmp(name, "right")) sc = SDL_SCANCODE_RIGHT;
				else if (!strcmp(name, "return")) sc = SDL_SCANCODE_RETURN;
				else if (!strcmp(name, "space")) sc = SDL_SCANCODE_SPACE;
				else if (!strcmp(name, "escape")) sc = SDL_SCANCODE_ESCAPE;
				else if (!strcmp(name, "y")) sc = SDL_SCANCODE_Y;
				else if (!strcmp(name, "n")) sc = SDL_SCANCODE_N;
				if (sc != SDL_SCANCODE_UNKNOWN)
				{
					SDL_Event ev;
					memset(&ev, 0, sizeof ev);
					ev.type = SDL_KEYDOWN;
					ev.key.state = SDL_PRESSED;
					ev.key.keysym.scancode = sc;
					ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
					SDL_PushEvent(&ev);
					ev.type = SDL_KEYUP;
					ev.key.state = SDL_RELEASED;
					SDL_PushEvent(&ev);
				}
			}
		}
	}

	++present_no;
	if (dir == NULL)
		return;

	if (present_no % every == 0)
	{
		int w = 0, h = 0;
		SDL_GetRendererOutputSize(main_window_renderer, &w, &h);
		SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (shot != NULL)
		{
			if (SDL_RenderReadPixels(main_window_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
			        shot->pixels, shot->pitch) == 0)
			{
				char path[512];
				snprintf(path, sizeof path, "%s/frame_%06ld.bmp", dir, present_no);
				SDL_SaveBMP(shot, path);
			}
			SDL_FreeSurface(shot);
		}
	}

	if (quitat > 0 && present_no >= quitat)
		exit(0);
}

static void scale_and_flip(SDL_Surface *src_surface)
{
	assert(src_surface->format->BitsPerPixel == 8);

	// HD in-flight compositor (Track B): draw the scaled 8-bit playfield exactly as
	// the classic tail would, then overlay the HD flight-sprite queue on top. With an
	// empty queue this produces byte-identical pixels to classic (same classic_scale_base
	// call, same apply_crt_overlay, same present). Placed before the backdrop branch:
	// gameplay has a scrolling background, not a PIC backdrop, so hd_backdrop_active is
	// false in flight.
	if (hd_mode && hd_flight_active)
	{
		SDL_Rect dst_rect;
		classic_scale_base(src_surface, &dst_rect);

		// Clip the HD overlay to the 264x184 playfield (VGA cols [0,264), rows
		// [0,184)); JE_starCompositeShow copies exactly that region into the 8-bit
		// base, so without this clip the overlay spills onto the HUD sidebar/bar.
		SDL_Rect play_clip = {
			dst_rect.x,
			dst_rect.y,
			264 * dst_rect.w / vga_width,
			184 * dst_rect.h / vga_height,
		};
		SDL_RenderSetClipRect(main_window_renderer, &play_clip);

		SDL_Texture *cur_tex = NULL;
		SDL_BlendMode cur_bm = SDL_BLENDMODE_NONE;
		Uint8 cur_r = 0, cur_g = 0, cur_b = 0, cur_a = 0;
		bool state_valid = false;

		for (int i = 0; i < hd_flight_queue_count; ++i)
		{
			HDFlightQueueEntry *entry = &hd_flight_queue[i];

			SDL_Rect window_rect;
			window_rect.x = dst_rect.x + entry->lx * dst_rect.w / vga_width;
			window_rect.y = dst_rect.y + entry->ly * dst_rect.h / vga_height;
			window_rect.w = dst_rect.x + (entry->lx + entry->lw) * dst_rect.w / vga_width - window_rect.x;
			window_rect.h = dst_rect.y + (entry->ly + entry->lh) * dst_rect.h / vga_height - window_rect.y;

			// Cheap dedupe: only re-set mod state on the texture when it changed since
			// the previous entry (SDL texture mod is sticky per texture).
			bool tex_changed = (entry->tex != cur_tex) || !state_valid;
			if (tex_changed || entry->blendmode != cur_bm)
			{
				SDL_SetTextureBlendMode(entry->tex, entry->blendmode);
				cur_bm = entry->blendmode;
			}
			if (tex_changed || entry->r != cur_r || entry->g != cur_g || entry->b != cur_b)
			{
				SDL_SetTextureColorMod(entry->tex, entry->r, entry->g, entry->b);
				cur_r = entry->r; cur_g = entry->g; cur_b = entry->b;
			}
			if (tex_changed || entry->a != cur_a)
			{
				SDL_SetTextureAlphaMod(entry->tex, entry->a);
				cur_a = entry->a;
			}
			cur_tex = entry->tex;
			state_valid = true;

			SDL_RenderCopy(main_window_renderer, entry->tex, &entry->src, &window_rect);
		}

		SDL_RenderSetClipRect(main_window_renderer, NULL);

		// HD in-flight HUD overlay (REMASTER_HUD.md, phases H0-H2): sidebar +
		// bottom-bar panel art and vectorized bars/gauges, drawn above the
		// flight sprites (which are clipped to the playfield above) and
		// below the HD text/cursor. No-op (classic panel shows through) if
		// 2P or the panel asset is unavailable.
		hd_hud_draw(main_window_renderer, &dst_rect);

		// HD text on top of the flight sprites, HD cursor above the text.
		draw_hd_font_queue(&dst_rect);
		draw_hd_cursor(&dst_rect);

		apply_crt_overlay(&dst_rect);
		maybe_dump_frame();
		SDL_RenderPresent(main_window_renderer);
		hd_flight_clear();
		last_output_rect = dst_rect;
		return;
	}

	if (hd_mode && hd_backdrop_active && hd_backdrop_cur_tex != NULL)
	{
		SDL_Rect dst_rect;
		calc_dst_render_rect(src_surface, &dst_rect);

		SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
		SDL_RenderClear(main_window_renderer);

		Uint8 f = (Uint8)(hd_backdrop_fade < 0 ? 0 : hd_backdrop_fade > 255 ? 255 : hd_backdrop_fade);

		// HD backdrop (behind); drawn via hd_backdrop_cur_tex regardless of whether it was
		// activated by the numeric PIC path (hd_set_backdrop) or the filename-keyed asset
		// path (hd_set_backdrop_asset).
		SDL_Texture *hd_tex = hd_backdrop_cur_tex;
		SDL_SetTextureColorMod(hd_tex, f, f, f);
		SDL_RenderCopy(main_window_renderer, hd_tex, NULL, &dst_rect);

		// HD sprite overlays (e.g. title logo) go *between* the backdrop and the indexed
		// overlay: the original z-order draws the logo first, then menu text, then the
		// mouse cursor on top. The indexed overlay carries the menu text and the baked-in
		// mouse cursor (index 0 keyed transparent everywhere the logo was suppressed), so
		// drawing the HD sprite before it lets those composite on top -- otherwise the HD
		// logo paints over the menu items and hides the cursor. Shares the fade factor `f`.
		for (int i = 0; i < hd_sprite_queue_count; ++i)
		{
			HDSpriteQueueEntry *entry = &hd_sprite_queue[i];

			SDL_Rect window_rect;
			window_rect.x = dst_rect.x + entry->lx * dst_rect.w / vga_width;
			window_rect.y = dst_rect.y + entry->ly * dst_rect.h / vga_height;
			window_rect.w = entry->lw * dst_rect.w / vga_width;
			window_rect.h = entry->lh * dst_rect.h / vga_height;

			SDL_SetTextureColorMod(entry->tex, f, f, f);
			SDL_RenderCopy(main_window_renderer, entry->tex, NULL, &window_rect);
		}
		hd_sprite_queue_count = 0;

		// Indexed overlay (menu text / version / mouse) on top, with palette index 0 keyed
		// transparent. src_surface is the 8-bit VGAScreen. Colour it with the *full* target
		// palette (not the live, mid-fade one) so the overlay dims only via the colour-mod
		// `f` below — dimming it by both the fading palette and `f` would fade it
		// quadratically, lagging behind the linearly-fading backdrop.
		// Snapshot the surface's SDL palette and restore it after building the
		// overlay texture. The classic scaler reads the global rgb_palette[], never
		// this SDL palette, so leaving it mutated has no effect on rendering -- but
		// it silently breaks any later 8-bit->8-bit SDL_BlitSurface (which matches by
		// palette RGB): e.g. JE_scaleInPicture's ship-spec blit would remap green to
		// white once this desynced VGAScreen's palette from game_screen's. Save/restore
		// keeps every 8-bit surface's SDL palette at its creation default.
		SDL_Color saved_palette[256];
		memcpy(saved_palette, src_surface->format->palette->colors, sizeof(saved_palette));
		SDL_SetPaletteColors(src_surface->format->palette, colors, 0, 256);
		SDL_SetColorKey(src_surface, SDL_TRUE, 0);
		SDL_Texture *overlay = SDL_CreateTextureFromSurface(main_window_renderer, src_surface);
		SDL_SetColorKey(src_surface, SDL_FALSE, 0);
		SDL_SetPaletteColors(src_surface->format->palette, saved_palette, 0, 256);
		if (overlay != NULL)
		{
			SDL_SetTextureColorMod(overlay, f, f, f);
			SDL_RenderCopy(main_window_renderer, overlay, NULL, &dst_rect);
			SDL_DestroyTexture(overlay);
		}

		// HD text on top of the indexed menu overlay, HD cursor above the text.
		draw_hd_font_queue(&dst_rect);
		draw_hd_cursor(&dst_rect);

		apply_crt_overlay(&dst_rect);

		maybe_dump_frame();
		SDL_RenderPresent(main_window_renderer);
		last_output_rect = dst_rect;
		return;
	}

	// Not compositing HD this frame (HD mode off, no active backdrop, or its load
	// failed) -- drop any HD sprite overlays queued this frame so they never leak
	// into a later frame; they were never drawn, so hd_set_sprite's caller already
	// fell back to the classic blit.
	hd_sprite_queue_count = 0;

	// Do software scaling and blit the output texture to the window.
	SDL_Rect dst_rect;
	classic_scale_base(src_surface, &dst_rect);
	// HD text on top of the classic base (topmost UI layer). Drains the queue even
	// when empty (HD mode off / nothing emitted), so it can never leak into a later
	// frame; with an empty queue this draws nothing and stays byte-identical.
	draw_hd_font_queue(&dst_rect);
	draw_hd_cursor(&dst_rect);
	apply_crt_overlay(&dst_rect);
	maybe_dump_frame();
	SDL_RenderPresent(main_window_renderer);

	// Save output rect to be used by mouse functions
	last_output_rect = dst_rect;
}

/**
 * Fetches the window size (in POINTS) and renderer output size (in PIXELS) so that
 * mouse coordinates (which SDL delivers in points) can be reconciled with
 * last_output_rect (which is computed in the renderer's pixel space, see
 * calc_dst_render_rect). Falls back to a 1:1 mapping if either size is unavailable
 * (e.g. very early in startup).
 */
static void get_window_and_output_size(int *win_w, int *win_h, int *out_w, int *out_h)
{
	SDL_GetWindowSize(main_window, win_w, win_h);
	SDL_GetRendererOutputSize(main_window_renderer, out_w, out_h);

	if (*win_w <= 0 || *win_h <= 0 || *out_w <= 0 || *out_h <= 0)
	{
		*win_w = *win_h = *out_w = *out_h = 1;
	}
}

/** Maps a specified point in game screen coordinates to window coordinates. */
void mapScreenPointToWindow(Sint32 *const inout_x, Sint32 *const inout_y)
{
	int win_w, win_h, out_w, out_h;
	get_window_and_output_size(&win_w, &win_h, &out_w, &out_h);

	long px = (2L * *inout_x + 1) * last_output_rect.w / (2 * VGAScreen->w) + last_output_rect.x;
	long py = (2L * *inout_y + 1) * last_output_rect.h / (2 * VGAScreen->h) + last_output_rect.y;

	*inout_x = px * win_w / out_w;
	*inout_y = py * win_h / out_h;
}

/** Maps a specified point in window coordinates to game screen coordinates. */
void mapWindowPointToScreen(Sint32 *const inout_x, Sint32 *const inout_y)
{
	int win_w, win_h, out_w, out_h;
	get_window_and_output_size(&win_w, &win_h, &out_w, &out_h);

	long px = (long)*inout_x * out_w / win_w;
	long py = (long)*inout_y * out_h / win_h;

	*inout_x = (2 * (px - last_output_rect.x) + 1) * VGAScreen->w / (2 * last_output_rect.w);
	*inout_y = (2 * (py - last_output_rect.y) + 1) * VGAScreen->h / (2 * last_output_rect.h);
}

/** Scales a distance in window coordinates to game screen coordinates. */
void scaleWindowDistanceToScreen(Sint32 *const inout_x, Sint32 *const inout_y)
{
	int win_w, win_h, out_w, out_h;
	get_window_and_output_size(&win_w, &win_h, &out_w, &out_h);

	long px = (long)*inout_x * out_w / win_w;
	long py = (long)*inout_y * out_h / win_h;

	*inout_x = (2 * px + 1) * VGAScreen->w / (2 * last_output_rect.w);
	*inout_y = (2 * py + 1) * VGAScreen->h / (2 * last_output_rect.h);
}
