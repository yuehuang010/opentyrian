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
#ifndef VIDEO_H
#define VIDEO_H

#include "opentyr.h"

#include "SDL.h"

#define vga_width 320
#define vga_height 200

typedef enum {
	SCALE_CENTER,
	SCALE_INTEGER,
	SCALE_ASPECT_8_5,
	SCALE_ASPECT_4_3,
	ScalingMode_MAX
} ScalingMode;

extern const char *const scaling_mode_names[ScalingMode_MAX];

extern int fullscreen_display; // -1 means windowed
extern ScalingMode scaling_mode;

extern int window_width, window_height; // last free (windowed) window size chosen by the user; 0 means unset

extern bool hd_mode;             // master HD-remaster toggle
extern bool hd_backdrop_active;  // set while an HD backdrop should composite
extern int  hd_backdrop_id;      // which PIC (1..13) is the active HD backdrop
extern int  hd_backdrop_fade;    // 0..255, drives the HD backdrop fade-in

extern bool crt_mode;            // optional CRT scanline + vignette post-process

bool hd_set_backdrop(int pic_num); // begin HD compositing for PIC pic_num if its HD asset loads; returns success
void hd_clear_backdrop(void);       // stop HD compositing (revert to classic)

// Immediate-mode HD sprite overlays (e.g. the title logo). Call every frame, before
// JE_showVGA(), for each HD-backed sprite that should composite on top of the backdrop;
// the queue is drained by scale_and_flip() and must be re-populated each frame.
bool hd_set_sprite(const char *asset_name, int lx, int ly, int lw, int lh); // queue an HD sprite overlay at the given logical VGA rect; returns success
void hd_clear_sprites(void);       // drop any queued HD sprite overlays (e.g. when leaving a screen)

extern SDL_Surface *VGAScreen, *VGAScreenSeg;
extern SDL_Surface *game_screen;
extern SDL_Surface *VGAScreen2;

extern SDL_Window *main_window;
extern SDL_PixelFormat *main_window_tex_format;

void init_video(void);

void video_on_win_resize(void);
void reinit_fullscreen(int new_display);
void toggle_fullscreen(void);
bool init_scaler(unsigned int new_scaler);
bool set_scaling_mode_by_name(const char *name);

void deinit_video(void);

void JE_clr256(SDL_Surface *);
void JE_showVGA(void);

void mapScreenPointToWindow(Sint32 *inout_x, Sint32 *inout_y);
void mapWindowPointToScreen(Sint32 *inout_x, Sint32 *inout_y);
void scaleWindowDistanceToScreen(Sint32 *inout_x, Sint32 *inout_y);

#endif /* VIDEO_H */
