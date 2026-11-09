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

/* The sound effects bus: PCM sounds the host plays on top of the song.
 * They're resampled, panned and summed into the song's mix buffer before
 * the final clip and conversion, so they come out of both live playback
 * and the disk writer. See docs/sfx-bus.md. */

#ifndef SCHISM_SFX_BUS_H_
#define SCHISM_SFX_BUS_H_

#include "headers.h"

typedef struct sfx_source sfx_source_t;

/* A playing voice. The low 16 bits are the slot number plus one and the
 * high 16 bits a generation count, so a handle to a voice that has ended
 * stays invalid after its slot is reused (until the count wraps). */
typedef uint32_t sfx_voice_t;
#define SFX_VOICE_NONE 0

/* the largest pool sfx_bus_init() accepts */
#define SFX_MAX_VOICES 65535

/* formats for sfx_source_from_pcm(); 16-bit is native endian */
enum {
	SFX_PCM_U8,
	SFX_PCM_S8,
	SFX_PCM_S16,
};

struct sfx_play_args {
	float volume;       /* linear gain, 1.0 plays at the source's level */
	float pan;          /* -1 hard left, 0 centre, +1 hard right */
	float pitch;        /* rate multiplier, 1.0 (or 0) is the source rate */
	uint32_t frequency; /* playback rate in Hz; overrides pitch if nonzero */
	int loop;           /* loop the source's loop (or all of it) until stopped */
	double delay;       /* seconds of output to wait before starting */
	double duration;    /* stop after this many seconds of play (0: never) */
};

/* Set up the bus with room for max_voices voices at once (1 to
 * SFX_MAX_VOICES). Returns 0 on success, -1 if it's already running or the
 * count is out of range or memory ran out. */
int sfx_bus_init(uint32_t max_voices);
/* Stop everything and free the voice pool. Sources aren't freed. */
void sfx_bus_quit(void);
int sfx_bus_running(void);
uint32_t sfx_bus_max_voices(void);
/* number of voices playing or waiting to start */
uint32_t sfx_bus_voices_playing(void);

/* Master volume for the bus, separate from the song's (linear, default 1). */
void sfx_bus_set_volume(float volume);
float sfx_bus_get_volume(void);

/* Sources. Each returns NULL on failure. The data is always copied. */
sfx_source_t *sfx_source_from_pcm(const void *data, uint32_t frames, int format,
	int channels, uint32_t rate);
/* through Schism's sample loaders: WAV, AIFF, AU, FLAC, ITS, and whatever
 * the configured decoders handle (Ogg, MP3, ...) */
sfx_source_t *sfx_source_load(const char *filename);
/* a copy of sample n (1-99) of the current song, with its C-5 rate and
 * loop points */
sfx_source_t *sfx_source_from_song_sample(int n);
/* Loop points in frames, used when a voice is started with loop set. end 0
 * (the default for PCM and for samples with no loop) loops the whole thing. */
void sfx_source_set_loop(sfx_source_t *src, uint32_t start, uint32_t end);
uint32_t sfx_source_frames(const sfx_source_t *src);
uint32_t sfx_source_rate(const sfx_source_t *src);
int sfx_source_channels(const sfx_source_t *src);
/* Stops any voice playing src, then frees it. */
void sfx_source_free(sfx_source_t *src);

/* Start a voice. Returns SFX_VOICE_NONE if the bus isn't running, src is
 * NULL, or all voices are busy (nothing is stolen). */
sfx_voice_t sfx_play_ex(sfx_source_t *src, const struct sfx_play_args *args);
sfx_voice_t sfx_play(sfx_source_t *src, float volume, float pan, float pitch, int loop);

/* These do nothing for a handle that has ended. */
void sfx_stop(sfx_voice_t voice);
void sfx_stop_all(void);
int sfx_is_playing(sfx_voice_t voice);
void sfx_set_volume(sfx_voice_t voice, float volume);
void sfx_set_pan(sfx_voice_t voice, float pan);
void sfx_set_pitch(sfx_voice_t voice, float pitch);
void sfx_set_frequency(sfx_voice_t voice, uint32_t hz);

/* ------------------------------------------------------------------------ */
/* for the engine's mixer; hosts don't need these */

struct sfx_bus;

/* the running bus, or NULL */
struct sfx_bus *sfx_bus_get(void);
/* nonzero if any voice is playing or waiting; call with the audio lock held */
int sfx_bus_busy(const struct sfx_bus *bus);
/* Add frames of every voice to buf (interleaved stereo if channels is 2,
 * mono otherwise), in mixer units. Call with the audio lock held. */
void sfx_bus_mix(struct sfx_bus *bus, int32_t *buf, uint32_t frames, int channels, uint32_t rate);

#endif /* SCHISM_SFX_BUS_H_ */
