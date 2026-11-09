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

/* The effects voice pool. Each voice is a song_voice_t that goes through
 * csf_voice_update() once a tick and csf_mix_voice() in between, the same
 * two functions the song's voices go through, with a struct csf_voice_env
 * that belongs to the pool. Every voice keeps its own tick clock, which
 * starts when the note does, so a note's envelopes line up with its start
 * to the sample.
 *
 * Threading follows the effects bus: the host calls the public functions
 * from one thread, they take the audio lock while they touch voices, and
 * the mixer calls sfx_pool_mix() with the lock held. Nothing allocates
 * with the lock held, and the mixing side never allocates. */

#include "headers.h"

#include "sfx-voices.h"
#include "song.h"
#include "engine-alloc.h"
#include "player/sndfile.h"
#include "player/cmixer.h"

#include <math.h>

struct sfx_bank {
	song_sample_t *samples;         /* [nsamples + 1]; 0 is unused */
	song_instrument_t *instruments; /* [ninstruments + 1]; NULL in sample mode */
	uint8_t *have_instrument;       /* [ninstruments + 1] */
	uint32_t nsamples, ninstruments;
};

struct pool_voice {
	song_voice_t v;
	struct sfx_bank *bank; /* NULL when the slot is free */
	uint16_t gen;
	uint32_t active_index;
	uint64_t serial;       /* start order, for SFX_STEAL_OLDEST */
	uint32_t wait;         /* frames until the note starts */
	uint32_t tick_left;    /* frames until the next tick */
	int64_t keyoff_left;   /* frames until an automatic note-off, -1 for none */
	int32_t base_frequency;
	int first_tick;        /* the note-on tick hasn't run yet */
	int filter_dirty;
};

struct sfx_pool {
	struct pool_voice *voices;
	uint32_t *active; /* indices of the voices in use */
	uint32_t nactive;
	uint32_t max_voices;
	uint64_t serial;

	int steal;
	uint32_t tempo;
	int32_t gain_q16; /* group volume, 16.16 */
	float gain;
	int32_t mixing_volume;
	int interpolation;
	uint32_t rng;

	/* leftover DC offset from voices that stopped, ramped out the way the
	 * song does it (stereo_fill) */
	int32_t dry_rofs, dry_lofs;

	int32_t scratch[MIXBUFFERSIZE * 2];
};

static struct sfx_pool *pool_running = NULL;

/* ------------------------------------------------------------------------ */
/* Memory. Voices and banks are ENGINE memory: they belong to the host, not
 * to any song, and banks outlive song changes. */

static void *pool_alloc(size_t size)
{
	return engine_calloc(1, size, ENGINE_MEM_ENGINE);
}

static void pool_dealloc(void *p)
{
	engine_free(p, ENGINE_MEM_ENGINE);
}

/* ------------------------------------------------------------------------ */

int sfx_pool_init(uint32_t max_voices)
{
	struct sfx_pool *pool;

	if (pool_running || !max_voices || max_voices > SFX_POOL_MAX_VOICES)
		return -1;

	pool = pool_alloc(sizeof(*pool));
	if (!pool)
		return -1;
	pool->voices = pool_alloc(max_voices * sizeof(*pool->voices));
	pool->active = pool_alloc(max_voices * sizeof(*pool->active));
	if (!pool->voices || !pool->active) {
		pool_dealloc(pool->active);
		pool_dealloc(pool->voices);
		pool_dealloc(pool);
		return -1;
	}
	pool->max_voices = max_voices;
	pool->steal = SFX_STEAL_NONE;
	pool->tempo = 125;
	pool->gain = 1.0f;
	pool->gain_q16 = 65536;
	pool->mixing_volume = 48;
	pool->interpolation = -1;
	pool->rng = 0x2545f491;

	song_lock_audio();
	pool_running = pool;
	song_unlock_audio();

	return 0;
}

void sfx_pool_quit(void)
{
	struct sfx_pool *pool;

	song_lock_audio();
	pool = pool_running;
	pool_running = NULL;
	song_unlock_audio();

	if (pool) {
		pool_dealloc(pool->active);
		pool_dealloc(pool->voices);
		pool_dealloc(pool);
	}
}

int sfx_pool_running(void)
{
	return pool_running != NULL;
}

