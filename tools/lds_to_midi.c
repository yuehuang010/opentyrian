/*
 * lds_to_midi - Phase S1+ LDS/OPL2 -> Standard MIDI File extraction tool
 *
 * Drives the game's *actual* sequencer (src/lds_play.c, linked in unmodified)
 * exactly like tools/render_music.c's render_track() does, but instead of
 * synthesizing audio (src/opl.c), this tool implements the adlib_* register
 * interface itself as a "tap": every OPL2 register write is captured and
 * decoded into MIDI note/CC/pitch-bend/program-change events, tick-accurate
 * (1 sequencer tick == 1 MIDI tick).
 *
 * This gives you an editable/re-orchestratable MIDI of each track, so a GM
 * soundfont (fluidsynth) or DAW can render an alternative "HD" arrangement,
 * which tools/hd_music_pipeline.sh can then A/B against the classic OPL
 * render and drop into the data dir as hdmusic_NN.ogg.
 *
 * Build (same pattern as render_music.c, but WITHOUT src/opl.c -- this file
 * provides its own adlib_* register-tap implementation):
 *
 *   cc -O2 -std=iso9899:1999 $(pkg-config --cflags sdl2) \
 *      tools/lds_to_midi.c src/lds_play.c src/musmast.c \
 *      $(pkg-config --libs sdl2) -lm -o lds_to_midi
 *
 * Usage:
 *   lds_to_midi <data_dir> <out_dir> [song_1based]
 *
 * data_dir defaults to "tyrian21", out_dir defaults to ".".  With the
 * optional 3rd arg, only that (1-based) song is converted; otherwise all
 * MUSIC_NUM tracks are.
 *
 * For each song N (1-based) this writes into out_dir:
 *   hdmusic_NN.mid       - SMF format 0, division=139 (ticks/quarter),
 *                          single tempo meta event (2,000,000 us/quarter,
 *                          i.e. exactly 69.5 MIDI ticks/sec == 1 LDS tick).
 *                          OPL channel N -> MIDI channel N (0..8); OPL
 *                          rhythm-mode percussion -> MIDI channel 9.
 *   hdmusic_NN.loopinfo  - (looping tracks only) tick + 44.1kHz sample loop
 *                          points, computed with the *same* incremental
 *                          sample math as render_music.c, so they can be
 *                          cross-checked against the shipped hdmusic_NN.ogg's
 *                          LOOPSTART/LOOPLENGTH Vorbis comments.
 *   hdmusic_NN.patches   - report of every distinct OPL instrument
 *                          "fingerprint" seen (FNV-1a hash over the voice's
 *                          register set), which OPL channel(s) it appeared
 *                          on, first tick, note count, and whether
 *                          tools/lds_gm_map.txt maps it to a GM program.
 *                          This is what you hand-edit lds_gm_map.txt from.
 *
 * Instrument mapping file: tools/lds_gm_map.txt (optional; read relative to
 * the current working directory AND next to this executable's invocation --
 * see find_gm_map_path()).  Lines:
 *   <8-hex-fingerprint> <gm_program 0-127> [semitone_transpose] [volume 0-127] [cents -99..99]
 * '#' starts a comment.  Unmapped fingerprints default to GM program 0
 * (Acoustic Grand Piano) with no transpose at volume 100 and no fine-tune.
 * volume is a per-instrument mix level emitted as CC7 whenever the
 * fingerprint takes over a MIDI channel (it scales the decoded
 * velocities/CC11 expression, which are left untouched).  cents is an
 * optional per-instrument fine-tuning offset (RPN 1, channel fine tuning),
 * emitted whenever the fingerprint takes over a MIDI channel and its cents
 * value differs from what that channel last had; use it to correct a GM
 * soundfont preset's tuning to match the OPL original's actual frequency
 * (see tools/validate_pitch.py TEST B), independent of the semitone
 * transpose column.
 *
 * Looping: when the sequencer's songlooped flag fires (same detection as
 * render_music.c: record each tick's sequencer position, and match the
 * post-loop target position against the earliest tick that had it), we keep
 * sequencing for exactly one more full loop iteration (so the MIDI file
 * contains intro + 2 loop iterations -- enough for a GM renderer's
 * reverb/release tails to bake into the 2nd iteration, which becomes the
 * loop region seen by the pipeline).  Non-looping tracks just run to their
 * natural end.  Ticks are capped at 800000 as a safety net.
 */

#include "../src/lds_play.h"
#include "../src/opl.h"
#include "../src/musmast.h"
#include "../src/opentyr.h"
#include "../src/file.h" // for fread_u16_die/fread_u32_die + fread_die/ftell_eof decls

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// ---------------------------------------------------------------------
// file.h stubs (same semantics as render_music.c; avoids linking file.c)
// ---------------------------------------------------------------------

int audioSampleRate = 44100; // needed at link time by opl_init() macro

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

// ---------------------------------------------------------------------
// Sample-accurate cadence math, identical to render_music.c / loudness.c
// ---------------------------------------------------------------------

static const int ldsUpdate2Rate = 139; // 69.5 * 2
static int samplesPerLdsUpdate;
static int samplesPerLdsUpdateFrac;

#define SAMPLE_RATE 44100
#define MAX_TICKS 800000

// 1 LDS sequencer tick == 1 MIDI tick (see write_midi_file()'s tempo meta
// event) == 113.669048/7900 s ~= 14.388 ms.
#define LDS_TICK_MS (113.669048 * 1000.0 / 7900.0)

// LDS_MIN_GATE_MS (env, float ms, default 0 = off): floor on emitted MIDI
// note gate length. Some OPL voices (e.g. fast chord stabs) fire note-offs
// as little as 20-30ms after note-on -- shorter than a sampled GM
// instrument's attack ramp, so they render as pitchless blips under
// fluidsynth. When set, note_off() delays the emitted note-off (not the
// .notes dump, which stays ground truth) so the *emitted* gate is at least
// this long. See note_on()'s pending-off clamp for how a same-pitch
// retrigger on the same channel is kept from being killed by a
// still-pending stretched note-off.
static double gMinGateMs = 0.0;

static int min_gate_ticks(void)
{
	if (gMinGateMs <= 0.0)
		return 0;
	return (int)ceil(gMinGateMs / LDS_TICK_MS);
}

