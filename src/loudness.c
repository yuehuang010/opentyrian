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
/** @file loudness.c
 * Audio subsystem init/teardown and song/sample playback control glue.
 *
 * Entry points: init_audio(), deinit_audio(), refresh_current_song(),
 * restart_song(), stop_song(), fade_song().
 */

#include "loudness.h"

#include "file.h"
#include "lds_play.h"
#include "nortsong.h"
#include "opentyr.h"
#include "params.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Vendored OGG Vorbis decoder for the HD streamed-music path (Phase S1). The
// implementation lives in its own translation unit (obj/stb_vorbis.o); here we
// pull in declarations only.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define OUTPUT_QUALITY 4  // 44.1 kHz

int audioSampleRate = 0;

bool music_stopped = true;
unsigned int song_playing = 0;

bool audio_disabled = false, music_disabled = false, samples_disabled = false;

static SDL_AudioDeviceID audioDevice = 0;

static Uint8 musicVolume = 255;
static Uint8 sampleVolume = 255;

static const float volumeRange = 30.0f;  // dB

// Fixed point Q20.12; needs to be able to store (10 * INT16_MIN/MAX)
static Sint32 volumeFactorTable[256];
#define TO_FIXED(x) ((Sint32)((x) * (1 << 12)))
#define FIXED_TO_INT(x) ((Sint32)((x) >> 12))

// Twice the Loudness update rate (in updates/second).  In Tyrian, Loudness
// updates were performed at the same rate as the game timer, which varied
// depending on the game speed (~69.57 Hz at most game speeds).  We don't have
// the same limitations, so we'll keep the update rate constant, but we do want
// to stick to integer math, so we'll update at 69.5 Hz.
static const int ldsUpdate2Rate = 139;  // 69.5 * 2

static int samplesPerLdsUpdate;
static int samplesPerLdsUpdateFrac;

static int samplesUntilLdsUpdate = 0;
static int samplesUntilLdsUpdateFrac = 0;

static FILE *music_file = NULL;
static Uint32 *song_offset;
static Uint16 song_count = 0;

#define CHANNEL_COUNT 8
static const Sint16 *channelSamples[CHANNEL_COUNT];
static size_t channelSampleCount[CHANNEL_COUNT] = { 0 };
static Uint8 channelVolume[CHANNEL_COUNT];
#define CHANNEL_VOLUME_LEVELS 8

// --- HD streamed music (Phase S1) -----------------------------------------
//
// When hd_music is on and an "hdmusic_NN.ogg" exists for the playing track
// (NN = song_num + 1, matching tools/render_music.c's 1-based output), the
// audio callback decodes that OGG in place of running the LDS/OPL2 synth. The
// OGGs are pre-rendered from the *same* sequencer (see tools/render_music.c),
// stereo 44.1 kHz, with LOOPSTART/LOOPLENGTH Vorbis comments (per-channel
// sample offsets) marking the sequencer's natural loop-back point.
//
// Fallback is per-song: any missing/undecodable OGG, or a sample-rate that
// doesn't match the audio device, leaves hd_active false so the LDS synth
// (still loaded by load_song) plays that track exactly as before. The
// hd_music master toggle (default on) gates the whole path.
bool hd_music = true;

static stb_vorbis *hd_vorbis = NULL;  // decoder for the current OGG, or NULL
static Uint8 *hd_ogg_data = NULL;     // in-memory OGG backing hd_vorbis (owned)
static bool hd_active = false;        // an OGG is loaded and should be decoded
static bool hd_ended = false;         // a non-looping OGG has played to its end
static bool hd_has_loop = false;
static Uint32 hd_loop_start = 0;      // per-channel sample offset to loop back to
static Uint32 hd_loop_end = 0;        // per-channel sample offset of loop end

// Fade-out for the HD path (fade_song): a Q12 gain ramped down to zero, at
// which point the track is stopped. Mirrors what lds_fade() does for the synth.
static bool hd_fade_active = false;
static Sint32 hd_fade_gain = 0;       // Q12; 4096 == unity
static Sint32 hd_fade_step = 0;       // per-frame decrement