uint32_t sfx_pool_max_voices(void)
{
	return pool_running ? pool_running->max_voices : 0;
}

uint32_t sfx_pool_voices_playing(void)
{
	uint32_t n = 0;

	song_lock_audio();
	if (pool_running)
		n = pool_running->nactive;
	song_unlock_audio();

	return n;
}

void sfx_pool_set_steal(int policy)
{
	if (policy < SFX_STEAL_NONE || policy > SFX_STEAL_QUIETEST)
		return;
	song_lock_audio();
	if (pool_running)
		pool_running->steal = policy;
	song_unlock_audio();
}

int sfx_pool_get_steal(void)
{
	return pool_running ? pool_running->steal : SFX_STEAL_NONE;
}

void sfx_pool_set_tick_rate(uint32_t tempo)
{
	tempo = CLAMP(tempo, 1, 10000);
	song_lock_audio();
	if (pool_running)
		pool_running->tempo = tempo;
	song_unlock_audio();
}

uint32_t sfx_pool_get_tick_rate(void)
{
	return pool_running ? pool_running->tempo : 0;
}

void sfx_pool_set_volume(float volume)
{
	volume = MAX(volume, 0.0f);
	volume = MIN(volume, 16.0f);
	song_lock_audio();
	if (pool_running) {
		pool_running->gain = volume;
		pool_running->gain_q16 = (int32_t)lrintf(volume * 65536.0f);
	}
	song_unlock_audio();
}

float sfx_pool_get_volume(void)
{
	return pool_running ? pool_running->gain : 0.0f;
}

void sfx_pool_set_mixing_volume(int volume)
{
	volume = CLAMP(volume, 0, 128);
	song_lock_audio();
	if (pool_running)
		pool_running->mixing_volume = volume;
	song_unlock_audio();
}

void sfx_pool_set_interpolation(int mode)
{
	if (mode < -1 || mode >= NUM_SRC_MODES)
		return;
	song_lock_audio();
	if (pool_running)
		pool_running->interpolation = mode;
	song_unlock_audio();
}

void sfx_pool_set_seed(uint32_t seed)
{
	song_lock_audio();
	if (pool_running)
		pool_running->rng = seed ? seed : 0x2545f491;
	song_unlock_audio();
}

struct sfx_pool *sfx_pool_get(void)
{
	return pool_running;
}

int sfx_pool_busy(const struct sfx_pool *pool)
{
	return pool && (pool->nactive || pool->dry_rofs || pool->dry_lofs);
}

/* xorshift32, scaled to 0..RAND_MAX like csf_random() */
static int pool_random(void *ctx)
{
	struct sfx_pool *pool = ctx;
	uint32_t x = pool->rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	pool->rng = x;

	return (int)(((uint64_t)x * ((uint64_t)RAND_MAX + 1)) >> 32);
}

/* ------------------------------------------------------------------------ */
/* banks */

static void bank_free_data(struct sfx_bank *bank)
{
	uint32_t i;

	if (!bank)
		return;
	if (bank->samples)
		for (i = 1; i <= bank->nsamples; i++)
			csf_free_sample_ex(bank->samples[i].data, ENGINE_MEM_ENGINE);
	pool_dealloc(bank->samples);
	pool_dealloc(bank->instruments);
	pool_dealloc(bank->have_instrument);
	pool_dealloc(bank);
}

static int copy_sample(song_sample_t *dst, const song_sample_t *src)
{
	uint32_t bytes;

	*dst = *src;
	dst->data = NULL;
	if (!src->data || !src->length || (src->flags & CHN_ADLIB)) {
		dst->length = 0;
		return 1;
	}

	bytes = src->length;
	if (bytes > UINT32_MAX / 4)
		return 0;
	if (src->flags & CHN_16BIT)
		bytes *= 2;
	if (src->flags & CHN_STEREO)
		bytes *= 2;

	dst->data = csf_allocate_sample_ex(bytes, ENGINE_MEM_ENGINE);
	if (!dst->data)
		return 0;
	memcpy(dst->data, src->data, bytes);
	csf_adjust_sample_loop(dst);

	return 1;
}

static struct sfx_bank *bank_from_song_t(song_t *song)
{
	struct sfx_bank *bank;
	uint32_t i, nsmp, nins;