// ---------------------------------------------------------------------
// OPL2 register tap: adlib_* implementation (no synthesis, just capture)
// ---------------------------------------------------------------------

static Bit8u opl_regs[256];
static int currentTick = 0; // updated by the sequencing loop before each opl_write

// Forward decl of the event sink defined further down.
static void on_reg_write(unsigned reg, unsigned val, int tick);

void adlib_init(Bit32u samplerate)
{
	(void)samplerate;
	memset(opl_regs, 0, sizeof opl_regs);
}

void adlib_write(Bitu idx, Bit8u val)
{
	unsigned reg = (unsigned)idx & 0xff;
	unsigned oldval = opl_regs[reg];
	opl_regs[reg] = val;
	if (oldval != val)
		on_reg_write(reg, val, currentTick);
}

void adlib_getsample(Bit16s *sndptr, Bits numsamples)
{
	// No synthesis -- this tool only taps register writes.
	if (sndptr != NULL && numsamples > 0)
		memset(sndptr, 0, (size_t)numsamples * sizeof(Bit16s));
}

Bitu adlib_reg_read(Bitu port)
{
	(void)port;
	return 0;
}

void adlib_write_index(Bitu port, Bit8u val)
{
	(void)port;
	(void)val;
}

// ---------------------------------------------------------------------
// MIDI event buffer
// ---------------------------------------------------------------------

typedef struct
{
	int tick;
	unsigned char status;   // includes channel nibble where applicable
	unsigned char data1;
	unsigned char data2;    // unused for 1-data-byte events (program change)
	int dataLen;             // 1 or 2 data bytes (status implies)
} MidiEvent;

static MidiEvent *events = NULL;
static size_t eventCount = 0, eventCap = 0;

static void push_event(int tick, unsigned char status, unsigned char d1, unsigned char d2, int dataLen)
{
	if (eventCount >= eventCap)
	{
		eventCap = eventCap ? eventCap * 2 : 4096;
		events = realloc(events, eventCap * sizeof(MidiEvent));
	}
	events[eventCount].tick = tick;
	events[eventCount].status = status;
	events[eventCount].data1 = d1;
	events[eventCount].data2 = d2;
	events[eventCount].dataLen = dataLen;
	eventCount++;
}

// ---------------------------------------------------------------------
// GM instrument map (tools/lds_gm_map.txt)
// ---------------------------------------------------------------------

typedef struct
{
	uint32_t fingerprint;
	int program;
	int transpose;
	int volume; // CC7 mix level, 0-127
	int cents;  // channel fine tuning (RPN 1), -99..99, 0 = none
	int reverb; // CC91 reverb send, 0-127; -1 = leave synth default (GM: 40)
	int chorus; // CC93 chorus send, 0-127; -1 = leave synth default
	int gatePct; // emitted gate as percent of the OPL gate, 10-100 (100 = as played)
} GmMapEntry;

static GmMapEntry *gmMap = NULL;
static int gmMapCount = 0;

static void load_gm_map(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return;

	char line[256];
	while (fgets(line, sizeof line, f) != NULL)
	{
		char *h = line;
		while (*h == ' ' || *h == '\t')
			h++;
		if (*h == '#' || *h == '\n' || *h == '\0')
			continue;

		unsigned fp;
		int prog, transpose = 0, volume = 100, cents = 0;
		int consumed = 0;
		int n = sscanf(h, "%x %d %d %d %d%n", &fp, &prog, &transpose, &volume, &cents, &consumed);
		if (n >= 2)
		{
			if (volume < 0) volume = 0;
			if (volume > 127) volume = 127;
			if (cents < -99) cents = -99;
			if (cents > 99) cents = 99;

			// Optional trailing key=value options after the positional
			// columns: rev=0..127 (CC91), cho=0..127 (CC93), gate=10..100
			// (emitted gate percent). Anything after '#' is a comment.
			int reverb = -1, chorus = -1, gatePct = 100;
			if (n == 5 && consumed > 0)
			{
				char *t = h + consumed;
				while (*t != '\0' && *t != '#' && *t != '\n')
				{
					while (*t == ' ' || *t == '\t')
						t++;
					char key[16];
					int val, used = 0;
					if (sscanf(t, "%15[a-z]=%d%n", key, &val, &used) == 2)
					{
						if (strcmp(key, "rev") == 0 && val >= 0 && val <= 127)
							reverb = val;
						else if (strcmp(key, "cho") == 0 && val >= 0 && val <= 127)
							chorus = val;
						else if (strcmp(key, "gate") == 0 && val >= 10 && val <= 100)
							gatePct = val;
						t += used;
					}
					else
						break;
				}
			}

			gmMap = realloc(gmMap, (gmMapCount + 1) * sizeof(GmMapEntry));
			gmMap[gmMapCount].fingerprint = (uint32_t)fp;
			gmMap[gmMapCount].program = prog;
			gmMap[gmMapCount].transpose = transpose;
			gmMap[gmMapCount].volume = volume;
			gmMap[gmMapCount].cents = cents;
			gmMap[gmMapCount].reverb = reverb;
			gmMap[gmMapCount].chorus = chorus;
			gmMap[gmMapCount].gatePct = gatePct;
			gmMapCount++;
		}
	}
	fclose(f);
}

static int lookup_gm_map(uint32_t fp, int *outTranspose, int *outVolume, int *outCents,
                         int *outReverb, int *outChorus, int *outGatePct)
{
	for (int i = 0; i < gmMapCount; ++i)
	{
		if (gmMap[i].fingerprint == fp)
		{
			*outTranspose = gmMap[i].transpose;
			*outVolume = gmMap[i].volume;
			*outCents = gmMap[i].cents;
			*outReverb = gmMap[i].reverb;
			*outChorus = gmMap[i].chorus;
			*outGatePct = gmMap[i].gatePct;
			return gmMap[i].program;
		}
	}
	*outTranspose = 0;
	*outVolume = 100;
	*outCents = 0;
	*outReverb = -1;
	*outChorus = -1;
	*outGatePct = 100;
	return -1; // unmapped
}

// ---------------------------------------------------------------------
// Patch-report bookkeeping
// ---------------------------------------------------------------------