// Scratch for interleaved-stereo decode before downmix to the engine's mono
// mixing buffer. Sized for a generous callback; larger callbacks decode in
// chunks against it.
#define HD_DECODE_FRAMES 4096
static short hd_stereo[HD_DECODE_FRAMES * 2];

static void audioCallback(void *userdata, Uint8 *stream, int size);

static void load_song(unsigned int song_num);

// Releases the current OGG decoder and its backing memory. Caller must hold the
// audio lock (or be certain the callback can't run).
static void hd_music_close(void)
{
	if (hd_vorbis != NULL)
	{
		stb_vorbis_close(hd_vorbis);
		hd_vorbis = NULL;
	}
	free(hd_ogg_data);
	hd_ogg_data = NULL;
	hd_active = false;
	hd_ended = false;
	hd_has_loop = false;
	hd_fade_active = false;
}

// Parses LOOPSTART / LOOPLENGTH Vorbis comments into hd_loop_*.
static void hd_parse_loop_comments(stb_vorbis *v)
{
	hd_has_loop = false;
	hd_loop_start = 0;
	hd_loop_end = 0;

	stb_vorbis_comment vc = stb_vorbis_get_comment(v);
	long loopStart = -1, loopLength = -1;
	for (int i = 0; i < vc.comment_list_length; ++i)
	{
		const char *c = vc.comment_list[i];
		if (c == NULL)
			continue;
		if (SDL_strncasecmp(c, "LOOPSTART=", 10) == 0)
			loopStart = strtol(c + 10, NULL, 10);
		else if (SDL_strncasecmp(c, "LOOPLENGTH=", 11) == 0)
			loopLength = strtol(c + 11, NULL, 10);
	}

	if (loopStart >= 0 && loopLength > 0)
	{
		hd_loop_start = (Uint32)loopStart;
		hd_loop_end = (Uint32)(loopStart + loopLength);
		hd_has_loop = true;
	}
}

// Tries to load hdmusic_(song_num+1).ogg into an HD decoder. On success sets
// hd_active; on any failure leaves hd_active false (LDS fallback). Caller must
// hold the audio lock (or be certain the callback can't run).
static void hd_music_load_song(unsigned int song_num)
{
	hd_music_close();

	if (!hd_music || music_disabled)
		return;

	char filename[32];
	snprintf(filename, sizeof(filename), "hdmusic_%02u.ogg", song_num + 1);

	FILE *f = dir_fopen(data_dir(), filename, "rb");
	if (f == NULL)
		return;  // no HD track for this song: LDS fallback

	long size = ftell_eof(f);
	if (size <= 0 || size > 64 * 1024 * 1024)
	{
		fclose(f);
		return;
	}

	Uint8 *data = malloc((size_t)size);
	if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size)
	{
		free(data);
		fclose(f);
		return;
	}
	fclose(f);

	int err = 0;
	stb_vorbis *v = stb_vorbis_open_memory(data, (int)size, &err, NULL);
	if (v == NULL)
	{
		fprintf(stderr, "warning: hd_music: failed to open %s (err %d); using synth\n", filename, err);
		free(data);
		return;
	}

	stb_vorbis_info info = stb_vorbis_get_info(v);
	if (info.channels != 2 || (int)info.sample_rate != audioSampleRate)
	{
		// The renderer emits stereo at the device rate; anything else would
		// need resampling we don't do -- fall back to the synth instead.
		fprintf(stderr, "warning: hd_music: %s is %dch/%dHz (want 2ch/%dHz); using synth\n",
		        filename, info.channels, info.sample_rate, audioSampleRate);
		stb_vorbis_close(v);
		free(data);
		return;
	}

	hd_vorbis = v;
	hd_ogg_data = data;
	hd_active = true;
	hd_ended = false;
	hd_fade_active = false;
	hd_parse_loop_comments(v);
}

