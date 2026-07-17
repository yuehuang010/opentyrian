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
/** @file nortsong.c
 * Sound API: frame timing/pacing, sound file loading, and sample/volume playback.
 *
 * Entry points: delayUntilElapsed(), loadSndFile(), JE_playSampleNum(),
 * JE_changeVolume(), getFrameCountTicks(), getTickInterpAlpha().
 */

#include "nortsong.h"

#include "controller.h"
#include "file.h"
#include "keyboard.h"
#include "loudness.h"
#include "musmast.h"
#include "network.h"
#include "opentyr.h"
#include "params.h"
#include "sndmast.h"
#include "vga256d.h"

#include "SDL.h"

#include <string.h>

JE_word frameCountMax;

Sint16 *soundSamples[SOUND_COUNT] = { NULL }; /* [1..soundnum + 9] */  // FKA digiFx
size_t soundSampleCount[SOUND_COUNT] = { 0 }; /* [1..soundnum + 9] */  // FKA fxSize

bool hd_sfx = true;

JE_word tyrMusicVolume, fxVolume;
const JE_word fxPlayVol = 4;
JE_word tempVolume;

// The frequency of the x86 programmable interval timer is (315 / 88 / 3) MHz.
// The PIT was configured to generate an interrupt every `speed` cycles, which
// decremented `frameCount`.

static Uint16 frameSpeed = 0x4300;

// Fixed point UQ6.10 in milliseconds.
static Uint16 framePeriod = ((Uint64)0x4300 << 10) * 1000 * 88 * 3 / 315000000;

// Fixed point UQ22.10 in milliseconds.
static Uint32 frameCountEnd = 0;
static Uint32 frameCount2End = 0;

void setFrameSpeed(Uint16 speed)  // FKA NortSong.speed and NortSong.setTimerInt
{
	frameSpeed = speed;
	framePeriod = ((Uint64)speed << 10) * 1000 * 88 * 3 / 315000000;

	Uint32 now = SDL_GetTicks() << 10;
	frameCountEnd = now;
}

void setFrameCount(JE_word frameCount)  // FKA NortSong.frameCount
{
	// Keep the partial timer period that has already elapsed.
	Uint32 now = SDL_GetTicks() << 10;
	Sint32 diff = now - frameCountEnd;
	if (diff >= framePeriod)
		frameCountEnd = now - (Uint32)diff % framePeriod;
	else if (-diff >= framePeriod)
		frameCountEnd = now + (Uint32)-diff % framePeriod;

	frameCountEnd += frameCount * framePeriod;
}

void setFrameCount2(JE_word frameCount2)  // FKA NortSong.frameCount2
{
	// Keep the partial timer period that has already elapsed.
	Uint32 now = SDL_GetTicks() << 10;
	Sint32 diff = now - frameCount2End;
	if (diff >= framePeriod)
		frameCount2End = now - (Uint32)diff % framePeriod;
	else if (-diff >= framePeriod)
		frameCount2End = now + (Uint32)-diff % framePeriod;

	frameCount2End += frameCount2 * framePeriod;
}

Uint32 getFrameCountTicks(void)
{
	const Uint32 half = 1 << 9;
	Uint32 now = SDL_GetTicks() << 10;
	Sint32 diff = frameCountEnd - now;
	return diff >= 0 ? ((Uint32)diff + half) >> 10 : 0;
}

Uint32 getFrameCount2Ticks(void)
{
	const Uint32 half = 1 << 9;
	Uint32 now = SDL_GetTicks() << 10;
	Sint32 diff = frameCount2End - now;
	return diff >= 0 ? ((Uint32)diff + half) >> 10 : 0;
}

void delayUntilElapsed(void)
{
	const Uint32 half = 1 << 9;
	Uint32 now = SDL_GetTicks() << 10;
	Sint32 diff = frameCountEnd - now;
	if (diff >= 0)
		SDL_Delay(((Uint32)diff + half) >> 10);
}

// Interpolation factor in [0,1] for the current simulation tick: 0.0 at the tick's
// start, 1.0 at its scheduled end (frameCountEnd). The high-fps render loop uses
// this to blend previous and current entity positions. The tick period is
// frameCountMax base periods; frameCountMax can vary between ticks (e.g. pentiumMode
// alternates 2/3), which only perturbs the blend by a sub-tick fraction.
float getTickInterpAlpha(void)
{
	Uint32 period = (Uint32)framePeriod * frameCountMax; // UQ22.10 ms
	if (period == 0)
		return 1.0f;

	Uint32 now = SDL_GetTicks() << 10;
	Sint32 remaining = (Sint32)(frameCountEnd - now);
	if (remaining <= 0)
		return 1.0f;
	if ((Uint32)remaining >= period)
		return 0.0f;

	return 1.0f - (float)remaining / (float)period;
}

