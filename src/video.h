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

extern bool hd_flight_active;    // set while the in-flight HD sprite compositor should overlay
extern bool hd_tiles;            // HD level tilesets (Phase S3); overlay upscaled truecolor tiles

bool hd_set_backdrop(int pic_num); // begin HD compositing for PIC pic_num if its HD asset loads; returns success
bool hd_set_backdrop_asset(const char *name); // begin HD compositing for a filename-keyed full-screen asset ("hdpcx_<name>.dat"); returns success
void hd_clear_backdrop(void);       // stop HD compositing (revert to classic); covers both hd_set_backdrop and hd_set_backdrop_asset

// HD ending-cutscene streaming compositor (tyrend.anm). Separate from the fail-once
// PIC/asset backdrop cache: one persistent STREAMING texture, created lazily on the
// first frame and SDL_UpdateTexture'd in place per frame, routed through the existing
// hd_backdrop draw path. hd_anim_end() must be called on every playback exit path.
void hd_anim_begin(void);                                    // arm the anim streaming compositor (texture created lazily)
bool hd_anim_show_frame(const char *basename, int frame_index); // stream frame "hd<basename>_%04d.dat"; true on success, false leaves classic frame showing
void hd_anim_end(void);                                      // deactivate + destroy the streaming texture (idempotent)

// Immediate-mode HD sprite overlays (e.g. the title logo). Call every frame, before
// JE_showVGA(), for each HD-backed sprite that should composite on top of the backdrop;
// the queue is drained by scale_and_flip() and must be re-populated each frame.
bool hd_set_sprite(const char *asset_name, int lx, int ly, int lw, int lh); // queue an HD sprite overlay at the given logical VGA rect; returns success
void hd_clear_sprites(void);       // drop any queued HD sprite overlays (e.g. when leaving a screen)

void hd_set_cursor(int sprite_index, int lx, int ly); // arm the HD mouse cursor overlay (a 2x2 shopSpriteSheet blit at logical x,y); drawn topmost, drained every present

// HD in-flight sprite compositor (Track B). load_hd_sheet_frame lazily loads and
// caches one HDPX frame of an HD sheet (keyed by sheet id + frame index), mirroring
// the "load once, remember failure, never retry" pattern of the menu HD sprite
// cache. The flight queue is rebuilt every presented frame (immediate mode) from
// the interp display list and drained by scale_and_flip().
SDL_Texture *load_hd_sheet_frame(int sheet_id, int index); // load+cache one HD sheet frame; NULL if unavailable
bool hd_flight_lookup(int sheet_id, int index);            // true iff a frame texture is available (loading it if needed)

// Recoloring parity for blit_sprite2_filter's hue-band remap (enemy tints; see
// internal/plan/REMASTER_FLIGHT_COMPOSITOR.md §3). Synthesizes (and caches, LRU-capped) an
// RGBA texture for one (sheet, frame index, filter byte) combination from the
// "hdcompb_<stem>_NN.dat" brightness-map asset + the live palette (`colors[]`),
// smoothly interpolating between the two nearest of the filter byte's 16 shades.
// Returns NULL (never suppressing the 8-bit fallback) if the brightness asset is
// missing, the cache is full, or synthesis fails for any reason.
SDL_Texture *load_hd_sheet_frame_filtered(int sheet_id, int index, Uint8 filter);
// HD level-tileset atlas (Phase S3). Loads (fail-once cached) the HDPX grid atlas
// "hdtile_<bankchar>_p<NN>.dat" for one (bank 0..4, palette 0..22) pair; returns
// NULL when the atlas is missing so callers fall back to the classic 8-bit blit.
// hd_tile_atlas_src fills `src` with the 96x112 cell rect for bank-z index `z`
// (0..599) in the 20-col grid. current_palette_index memcmp's the live colors[]
// against palettes[0..22], returning the matching base-palette index or -1 (a
// faded/filtered/custom palette -> HD tiles off this frame).
SDL_Texture *load_hd_tile_atlas(int bank, int palette);
void hd_tile_atlas_src(int z, SDL_Rect *src);
int  current_palette_index(void);

void hd_flight_begin(void);                                // reset the flight queue (call once per present, before recording)
void hd_flight_set(SDL_Texture *tex, SDL_Rect src, int lx, int ly, int lw, int lh, SDL_BlendMode blendmode, Uint8 r, Uint8 g, Uint8 b, Uint8 a); // push one entry (bounds-checked)
void hd_flight_clear(void);                                // reset the flight queue (call after presenting)

// HD font glyph compositor (Track C). Font glyphs are runtime-recolored exactly
// like flight sprites: each classic draw passes a hue/colorbank band + a signed
// brightness "value" into blit_sprite_hv / _hv_unsafe / _hv_blend / _dark
// (src/sprite.c). hd_font_emit() synthesizes (and LRU-caches) one truecolor RGBA
// texture reproducing that per-pixel index math from the grayscale brightness-map
// asset (hdfont_<stem>_NN.dat) against the live palette, queues it at the glyph's
// absolute VGA position, and returns true so the caller SKIPS its classic blit.
// Returns false -- so the caller MUST fall back to the classic glyph -- whenever
// HD text can't be produced or safely composited (hd_mode off, in-flight, a
// non-presented surface, a missing/oversized asset, or the queue/cache full).
// This is the guarded-fallback safety net: any failure -> classic text renders.
typedef enum
{
	HD_FONT_MODE_HV = 0,    // opaque recolor (blit_sprite_hv / blit_sprite_hv_unsafe)
	HD_FONT_MODE_HV_BLEND,  // ~50% recolor over destination (blit_sprite_hv_blend)
	HD_FONT_MODE_DARK,      // ~50% black shadow (blit_sprite_dark, black == false)
	HD_FONT_MODE_BLACK,     // solid black glyph (blit_sprite_dark, black == true)
} HDFontMode;

bool hd_font_emit(SDL_Surface *screen, unsigned int table, unsigned int index, int lx, int ly, HDFontMode mode, Uint8 hue, Sint8 value);

// True iff hd_font_emit*() will claim glyphs for this surface right now (HD mode on,
// not forced classic, targeting the presented VGAScreenSeg outside the flight loop).
// Lets multi-pass shadowed-text callers preserve exact classic ordering when HD is
// off and switch to the single-quad combined path only when it is on.
bool hd_font_active(SDL_Surface *screen);

// Emit a glyph composited with its own drop shadow / 4-way outline as a SINGLE HD
// quad, with the shadow knocked out under the glyph so anti-aliased glyph edges
// blend against the background (not the black shadow) -- matching the classic
// opaque-glyph-over-shadow look. shadow_mode is HD_FONT_MODE_DARK or
// HD_FONT_MODE_BLACK; the shadow is the same glyph offset by (sdx,sdy) logical px
// (full => a 4-direction {(0,-d),(+d,0),(0,+d),(-d,0)} outline with d==sdx==sdy).
// lx,ly is the MAIN glyph's draw position. Returns false -> caller must draw the
// classic shadow + glyph passes itself.
bool hd_font_emit_shadowed(SDL_Surface *screen, unsigned int table, unsigned int index, int lx, int ly,
	HDFontMode glyph_mode, Uint8 hue, Sint8 value, HDFontMode shadow_mode, int sdx, int sdy, bool full);

// Set while text must paint persistent classic pixels even though it targets
// VGAScreenSeg outside the flight loop -- e.g. the level-start HUD text, which is
// drawn once before hd_flight_active goes true and must survive every present of
// the level. hd_font_emit() declines while set. Set, draw, restore.
extern bool hd_font_force_classic;

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