// Decodes `count` mono frames of the current OGG into `out`, handling looping
// and end-of-stream. Runs on the audio thread.
static void hd_music_decode(Sint16 *out, int count)
{
	int filled = 0;
	while (filled < count)
	{
		if (hd_ended)
		{
			// Non-looping track finished: silence for the rest.
			memset(out + filled, 0, (count - filled) * sizeof(Sint16));
			return;
		}

		int want = MIN(count - filled, HD_DECODE_FRAMES);
		int got = stb_vorbis_get_samples_short_interleaved(hd_vorbis, 2, hd_stereo, want * 2);

		for (int i = 0; i < got; ++i)
			out[filled + i] = (Sint16)(((Sint32)hd_stereo[2 * i] + hd_stereo[2 * i + 1]) / 2);
		filled += got;

		if (got < want)
		{
			// Reached end of stream mid-request.
			if (hd_has_loop)
				stb_vorbis_seek(hd_vorbis, hd_loop_start);
			else
				hd_ended = true;
		}
	}
}

bool init_audio(void)
{
	if (audio_disabled)
		return false;

	SDL_AudioSpec ask, got;

	ask.freq = 11025 * OUTPUT_QUALITY;
	ask.format = AUDIO_S16SYS;
	ask.channels = 1;
	ask.samples = 256 * OUTPUT_QUALITY; // ~23 ms
	ask.callback = audioCallback;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		fprintf(stderr, "error: failed to initialize SDL audio: %s\n", SDL_GetError());
		audio_disabled = true;
		return false;
	}

	int allowedChanges = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE;
#if SDL_VERSION_ATLEAST(2, 0, 9)
	allowedChanges |= SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
#endif
	audioDevice = SDL_OpenAudioDevice(/*device*/ NULL, /*iscapture*/ 0, &ask, &got, allowedChanges);

	if (audioDevice == 0)
	{
		fprintf(stderr, "error: SDL failed to open audio device: %s\n", SDL_GetError());
		audio_disabled = true;
		return false;
	}

	audioSampleRate = got.freq;

	samplesPerLdsUpdate = 2 * (audioSampleRate / ldsUpdate2Rate);
	samplesPerLdsUpdateFrac = 2 * (audioSampleRate % ldsUpdate2Rate);

	volumeFactorTable[0] = 0;
	for (size_t i = 1; i < 256; ++i)
		volumeFactorTable[i] = TO_FIXED(powf(10, (255 - i) * (-volumeRange / (20.0f * 255))));

	opl_init();

	SDL_PauseAudioDevice(audioDevice, 0); // unpause

	return true;
}