typedef struct
{
	uint32_t fingerprint;
	int channels[9];
	int channelCount;
	int firstTick;
	int noteCount;
	int mappedProgram; // -1 if unmapped
	int minNote, maxNote;   // sounding MIDI note range
	long long totalGateTicks; // summed note-on..note-off durations
} PatchRecord;

static PatchRecord *patches = NULL;
static int patchCount = 0;

static PatchRecord *find_or_add_patch(uint32_t fp, int tick)
{
	for (int i = 0; i < patchCount; ++i)
		if (patches[i].fingerprint == fp)
			return &patches[i];

	patches = realloc(patches, (patchCount + 1) * sizeof(PatchRecord));
	PatchRecord *p = &patches[patchCount++];
	p->fingerprint = fp;
	p->channelCount = 0;
	p->firstTick = tick;
	p->noteCount = 0;
	p->mappedProgram = -1;
	p->minNote = 128;
	p->maxNote = -1;
	p->totalGateTicks = 0;
	return p;
}

static void patch_note_channel(PatchRecord *p, int ch)
{
	for (int i = 0; i < p->channelCount; ++i)
		if (p->channels[i] == ch)
			return;
	if (p->channelCount < 9)
		p->channels[p->channelCount++] = ch;
}

// ---------------------------------------------------------------------
// FNV-1a fingerprint over a voice's timbre-defining registers
// ---------------------------------------------------------------------

static const unsigned char op_table_local[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};

static uint32_t fingerprint_voice(int ch)
{
	unsigned mod = op_table_local[ch];
	unsigned car = op_table_local[ch] + 3;
	unsigned char bytes[11];
	unsigned char offs[5] = {0x20, 0x40, 0x60, 0x80, 0xe0};
	int i = 0;
	for (int k = 0; k < 5; ++k)
		bytes[i++] = opl_regs[offs[k] + mod];
	for (int k = 0; k < 5; ++k)
	{
		unsigned char v = opl_regs[offs[k] + car];
		// The carrier's 0x40 low 6 bits are total level -- the sequencer's
		// per-note *volume*, not timbre.  Hashing them would mint a new
		// "instrument" per volume step; keep only the KSL bits.
		if (offs[k] == 0x40)
			v &= 0xc0;
		bytes[i++] = v;
	}
	bytes[i++] = opl_regs[0xc0 + ch];

	uint32_t hash = 2166136261u;
	for (int k = 0; k < 11; ++k)
	{
		hash ^= bytes[k];
		hash *= 16777619u;
	}
	return hash;
}

// ---------------------------------------------------------------------
// Per-channel decode state
// ---------------------------------------------------------------------

typedef struct
{
	bool keyOn;
	int note;         // currently sounding MIDI note, -1 if none
	double freqHz;    // last computed exact frequency while key held
	double onHz;      // exact OPL2 frequency captured at NoteOn (pre-slide)
	unsigned onFnum, onBlock; // fnum/block captured at NoteOn (pre-slide)
	int bendValue;    // last emitted 14-bit bend value (8192 = center), -1 = none yet
	int expression;   // last emitted CC11 value, -1 = none yet
	uint32_t fingerprint; // current voice fingerprint, 0 = none yet
	int noteOnTick;   // tick of the current note's NoteOn (for gate stats)
	int transpose;
	int program;      // last emitted GM program, -1 = none yet
	int volume;       // last emitted CC7 value, -1 = none yet (synth default 100)
	int cents;        // last emitted RPN1 fine-tune value, CENTS_NONE = none yet
	int reverb;       // last emitted CC91 value, -1 = none yet (synth default)
	int chorus;       // last emitted CC93 value, -1 = none yet (synth default)
	int gatePct;      // current voice's emitted-gate percent (map gate=), 100 = as played
	bool used;        // has this MIDI channel had any event at all

	// LDS_MIN_GATE_MS bookkeeping: index (into events[]) of the most recent
	// note-off on this channel that was stretched past its natural tick,
	// -1 if none is outstanding. Used by note_on() to clamp a same-pitch
	// retrigger's pending stretched off so it can't land at/after the new
	// note-on and kill it.
	int pendingOffEventIndex;
	int pendingOffNote;      // note number of that pending off
	int pendingOffOnsetTick; // the note-on tick the pending off belongs to (clamp floor)
} ChanState;

static ChanState chans[9];

// Sentinel for ChanState.cents meaning "no RPN1 fine-tune emitted yet" --
// outside the valid -99..99 clamp range so it never collides with a real value.
#define CENTS_NONE (-1000)

// rhythm mode state (MIDI channel 9)
typedef struct
{
	bool on;
	int note;
} RhythmVoice;
static RhythmVoice rhythmVoices[5]; // HH, CY, TT, SD, BD (bits 0..4)
static const int rhythmNote[5] = {42, 49, 45, 38, 36}; // HH, CY, TT, SD, BD
static bool rhythmUsed = false;

// per-note log (for the .notes dump: voice-level piano-roll data)
typedef struct
{
	int on, off, note, ch;
	uint32_t fp;
	// TEST A (OPL->MIDI pitch verification): the *initial* note-on OPL2
	// frequency, captured before any vibrato/glide/arpeggio slide mutates
	// the fnum/block registers. onHz is the exact chip frequency computed
	// from (onFnum, onBlock) via the OPL2 formula; transpose is the
	// semitone offset applied to the emitted MIDI note (from lds_gm_map).
	double onHz;
	unsigned onFnum, onBlock;
	int transpose;
} NoteLog;
static NoteLog *noteLog = NULL;
static int noteLogCount = 0, noteLogCap = 0;

static void log_note(int on, int off, int note, int ch, uint32_t fp,
                     double onHz, unsigned onFnum, unsigned onBlock, int transpose)
{
	if (noteLogCount == noteLogCap)
	{
		noteLogCap = noteLogCap ? noteLogCap * 2 : 4096;
		noteLog = realloc(noteLog, noteLogCap * sizeof(NoteLog));
	}
	noteLog[noteLogCount++] = (NoteLog){on, off, note, ch, fp, onHz, onFnum, onBlock, transpose};
}

