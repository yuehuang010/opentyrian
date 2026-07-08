/*
 * render_music - Phase S1 offline OPL renderer
 *
 * Renders every LDS/OPL2 track in music.mus to 44.1 kHz 16-bit stereo PCM
 * WAV files using the game's *actual* sequencer/synth (src/lds_play.c +
 * src/opl.c, linked in unmodified) rather than a reimplementation.
 *
 * For each track this also detects the sequencer's natural loop-back point
 * (LDS songs jump backward to an earlier position once fully played) and
 * writes a small ".loop" sidecar with the sample offsets:
 *
 *     LOOPSTART <samples>
 *     LOOPLENGTH <samples>
 *
 * where the rendered WAV itself ends exactly at the loop point (intro +
 * one full loop iteration), following the common OGG loop-tag convention:
 * on end-of-file playback seeks back to LOOPSTART and continues.  Tracks
 * that terminate instead of looping (an explicit "stop" command in the
 * LDS data) get no sidecar loop points.
 *
 * Usage:
 *   render_music [data_dir] [out_dir]
 *
 * data_dir defaults to "tyrian21", out_dir defaults to "." .
 * Reads data_dir/music.mus directly (plain fopen -- offline tool, not the
 * engine's dir_fopen/bundle chain).
 */

#include "../src/lds_play.h"
#include "../src/opl.h"
#include "../src/musmast.h"
#include "../src/opentyr.h"
#include "../src/file.h" // for fread_u16_die/fread_u32_die + the fread_die/ftell_eof declarations we provide below

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// loudness.c normally defines this (SDL-negotiated device rate); lds_play.c
// needs it at link time for the opl_init() macro (adlib_init(audioSampleRate)).
int audioSampleRate = 44100;

// Minimal standalone replacements for the file.h helpers lds_play.c calls,
// so this tool doesn't have to link file.c/bundle.c/varz.c just to read one
// file.  Semantics match file.c exactly.
void fread_die(void *buffer, size_t size, size_t count, FILE *stream)
{
	if (fread(buffer, size, count, stream) != count)
	{
		fprintf(stderr, "error: unexpected end of file/read error\n");
		exit(1);
	}
}

long ftell_eof(FILE *f)
{
	long pos = ftell(f);
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, pos, SEEK_SET);
	return size;
}

// Same cadence math as loudness.c's audioCallback (OUTPUT_QUALITY=4 -> 44100 Hz).
static const int ldsUpdate2Rate = 139; // 69.5 * 2
static int samplesPerLdsUpdate;
static int samplesPerLdsUpdateFrac;

#define SAMPLE_RATE 44100

// Generous safety cap on sequencer ticks so a malformed/never-looping track
// can't spin forever.
#define MAX_TICKS 400000

typedef struct
{
	unsigned int position;
	long long sample_offset;
} TickMark;

static void write_wav_stereo(const char *path, const Sint16 *mono, long long frameCount)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		fprintf(stderr, "error: failed to open '%s' for writing\n", path);
		exit(1);
	}

	Uint32 dataBytes = (Uint32)(frameCount * 2 /*channels*/ * 2 /*bytes/sample*/);
	Uint32 riffBytes = 36 + dataBytes;
	Uint16 channels = 2;
	Uint32 sampleRate = SAMPLE_RATE;
	Uint16 bitsPerSample = 16;
	Uint16 blockAlign = channels * bitsPerSample / 8;
	Uint32 byteRate = sampleRate * blockAlign;

	fwrite("RIFF", 1, 4, f);
	fwrite(&riffBytes, 4, 1, f);
	fwrite("WAVE", 1, 4, f);

	fwrite("fmt ", 1, 4, f);
	Uint32 fmtChunkSize = 16;
	fwrite(&fmtChunkSize, 4, 1, f);
	Uint16 audioFormat = 1; // PCM
	fwrite(&audioFormat, 2, 1, f);
	fwrite(&channels, 2, 1, f);
	fwrite(&sampleRate, 4, 1, f);
	fwrite(&byteRate, 4, 1, f);
	fwrite(&blockAlign, 2, 1, f);
	fwrite(&bitsPerSample, 2, 1, f);

	fwrite("data", 1, 4, f);
	fwrite(&dataBytes, 4, 1, f);

	// Duplicate the engine's mono OPL output to stereo L/R -- the synth
	// itself only ever produced one channel; this is not new content.
	for (long long i = 0; i < frameCount; ++i)
	{
		Sint16 s = mono[i];
		fwrite(&s, 2, 1, f);
		fwrite(&s, 2, 1, f);
	}

	fclose(f);
}