static void audioCallback(void *userdata, Uint8 *stream, int size)
{
	(void)userdata;

	Sint16 *const samples = (Sint16 *)stream;
	const int samplesCount = size / sizeof (Sint16);

	if (!music_disabled && !music_stopped && hd_active)
	{
		// HD streamed path: decode the OGG in place of the synth.
		hd_music_decode(samples, samplesCount);

		if (hd_fade_active)
		{
			for (int i = 0; i < samplesCount; ++i)
			{
				samples[i] = (Sint16)FIXED_TO_INT((Sint32)samples[i] * hd_fade_gain);
				hd_fade_gain -= hd_fade_step;
				if (hd_fade_gain <= 0)
				{
					hd_fade_gain = 0;
					hd_fade_active = false;
					music_stopped = true;
					// Zero the tail once faded out.
					for (int j = i + 1; j < samplesCount; ++j)
						samples[j] = 0;
					break;
				}
			}
		}
	}
	else if (!music_disabled && !music_stopped)
	{
		Sint16 *remaining = samples;
		int remainingCount = samplesCount;
		while (remainingCount > 0)
		{
			if (samplesUntilLdsUpdate == 0)
			{
				lds_update();

				// The number of samples that should be produced per Loudness
				// update is not an integer, but we can only produce an integer
				// number of samples, so we accumulate the fractional samples
				// until it amounts to a whole sample.
				samplesUntilLdsUpdate += samplesPerLdsUpdate;
				samplesUntilLdsUpdateFrac += samplesPerLdsUpdateFrac;
				if (samplesUntilLdsUpdateFrac >= ldsUpdate2Rate)
				{
					samplesUntilLdsUpdate += 1;
					samplesUntilLdsUpdateFrac -= ldsUpdate2Rate;
				}
			}

			int count = MIN(samplesUntilLdsUpdate, remainingCount);

			opl_update(remaining, count);

			remaining += count;
			remainingCount -= count;

			samplesUntilLdsUpdate -= count;
		}
	}
	else
	{
		for (int i = 0; i < samplesCount; ++i)
			samples[i] = 0;
	}

	Sint32 musicVolumeFactor = volumeFactorTable[musicVolume];
	if (!hd_active)
		musicVolumeFactor *= 2;  // OPL emulator is too quiet; HD audio is already full-scale

	if (samples_disabled && !music_disabled)
	{
		// Mix music
		Sint16 *remaining = samples;
		int remainingCount = samplesCount;
		while (remainingCount > 0)
		{
			Sint32 sample = *remaining * musicVolumeFactor;

			sample = FIXED_TO_INT(sample);
			*remaining = MIN(MAX(INT16_MIN, sample), INT16_MAX);

			remaining += 1;
			remainingCount -= 1;
		}
	}
	else if (!samples_disabled)
	{
		Sint32 sampleVolumeFactor = volumeFactorTable[sampleVolume];
		Sint32 sampleVolumeFactors[CHANNEL_VOLUME_LEVELS];
		for (int i = 0; i < CHANNEL_VOLUME_LEVELS; ++i)
			sampleVolumeFactors[i] = sampleVolumeFactor * (i + 1) / CHANNEL_VOLUME_LEVELS;

		// Mix music and channels
		Sint16 *remaining = samples;
		int remainingCount = samplesCount;
		while (remainingCount > 0)
		{
			Sint32 sample = *remaining * musicVolumeFactor;

			for (size_t i = 0; i < CHANNEL_COUNT; ++i)
			{
				if (channelSampleCount[i] > 0)
				{
					sample += *channelSamples[i] * sampleVolumeFactors[channelVolume[i]];

					channelSamples[i] += 1;
					channelSampleCount[i] -= 1;
				}
			}

			sample = FIXED_TO_INT(sample);
			*remaining = MIN(MAX(INT16_MIN, sample), INT16_MAX);

			remaining += 1;
			remainingCount -= 1;
		}
	}
}

void deinit_audio(void)
{
	if (audio_disabled)
		return;

	if (audioDevice != 0)
	{
		SDL_PauseAudioDevice(audioDevice, 1); // pause
		SDL_CloseAudioDevice(audioDevice);
		audioDevice = 0;
	}

	SDL_QuitSubSystem(SDL_INIT_AUDIO);

	memset(channelSampleCount, 0, sizeof channelSampleCount);

	hd_music_close();
	lds_free();
}

void load_music(void)  // FKA NortSong.loadSong
{
	if (music_file == NULL)
	{
		music_file = dir_fopen_die(data_dir(), "music.mus", "rb");

		fread_u16_die(&song_count, 1, music_file);

		song_offset = malloc((song_count + 1) * sizeof(*song_offset));

		fread_u32_die(song_offset, song_count, music_file);

		song_offset[song_count] = ftell_eof(music_file);
	}
}

static void load_song(unsigned int song_num)  // FKA NortSong.loadSong
{
	if (song_num < song_count)
	{
		unsigned int song_size = song_offset[song_num + 1] - song_offset[song_num];
		lds_load(music_file, song_offset[song_num], song_size);
	}
	else
	{
		fprintf(stderr, "warning: failed to load song %d\n", song_num + 1);
	}
}