static int hz_to_midi_note(double hz)
{
	if (hz <= 0.0)
		return -1;
	double n = 69.0 + 12.0 * log2(hz / 440.0);
	int note = (int)lround(n);
	if (note < 0) note = 0;
	if (note > 127) note = 127;
	return note;
}

static double fnum_block_to_hz(unsigned fnum, unsigned block)
{
	return fnum * 49716.0 / (double)(1u << (20 - block));
}

static void get_freq(int ch, unsigned *outFnum, unsigned *outBlock, double *outHz)
{
	unsigned bReg = opl_regs[0xb0 + ch];
	unsigned aReg = opl_regs[0xa0 + ch];
	unsigned fnum = ((bReg & 0x3) << 8) | aReg;
	unsigned block = (bReg >> 2) & 0x7;
	*outFnum = fnum;
	*outBlock = block;
	*outHz = fnum_block_to_hz(fnum, block);
}

static int get_carrier_tl(int ch)
{
	unsigned car = op_table_local[ch] + 3;
	return opl_regs[0x40 + car] & 0x3f;
}

static void emit_program_change_if_needed(int ch, int tick)
{
	uint32_t fp = fingerprint_voice(ch);
	if (fp == chans[ch].fingerprint)
		return;

	chans[ch].fingerprint = fp;

	int transpose = 0, volume = 100, cents = 0;
	int reverb = -1, chorus = -1, gatePct = 100;
	int prog = lookup_gm_map(fp, &transpose, &volume, &cents, &reverb, &chorus, &gatePct);
	chans[ch].transpose = transpose;
	chans[ch].gatePct = gatePct;

	PatchRecord *p = find_or_add_patch(fp, tick);
	patch_note_channel(p, ch);
	p->mappedProgram = prog; // -1 unmapped; last-seen mapping recorded

	int emitProg = (prog >= 0) ? prog : 0;
	if (emitProg != chans[ch].program)
	{
		push_event(tick, 0xC0 | ch, (unsigned char)emitProg, 0, 1);
		chans[ch].program = emitProg;
	}
	if (volume != chans[ch].volume)
	{
		push_event(tick, 0xB0 | ch, 7, (unsigned char)volume, 2);
		chans[ch].volume = volume;
	}
	if (reverb >= 0 && reverb != chans[ch].reverb)
	{
		push_event(tick, 0xB0 | ch, 91, (unsigned char)reverb, 2);
		chans[ch].reverb = reverb;
	}
	if (chorus >= 0 && chorus != chans[ch].chorus)
	{
		push_event(tick, 0xB0 | ch, 93, (unsigned char)chorus, 2);
		chans[ch].chorus = chorus;
	}
	if (cents != chans[ch].cents)
	{
		// RPN 1 (channel fine tuning): 14-bit value, 8192 = center (0 cents),
		// full range is +-100 cents -> value = 8192 + round(cents*8192/100).
		int value = 8192 + (int)lround(cents * 8192.0 / 100.0);
		if (value < 0) value = 0;
		if (value > 16383) value = 16383;
		unsigned char msb = (unsigned char)((value >> 7) & 0x7f);
		unsigned char lsb = (unsigned char)(value & 0x7f);
		push_event(tick, 0xB0 | ch, 101, 0, 2);   // RPN MSB = 0
		push_event(tick, 0xB0 | ch, 100, 1, 2);   // RPN LSB = 1 (fine tuning)
		push_event(tick, 0xB0 | ch, 6, msb, 2);   // Data Entry MSB
		push_event(tick, 0xB0 | ch, 38, lsb, 2);  // Data Entry LSB
		push_event(tick, 0xB0 | ch, 101, 127, 2); // RPN null (deselect)
		push_event(tick, 0xB0 | ch, 100, 127, 2);
		chans[ch].cents = cents;
	}
}

static void note_off(int ch, int tick)
{
	if (chans[ch].note >= 0)
	{
		int noteOnTick = chans[ch].noteOnTick;
		int emitTick = tick;

		// Per-voice gate shortening (map gate=NN percent), applied first.
		if (chans[ch].gatePct < 100)
		{
			int gate = (tick - noteOnTick) * chans[ch].gatePct / 100;
			if (gate < 1)
				gate = 1;
			emitTick = noteOnTick + gate;
		}

		int floorTicks = min_gate_ticks();
		if (floorTicks > 0 && emitTick - noteOnTick < floorTicks)
			emitTick = noteOnTick + floorTicks;

		push_event(emitTick, 0x80 | ch, (unsigned char)chans[ch].note, 64, 2);

		if (emitTick > tick)
		{
			// Stretched past the natural gate: remember it so a same-pitch
			// retrigger on this channel can pull it back in note_on().
			chans[ch].pendingOffEventIndex = (int)(eventCount - 1);
			chans[ch].pendingOffNote = chans[ch].note;
			chans[ch].pendingOffOnsetTick = noteOnTick;
		}
		else
		{
			chans[ch].pendingOffEventIndex = -1;
		}

		if (chans[ch].fingerprint != 0)
		{
			PatchRecord *p = find_or_add_patch(chans[ch].fingerprint, tick);
			p->totalGateTicks += tick - chans[ch].noteOnTick;
		}
		// .notes dump keeps reporting the ORIGINAL (unstretched) gate --
		// it's ground truth for analysis/validation, independent of
		// LDS_MIN_GATE_MS; only the emitted MIDI event above stretches.
		log_note(chans[ch].noteOnTick, tick, chans[ch].note, ch, chans[ch].fingerprint,
		         chans[ch].onHz, chans[ch].onFnum, chans[ch].onBlock, chans[ch].transpose);
		chans[ch].note = -1;
	}
	chans[ch].keyOn = false;
	chans[ch].bendValue = -1;
}