// --- HD SFX/voice bank support (Phase S2) ---------------------------------
//
// An HD bank ("hdsnd_sfx.dat" / "hdsnd_voices.dat" / "hdsnd_voicesc.dat",
// baked offline by tools/mkhdsnd.py from tools/hd_upsample_snd.py's "best"
// method -- see tools/HDSFX_EXPERIMENT.md) holds 16-bit signed mono PCM at a
// fixed sample rate (44100), packaged with the same count+offset-table shape
// as the classic .snd files:
//
//   u32 magic ("HSND", literal bytes, not byte-swapped)
//   u16 count
//   u32 sampleRate
//   (count+1) x u32 absolute byte offsets (offset[count] == EOF)
//   ... contiguous Sint16 LE PCM data, one sample after another
//
// This is purely additive: loadSndFile() below runs the classic 8-bit/11025Hz
// path unchanged first, then -- if hd_sfx is enabled -- overlays each index
// with the HD-converted sample when the bank and that sample both parse
// cleanly. Any missing file, header/offset corruption, or out-of-range/
// malformed sample index leaves the classic result already in
// soundSamples[]/soundSampleCount[] untouched (per-sample fallback).

typedef struct
{
	FILE *f;
	Uint16 count;
	Uint32 sampleRate;
	Uint32 *offsets; // count+1 entries, offsets[count] == EOF marker
} HdSndBank;

// Opens and structurally validates an HD bank. Never dies: on any problem
// (missing file, bad magic, truncated header/offsets, bad offsets) this
// returns false and the caller falls back to classic per-sample. The
// fread_*_die helpers are used for the actual multi-byte reads (per project
// convention, for correct byte-swapping on big-endian), but only once the
// file has been confirmed to hold enough bytes for them to succeed -- so the
// "_die" abort path is never actually reachable here.
static bool hdSndBankOpen(const char *filename, HdSndBank *bank)
{
	memset(bank, 0, sizeof(*bank));

	FILE *f = dir_fopen(data_dir(), filename, "rb");
	if (f == NULL)
		return false;

	long fileSize = ftell_eof(f);
	if (fileSize < (long)(4 + 2 + 4)) // magic + count + sampleRate
	{
		fclose(f);
		return false;
	}

	Uint8 magic[4];
	fread_u8_die(magic, 4, f);
	if (memcmp(magic, "HSND", 4) != 0)
	{
		fclose(f);
		return false;
	}

	Uint16 count;
	fread_u16_die(&count, 1, f);

	Uint32 sampleRate;
	fread_u32_die(&sampleRate, 1, f);

	// Sanity bounds -- SOUND_COUNT is 38; a well-formed bank never needs more
	// than a couple hundred entries.
	if (sampleRate == 0 || count == 0 || count > 1000)
	{
		fclose(f);
		return false;
	}

	long offsetTableEnd = 4 + 2 + 4 + (long)sizeof(Uint32) * (count + 1);
	if (fileSize < offsetTableEnd)
	{
		fclose(f);
		return false;
	}

	Uint32 *offsets = malloc(sizeof(Uint32) * (count + 1));
	if (offsets == NULL)
	{
		fclose(f);
		return false;
	}
	fread_u32_die(offsets, count + 1, f);

	bool ok = true;
	for (Uint32 i = 0; i <= count; ++i)
	{
		if (offsets[i] > (Uint32)fileSize)
		{
			ok = false;
			break;
		}
		if (i > 0 && offsets[i] < offsets[i - 1])
		{
			ok = false;
			break;
		}
	}
	if (!ok)
	{
		free(offsets);
		fclose(f);
		return false;
	}

	bank->f = f;
	bank->count = count;
	bank->sampleRate = sampleRate;
	bank->offsets = offsets;
	return true;
}

static void hdSndBankClose(HdSndBank *bank)
{
	if (bank->f != NULL)
		fclose(bank->f);
	free(bank->offsets);
	memset(bank, 0, sizeof(*bank));
}

