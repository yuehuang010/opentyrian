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
#include "video.h"

#include "file.h"
#include "keyboard.h"
#include "opentyr.h"
#include "palette.h"
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

static SDL_Texture *crt_scanline_tex = NULL;
static SDL_Texture *crt_vignette_tex = NULL;
static int crt_built_w = 0;
static int crt_built_h = 0;

#define HD_BACKDROP_COUNT 13 // matches PCX_NUM (src/pcxmast.h); PIC numbers are 1-based
static SDL_Texture *hd_backdrop_tex[HD_BACKDROP_COUNT + 1];        // [0] unused
static bool hd_backdrop_load_failed[HD_BACKDROP_COUNT + 1];

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
static void scale_and_flip(SDL_Surface *);
static bool load_hd_backdrop(int pic_num);
static SDL_Texture *load_hd_sprite(const char *asset_name);
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

	bool ok = false;

	char name[16];
	snprintf(name, sizeof name, "hdpic%02d.dat", pic_num);

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
					SDL_Texture *tex = SDL_CreateTexture(main_window_renderer, SDL_PIXELFORMAT_RGBA32,
						SDL_TEXTUREACCESS_STATIC, (int)width, (int)height);

					if (tex != NULL)
					{
						if (SDL_UpdateTexture(tex, NULL, pixels, (int)(width * 4)) == 0)
						{
							SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
							hd_backdrop_tex[pic_num] = tex;
							ok = true;
						}
						else
						{
							SDL_DestroyTexture(tex);
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
		fprintf(stderr, "warning: HD backdrop hdpic%02d.dat unavailable; falling back\n", pic_num);
		hd_backdrop_load_failed[pic_num] = true;
	}

	return ok;
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
	hd_backdrop_active = true;
	hd_backdrop_fade = 0;
	return true;
}

void hd_clear_backdrop(void)
{
	hd_backdrop_active = false;
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

	bool ok = false;
	SDL_Texture *tex = NULL;

	FILE *f = dir_fopen(data_dir(), asset_name, "rb");
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

static void scale_and_flip(SDL_Surface *src_surface)
{
	assert(src_surface->format->BitsPerPixel == 8);

	if (hd_mode && hd_backdrop_active && load_hd_backdrop(hd_backdrop_id))
	{
		SDL_Rect dst_rect;
		calc_dst_render_rect(src_surface, &dst_rect);

		SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
		SDL_RenderClear(main_window_renderer);

		Uint8 f = (Uint8)(hd_backdrop_fade < 0 ? 0 : hd_backdrop_fade > 255 ? 255 : hd_backdrop_fade);

		// HD backdrop (behind)
		SDL_Texture *hd_tex = hd_backdrop_tex[hd_backdrop_id];
		SDL_SetTextureColorMod(hd_tex, f, f, f);
		SDL_RenderCopy(main_window_renderer, hd_tex, NULL, &dst_rect);

		// Indexed overlay (logo/version/menu) on top, with palette index 0 keyed transparent.
		// src_surface is the 8-bit VGAScreen. Colour it with the *full* target palette
		// (not the live, mid-fade one) so the overlay dims only via the colour-mod `f`
		// below — dimming it by both the fading palette and `f` would fade it quadratically,
		// lagging behind the linearly-fading backdrop.
		SDL_SetPaletteColors(src_surface->format->palette, colors, 0, 256);
		SDL_SetColorKey(src_surface, SDL_TRUE, 0);
		SDL_Texture *overlay = SDL_CreateTextureFromSurface(main_window_renderer, src_surface);
		SDL_SetColorKey(src_surface, SDL_FALSE, 0);
		if (overlay != NULL)
		{
			SDL_SetTextureColorMod(overlay, f, f, f);
			SDL_RenderCopy(main_window_renderer, overlay, NULL, &dst_rect);
			SDL_DestroyTexture(overlay);
		}

		// HD sprite overlays (e.g. title logo), on top of the indexed overlay, sharing
		// the same fade factor `f` so nothing fades out of lockstep with the backdrop.
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

		apply_crt_overlay(&dst_rect);

		SDL_RenderPresent(main_window_renderer);
		last_output_rect = dst_rect;
		return;
	}

	// Not compositing HD this frame (HD mode off, no active backdrop, or its load
	// failed) -- drop any HD sprite overlays queued this frame so they never leak
	// into a later frame; they were never drawn, so hd_set_sprite's caller already
	// fell back to the classic blit.
	hd_sprite_queue_count = 0;

	// Do software scaling
	assert(scaler_function != NULL);
	scaler_function(src_surface, main_window_texture);

	SDL_Rect dst_rect;
	calc_dst_render_rect(src_surface, &dst_rect);

	// Clear the window and blit the output texture to it
	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderCopy(main_window_renderer, main_window_texture, NULL, &dst_rect);
	apply_crt_overlay(&dst_rect);
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