static void note_on(int ch, int tick)
{
	unsigned fnum, block;
	double hz;
	get_freq(ch, &fnum, &block, &hz);

	emit_program_change_if_needed(ch, tick);

	int note = hz_to_midi_note(hz) + chans[ch].transpose;
	if (note < 0) note = 0;
	if (note > 127) note = 127;

	// A gate-floor-stretched note-off from this channel's previous note may
	// still be pending (scheduled for a future tick). If this retrigger is
	// the SAME pitch, letting that stretched off fire at/after this note-on
	// would immediately kill the new note; clamp it to just before this
	// note-on instead. Different pitches overlapping on the same channel are
	// ordinary legato/polyphony and are left alone.
	if (chans[ch].pendingOffEventIndex >= 0 && chans[ch].pendingOffNote == note
	    && events[chans[ch].pendingOffEventIndex].tick >= tick)
	{
		int clamped = tick - 1;
		if (clamped < chans[ch].pendingOffOnsetTick)
			clamped = chans[ch].pendingOffOnsetTick;
		events[chans[ch].pendingOffEventIndex].tick = clamped;
		chans[ch].pendingOffEventIndex = -1;
	}

	int tl = get_carrier_tl(ch);
	int vel = 127 - tl * 2;
	if (vel < 1) vel = 1;
	if (vel > 127) vel = 127;

	push_event(tick, 0x90 | ch, (unsigned char)note, (unsigned char)vel, 2);

	chans[ch].keyOn = true;
	chans[ch].note = note;
	chans[ch].freqHz = hz;
	chans[ch].onHz = hz;       // TEST A: exact note-on frequency, pre-slide
	chans[ch].onFnum = fnum;
	chans[ch].onBlock = block;
	chans[ch].bendValue = 8192; // center, implicit
	chans[ch].used = true;

	PatchRecord *p = find_or_add_patch(chans[ch].fingerprint, tick);
	p->noteCount++;
	if (note < p->minNote) p->minNote = note;
	if (note > p->maxNote) p->maxNote = note;
	chans[ch].noteOnTick = tick;
}

static void emit_rpn_bend_range(int ch, int tick)
{
	// RPN 0 (pitch bend range), set to 2 semitones / 0 cents.
	push_event(tick, 0xB0 | ch, 101, 0, 2); // RPN MSB
	push_event(tick, 0xB0 | ch, 100, 0, 2); // RPN LSB
	push_event(tick, 0xB0 | ch, 6, 2, 2);   // Data Entry MSB = 2 semitones
	push_event(tick, 0xB0 | ch, 38, 0, 2);  // Data Entry LSB = 0 cents
	push_event(tick, 0xB0 | ch, 101, 127, 2); // RPN null (deselect)
	push_event(tick, 0xB0 | ch, 100, 127, 2);
}

static void handle_freq_write_while_held(int ch, int tick)
{
	unsigned fnum, block;
	double hz;
	get_freq(ch, &fnum, &block, &hz);

	// Reference the *original* (untransposed) sounding pitch: recompute
	// what MIDI note (untransposed) corresponds to the currently-held note.
	int soundingNoteUntransposed = chans[ch].note - chans[ch].transpose;
	double refHz = 440.0 * pow(2.0, (soundingNoteUntransposed - 69) / 12.0);
	double semitoneDelta = 12.0 * log2(hz / refHz);

	if (semitoneDelta > -2.0 && semitoneDelta < 2.0)
	{
		// Pitch bend, range assumed +-2 semitones (RPN0 set at track start).
		int bend = 8192 + (int)lround((semitoneDelta / 2.0) * 8192.0);
		if (bend < 0) bend = 0;
		if (bend > 16383) bend = 16383;

		if (bend != chans[ch].bendValue)
		{
			push_event(tick, 0xE0 | ch, (unsigned char)(bend & 0x7f), (unsigned char)((bend >> 7) & 0x7f), 2);
			chans[ch].bendValue = bend;
		}
		chans[ch].freqHz = hz;
	}
	else
	{
		// Retrigger: NoteOff + NoteOn at the new pitch.
		note_off(ch, tick);
		note_on(ch, tick);
	}
}

static void handle_tl_write_while_held(int ch, int tick)
{
	int tl = get_carrier_tl(ch);
	int expr = 127 - tl * 2;
	if (expr < 0) expr = 0;
	if (expr > 127) expr = 127;

	if (expr != chans[ch].expression)
	{
		push_event(tick, 0xB0 | ch, 11, (unsigned char)expr, 2);
		chans[ch].expression = expr;
	}
}

static void rhythm_note_on_off(int bit, bool on, int tick)
{
	if (on)
	{
		if (!rhythmVoices[bit].on)
		{
			push_event(tick, 0x99, (unsigned char)rhythmNote[bit], 100, 2); // channel 9 note on
			rhythmVoices[bit].on = true;
			rhythmVoices[bit].note = rhythmNote[bit];
			rhythmUsed = true;
		}
	}
	else
	{
		if (rhythmVoices[bit].on)
		{
			push_event(tick, 0x89, (unsigned char)rhythmVoices[bit].note, 64, 2);
			rhythmVoices[bit].on = false;
		}
	}
}

// ---------------------------------------------------------------------
// Register write dispatch
// ---------------------------------------------------------------------

static void on_reg_write(unsigned reg, unsigned val, int tick)
{
	(void)val;

	if (reg == 0xbd)
	{
		bool rhythmMode = (opl_regs[0xbd] & 0x20) != 0;
		if (!rhythmMode)
		{
			// leaving rhythm mode (or bit changes with rhythm off): release
			// any still-sounding percussion voices.
			for (int b = 0; b < 5; ++b)
				rhythm_note_on_off(b, false, tick);
			return;
		}
		for (int b = 0; b < 5; ++b)
		{
			bool bitSet = (opl_regs[0xbd] & (1u << b)) != 0;
			rhythm_note_on_off(b, bitSet, tick);
		}
		return;
	}

	if (reg >= 0xb0 && reg <= 0xb8)
	{
		int ch = reg - 0xb0;
		bool keyBit = (opl_regs[reg] & 0x20) != 0;
		if (keyBit && !chans[ch].keyOn)
		{
			note_on(ch, tick);
		}
		else if (!keyBit && chans[ch].keyOn)
		{
			note_off(ch, tick);
		}
		else if (keyBit && chans[ch].keyOn)
		{
			// block/fnum-high changed while held
			handle_freq_write_while_held(ch, tick);
		}
		return;
	}

	if (reg >= 0xa0 && reg <= 0xa8)
	{
		int ch = reg - 0xa0;
		if (chans[ch].keyOn)
			handle_freq_write_while_held(ch, tick);
		return;
	}

	// Carrier total-level write while a note is held -> expression.
	for (int ch = 0; ch < 9; ++ch)
	{
		unsigned car = op_table_local[ch] + 3;
		if (reg == 0x40 + car)
		{
			if (chans[ch].keyOn)
				handle_tl_write_while_held(ch, tick);
			return;
		}
	}
}