	nsmp = csf_get_num_samples(song);
	nins = (song->flags & SONG_INSTRUMENTMODE) ? csf_get_num_instruments(song) : 0;
	if (!nsmp)
		return NULL;

	bank = pool_alloc(sizeof(*bank));
	if (!bank)
		return NULL;
	bank->samples = pool_alloc((nsmp + 1) * sizeof(*bank->samples));
	if (!bank->samples)
		goto fail;
	bank->nsamples = nsmp;
	for (i = 1; i <= nsmp; i++)
		if (!copy_sample(bank->samples + i, song->samples + i))
			goto fail;

	if (nins) {
		bank->instruments = pool_alloc((nins + 1) * sizeof(*bank->instruments));
		bank->have_instrument = pool_alloc(nins + 1);
		if (!bank->instruments || !bank->have_instrument)
			goto fail;
		bank->ninstruments = nins;
		for (i = 1; i <= nins; i++) {
			if (!song->instruments[i])
				continue;
			bank->instruments[i] = *song->instruments[i];
			bank->have_instrument[i] = 1;
		}
	}

	return bank;

fail:
	bank_free_data(bank);
	return NULL;
}

sfx_bank_t *sfx_bank_load(const char *filename)
{
	struct sfx_bank *bank = NULL;
	song_sample_t smp;
	song_t *song;

	if (!filename)
		return NULL;

	/* a module */
	song = song_create_load(filename);
	if (song) {
		bank = bank_from_song_t(song);
		csf_free(song);
		return bank;
	}

	/* an instrument, loaded into a scratch song */
	song = csf_allocate();
	if (!song)
		return NULL;
	song->flags |= SONG_INSTRUMENTMODE;
	if (song_load_instrument_into(song, filename))
		bank = bank_from_song_t(song);
	csf_free(song);
	if (bank)
		return bank;

	/* a sample */
	if (!song_load_sample_file(filename, &smp))
		return NULL;
	bank = pool_alloc(sizeof(*bank));
	if (bank) {
		bank->samples = pool_alloc(2 * sizeof(*bank->samples));
		if (bank->samples && copy_sample(bank->samples + 1, &smp) && bank->samples[1].data) {
			bank->nsamples = 1;
		} else {
			bank_free_data(bank);
			bank = NULL;
		}
	}
	csf_free_sample(smp.data);

	return bank;
}

sfx_bank_t *sfx_bank_from_song(void)
{
	return current_song ? bank_from_song_t(current_song) : NULL;
}

int sfx_bank_num_instruments(const sfx_bank_t *bank)
{
	return bank ? (int)bank->ninstruments : 0;
}

int sfx_bank_num_samples(const sfx_bank_t *bank)
{
	return bank ? (int)bank->nsamples : 0;
}

/* ------------------------------------------------------------------------ */
/* voices */

static void voice_release(struct sfx_pool *pool, struct pool_voice *pv)
{
	uint32_t i = pv->active_index;
	uint32_t last = pool->active[--pool->nactive];

	pool->active[i] = last;
	pool->voices[last].active_index = i;
	pv->bank = NULL;
}

/* Stop a voice outright. Whatever it was outputting ramps away through the
 * pool's DC offset, so a stolen voice doesn't click. */
static void voice_kill(struct sfx_pool *pool, struct pool_voice *pv)
{
	pool->dry_rofs += pv->v.rofs;
	pool->dry_lofs += pv->v.lofs;
	voice_release(pool, pv);
}

void sfx_bank_free(sfx_bank_t *bank)
{
	if (!bank)
		return;

	song_lock_audio();
	if (pool_running) {
		uint32_t i = pool_running->nactive;
		while (i--) {
			struct pool_voice *pv = pool_running->voices + pool_running->active[i];
			if (pv->bank == bank)
				voice_kill(pool_running, pv);
		}
	}
	song_unlock_audio();

	bank_free_data(bank);
}

void sfx_pool_stop_all(void)
{
	song_lock_audio();
	if (pool_running)
		while (pool_running->nactive)
			voice_kill(pool_running, pool_running->voices + pool_running->active[0]);
	song_unlock_audio();
}

static struct pool_voice *voice_lookup(struct sfx_pool *pool, sfx_note_t note)
{
	uint32_t slot = (note & 0xffff);
	struct pool_voice *pv;