// Renders one track.  Returns the rendered mono sample buffer (caller frees)
// and sets *outFrameCount, *outLoopStart, *outLoopLength (-1 if no loop
// detected -- track terminates instead).
static Sint16 *render_track(FILE *musicFile, unsigned int songOffset, unsigned int songSize,
                             long long *outFrameCount, long long *outLoopStart, long long *outLoopLength)
{
	if (!lds_load(musicFile, songOffset, songSize))
	{
		fprintf(stderr, "error: lds_load failed\n");
		exit(1);
	}

	long long capacity = 1 << 20;
	Sint16 *buf = malloc(capacity * sizeof(Sint16));
	long long frameCount = 0;

	TickMark *marks = malloc(MAX_TICKS * sizeof(TickMark));
	int markCount = 0;

	int samplesUntilLdsUpdate = 0;
	int samplesUntilLdsUpdateFrac = 0;

	long long loopStart = -1, loopEnd = -1;
	bool wasLooped = false;

	for (int tick = 0; tick < MAX_TICKS; ++tick)
	{
		if (!playing)
			break;

		unsigned int posBefore = lds_get_position();
		if (markCount < MAX_TICKS)
		{
			marks[markCount].position = posBefore;
			marks[markCount].sample_offset = frameCount;
			markCount++;
		}

		lds_update();

		samplesUntilLdsUpdate += samplesPerLdsUpdate;
		samplesUntilLdsUpdateFrac += samplesPerLdsUpdateFrac;
		if (samplesUntilLdsUpdateFrac >= ldsUpdate2Rate)
		{
			samplesUntilLdsUpdate += 1;
			samplesUntilLdsUpdateFrac -= ldsUpdate2Rate;
		}

		int count = samplesUntilLdsUpdate;
		samplesUntilLdsUpdate = 0;

		if (frameCount + count > capacity)
		{
			while (frameCount + count > capacity)
				capacity *= 2;
			buf = realloc(buf, capacity * sizeof(Sint16));
		}

		opl_update(buf + frameCount, count);
		frameCount += count;

		if (songlooped && !wasLooped)
		{
			// The row that just played issued the loop-back jump; the
			// audio for this tick (already appended above) is the last of
			// the pre-loop content.  posplay has already been updated to
			// the loop target for the next tick.
			wasLooped = true;
			loopEnd = frameCount;

			unsigned int targetPos = lds_get_position();

			// Find the earliest tick whose position matches the loop
			// target -- that tick's sample offset is where playback
			// should resume from.
			for (int i = 0; i < markCount; ++i)
			{
				if (marks[i].position == targetPos)
				{
					loopStart = marks[i].sample_offset;
					break;
				}
			}

			break; // one intro + one loop iteration is enough to render
		}
	}

	free(marks);

	*outFrameCount = frameCount;
	if (wasLooped && loopStart >= 0)
	{
		*outLoopStart = loopStart;
		*outLoopLength = loopEnd - loopStart;
	}
	else
	{
		*outLoopStart = -1;
		*outLoopLength = -1;
	}

	return buf;
}

int main(int argc, char **argv)
{
	const char *dataDir = (argc > 1) ? argv[1] : "tyrian21";
	const char *outDir = (argc > 2) ? argv[2] : ".";

	samplesPerLdsUpdate = 2 * (SAMPLE_RATE / ldsUpdate2Rate);
	samplesPerLdsUpdateFrac = 2 * (SAMPLE_RATE % ldsUpdate2Rate);

	char musicPath[4096];
	snprintf(musicPath, sizeof musicPath, "%s/music.mus", dataDir);

	FILE *musicFile = fopen(musicPath, "rb");
	if (musicFile == NULL)
	{
		fprintf(stderr, "error: failed to open '%s'\n", musicPath);
		return 1;
	}

	Uint16 songCount = 0;
	fread_u16_die(&songCount, 1, musicFile);

	Uint32 *songOffset = malloc((songCount + 1) * sizeof(*songOffset));
	fread_u32_die(songOffset, songCount, musicFile);
	songOffset[songCount] = (Uint32)ftell_eof(musicFile);

	if (songCount != MUSIC_NUM)
		fprintf(stderr, "warning: music.mus has %u tracks, expected MUSIC_NUM=%u\n", songCount, MUSIC_NUM);

	for (unsigned int n = 0; n < songCount; ++n)
	{
		unsigned int size = songOffset[n + 1] - songOffset[n];

		long long frameCount, loopStart, loopLength;
		Sint16 *pcm = render_track(musicFile, songOffset[n], size, &frameCount, &loopStart, &loopLength);

		char wavPath[4096], loopPath[4096];
		snprintf(wavPath, sizeof wavPath, "%s/hdmusic_%02u.wav", outDir, n + 1);
		snprintf(loopPath, sizeof loopPath, "%s/hdmusic_%02u.loop", outDir, n + 1);

		write_wav_stereo(wavPath, pcm, frameCount);

		if (loopStart >= 0)
		{
			FILE *lf = fopen(loopPath, "w");
			if (lf != NULL)
			{
				fprintf(lf, "LOOPSTART %lld\n", loopStart);
				fprintf(lf, "LOOPLENGTH %lld\n", loopLength);
				fclose(lf);
			}
		}

		printf("track %2u: %-40s %8lld frames (%.2fs)%s\n",
		       n + 1,
		       (n < MUSIC_NUM) ? musicTitle[n] : "?",
		       frameCount, (double)frameCount / SAMPLE_RATE,
		       (loopStart >= 0) ? "" : "  [no loop detected]");
		if (loopStart >= 0)
			printf("           loop: start=%lld length=%lld\n", loopStart, loopLength);

		free(pcm);
	}

	free(songOffset);
	fclose(musicFile);

	lds_free();

	return 0;
}
