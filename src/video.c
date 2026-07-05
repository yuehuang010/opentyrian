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
static SDL_Rect last_output_rect = { 0, 0, vga_width, vga_height };

bool hd_mode = true;
bool hd_backdrop_active = false;
int hd_backdrop_id = 0;
int hd_backdrop_fade = 0;

#define HD_BACKDROP_COUNT 13 // matches PCX_NUM (src/pcxmast.h); PIC numbers are 1-based
static SDL_Texture *hd_backdrop_tex[HD_BACKDROP_COUNT + 1];        // [0] unused
static bool hd_backdrop_load_failed[HD_BACKDROP_COUNT + 1];

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

	SDL_ShowWindow(main_window);

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderPresent(main_window_renderer);
}

void deinit_video(void)
{
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
	SDL_SetWindowSize(main_window, scalers[scaler].width, scalers[scaler].height);

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

void hd_set_backdrop(int pic_num)
{
	hd_backdrop_id = pic_num;
	hd_backdrop_active = true;
	hd_backdrop_fade = 0;
}

void hd_clear_backdrop(void)
{
	hd_backdrop_active = false;
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
		// src_surface is the 8-bit VGAScreen. Give it the live palette, key color 0, make a texture.
		SDL_SetPaletteColors(src_surface->format->palette, get_live_palette(), 0, 256);
		SDL_SetColorKey(src_surface, SDL_TRUE, 0);
		SDL_Texture *overlay = SDL_CreateTextureFromSurface(main_window_renderer, src_surface);
		SDL_SetColorKey(src_surface, SDL_FALSE, 0);
		if (overlay != NULL)
		{
			SDL_SetTextureColorMod(overlay, f, f, f);
			SDL_RenderCopy(main_window_renderer, overlay, NULL, &dst_rect);
			SDL_DestroyTexture(overlay);
		}

		SDL_RenderPresent(main_window_renderer);
		last_output_rect = dst_rect;
		return;
	}

	// Do software scaling
	assert(scaler_function != NULL);
	scaler_function(src_surface, main_window_texture);

	SDL_Rect dst_rect;
	calc_dst_render_rect(src_surface, &dst_rect);

	// Clear the window and blit the output texture to it
	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderCopy(main_window_renderer, main_window_texture, NULL, &dst_rect);
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