	if (!pool || !slot || slot > pool->max_voices)
		return NULL;
	pv = pool->voices + slot - 1;
	if (!pv->bank || pv->gen != (note >> 16))
		return NULL;
	return pv;
}

static struct pool_voice *voice_grab(struct sfx_pool *pool)
{
	struct pool_voice *pv, *best = NULL;
	uint32_t i;

	if (pool->nactive < pool->max_voices) {
		/* the slots after the active ones aren't kept in order, so look
		 * for a free one; nothing here runs on the audio thread */
		for (i = 0; i < pool->max_voices; i++)
			if (!pool->voices[i].bank)
				return pool->voices + i;
	}

	switch (pool->steal) {
	case SFX_STEAL_OLDEST:
		for (i = 0; i < pool->nactive; i++) {
			pv = pool->voices + pool->active[i];
			if (!best || pv->serial < best->serial)
				best = pv;
		}
		break;
	case SFX_STEAL_QUIETEST:
		/* a note that hasn't started yet is only taken if nothing has */
		for (i = 0; i < pool->nactive; i++) {
			pv = pool->voices + pool->active[i];
			if (!best
			    || (best->wait && !pv->wait)
			    || (!best->wait == !pv->wait
				&& (pv->v.final_volume < best->v.final_volume
				    || (pv->v.final_volume == best->v.final_volume && pv->serial < best->serial))))
				best = pv;
		}
		break;
	default:
		return NULL;
	}

	if (best)
		voice_kill(pool, best);
	return best;
}

void sfx_note_args_init(struct sfx_note_args *args)
{
	memset(args, 0, sizeof(*args));
	args->note = NOTE_MIDC;
	args->volume = args->pan = SFX_DEFAULT;
	args->cutoff = args->resonance = SFX_DEFAULT;
}

/* Sets up v the way csf_instrument_change() and csf_note_change() set up an
 * idle pattern channel for a note with an instrument number and no effects,
 * with every IT compatibility quirk on. Returns 0 if nothing plays. */