// ---------------------------------------------------------------------
// SMF writer
// ---------------------------------------------------------------------

static void write_u32be(FILE *f, uint32_t v)
{
	fputc((v >> 24) & 0xff, f);
	fputc((v >> 16) & 0xff, f);
	fputc((v >> 8) & 0xff, f);
	fputc(v & 0xff, f);
}

static void write_u16be(FILE *f, uint16_t v)
{
	fputc((v >> 8) & 0xff, f);
	fputc(v & 0xff, f);
}

static void write_midi_file(const char *path, int lastTick)
{
	// Stable sort by tick (insertion sort on top of qsort's tick-only
	// comparison would reorder same-tick events; use a stable merge via
	// index-preserving decoration instead).
	typedef struct { MidiEvent ev; size_t origIndex; } Decorated;
	Decorated *dec = malloc(eventCount * sizeof(Decorated));
	for (size_t i = 0; i < eventCount; ++i)
	{
		dec[i].ev = events[i];
		dec[i].origIndex = i;
	}
	// simple stable sort (insertion sort is fine; event counts are modest,
	// but guard with a merge sort style approach for larger files)
	for (size_t i = 1; i < eventCount; ++i)
	{
		Decorated key = dec[i];
		size_t j = i;
		while (j > 0 && (dec[j-1].ev.tick > key.ev.tick))
		{
			dec[j] = dec[j-1];
			j--;
		}
		dec[j] = key;
	}

	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		fprintf(stderr, "error: failed to open '%s' for writing\n", path);
		exit(1);
	}

	// Header chunk
	fwrite("MThd", 1, 4, f);
	write_u32be(f, 6);
	write_u16be(f, 0);   // format 0
	write_u16be(f, 1);   // 1 track
	write_u16be(f, 139); // division: 139 ticks/quarter

	// Track chunk: buffer to a memory stream so we can backpatch length.
	// (Simplify: write to a temp buffer via a dynamically grown array.)
	unsigned char *trackBuf = NULL;
	size_t trackLen = 0, trackCap = 0;
#define TRACK_PUT(byteval) do { \
		if (trackLen >= trackCap) { trackCap = trackCap ? trackCap*2 : 4096; trackBuf = realloc(trackBuf, trackCap); } \
		trackBuf[trackLen++] = (unsigned char)(byteval); \
	} while (0)
#define TRACK_VLQ(val) do { \
		unsigned long _v = (val); unsigned char _b[4]; int _l = 0; \
		_b[_l++] = _v & 0x7f; _v >>= 7; \
		while (_v > 0) { _b[_l++] = 0x80 | (_v & 0x7f); _v >>= 7; } \
		for (int _i = _l - 1; _i >= 0; --_i) TRACK_PUT(_b[_i]); \
	} while (0)

	// Tempo meta event at tick 0: 2,000,000 us/quarter -> 69.5 ticks/sec
	// (division=139 ticks/quarter / (60/tempo_qpm) ... see header comment;
	// this makes 1 MIDI tick == 1 LDS sequencer tick exactly).
	TRACK_VLQ(0);
	TRACK_PUT(0xFF); TRACK_PUT(0x51); TRACK_PUT(0x03);
	TRACK_PUT((2000000 >> 16) & 0xff);
	TRACK_PUT((2000000 >> 8) & 0xff);
	TRACK_PUT(2000000 & 0xff);

	int lastEventTick = 0;
	for (size_t i = 0; i < eventCount; ++i)
	{
		MidiEvent *e = &dec[i].ev;
		int delta = e->tick - lastEventTick;
		if (delta < 0) delta = 0;
		TRACK_VLQ((unsigned long)delta);
		lastEventTick = e->tick;

		TRACK_PUT(e->status);
		TRACK_PUT(e->data1);
		if (e->dataLen == 2)
			TRACK_PUT(e->data2);
	}

	// All-notes-off (CC123) on every channel that had activity, then EOT.
	int finalDelta = lastTick - lastEventTick;
	if (finalDelta < 0) finalDelta = 0;
	bool first = true;
	for (int ch = 0; ch < 9; ++ch)
	{
		if (chans[ch].used)
		{
			TRACK_VLQ(first ? (unsigned long)finalDelta : 0);
			first = false;
			TRACK_PUT(0xB0 | ch);
			TRACK_PUT(123);
			TRACK_PUT(0);
		}
	}
	if (rhythmUsed)
	{
		TRACK_VLQ(first ? (unsigned long)finalDelta : 0);
		first = false;
		TRACK_PUT(0xB9);
		TRACK_PUT(123);
		TRACK_PUT(0);
	}

	TRACK_VLQ(0);
	TRACK_PUT(0xFF); TRACK_PUT(0x2F); TRACK_PUT(0x00);

	fwrite("MTrk", 1, 4, f);
	write_u32be(f, (uint32_t)trackLen);
	fwrite(trackBuf, 1, trackLen, f);

	free(trackBuf);
	free(dec);
	fclose(f);
#undef TRACK_PUT
#undef TRACK_VLQ
}

// ---------------------------------------------------------------------
// Per-track conversion (mirrors render_music.c's render_track structure)
// ---------------------------------------------------------------------

typedef struct
{
	unsigned int position;
	int tick;
	long long sample_offset;
} TickMark;

static void reset_decode_state(void)
{
	memset(chans, 0, sizeof chans);
	for (int i = 0; i < 9; ++i)
	{
		chans[i].note = -1;
		chans[i].bendValue = -1;
		chans[i].expression = -1;
		chans[i].program = -1;
		chans[i].volume = -1;
		chans[i].cents = CENTS_NONE;
		chans[i].reverb = -1;
		chans[i].chorus = -1;
		chans[i].gatePct = 100;
		chans[i].fingerprint = 0;
		chans[i].pendingOffEventIndex = -1;
	}
	memset(rhythmVoices, 0, sizeof rhythmVoices);
	rhythmUsed = false;
	noteLogCount = 0;

	eventCount = 0;
	patchCount = 0;
}

