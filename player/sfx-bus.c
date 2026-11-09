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

/* Sound effects bus. The host calls the public functions from its main
 * thread; each one takes the audio lock while it touches voice state, and
 * the mixer calls sfx_bus_mix() with the lock already held. Nothing here
 * allocates or frees memory with the lock held, and the mixing side never
 * allocates at all. See docs/sfx-bus.md for the volume and pan law. */

#include "headers.h"

#include "sfx-bus.h"
#include "song.h"
#include "fmt.h"
#include "slurp.h"
#include "engine-alloc.h"
#include "player/sndfile.h"
#include "player/cmixer.h"

#include <math.h>

struct sfx_source {
	int16_t *data; /* frames * channels, interleaved */
	uint32_t frames;
	uint32_t channels;
	uint32_t rate;
	uint32_t loop_start;
	uint32_t loop_end; /* 0 means the end of the data */
};

struct sfx_voice {
	const struct sfx_source *src; /* NULL when the slot is free */
	uint16_t gen;

	uint64_t pos; /* 32.32 fixed point, in source frames */
	double freq;
	uint64_t inc;
	uint32_t inc_rate; /* output rate inc was worked out for, 0 if stale */
	int loop;

	float volume;
	float pan;

	/* current and target gains in mixer units, and the ramp between them */
	float gl, gr;
	float tl, tr;
	float dl, dr;
	uint32_t ramp_left;
	int audible; /* has produced at least one frame */
	int stopping;

	/* converted to frames the first time the voice is mixed */
	double delay, duration;
	int timed;
	uint64_t wait;     /* frames until the voice starts */
	int64_t play_left; /* frames until it stops, -1 for no limit */
};

struct sfx_bus {
	struct sfx_voice *voices;
	uint32_t max_voices;
	uint32_t playing;
	float master;
};

static struct sfx_bus *bus_running = NULL;

/* one 16-bit sample step, in the mixer's 27-bit units */
#define SFX_UNIT ((float)(1 << (16 - MIXING_ATTENUATION)))

/* ------------------------------------------------------------------------ */
/* Memory. Everything the bus allocates comes through these two, always on
 * the host's thread and never with the audio lock held.
 *
 * The voice pool and the sources are ENGINE memory. Neither belongs to a
 * song: a source outlives any number of song changes, including the
 * song_new() that ends the song a source was copied from, so SONG would get
 * it thrown away. They aren't SCRATCH either, since they live until the
 * host frees them. A host on the arena allocator should load its effects
 * once, after the engine has set up, and size the ENGINE region for them;
 * the region only gets space back when the newest blocks are freed. */

static void *sfx_alloc(size_t size)
{
	return engine_calloc(1, size, ENGINE_MEM_ENGINE);
}

static void sfx_dealloc(void *p)
{
	engine_free(p, ENGINE_MEM_ENGINE);
}

/* ------------------------------------------------------------------------ */

int sfx_bus_init(uint32_t max_voices)
{
	struct sfx_bus *bus;

	if (bus_running || !max_voices || max_voices > SFX_MAX_VOICES)
		return -1;

	bus = sfx_alloc(sizeof(*bus));
	if (!bus)
		return -1;
	bus->voices = sfx_alloc(max_voices * sizeof(*bus->voices));
	if (!bus->voices) {
		sfx_dealloc(bus);
		return -1;
	}
	bus->max_voices = max_voices;
	bus->master = 1.0f;

	song_lock_audio();
	bus_running = bus;
	song_unlock_audio();

	return 0;
}

void sfx_bus_quit(void)
{
	struct sfx_bus *bus;

	song_lock_audio();
	bus = bus_running;
	bus_running = NULL;
	song_unlock_audio();

	if (bus) {
		sfx_dealloc(bus->voices);
		sfx_dealloc(bus);
	}
}

int sfx_bus_running(void)
{
	return bus_running != NULL;
}

uint32_t sfx_bus_max_voices(void)
{
	return bus_running ? bus_running->max_voices : 0;
}

uint32_t sfx_bus_voices_playing(void)
{
	uint32_t n = 0;

	song_lock_audio();
	if (bus_running)
		n = bus_running->playing;
	song_unlock_audio();

	return n;
}

void sfx_bus_set_volume(float volume)
{
	song_lock_audio();
	if (bus_running)
		bus_running->master = MAX(volume, 0.0f);
	song_unlock_audio();
}

float sfx_bus_get_volume(void)
{
	return bus_running ? bus_running->master : 0.0f;
}

struct sfx_bus *sfx_bus_get(void)
{
	return bus_running;
}

int sfx_bus_busy(const struct sfx_bus *bus)
{
	return bus && bus->playing;
}