static int voice_start(struct sfx_pool *pool, song_voice_t *v, const struct sfx_bank *bank,
	const struct sfx_note_args *args)
{
	song_instrument_t *penv = NULL;
	song_sample_t *psmp = NULL;
	int note = args->note, real_note;
	uint32_t frequency;

	if (!NOTE_IS_NOTE(note))
		return 0;

	if (args->instrument) {
		uint32_t n;

		if (args->instrument < 1 || (uint32_t)args->instrument > bank->ninstruments
		    || !bank->have_instrument[args->instrument])
			return 0;
		penv = bank->instruments + args->instrument;
		n = penv->sample_map[note - NOTE_FIRST];
		if (!n || n > bank->nsamples || penv->note_map[note - NOTE_FIRST] > NOTE_LAST)
			return 0;
		psmp = bank->samples + n;
		real_note = penv->note_map[note - NOTE_FIRST];
	} else {
		if (args->sample < 1 || (uint32_t)args->sample > bank->nsamples)
			return 0;
		psmp = bank->samples + args->sample;
		real_note = note;
	}

	if (!psmp->data || !psmp->length)
		return 0;
	frequency = get_frequency_from_note(CLAMP(real_note, NOTE_FIRST, NOTE_LAST), psmp->c5speed);
	if (!frequency)
		return 0;

	/* a fresh voice, as set_current_pos_0() leaves one */
	memset(v, 0, sizeof(*v));
	v->new_note = 1;
	v->cutoff = 0x7F;
	v->panning = 128;
	v->global_volume = 64;

	/* csf_instrument_change */
	v->volume = psmp->volume;
	v->ptr_instrument = penv;
	if (penv) {
		v->instrument_volume = (psmp->global_volume * penv->global_volume) >> 7;
		v->flags |= CHN_FASTVOLRAMP;
		if (penv->vol_swing) {
			double d = 2 * (((double) pool_random(pool)) / RAND_MAX) - 1;
			v->vol_swing = floor(d * penv->vol_swing / 100.0 * v->instrument_volume);
		}
		if (penv->pan_swing) {
			double d = 2 * (((double) pool_random(pool)) / RAND_MAX) - 1;
			v->pan_swing = d * penv->pan_swing * 4;
		}
		if (penv->flags & ENV_VOLUME)
			v->flags |= CHN_VOLENV;
		if (penv->flags & ENV_PANNING)
			v->flags |= CHN_PANENV;
		if (penv->flags & ENV_PITCH)
			v->flags |= CHN_PITCHENV;
	} else {
		v->instrument_volume = psmp->global_volume;
		v->flags |= CHN_FASTVOLRAMP;
	}

	/* csf_note_change */
	v->c5speed = psmp->c5speed;
	v->note = note;
	v->frequency = frequency;
	v->ptr_sample = psmp;
	v->current_sample_data = psmp->data;
	v->length = psmp->length;
	v->loop_start = 0;
	v->loop_end = psmp->length;
	v->flags |= psmp->flags & CHN_SAMPLE_FLAGS;
	if (v->flags & CHN_SUSTAINLOOP) {
		v->loop_start = psmp->sustain_start;
		v->loop_end = psmp->sustain_end;
		v->flags &= ~CHN_PINGPONGLOOP;
		v->flags |= CHN_LOOP;
		if (v->flags & CHN_PINGPONGSUSTAIN)
			v->flags |= CHN_PINGPONGLOOP;
		if (v->length > v->loop_end)
			v->length = v->loop_end;
	} else if (v->flags & CHN_LOOP) {
		v->loop_start = psmp->loop_start;
		v->loop_end = psmp->loop_end;
		if (v->length > v->loop_end)
			v->length = v->loop_end;
	}

	if (penv && (penv->flags & ENV_SETPANNING))
		v->panning = penv->panning;
	else if (psmp->flags & CHN_PANNING)
		v->panning = psmp->panning;
	if (args->pan != SFX_DEFAULT)
		v->panning = CLAMP(args->pan, 0, 64) * 4;

	if (penv && penv->pitch_pan_separation) {
		int delta = (int)(v->note - penv->pitch_pan_center - NOTE_FIRST) * penv->pitch_pan_separation / 2;
		v->panning = CLAMP(v->panning + delta, 0, 256);
	}

	if (penv)
		v->nna = penv->nna;
	v->fadeout_volume = 65536;
	v->strike = 4;
	v->flags |= CHN_FASTVOLRAMP | CHN_NEWNOTE;
	if (penv) {
		if (penv->ifr & 0x80)
			v->resonance = penv->ifr & 0x7F;
		if (penv->ifc & 0x80)
			v->cutoff = penv->ifc & 0x7F;
	}

	if (args->volume != SFX_DEFAULT)
		v->volume = CLAMP(args->volume, 0, 64) * 4;
	if (args->cutoff != SFX_DEFAULT)
		v->cutoff = CLAMP(args->cutoff, 0, 127);
	if (args->resonance != SFX_DEFAULT)
		v->resonance = CLAMP(args->resonance, 0, 127);

	return 1;
}

sfx_note_t sfx_note_on_ex(sfx_bank_t *bank, const struct sfx_note_args *args)
{
	struct sfx_pool *pool;
	struct pool_voice *pv;
	song_voice_t v;
	sfx_note_t handle = SFX_NOTE_NONE;

	if (!bank || !args)
		return SFX_NOTE_NONE;

	song_lock_audio();
	pool = pool_running;
	if (!pool || !voice_start(pool, &v, bank, args))
		goto done;

	pv = voice_grab(pool);
	if (!pv)
		goto done;

	pv->v = v;
	pv->bank = bank;
	pv->gen++;
	pv->serial = pool->serial++;
	pv->wait = args->delay;
	pv->tick_left = 0;
	pv->keyoff_left = args->duration ? (int64_t)args->duration : -1;
	pv->base_frequency = v.frequency;
	pv->first_tick = 1;
	pv->filter_dirty = 0;
	pv->active_index = pool->nactive;
	pool->active[pool->nactive++] = (uint32_t)(pv - pool->voices);

	handle = ((uint32_t)pv->gen << 16) | (uint32_t)(pv - pool->voices + 1);

done:
	song_unlock_audio();
	return handle;
}