void play_song(unsigned int song_num)  // FKA NortSong.playSong
{
	if (audio_disabled)
		return;

	if (song_num != song_playing)
	{
		SDL_LockAudioDevice(audioDevice);

		music_stopped = true;

		SDL_UnlockAudioDevice(audioDevice);

		load_song(song_num);
		// music_stopped is true here, so the callback won't touch HD/LDS state
		// while we swap it -- same reasoning as load_song above.
		hd_music_load_song(song_num);

		song_playing = song_num;
	}

	SDL_LockAudioDevice(audioDevice);

	music_stopped = false;

	SDL_UnlockAudioDevice(audioDevice);
}

// Reloads the currently-playing song so a runtime hd_music change takes effect
// immediately (switches the live track between the OGG stream and the synth).
// Restarts the track from the top, preserving play/stop state.
void refresh_current_song(void)
{
	if (audio_disabled)
		return;

	unsigned int song_num = song_playing;
	bool wasStopped = music_stopped;

	song_playing = ~0u;  // force play_song to treat this as a new song and reload
	play_song(song_num);

	if (wasStopped)
		stop_song();
}

// Read-only peek at hd_active for UI (e.g. the jukebox's [HD]/[OPL] indicator).
// A plain bool read racing the audio thread's writes is fine here -- same as
// the other shared playback flags (playing, songlooped, etc.).
bool hd_music_playing(void)
{
	return hd_active;
}

// Reloads the sound-effect/voice banks so a runtime hd_sfx change takes effect
// immediately. Held under the audio lock, and any in-flight channels are
// silenced first, because loadSndFile() frees and reallocates the sample
// buffers the callback may be reading through channelSamples[].
void reload_sound_samples(bool xmas)
{
	if (audio_disabled)
	{
		loadSndFile(xmas);
		return;
	}

	SDL_LockAudioDevice(audioDevice);
	memset(channelSampleCount, 0, sizeof channelSampleCount);
	loadSndFile(xmas);
	SDL_UnlockAudioDevice(audioDevice);
}

void restart_song(void)  // FKA Player.selectSong(1)
{
	if (audio_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	if (hd_active)
	{
		stb_vorbis_seek_start(hd_vorbis);
		hd_ended = false;
		hd_fade_active = false;
	}
	else
	{
		lds_rewind();
	}

	music_stopped = false;

	SDL_UnlockAudioDevice(audioDevice);
}

void stop_song(void)  // FKA Player.selectSong(0)
{
	if (audio_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	music_stopped = true;

	SDL_UnlockAudioDevice(audioDevice);
}

void fade_song(void)  // FKA Player.selectSong($C001)
{
	if (audio_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	if (hd_active)
	{
		// Ramp the HD gain to zero over ~1.5 s, then stop (see the callback).
		hd_fade_active = true;
		hd_fade_gain = 1 << 12;  // unity, Q12
		hd_fade_step = (1 << 12) / (audioSampleRate * 3 / 2) + 1;
	}
	else
	{
		lds_fade(1);
	}

	SDL_UnlockAudioDevice(audioDevice);
}

void set_volume(Uint8 musicVolume_, Uint8 sampleVolume_)  // FKA NortSong.setVol and Player.setVol
{
	if (audio_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	musicVolume = musicVolume_;
	sampleVolume = sampleVolume_;

	SDL_UnlockAudioDevice(audioDevice);
}

void multiSamplePlay(const Sint16 *samples, size_t sampleCount, Uint8 chan, Uint8 vol)  // FKA Player.multiSamplePlay
{
	assert(chan < CHANNEL_COUNT);
	assert(vol < CHANNEL_VOLUME_LEVELS);

	if (audio_disabled || samples_disabled)
		return;

	SDL_LockAudioDevice(audioDevice);

	channelSamples[chan] = samples;
	channelSampleCount[chan] = sampleCount;
	channelVolume[chan] = vol;

	SDL_UnlockAudioDevice(audioDevice);
}
