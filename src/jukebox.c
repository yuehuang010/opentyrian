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
/** @file jukebox.c
 * Standalone in-game music jukebox menu (track selection/playback UI).
 *
 * Entry points: jukebox().
 */

#include "jukebox.h"

#include "controller.h"
#include "font.h"
#include "keyboard.h"
#include "lds_play.h"
#include "loudness.h"
#include "mtrand.h"
#include "nortsong.h"
#include "opentyr.h"
#include "palette.h"
#include "sprite.h"
#include "starlib.h"
#include "vga256d.h"
#include "vga_palette.h"
#include "video.h"

#include <math.h>
#include <stdio.h>

// Goertzel-algorithm magnitude of `samples` (n of them, sampled at
// `sampleRate` Hz) at a single target frequency `freq`, unwindowed. Used by
// the jukebox spectrum analyzer to estimate per-band energy without a full
// FFT (only 24 bins are needed, so this is cheaper and simpler).
static float goertzel_mag(const Sint16 *samples, int n, double sampleRate, double freq)
{
	const double w = 2.0 * M_PI * freq / sampleRate;
	const double coeff = 2.0 * cos(w);

	double s0 = 0, s1 = 0, s2 = 0;
	for (int i = 0; i < n; ++i)
	{
		s0 = (double)samples[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}

	double real = s1 - s2 * cos(w);
	double imag = s2 * sin(w);
	return (float)sqrt(real * real + imag * imag);
}

void jukebox(void)  // FKA Setup.jukeboxGo
{
	bool trigger_quit = false,  // true when user wants to quit
	     quitting = false;
	
	bool hide_text = false;

	bool fade_looped_songs = true, fading_song = false;
	bool stopped = false;

	bool fx = false;
	int fx_num = 0;

	// bit 1 = oscilloscope, bit 2 = spectrum; 3 = both (default on), 0 = off.
	// Cycled by V: 3 -> 1 -> 2 -> 0 -> 3 -> ...
	int visualizer_mode = 3;
	// Smoothed instantaneous bar heights and peak-hold markers for the
	// spectrum analyzer; persist across frames (decay each frame).
	float barHeight[24] = { 0 };
	float barPeak[24] = { 0 };

	int palette_fade_steps = 15;

	int diff[256][3];
	init_step_fade_palette(diff, vga_palette, 0, 255);

	JE_starlib_init();

	int fade_volume = tyrMusicVolume;
	
	for (; ; )
	{
		if (!stopped && !audio_disabled)
		{
			if (songlooped && fade_looped_songs)
				fading_song = true;

			if (fading_song)
			{
				if (fade_volume > 5)
				{
					fade_volume -= 2;
				}
				else
				{
					fade_volume = tyrMusicVolume;

					fading_song = false;
				}

				set_volume(fade_volume, fxVolume);
			}

			if (!playing || (songlooped && fade_looped_songs && !fading_song))
				play_song(mt_rand() % MUSIC_NUM);
		}

		setFrameCount(1);

		SDL_FillRect(VGAScreenSeg, NULL, 0);

		KeyboardInput keyboardInput;

		bool gotKeyboardInput = starLibMain(&keyboardInput);

		if (visualizer_mode != 0)
		{
			Sint16 vis[2048];
			audio_visualizer_snapshot(vis, 2048);

			if (visualizer_mode & 1)
			{
				// Oscilloscope: newest 320 samples as a connected waveform
				// across the full screen width, centered at y=64.
				int prevY = 0;
				for (int sx = 0; sx < 320; ++sx)
				{
					Sint16 sample = vis[2048 - 320 + sx];
					int y = 64 - (sample * 40 / 32768);
					if (y < 2)
						y = 2;
					if (y > 197)
						y = 197;

					if (sx == 0)
					{
						JE_pix(VGAScreen, 0, y, 10);
					}
					else
					{
						int y0 = prevY, y1 = y;
						if (y0 > y1)
						{
							int t = y0; y0 = y1; y1 = t;
						}
						for (int py = y0; py <= y1; ++py)
							JE_pix(VGAScreen, sx, py, 10);
					}
					prevY = y;
				}
			}

			if (visualizer_mode & 2)
			{
				// Spectrum: 24 log-spaced bars, 60 Hz .. 10000 Hz, magnitude
				// via Goertzel over the newest 1024 samples.
				const Sint16 *spectrumSamples = vis + (2048 - 1024);

				for (int i = 0; i < 24; ++i)
				{
					double freq = 60.0 * pow(10000.0 / 60.0, i / 23.0);
					float mag = goertzel_mag(spectrumSamples, 1024, audioSampleRate, freq);

					double db = 20.0 * log10(mag / (512.0 * 32768.0) + 1e-9);
					float instant;
					if (db <= -60.0)
						instant = 0.0f;
					else if (db >= 0.0)
						instant = 48.0f;
					else
						instant = (float)((db + 60.0) / 60.0 * 48.0);

					barHeight[i] = fmaxf(instant, barHeight[i] - 4.0f);

					if (barHeight[i] > barPeak[i])
						barPeak[i] = barHeight[i];
					else
						barPeak[i] -= 0.4f;
					if (barPeak[i] < 0)
						barPeak[i] = 0;

					int x1 = 60 + i * 8;
					int x2 = x1 + 6;

					if (barHeight[i] >= 1.0f)
						fill_rectangle_xy(VGAScreen, x1, 150 - (int)barHeight[i], x2, 150, 13);

					int peakY = 150 - (int)barPeak[i];
					if (peakY < 102)
						peakY = 102;
					if (peakY > 150)
						peakY = 150;
					fill_rectangle_xy(VGAScreen, x1, peakY, x2, peakY, 15);
				}
			}
		}

		if (!hide_text)
		{
			char buffer[60];

			if (fx)
				snprintf(buffer, sizeof(buffer), "%d %s", fx_num + 1, soundTitle[fx_num]);
			else
				snprintf(buffer, sizeof(buffer), "%d %s [%s]", song_playing + 1, musicTitle[song_playing],
				         hd_music_playing() ? "HD" : "Classic");

			const int x = VGAScreen->w / 2;

			// Progress bar for the current track, when a length is known (i.e.
			// an HD rendition exists to clock the duration) and not in FX mode.
			if (!fx)
			{
				Uint32 pos, len;
				music_position(&pos, &len);
				if (len > 0)
				{
					const int bx1 = 60, bx2 = 260, by = 157;
					const int inner = bx2 - bx1 - 2;
					int fill = (int)((Uint64)inner * pos / len);
					if (fill > inner)
						fill = inner;

					JE_rectangle(VGAScreen, bx1, by, bx2, by + 3, 8);
					if (fill > 0)
						fill_rectangle_xy(VGAScreen, bx1 + 1, by + 1, bx1 + fill, by + 2, 13);
				}
			}

			drawFontHvAligned(VGAScreen, x, 163, "Press ESC to quit the jukebox.",       FONT_SMALL, ALIGN_CENTER, 1, 0);
			drawFontHvAligned(VGAScreen, x, 172, "Up/Down change song. Left/Right seek.", FONT_SMALL, ALIGN_CENTER, 1, 0);
			drawFontHvAligned(VGAScreen, x, 181, "H toggles HD/Classic. V cycles visualizer.", FONT_SMALL, ALIGN_CENTER, 1, 0);
			drawFontHvAligned(VGAScreen, x, 190, buffer,                                 FONT_SMALL, ALIGN_CENTER, 1, 4);
		}

		if (palette_fade_steps > 0)
			step_fade_palette(diff, palette_fade_steps--, 0, 255);
		
		JE_showVGA();

		waitUntilElapsed();

		// Quit on mouse click.
		if (mouseGetInput(INPUT_NO_MOTION, NULL))
			trigger_quit = true;

		if (gotKeyboardInput)
		{
			switch (KEY_COMBO(keyboardInput.mod, keyboardInput.scancode))
			{
			case SDL_SCANCODE_ESCAPE:
			case SDL_SCANCODE_Q:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_Q):
				trigger_quit = true;
				break;

			case SDL_SCANCODE_SPACE:
				hide_text = !hide_text;
				break;

			case SDL_SCANCODE_F:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_F):
				fading_song = !fading_song;
				break;
			case SDL_SCANCODE_N:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_N):
				fade_looped_songs = !fade_looped_songs;
				break;
			case SDL_SCANCODE_H:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_H):
				set_hd_music_playing(!hd_music);
				break;
			case SDL_SCANCODE_V:
				if (visualizer_mode == 3)
					visualizer_mode = 1;
				else if (visualizer_mode == 1)
					visualizer_mode = 2;
				else if (visualizer_mode == 2)
					visualizer_mode = 0;
				else
					visualizer_mode = 3;
				break;
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_V):
				// Not implemented.
				break;
			case SDL_SCANCODE_T:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_T):
				// Not implemented.
				break;

			case SDL_SCANCODE_SLASH:
				fx = !fx;
				break;
			case SDL_SCANCODE_COMMA:
				if (fx && --fx_num < 0)
					fx_num = SOUND_COUNT - 1;
				break;
			case SDL_SCANCODE_PERIOD:
				if (fx && ++fx_num >= SOUND_COUNT)
					fx_num = 0;
				break;
			case SDL_SCANCODE_SEMICOLON:
				if (fx)
					JE_playSampleNum(fx_num + 1);
				break;

			case SDL_SCANCODE_UP:
				play_song((song_playing > 0 ? song_playing : MUSIC_NUM) - 1);
				stopped = false;
				break;
			case SDL_SCANCODE_RETURN:
			case SDL_SCANCODE_DOWN:
				play_song((song_playing + 1) % MUSIC_NUM);
				stopped = false;
				break;
			case SDL_SCANCODE_LEFT:
				music_seek_relative(-5 * audioSampleRate);
				break;
			case SDL_SCANCODE_RIGHT:
				music_seek_relative(5 * audioSampleRate);
				break;
			case SDL_SCANCODE_S:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_S):
				stop_song();
				stopped = true;
				break;
			case SDL_SCANCODE_R:
			case KEY_COMBO(KMOD_SHIFT, SDL_SCANCODE_R):
				restart_song();
				stopped = false;
				break;

			default:
				break;
			}
		}
		
		// user wants to quit, start fade-out
		if (trigger_quit && !quitting)
		{
			palette_fade_steps = 15;
			
			SDL_Color black = { 0, 0, 0 };
			init_step_fade_solid(diff, black, 0, 255);
			
			quitting = true;
		}
		
		// if fade-out finished, we can finally quit
		if (quitting && palette_fade_steps == 0)
			break;
	}

	set_volume(tyrMusicVolume, fxVolume);
}