// Loads sample `index` from an open, validated HD bank as raw Sint16 LE PCM
// at bank->sampleRate. Returns false (no allocation performed) if the index
// is out of range or the sample's byte span isn't a whole number of Sint16
// frames -- the caller then leaves the classic result in place for that slot.
static bool hdSndBankLoadSample(HdSndBank *bank, Uint16 index, Sint16 **outSamples, size_t *outCount)
{
	if (index >= bank->count)
		return false;

	Uint32 start = bank->offsets[index];
	Uint32 end = bank->offsets[index + 1];
	if (end < start)
		return false;

	Uint32 byteLen = end - start;
	if (byteLen == 0 || (byteLen % sizeof(Sint16)) != 0)
		return false;

	size_t frameCount = byteLen / sizeof(Sint16);

	Sint16 *samples = malloc(byteLen);
	if (samples == NULL)
		return false;

	fseek(bank->f, (long)start, SEEK_SET);
	fread_s16_die(samples, frameCount, bank->f);

	*outSamples = samples;
	*outCount = frameCount;
	return true;
}

// Overlays soundSamples[baseIndex .. baseIndex+sampleCount) with HD-converted
// audio from `bank`, per-sample-falling-back (leaving the classic result already
// in place) on any load or conversion failure.
static void hdSndBankApply(HdSndBank *bank, size_t baseIndex, Uint16 sampleCount)
{
	SDL_AudioCVT cvt;
	if (SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, 1, (int)bank->sampleRate, AUDIO_S16SYS, 1, audioSampleRate) < 0)
	{
		fprintf(stderr, "warning: hd_sfx: failed to build HD audio converter: %s\n", SDL_GetError());
		return;
	}

	for (Uint16 i = 0; i < sampleCount; ++i)
	{
		Sint16 *raw;
		size_t rawCount;
		if (!hdSndBankLoadSample(bank, i, &raw, &rawCount))
			continue; // missing/malformed sample: classic result stands

		size_t rawBytes = rawCount * sizeof(Sint16);

		cvt.buf = malloc(rawBytes * (size_t)cvt.len_mult);
		if (cvt.buf == NULL)
		{
			free(raw);
			continue;
		}
		memcpy(cvt.buf, raw, rawBytes);
		cvt.len = (int)rawBytes;
		free(raw);

		if (SDL_ConvertAudio(&cvt) != 0)
		{
			fprintf(stderr, "warning: hd_sfx: failed to convert HD sample %zu: %s\n",
			        baseIndex + i, SDL_GetError());
			free(cvt.buf);
			continue;
		}

		free(soundSamples[baseIndex + i]);
		soundSamples[baseIndex + i] = malloc(cvt.len_cvt);
		memcpy(soundSamples[baseIndex + i], cvt.buf, cvt.len_cvt);
		soundSampleCount[baseIndex + i] = cvt.len_cvt / sizeof(Sint16);

		free(cvt.buf);
	}
}