sfx_note_t sfx_note_on(sfx_bank_t *bank, int index, int note, int volume, int pan)
{
	struct sfx_note_args args;

	if (!bank)
		return SFX_NOTE_NONE;

	sfx_note_args_init(&args);
	if (bank->ninstruments)
		args.instrument = index;
	else
		args.sample = index;
	args.note = note;
	args.volume = volume;
	args.pan = pan;

	return sfx_note_on_ex(bank, &args);
}

void sfx_note_off(sfx_note_t note)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv) {
		if (pv->wait) {
			/* hasn't started: key it off as it starts */
			pv->keyoff_left = 0;
		} else {
			csf_voice_key_off(&pv->v, 1);
			pv->keyoff_left = -1;
		}
	}
	song_unlock_audio();
}

void sfx_note_cut(sfx_note_t note)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv) {
		if (pv->wait) {
			voice_release(pool_running, pv);
		} else {
			/* what fx_note_cut does; the next tick ramps it out */
			pv->v.flags |= CHN_NOTEFADE | CHN_FASTVOLRAMP;
			pv->v.fadeout_volume = 0;
			pv->tick_left = 0;
		}
	}
	song_unlock_audio();
}

int sfx_note_playing(sfx_note_t note)
{
	int r;

	song_lock_audio();
	r = voice_lookup(pool_running, note) != NULL;
	song_unlock_audio();

	return r;
}

void sfx_note_set_volume(sfx_note_t note, int volume)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv)
		pv->v.volume = (volume == SFX_DEFAULT && pv->v.ptr_sample)
			? (int32_t)pv->v.ptr_sample->volume
			: CLAMP(volume, 0, 64) * 4;
	song_unlock_audio();
}

void sfx_note_set_pan(sfx_note_t note, int pan)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv) {
		pv->v.panning = CLAMP(pan, 0, 64) * 4;
		pv->v.flags &= ~CHN_SURROUND;
	}
	song_unlock_audio();
}

void sfx_note_set_pitch(sfx_note_t note, float ratio)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv && ratio > 0.0f) {
		double f = (double)pv->base_frequency * ratio;
		pv->v.frequency = (int32_t)CLAMP(f, 1.0, (double)INT32_MAX / 4);
	}
	song_unlock_audio();
}

void sfx_note_set_cutoff(sfx_note_t note, int cutoff)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv) {
		pv->v.cutoff = CLAMP(cutoff, 0, 127);
		pv->filter_dirty = 1;
	}
	song_unlock_audio();
}

void sfx_note_set_resonance(sfx_note_t note, int resonance)
{
	struct pool_voice *pv;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv) {
		pv->v.resonance = CLAMP(resonance, 0, 127);
		pv->filter_dirty = 1;
	}
	song_unlock_audio();
}

int sfx_note_get_info(sfx_note_t note, struct sfx_note_info *info)
{
	struct pool_voice *pv;
	int r = 0;

	song_lock_audio();
	pv = voice_lookup(pool_running, note);
	if (pv && info) {
		info->waiting = pv->wait != 0;
		info->released = !!(pv->v.flags & CHN_KEYOFF);
		info->fading = !!(pv->v.flags & CHN_NOTEFADE);
		info->final_volume = pv->v.final_volume;
		info->final_panning = pv->v.final_panning;
		info->position = csf_smp_pos_get_whole(pv->v.position);
		r = 1;
	} else if (pv) {
		r = 1;
	}
	song_unlock_audio();

	return r;
}

/* ------------------------------------------------------------------------ */
/* mixing */

/* One tick of a voice. Returns 0 once the voice has nothing left to play. */
static int voice_tick(struct pool_voice *pv, const struct csf_voice_env *env)
{
	song_voice_t *v = &pv->v;
	int r;

	/* a pattern channel loses these at the start of every tick
	 * (csf_process_effects), and the note-on sets them again */
	if (!pv->first_tick)
		v->flags &= ~(CHN_FASTVOLRAMP | CHN_NEWNOTE);
	pv->first_tick = 0;

	if (pv->filter_dirty) {
		setup_channel_filter(v, !(v->flags & CHN_FILTER), 256, env->mix_frequency);
		pv->filter_dirty = 0;
	}

	v->vu_meter = 0;
	r = csf_voice_update(env, v, 0);
	v->old_flags = v->flags;
	v->flags &= ~CHN_NEWNOTE;

	return r != CSF_VOICE_IDLE;
}