/* ------------------------------------------------------------------------ */
/* sources */

static struct sfx_source *source_new(uint32_t frames, uint32_t channels, uint32_t rate)
{
	struct sfx_source *src;

	if (!frames || channels < 1 || channels > 2 || !rate)
		return NULL;
	if (frames > UINT32_MAX / 4)
		return NULL;

	src = sfx_alloc(sizeof(*src));
	if (!src)
		return NULL;
	src->data = sfx_alloc((size_t)frames * channels * sizeof(int16_t));
	if (!src->data) {
		sfx_dealloc(src);
		return NULL;
	}
	src->frames = frames;
	src->channels = channels;
	src->rate = rate;

	return src;
}

sfx_source_t *sfx_source_from_pcm(const void *data, uint32_t frames, int format,
	int channels, uint32_t rate)
{
	struct sfx_source *src;
	size_t i, n;

	if (!data || (format != SFX_PCM_U8 && format != SFX_PCM_S8 && format != SFX_PCM_S16))
		return NULL;

	src = source_new(frames, channels, rate);
	if (!src)
		return NULL;

	n = (size_t)frames * channels;
	switch (format) {
	case SFX_PCM_U8:
		for (i = 0; i < n; i++)
			src->data[i] = (int16_t)((((const uint8_t *)data)[i] ^ 0x80) << 8);
		break;
	case SFX_PCM_S8:
		for (i = 0; i < n; i++)
			src->data[i] = (int16_t)(((const int8_t *)data)[i] * 256);
		break;
	case SFX_PCM_S16:
		memcpy(src->data, data, n * sizeof(int16_t));
		break;
	}

	return src;
}

/* takes the PCM out of a song sample; doesn't take ownership of it */
static struct sfx_source *source_from_sample(const song_sample_t *smp)
{
	struct sfx_source *src;
	int channels = (smp->flags & CHN_STEREO) ? 2 : 1;

	if (!smp->data || !smp->length || (smp->flags & CHN_ADLIB))
		return NULL;

	src = sfx_source_from_pcm(smp->data, smp->length,
		(smp->flags & CHN_16BIT) ? SFX_PCM_S16 : SFX_PCM_S8, channels,
		smp->c5speed ? smp->c5speed : 8363);
	if (!src)
		return NULL;

	if ((smp->flags & CHN_LOOP) && smp->loop_end > smp->loop_start
			&& smp->loop_end <= smp->length)
		sfx_source_set_loop(src, smp->loop_start, smp->loop_end);

	return src;
}

sfx_source_t *sfx_source_load(const char *filename)
{
	song_sample_t smp = {0};
	struct sfx_source *src;

	if (!song_load_sample_file(filename, &smp))
		return NULL;

	src = source_from_sample(&smp);
	csf_free_sample(smp.data);

	return src;
}

sfx_source_t *sfx_source_from_song_sample(int n)
{
	if (n < 1 || n >= MAX_SAMPLES || !current_song)
		return NULL;

	return source_from_sample(current_song->samples + n);
}

void sfx_source_set_loop(sfx_source_t *src, uint32_t start, uint32_t end)
{
	if (!src)
		return;
	if (end > src->frames || (end && start >= end))
		start = end = 0;

	/* voices read these while mixing */
	song_lock_audio();
	src->loop_start = start;
	src->loop_end = end;
	song_unlock_audio();
}

uint32_t sfx_source_frames(const sfx_source_t *src)
{
	return src ? src->frames : 0;
}

uint32_t sfx_source_rate(const sfx_source_t *src)
{
	return src ? src->rate : 0;
}

int sfx_source_channels(const sfx_source_t *src)
{
	return src ? (int)src->channels : 0;
}

static void voice_free(struct sfx_bus *bus, struct sfx_voice *v)
{
	v->src = NULL;
	bus->playing--;
}

void sfx_source_free(sfx_source_t *src)
{
	uint32_t i;

	if (!src)
		return;

	song_lock_audio();
	if (bus_running) {
		for (i = 0; i < bus_running->max_voices; i++)
			if (bus_running->voices[i].src == src)
				voice_free(bus_running, bus_running->voices + i);
	}
	song_unlock_audio();

	sfx_dealloc(src->data);
	sfx_dealloc(src);
}

/* ------------------------------------------------------------------------ */
/* voices */

static struct sfx_voice *voice_lookup(sfx_voice_t h)
{
	uint32_t idx = (h & 0xffff);

	if (!bus_running || !idx || idx > bus_running->max_voices)
		return NULL;

	struct sfx_voice *v = bus_running->voices + idx - 1;
	if (!v->src || v->gen != (h >> 16))
		return NULL;