void loadSndFile(bool xmas)
{
	FILE *f;

	f = dir_fopen_die(data_dir(), "tyrian.snd", "rb");

	Uint16 sfxCount;
	Uint32 sfxPositions[SFX_COUNT + 1];

	// Read number of sounds.
	fread_u16_die(&sfxCount, 1, f);
	if (sfxCount != SFX_COUNT)
		goto die;

	// Read positions of sounds.
	fread_u32_die(sfxPositions, sfxCount, f);

	// Determine end of last sound.
	fseek(f, 0, SEEK_END);
	sfxPositions[sfxCount] = ftell(f);

	// Read samples.
	for (size_t i = 0; i < sfxCount; ++i)
	{
		soundSampleCount[i] = sfxPositions[i + 1] - sfxPositions[i];

		// Sound size cannot exceed 64 KiB.
		if (soundSampleCount[i] > UINT16_MAX)
			goto die;

		free(soundSamples[i]);
		soundSamples[i] = malloc(soundSampleCount[i]);

		fseek(f, sfxPositions[i], SEEK_SET);
		fread_u8_die((Uint8 *)soundSamples[i], soundSampleCount[i], f);
	}

	fclose(f);

	f = dir_fopen_die(data_dir(), xmas ? "voicesc.snd" : "voices.snd", "rb");

	Uint16 voiceCount;
	Uint32 voicePositions[VOICE_COUNT + 1];

	// Read number of sounds.
	fread_u16_die(&voiceCount, 1, f);
	if (voiceCount != VOICE_COUNT)
		goto die;

	// Read positions of sounds.
	fread_u32_die(voicePositions, voiceCount, f);

	// Determine end of last sound.
	fseek(f, 0, SEEK_END);
	voicePositions[voiceCount] = ftell(f);

	for (size_t vi = 0; vi < voiceCount; ++vi)
	{
		size_t i = SFX_COUNT + vi;

		soundSampleCount[i] = voicePositions[vi + 1] - voicePositions[vi];

		// Voice sounds have some bad data at the end.
		soundSampleCount[i] = soundSampleCount[i] >= 100
			? soundSampleCount[i] - 100
			: 0;

		// Sound size cannot exceed 64 KiB.
		if (soundSampleCount[i] > UINT16_MAX)
			goto die;

		free(soundSamples[i]);
		soundSamples[i] = malloc(soundSampleCount[i]);

		fseek(f, voicePositions[vi], SEEK_SET);
		fread_u8_die((Uint8 *)soundSamples[i], soundSampleCount[i], f);
	}

	fclose(f);

	// Convert samples to output sample format and rate.

	SDL_AudioCVT cvt;
	if (SDL_BuildAudioCVT(&cvt, AUDIO_S8, 1, 11025, AUDIO_S16SYS, 1, audioSampleRate) < 0)
	{
		fprintf(stderr, "error: Failed to build audio converter: %s\n", SDL_GetError());

		for (int i = 0; i < SOUND_COUNT; ++i)
			soundSampleCount[i] = 0;

		return;
	}

	size_t maxSampleSize = 0;
	for (size_t i = 0; i < SOUND_COUNT; ++i)
		maxSampleSize = MAX(maxSampleSize, soundSampleCount[i]);

	cvt.buf = malloc(maxSampleSize * cvt.len_mult);

	for (size_t i = 0; i < SOUND_COUNT; ++i)
	{
		cvt.len = soundSampleCount[i];
		memcpy(cvt.buf, soundSamples[i], cvt.len);

		if (SDL_ConvertAudio(&cvt))
		{
			fprintf(stderr, "error: Failed to convert audio: %s\n", SDL_GetError());

			soundSampleCount[i] = 0;

			continue;
		}

		free(soundSamples[i]);
		soundSamples[i] = malloc(cvt.len_cvt);

		memcpy(soundSamples[i], cvt.buf, cvt.len_cvt);
		soundSampleCount[i] = cvt.len_cvt / sizeof (Sint16);
	}

	free(cvt.buf);

	// HD overlay (Phase S2): additive, per-sample fallback to the classic
	// result already populated above. See the HdSndBank helpers for details.
	if (hd_sfx)
	{
		HdSndBank bank;

		if (hdSndBankOpen("hdsnd_sfx.dat", &bank))
		{
			hdSndBankApply(&bank, 0, (Uint16)MIN((Uint32)SFX_COUNT, (Uint32)bank.count));
			hdSndBankClose(&bank);
		}

		if (hdSndBankOpen(xmas ? "hdsnd_voicesc.dat" : "hdsnd_voices.dat", &bank))
		{
			hdSndBankApply(&bank, SFX_COUNT, (Uint16)MIN((Uint32)VOICE_COUNT, (Uint32)bank.count));
			hdSndBankClose(&bank);
		}
	}

	return;

die:
	fprintf(stderr, "error: Unexpected data was read from a file.\n");
	SDL_Quit();
	exit(EXIT_FAILURE);
}

void JE_playSampleNum(JE_byte samplenum)
{
	multiSamplePlay(soundSamples[samplenum-1], soundSampleCount[samplenum-1], 0, fxPlayVol);
}

void JE_changeVolume(JE_word *music, int music_delta, JE_word *sample, int sample_delta)
{
	int music_temp = *music + music_delta,
	    sample_temp = *sample + sample_delta;
	
	if (music_delta)
	{
		if (music_temp > 255)
		{
			music_temp = 255;
			JE_playSampleNum(S_CLINK);
		}
		else if (music_temp < 0)
		{
			music_temp = 0;
			JE_playSampleNum(S_CLINK);
		}
	}
	
	if (sample_delta)
	{
		if (sample_temp > 255)
		{
			sample_temp = 255;
			JE_playSampleNum(S_CLINK);
		}
		else if (sample_temp < 0)
		{
			sample_temp = 0;
			JE_playSampleNum(S_CLINK);
		}
	}
	
	*music = music_temp;
	*sample = sample_temp;
	
	set_volume(*music, *sample);
}