static void pool_mix_chunk(struct sfx_pool *pool, const struct csf_voice_env *env,
	uint32_t interp, int32_t *buf, uint32_t frames)
{
	int32_t *scratch = pool->scratch;
	uint32_t tick_length = env->tick_length;
	uint32_t i, n;

	stereo_fill(scratch, frames, &pool->dry_rofs, &pool->dry_lofs);

	/* backwards, so a voice that ends can swap the last one into its
	 * place without it being skipped */
	i = pool->nactive;
	while (i--) {
		struct pool_voice *pv = pool->voices + pool->active[i];
		song_voice_t *v = &pv->v;
		uint32_t pos = 0;
		int alive = 1;

		if (pv->wait >= frames) {
			pv->wait -= frames;
			continue;
		}
		pos = pv->wait;
		pv->wait = 0;

		while (pos < frames) {
			uint32_t step;

			if (pv->keyoff_left == 0) {
				csf_voice_key_off(v, 1);
				pv->keyoff_left = -1;
			}
			if (!pv->tick_left) {
				if (!voice_tick(pv, env)) {
					alive = 0;
					break;
				}
				pv->tick_left = tick_length;
			}

			step = MIN(frames - pos, pv->tick_left);
			if (pv->keyoff_left > 0 && (uint64_t)pv->keyoff_left < step)
				step = (uint32_t)pv->keyoff_left;

			if ((v->current_sample_data && v->ptr_sample) || v->lofs || v->rofs)
				csf_mix_voice(v, scratch + pos * 2, step, interp,
					&pool->dry_rofs, &pool->dry_lofs, 0);

			pv->tick_left -= step;
			if (pv->keyoff_left > 0)
				pv->keyoff_left -= step;
			pos += step;

			/* ran off the end of the sample, or faded out, and has no
			 * DC offset left to ramp out */
			if (!v->length && !v->lofs && !v->rofs) {
				alive = 0;
				break;
			}
		}

		if (!alive)
			voice_release(pool, pv);
	}

	if (env->mix_channels < 2) {
		mono_from_stereo(scratch, frames);
		n = frames;
	} else {
		n = frames * 2;
	}

	if (pool->gain_q16 == 65536) {
		for (i = 0; i < n; i++)
			buf[i] += scratch[i];
	} else {
		for (i = 0; i < n; i++)
			buf[i] += (int32_t)(((int64_t)scratch[i] * pool->gain_q16) >> 16);
	}
}

void sfx_pool_mix(struct sfx_pool *pool, const song_t *csf, int32_t *buf, uint32_t frames)
{
	struct csf_voice_env env;
	uint32_t interp, rate, tick_length;
	int channels;

	if (!sfx_pool_busy(pool) || !frames)
		return;

	rate = csf->mix_frequency;
	channels = (csf->mix_channels >= 2) ? 2 : 1;
	interp = (pool->interpolation >= 0) ? (uint32_t)pool->interpolation : csf->mix_interpolation;
	/* the same sum as csf_calculate_tick_length with a tempo factor of 128 */
	tick_length = (uint32_t)(((uint64_t)rate * 5 * 128) / ((uint64_t)pool->tempo << 8));
	tick_length = MAX(tick_length, 1);

	memset(&env, 0, sizeof(env));
	env.csf = NULL;
	env.flags = SONG_INSTRUMENTMODE;
	env.mix_flags = csf->mix_flags & (SNDMIX_NORAMPING | SNDMIX_NOSURROUND | SNDMIX_REVERSESTEREO);
	env.global_volume = 128;
	env.master_vol = pool->mixing_volume << 2;
	env.mix_frequency = rate;
	env.mix_channels = channels;
	env.mix_interpolation = interp;
	env.freq_factor = 128;
	env.pan_separation = 128;
	env.ramping_samples_up = csf->ramping_samples_up;
	env.ramping_samples_down = csf->ramping_samples_down;
	env.tick_length = tick_length;
	env.pitch_pan_in_envelope = 0;
	env.random = pool_random;
	env.random_ctx = pool;

	while (frames) {
		uint32_t n = MIN(frames, MIXBUFFERSIZE);

		pool_mix_chunk(pool, &env, interp, buf, n);
		buf += n * channels;
		frames -= n;
	}
}