static void convert_track(FILE *musicFile, unsigned int songOffset, unsigned int songSize,
                           int *outLastTick,
                           long long *outLoopStartTicks, long long *outLoopLenTicks,
                           long long *outLoopStartSamples, long long *outLoopLenSamples,
                           long long *outTotalSamples,
                           long long *outValidateLoopStartSamples, long long *outValidateLoopLenSamples)
{
	reset_decode_state();

	if (!lds_load(musicFile, songOffset, songSize))
	{
		fprintf(stderr, "error: lds_load failed\n");
		exit(1);
	}

	TickMark *marks = malloc(MAX_TICKS * sizeof(TickMark));
	int markCount = 0;

	int samplesUntilLdsUpdate = 0;
	int samplesUntilLdsUpdateFrac = 0;
	long long sampleOffset = 0;

	long long loopStartTick = -1, loopEndTick = -1;
	long long loopStartSample = -1, loopEndSample = -1;
	bool wasLooped = false;
	long long extraTicksTarget = -1; // tick at which to stop after loop detected

	int tick = 0;
	for (; tick < MAX_TICKS; ++tick)
	{
		if (!playing)
			break;

		currentTick = tick;

		unsigned int posBefore = lds_get_position();
		if (markCount < MAX_TICKS)
		{
			marks[markCount].position = posBefore;
			marks[markCount].tick = tick;
			marks[markCount].sample_offset = sampleOffset;
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
		sampleOffset += count;

		if (songlooped && !wasLooped)
		{
			wasLooped = true;
			loopEndTick = tick + 1; // tick just completed is the last pre-loop tick
			loopEndSample = sampleOffset;

			unsigned int targetPos = lds_get_position();
			for (int i = 0; i < markCount; ++i)
			{
				if (marks[i].position == targetPos)
				{
					loopStartTick = marks[i].tick;
					loopStartSample = marks[i].sample_offset;
					break;
				}
			}

			if (loopStartTick >= 0)
			{
				long long loopLenTicks = loopEndTick - loopStartTick;
				extraTicksTarget = loopEndTick + loopLenTicks; // run one more full iteration
			}
			else
			{
				// Couldn't map the loop target to a seen tick; stop now.
				extraTicksTarget = loopEndTick;
			}
		}

		if (wasLooped && (long long)(tick + 1) >= extraTicksTarget)
		{
			tick = tick + 1; // account for the tick we just finished
			break;
		}
	}

	free(marks);

	int lastTick = tick;
	*outLastTick = lastTick;

	if (wasLooped && loopStartTick >= 0)
	{
		long long loopLenTicks = loopEndTick - loopStartTick;
		long long loopStart2Tick = loopStartTick + loopLenTicks; // 2nd iteration's loop point
		*outLoopStartTicks = loopStart2Tick;
		*outLoopLenTicks = loopLenTicks;

		// loopStartSample/loopEndSample bracket the FIRST iteration
		// (identical semantics to render_music.c's loopStart/loopEnd --
		// this is what's diffed against the shipped hdmusic_NN.ogg's
		// LOOPSTART/LOOPLENGTH Vorbis comments as a math sanity check).
		long long loopLenSamples = loopEndSample - loopStartSample;
		*outValidateLoopStartSamples = loopStartSample;
		*outValidateLoopLenSamples = loopLenSamples;

		// The MIDI covers intro + 2 loop iterations, so the loop region for
		// playback purposes is the SECOND iteration. Since the sequencer is
		// deterministic (same tick content -> same sample cadence), the 2nd
		// iteration starts exactly where the 1st iteration's loop-back
		// content resumes, i.e. at loopEndSample, and has the same length.
		*outLoopStartSamples = loopEndSample;             // offset of tick loopStart2Tick
		*outLoopLenSamples = loopLenSamples;               // same duration both iterations
		*outTotalSamples = loopEndSample + loopLenSamples; // end of 2nd iteration
	}
	else
	{
		*outLoopStartTicks = -1;
		*outLoopLenTicks = -1;
		*outLoopStartSamples = -1;
		*outLoopLenSamples = -1;
		*outTotalSamples = sampleOffset;
		*outValidateLoopStartSamples = -1;
		*outValidateLoopLenSamples = -1;
	}
}

// ---------------------------------------------------------------------
// Report + main
// ---------------------------------------------------------------------

static void write_patches_report(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return;

	fprintf(f, "# fingerprint  channels          first_tick  notes  note_range  avg_gate_ticks  mapped\n");
	for (int i = 0; i < patchCount; ++i)
	{
		PatchRecord *p = &patches[i];
		char chanbuf[64] = {0};
		int off = 0;
		for (int c = 0; c < p->channelCount; ++c)
			off += snprintf(chanbuf + off, sizeof(chanbuf) - off, "%s%d", c ? "," : "", p->channels[c]);

		char rangebuf[16] = "-";
		if (p->maxNote >= 0)
			snprintf(rangebuf, sizeof(rangebuf), "%d..%d", p->minNote, p->maxNote);
		double avgGate = (p->noteCount > 0) ? (double)p->totalGateTicks / p->noteCount : 0.0;

		if (p->mappedProgram >= 0)
			fprintf(f, "%08x  ch=%-14s  tick=%-8d  notes=%-5d  range=%-8s  gate=%-6.1f  mapped -> GM %d\n",
			        p->fingerprint, chanbuf, p->firstTick, p->noteCount, rangebuf, avgGate, p->mappedProgram);
		else
			fprintf(f, "%08x  ch=%-14s  tick=%-8d  notes=%-5d  range=%-8s  gate=%-6.1f  UNMAPPED (defaults to GM 0)\n",
			        p->fingerprint, chanbuf, p->firstTick, p->noteCount, rangebuf, avgGate);
	}
	fclose(f);
}

static void write_notes_dump(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return;

	// Columns 1-5 are the original piano-roll format (unchanged, so any
	// existing consumer keeps working). Columns 6-9 are the TEST A pitch-
	// verification data: opl_hz is the exact OPL2 chip frequency at the
	// initial note-on (freq = fnum * 49716 / 2^(20-block)); fnum/block are
	// the raw registers; transpose is the semitone offset lds_gm_map added
	// to the emitted midi_note. A faithful converter has
	// |12*log2(opl_hz / (440*2^((midi_note-transpose-69)/12)))| <= 0.5.
	fprintf(f, "# on_tick off_tick midi_note opl_channel fingerprint opl_hz fnum block transpose  (1 tick ~ 14.4 ms)\n");
	for (int i = 0; i < noteLogCount; ++i)
		fprintf(f, "%d %d %d %d %08x %.4f %u %u %d\n",
		        noteLog[i].on, noteLog[i].off, noteLog[i].note, noteLog[i].ch, noteLog[i].fp,
		        noteLog[i].onHz, noteLog[i].onFnum, noteLog[i].onBlock, noteLog[i].transpose);
	fclose(f);
}

static void emit_bend_range_setup(void)
{
	for (int ch = 0; ch < 9; ++ch)
		emit_rpn_bend_range(ch, 0);
}

int main(int argc, char **argv)
{
	const char *dataDir = (argc > 1) ? argv[1] : "tyrian21";
	const char *outDir = (argc > 2) ? argv[2] : ".";
	int onlySong = (argc > 3) ? atoi(argv[3]) : -1; // 1-based

	samplesPerLdsUpdate = 2 * (SAMPLE_RATE / ldsUpdate2Rate);
	samplesPerLdsUpdateFrac = 2 * (SAMPLE_RATE % ldsUpdate2Rate);

	const char *minGateEnv = getenv("LDS_MIN_GATE_MS");
	if (minGateEnv != NULL)
	{
		double v = atof(minGateEnv);
		if (v > 0.0)
			gMinGateMs = v;
	}

	// LDS_GM_MAP env overrides the mapping file entirely (used for stem
	// renders); otherwise try a couple of reasonable locations.
	const char *mapOverride = getenv("LDS_GM_MAP");
	if (mapOverride != NULL)
		load_gm_map(mapOverride);
	else
	{
		load_gm_map("tools/lds_gm_map.txt");
		load_gm_map("lds_gm_map.txt");
	}

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
		if (onlySong >= 0 && (int)(n + 1) != onlySong)
			continue;

		unsigned int size = songOffset[n + 1] - songOffset[n];

		// Emit RPN bend-range=2 setup on every channel at tick 0, before
		// sequencing begins, so events[] starts with these.
		reset_decode_state();
		emit_bend_range_setup();

		int lastTick;
		long long loopStartTicks, loopLenTicks, loopStartSamples, loopLenSamples, totalSamples;
		long long validateLoopStartSamples, validateLoopLenSamples;

		// convert_track() calls reset_decode_state() again internally,
		// which would wipe the bend-range events we just queued. Capture
		// them first, then re-inject after reset.
		MidiEvent *bendSetup = malloc(eventCount * sizeof(MidiEvent));
		size_t bendSetupCount = eventCount;
		memcpy(bendSetup, events, eventCount * sizeof(MidiEvent));

		convert_track(musicFile, songOffset[n], size, &lastTick,
		              &loopStartTicks, &loopLenTicks,
		              &loopStartSamples, &loopLenSamples, &totalSamples,
		              &validateLoopStartSamples, &validateLoopLenSamples);

		// Re-inject the bend-range setup events at the front (tick 0).
		size_t newTotal = bendSetupCount + eventCount;
		MidiEvent *merged = malloc(newTotal * sizeof(MidiEvent));
		memcpy(merged, bendSetup, bendSetupCount * sizeof(MidiEvent));
		memcpy(merged + bendSetupCount, events, eventCount * sizeof(MidiEvent));
		free(bendSetup);
		free(events);
		events = merged;
		eventCount = newTotal;
		eventCap = newTotal;

		char midPath[4096], loopinfoPath[4096], patchesPath[4096], notesPath[4096];
		snprintf(midPath, sizeof midPath, "%s/hdmusic_%02u.mid", outDir, n + 1);
		snprintf(loopinfoPath, sizeof loopinfoPath, "%s/hdmusic_%02u.loopinfo", outDir, n + 1);
		snprintf(patchesPath, sizeof patchesPath, "%s/hdmusic_%02u.patches", outDir, n + 1);
		snprintf(notesPath, sizeof notesPath, "%s/hdmusic_%02u.notes", outDir, n + 1);

		write_midi_file(midPath, lastTick);
		write_patches_report(patchesPath);
		write_notes_dump(notesPath);

		if (loopStartTicks >= 0)
		{
			FILE *lf = fopen(loopinfoPath, "w");
			if (lf != NULL)
			{
				fprintf(lf, "LOOPSTART_TICKS %lld\n", loopStartTicks);
				fprintf(lf, "LOOPLENGTH_TICKS %lld\n", loopLenTicks);
				fprintf(lf, "LOOPSTART %lld\n", loopStartSamples);
				fprintf(lf, "LOOPLENGTH %lld\n", loopLenSamples);
				fprintf(lf, "TOTALSAMPLES %lld\n", totalSamples);
				// First-iteration numbers, directly comparable to
				// render_music.c's output and the shipped hdmusic_NN.ogg's
				// LOOPSTART/LOOPLENGTH Vorbis comments (math sanity check).
				fprintf(lf, "VALIDATE_LOOPSTART %lld\n", validateLoopStartSamples);
				fprintf(lf, "VALIDATE_LOOPLENGTH %lld\n", validateLoopLenSamples);
				fclose(lf);
			}
		}

		printf("track %2u: %-40s %8d ticks, %5zu events, %3d patches%s\n",
		       n + 1,
		       (n < MUSIC_NUM) ? musicTitle[n] : "?",
		       lastTick, eventCount, patchCount,
		       (loopStartTicks >= 0) ? "" : "  [no loop]");
		if (loopStartTicks >= 0)
		{
			printf("           loop: start_tick=%lld len_ticks=%lld  start_samp=%lld len_samp=%lld total_samp=%lld\n",
			       loopStartTicks, loopLenTicks, loopStartSamples, loopLenSamples, totalSamples);
			printf("           validate (1st iter, compare vs shipped ogg): LOOPSTART=%lld LOOPLENGTH=%lld\n",
			       validateLoopStartSamples, validateLoopLenSamples);
		}
	}

	free(songOffset);
	fclose(musicFile);

	lds_free();

	free(events);
	free(patches);
	free(gmMap);

	return 0;
}
