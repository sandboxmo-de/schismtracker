/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
 * URL: http://schismtracker.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* The effects voice pool: instruments and samples played as sound effects
 * through the same per-voice code the song uses (envelopes, fadeout,
 * auto-vibrato, sustain loops, resonant filters, interpolation), in a pool
 * of voices that belongs to the host instead of the song. The song can't
 * cut them off and they never see the song's global volume, tempo or
 * effects. See docs/sfx-voices.md. */

#ifndef SCHISM_SFX_VOICES_H_
#define SCHISM_SFX_VOICES_H_

#include "headers.h"

typedef struct sfx_bank sfx_bank_t;

/* A playing note. The low 16 bits are the voice slot plus one, the high 16
 * bits a generation count, so a handle to a note that has ended stays
 * invalid after the slot is reused (until the count wraps). */
typedef uint32_t sfx_note_t;
#define SFX_NOTE_NONE 0

/* the largest pool sfx_pool_init() accepts */
#define SFX_POOL_MAX_VOICES 65535

/* for any int field of struct sfx_note_args and the setters: use the
 * instrument's or sample's own value */
#define SFX_DEFAULT (-1)

/* what sfx_note_on does when every voice is busy */
enum {
	SFX_STEAL_NONE,     /* fail */
	SFX_STEAL_OLDEST,   /* cut the note that started first */
	SFX_STEAL_QUIETEST, /* cut the quietest note that has started */
};

/* ------------------------------------------------------------------------ */
/* the pool */

/* Set up the pool with max_voices voices (1 to SFX_POOL_MAX_VOICES), all
 * allocated now. Returns 0, or -1 if it's already running, the count is out
 * of range or memory ran out. */
int sfx_pool_init(uint32_t max_voices);
/* Stop everything and free the voices. Banks aren't freed. */
void sfx_pool_quit(void);
int sfx_pool_running(void);
uint32_t sfx_pool_max_voices(void);
/* notes playing or waiting to start */
uint32_t sfx_pool_voices_playing(void);
/* cut every note at once */
void sfx_pool_stop_all(void);

void sfx_pool_set_steal(int policy);
int sfx_pool_get_steal(void);
/* The pool's tick rate, in IT tempo units: tempo 125 (the default) is 50
 * ticks a second. Envelopes, fadeout and auto-vibrato advance once a tick.
 * 1 to 10000. */
void sfx_pool_set_tick_rate(uint32_t tempo);
uint32_t sfx_pool_get_tick_rate(void);
/* Gain for the whole pool, linear, applied after mixing (default 1). */
void sfx_pool_set_volume(float volume);
float sfx_pool_get_volume(void);
/* The pool's own mixing volume, 0-128, which scales every voice the way a
 * song's mixing volume scales the song's. The default is 48, Impulse
 * Tracker's default, so a note comes out at the level the same instrument
 * would have in a new song. */
void sfx_pool_set_mixing_volume(int volume);
/* SRCMODE_NEAREST/LINEAR/SPLINE/POLYPHASE, or -1 (the default) to follow
 * the engine's interpolation setting. */
void sfx_pool_set_interpolation(int mode);
/* The pool has its own random numbers (volume/pan swing, random vibrato),
 * so effects don't change what the song's random numbers come out as. */
void sfx_pool_set_seed(uint32_t seed);

/* ------------------------------------------------------------------------ */
/* banks */

/* Load a bank: a module (every sample and, in instrument mode, every
 * instrument), an instrument file (.iti, .xi, ...: instrument 1) or a
 * sample file (.its, .wav, ...: sample 1, no instruments). The data is
 * copied into ENGINE memory and doesn't depend on the song. NULL on
 * failure. Call from the host thread, not with the audio lock held. */
sfx_bank_t *sfx_bank_load(const char *filename);
/* a copy of the current song's samples and instruments */
sfx_bank_t *sfx_bank_from_song(void);
/* Cuts any note playing from the bank, then frees it. */
void sfx_bank_free(sfx_bank_t *bank);
int sfx_bank_num_instruments(const sfx_bank_t *bank);
int sfx_bank_num_samples(const sfx_bank_t *bank);

/* ------------------------------------------------------------------------ */
/* notes */

struct sfx_note_args {
	int instrument;  /* 1-based; 0 plays .sample with no instrument */
	int sample;      /* 1-based, used when instrument is 0 */
	int note;        /* 1-120, middle C (C-5) is 61 */
	int volume;      /* 0-64, or SFX_DEFAULT for the sample's volume */
	int pan;         /* 0 (left) - 64 (right), or SFX_DEFAULT */
	int cutoff;      /* filter cutoff 0-127, or SFX_DEFAULT for the instrument's */
	int resonance;   /* 0-127, or SFX_DEFAULT */
	uint32_t delay;    /* output frames to wait before starting */
	uint32_t duration; /* frames from the start to the note-off; 0 for none */
};

/* fills in a C-5 at default volume, pan and filter, starting now */
void sfx_note_args_init(struct sfx_note_args *args);

/* Start a note. Returns SFX_NOTE_NONE if the pool isn't running, the
 * instrument or sample doesn't exist or has nothing mapped to the note, or
 * every voice is busy and the steal policy is SFX_STEAL_NONE. */
sfx_note_t sfx_note_on_ex(sfx_bank_t *bank, const struct sfx_note_args *args);
/* index is an instrument if the bank has instruments, otherwise a sample */
sfx_note_t sfx_note_on(sfx_bank_t *bank, int index, int note, int volume, int pan);

/* Key off: releases sustain loops (sample and envelope), the envelopes run
 * on and the instrument fades out as it would in a song. */
void sfx_note_off(sfx_note_t note);
/* Stop the note with a short ramp, like ^^^ in a pattern. */
void sfx_note_cut(sfx_note_t note);
int sfx_note_playing(sfx_note_t note);

/* These do nothing for a note that has ended. Volume and pan are 0-64; the
 * pitch is a multiplier on the note's frequency (1 is the note itself).
 * Changes take effect on the voice's next tick. */
void sfx_note_set_volume(sfx_note_t note, int volume);
void sfx_note_set_pan(sfx_note_t note, int pan);
void sfx_note_set_pitch(sfx_note_t note, float ratio);
void sfx_note_set_cutoff(sfx_note_t note, int cutoff);
void sfx_note_set_resonance(sfx_note_t note, int resonance);

struct sfx_note_info {
	int waiting;          /* hasn't reached its start yet */
	int released;         /* note-off has happened */
	int fading;           /* the instrument fadeout is running */
	int32_t final_volume; /* 0-16384, after envelopes and fadeout */
	int32_t final_panning; /* 0-256 */
	uint32_t position;    /* in sample frames */
};
/* 0 if the note has ended */
int sfx_note_get_info(sfx_note_t note, struct sfx_note_info *info);

/* ------------------------------------------------------------------------ */
/* for the engine's mixer; hosts don't need these */

struct sfx_pool;
struct song;

/* the running pool, or NULL */
struct sfx_pool *sfx_pool_get(void);
/* nonzero if any note is playing or waiting, or a stopped one is still
 * ramping out; call with the audio lock held */
int sfx_pool_busy(const struct sfx_pool *pool);
/* Add frames of every voice to buf (interleaved stereo, or mono if the
 * output is mono), in mixer units. The song is only read for the output
 * settings: rate, channels, interpolation, ramping and the stereo flags.
 * Call with the audio lock held. */
void sfx_pool_mix(struct sfx_pool *pool, const struct song *csf, int32_t *buf, uint32_t frames);

#endif /* SCHISM_SFX_VOICES_H_ */