	return v;
}

sfx_voice_t sfx_play_ex(sfx_source_t *src, const struct sfx_play_args *args)
{
	struct sfx_voice *v = NULL;
	sfx_voice_t h = SFX_VOICE_NONE;
	uint32_t i;

	if (!src || !args)
		return SFX_VOICE_NONE;

	song_lock_audio();
	if (bus_running && bus_running->playing < bus_running->max_voices) {
		for (i = 0; i < bus_running->max_voices; i++) {
			if (!bus_running->voices[i].src) {
				v = bus_running->voices + i;
				break;
			}
		}
	}
	if (v) {
		uint16_t gen = v->gen + 1;

		memset(v, 0, sizeof(*v));
		v->gen = gen;
		v->src = src;
		v->volume = MAX(args->volume, 0.0f);
		v->pan = CLAMP(args->pan, -1.0f, 1.0f);
		v->freq = args->frequency ? (double)args->frequency
			: src->rate * (args->pitch > 0 ? (double)args->pitch : 1.0);
		v->loop = !!args->loop;
		v->delay = MAX(args->delay, 0.0);
		v->duration = MAX(args->duration, 0.0);

		bus_running->playing++;
		h = ((uint32_t)gen << 16) | (uint32_t)(v - bus_running->voices + 1);
	}
	song_unlock_audio();

	return h;
}

sfx_voice_t sfx_play(sfx_source_t *src, float volume, float pan, float pitch, int loop)
{
	struct sfx_play_args args = {
		.volume = volume,
		.pan = pan,
		.pitch = pitch,
		.loop = loop,
	};

	return sfx_play_ex(src, &args);
}

/* A voice that hasn't been heard yet stops at once. One that has fades out
 * over the ramp, so stopping doesn't click. */
static void voice_stop(struct sfx_voice *v)
{
	if (!v->audible)
		voice_free(bus_running, v);
	else
		v->stopping = 1;
}

void sfx_stop(sfx_voice_t voice)
{
	struct sfx_voice *v;

	song_lock_audio();
	v = voice_lookup(voice);
	if (v)
		voice_stop(v);
	song_unlock_audio();
}

void sfx_stop_all(void)
{
	uint32_t i;

	song_lock_audio();
	if (bus_running) {
		for (i = 0; i < bus_running->max_voices; i++)
			if (bus_running->voices[i].src)
				voice_stop(bus_running->voices + i);
	}
	song_unlock_audio();
}

int sfx_is_playing(sfx_voice_t voice)
{
	int r;

	song_lock_audio();
	r = voice_lookup(voice) != NULL;
	song_unlock_audio();

	return r;
}

void sfx_set_volume(sfx_voice_t voice, float volume)
{
	struct sfx_voice *v;

	song_lock_audio();
	v = voice_lookup(voice);
	if (v)
		v->volume = MAX(volume, 0.0f);
	song_unlock_audio();
}

void sfx_set_pan(sfx_voice_t voice, float pan)
{
	struct sfx_voice *v;

	song_lock_audio();
	v = voice_lookup(voice);
	if (v)
		v->pan = CLAMP(pan, -1.0f, 1.0f);
	song_unlock_audio();
}

void sfx_set_pitch(sfx_voice_t voice, float pitch)
{
	struct sfx_voice *v;

	song_lock_audio();
	v = voice_lookup(voice);
	if (v && pitch > 0) {
		v->freq = v->src->rate * (double)pitch;
		v->inc_rate = 0;
	}
	song_unlock_audio();
}

void sfx_set_frequency(sfx_voice_t voice, uint32_t hz)
{
	struct sfx_voice *v;

	song_lock_audio();
	v = voice_lookup(voice);
	if (v && hz) {
		v->freq = hz;
		v->inc_rate = 0;
	}
	song_unlock_audio();
}

/* ------------------------------------------------------------------------ */
/* mixing, on the audio thread */

/* Balance law: the centre is full volume on both sides, and panning one
 * way fades the other side linearly down to nothing. */
static void voice_targets(const struct sfx_voice *v, float master, float *l, float *r)
{
	float g = v->volume * master * SFX_UNIT;

	if (v->stopping) {
		*l = *r = 0.0f;
		return;
	}
	*l = g * (v->pan > 0.0f ? 1.0f - v->pan : 1.0f);
	*r = g * (v->pan < 0.0f ? 1.0f + v->pan : 1.0f);
}

static void voice_update_gains(struct sfx_voice *v, float master, uint32_t ramp_len)
{
	float tl, tr;

	voice_targets(v, master, &tl, &tr);

	if (!v->audible) {
		v->gl = v->tl = tl;
		v->gr = v->tr = tr;
		v->ramp_left = 0;
	} else if (tl != v->tl || tr != v->tr) {
		v->tl = tl;
		v->tr = tr;
		v->dl = (tl - v->gl) / ramp_len;
		v->dr = (tr - v->gr) / ramp_len;
		v->ramp_left = ramp_len;
	}
}

static inline int32_t to_mix(float x)
{
	return (int32_t)lrintf(x);
}

/* Mixes frames [from, to) of buf. Returns how many it mixed, which is fewer
 * if a voice that doesn't loop runs off the end of its data. */
static uint32_t voice_mix_span(struct sfx_voice *v, int32_t *buf, uint32_t from, uint32_t to, int channels)
{
	const struct sfx_source *src = v->src;
	const int16_t *s = src->data;
	const uint32_t sch = src->channels;
	uint32_t ls = 0, le = src->frames;
	uint32_t i;

	if (v->loop && src->loop_end) {
		ls = src->loop_start;
		le = src->loop_end;
	}

	for (i = from; i < to; i++) {
		uint32_t idx = (uint32_t)(v->pos >> 32);
		uint32_t nx;
		float frac, a, b, l, r;

		if (idx >= le) {
			if (!v->loop)
				break;
			v->pos -= (uint64_t)(le - ls) << 32;
			while ((uint32_t)(v->pos >> 32) >= le)
				v->pos -= (uint64_t)(le - ls) << 32;
			idx = (uint32_t)(v->pos >> 32);
		}

		if (idx + 1 < le)
			nx = idx + 1;
		else
			nx = v->loop ? ls : idx;

		frac = (float)(uint32_t)v->pos * (1.0f / 4294967296.0f);

		a = s[idx * sch];
		b = s[nx * sch];
		l = a + (b - a) * frac;
		if (sch == 2) {
			a = s[idx * 2 + 1];
			b = s[nx * 2 + 1];
			r = a + (b - a) * frac;
		} else {
			r = l;
		}

		if (v->ramp_left) {
			v->gl += v->dl;
			v->gr += v->dr;
			if (!--v->ramp_left) {
				v->gl = v->tl;
				v->gr = v->tr;
			}
		}

		if (channels == 2) {
			buf[i * 2] += to_mix(l * v->gl);
			buf[i * 2 + 1] += to_mix(r * v->gr);
		} else {
			buf[i] += to_mix((l * v->gl + r * v->gr) * 0.5f);
		}

		v->pos += v->inc;
	}

	if (i > from)
		v->audible = 1;

	return i - from;
}

static void voice_mix(struct sfx_bus *bus, struct sfx_voice *v, int32_t *buf,
	uint32_t frames, int channels, uint32_t rate, uint32_t ramp_len)
{
	uint32_t i = 0;

	if (!v->timed) {
		v->wait = (uint64_t)llround(v->delay * rate);
		v->play_left = v->duration > 0 ? (int64_t)llround(v->duration * rate) : -1;
		v->timed = 1;
	}

	if (v->wait >= frames) {
		v->wait -= frames;
		return;
	}
	i = (uint32_t)v->wait;
	v->wait = 0;

	if (v->inc_rate != rate) {
		double ratio = v->freq / rate;

		ratio = CLAMP(ratio, 1.0 / 65536.0, 1024.0);
		v->inc = (uint64_t)llround(ratio * 4294967296.0);
		v->inc_rate = rate;
	}

	while (i < frames) {
		uint32_t end = frames, n;

		voice_update_gains(v, bus->master, ramp_len);

		if (v->play_left >= 0 && (uint64_t)v->play_left < end - i)
			end = i + (uint32_t)v->play_left;
		if (v->stopping && v->ramp_left < end - i)
			end = i + v->ramp_left;

		n = voice_mix_span(v, buf, i, end, channels);
		i += n;
		if (v->play_left > 0)
			v->play_left -= n;

		if (i < end || (v->stopping && !v->ramp_left)) {
			/* ran out of data, or finished fading out */
			voice_free(bus, v);
			return;
		}
		if (!v->play_left) {
			v->stopping = 1;
			v->play_left = -1;
		}
	}
}

void sfx_bus_mix(struct sfx_bus *bus, int32_t *buf, uint32_t frames, int channels, uint32_t rate)
{
	uint32_t i, ramp_len;

	if (!bus || !bus->playing || !frames || !rate)
		return;

	/* about 2 ms */
	ramp_len = MAX(rate / 500, 1);

	for (i = 0; i < bus->max_voices && bus->playing; i++)
		if (bus->voices[i].src)
			voice_mix(bus, bus->voices + i, buf, frames, channels, rate, ramp_len);
}
