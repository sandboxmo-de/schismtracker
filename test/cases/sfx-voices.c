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

#include "test.h"
#include "test-assertions.h"

#include "sfx-voices.h"
#include "song.h"
#include "player/sndfile.h"

#define RATE 44100
#define TICK 882 /* frames per tick at tempo 125 */

static int32_t mixbuf[RATE * 4 * 2];
static song_t *out; /* only supplies the output settings */

static void setup_output(void)
{
	out = csf_allocate();
	out->mix_frequency = RATE;
	out->mix_channels = 2;
	out->mix_bits_per_sample = 32;
	out->mix_interpolation = SRCMODE_LINEAR;
	out->ramping_samples_up = 16;
	out->ramping_samples_down = 42;
}

static void mix(uint32_t frames)
{
	uint32_t done, n;

	memset(mixbuf, 0, frames * 2 * sizeof(int32_t));
	/* in pieces the size the song mixer uses, so state carries between calls */
	for (done = 0; done < frames; done += n) {
		n = MIN(frames - done, 512);
		sfx_pool_mix(sfx_pool_get(), out, mixbuf + done * 2, n);
	}
}

static int32_t peak(uint32_t from, uint32_t to, int ch)
{
	int32_t p = 0;
	uint32_t i;

	for (i = from; i < to; i++)
		p = MAX(p, abs(mixbuf[i * 2 + ch]));
	return p;
}

/* A bank with one 16-bit sample (a square wave, 4000 frames at 44100 Hz)
 * and, if with_instrument, instrument 1 playing it with a sustained volume
 * envelope and a fadeout. With loops set, the sample has a sustain loop
 * inside a loop over all of it. It's built in a scratch song, so
 * sfx_bank_from_song() sees it the way it sees a real one. */
static sfx_bank_t *make_bank(int with_instrument, int loops)
{
	song_t *saved = current_song, *song = csf_allocate();
	song_sample_t *smp = song->samples + 1;
	sfx_bank_t *bank;
	int16_t *data;
	uint32_t i;

	smp->length = 4000;
	smp->c5speed = RATE;
	smp->volume = 64 * 4;
	smp->global_volume = 64;
	smp->flags = CHN_16BIT;
	if (loops) {
		smp->flags |= CHN_SUSTAINLOOP | CHN_LOOP;
		smp->sustain_start = 1000;
		smp->sustain_end = 2000;
		smp->loop_start = 0;
		smp->loop_end = 4000;
	}
	smp->data = csf_allocate_sample(smp->length * 2);
	data = (int16_t *)smp->data;
	for (i = 0; i < smp->length; i++)
		data[i] = ((i / 50) & 1) ? 8000 : -8000;
	csf_adjust_sample_loop(smp);

	if (with_instrument) {
		song_instrument_t *ins = csf_allocate_instrument();
		csf_init_instrument(ins, 1);
		ins->fadeout = 512; /* 1024 a tick, so 64 ticks */
		ins->flags |= ENV_VOLUME | ENV_VOLSUSTAIN;
		ins->vol_env.nodes = 3;
		ins->vol_env.ticks[0] = 0; ins->vol_env.values[0] = 64;
		ins->vol_env.ticks[1] = 2; ins->vol_env.values[1] = 64;
		ins->vol_env.ticks[2] = 10; ins->vol_env.values[2] = 32;
		ins->vol_env.sustain_start = ins->vol_env.sustain_end = 1;
		song->instruments[1] = ins;
		song->flags |= SONG_INSTRUMENTMODE;
	}

	current_song = song;
	bank = sfx_bank_from_song();
	current_song = saved;
	csf_free(song);

	return bank;
}

testresult_t test_sfx_voices_sample(void)
{
	sfx_bank_t *bank;
	sfx_note_t n;
	int32_t l, r, sig;

	setup_output();
	REQUIRE(sfx_pool_init(8) == 0);
	bank = make_bank(0, 0);
	REQUIRE(bank);
	ASSERT(sfx_bank_num_samples(bank) == 1);
	ASSERT(sfx_bank_num_instruments(bank) == 0);

	/* panned hard to one side, at C-5, which plays the sample at its own
	 * rate */
	n = sfx_note_on(bank, 1, NOTE_MIDC, 64, 0);
	REQUIRE(n != SFX_NOTE_NONE);
	ASSERT(sfx_note_playing(n));
	mix(6000);
	l = peak(100, 3900, 0);
	r = peak(100, 3900, 1);
	ASSERT((l == 0) != (r == 0));
	sig = MAX(l, r);
	/* it ends with the sample, the pool lets go of it, and what's left
	 * ramps away */
	ASSERT(!sfx_note_playing(n));
	ASSERT(sfx_pool_voices_playing() == 0);
	ASSERT(peak(5800, 6000, 0) < sig / 50 && peak(5800, 6000, 1) < sig / 50);

	/* the other side */
	n = sfx_note_on(bank, 1, NOTE_MIDC, 64, 64);
	REQUIRE(n != SFX_NOTE_NONE);
	mix(3000);
	/* (the first note's tail is still ramping out on its side) */
	ASSERT((peak(100, 2900, 0) > peak(100, 2900, 1)) == (l == 0));

	/* nothing outside the bank plays */
	ASSERT(sfx_note_on(bank, 2, NOTE_MIDC, 64, 32) == SFX_NOTE_NONE);
	ASSERT(sfx_note_on(bank, 1, 0, 64, 32) == SFX_NOTE_NONE);

	sfx_bank_free(bank);
	sfx_pool_quit();
	csf_free(out);
	RETURN_PASS;
}

testresult_t test_sfx_voices_delay_release(void)
{
	struct sfx_note_args args;
	struct sfx_note_info info;
	sfx_bank_t *bank;
	sfx_note_t n;
	uint32_t i;

	setup_output();
	REQUIRE(sfx_pool_init(8) == 0);
	bank = make_bank(1, 1);
	REQUIRE(bank);
	ASSERT(sfx_bank_num_instruments(bank) == 1);

	sfx_note_args_init(&args);
	args.instrument = 1;
	args.delay = 1000;
	args.duration = TICK * 20;
	n = sfx_note_on_ex(bank, &args);
	REQUIRE(n != SFX_NOTE_NONE);
	ASSERT(sfx_note_get_info(n, &info) && info.waiting);

	mix(1000 + TICK * 10);
	ASSERT(peak(0, 1000, 0) == 0 && peak(0, 1000, 1) == 0);
	ASSERT(peak(1000, 1000 + TICK * 10, 0) > 0);
	ASSERT(sfx_note_get_info(n, &info) && !info.waiting && !info.released);

	/* the note-off has happened; the envelope runs out, then the fade */
	mix(TICK * 15);
	ASSERT(sfx_note_get_info(n, &info) && info.released);
	for (i = 0; i < 200 && sfx_note_playing(n); i++)
		mix(TICK);
	ASSERT(i > 50);
	ASSERT(!sfx_note_playing(n));
	ASSERT(sfx_pool_voices_playing() == 0);

	sfx_bank_free(bank);
	sfx_pool_quit();
	csf_free(out);
	RETURN_PASS;
}

/* frames from the note-off until the note is gone, at a given tick rate */
static uint32_t fade_frames(sfx_bank_t *bank, uint32_t tempo)
{
	sfx_note_t n;
	uint32_t frames = 0;

	sfx_pool_set_tick_rate(tempo);
	n = sfx_note_on(bank, 1, NOTE_MIDC, 64, 32);
	if (n == SFX_NOTE_NONE)
		return 0;
	mix(TICK);
	sfx_note_off(n);
	while (sfx_note_playing(n) && frames < RATE * 10) {
		mix(64);
		frames += 64;
	}
	return frames;
}

testresult_t test_sfx_voices_tick_rate(void)
{
	sfx_bank_t *bank;
	uint32_t slow, fast;

	setup_output();
	REQUIRE(sfx_pool_init(8) == 0);
	bank = make_bank(1, 1);
	REQUIRE(bank);
	ASSERT(sfx_pool_get_tick_rate() == 125);

	slow = fade_frames(bank, 125);
	fast = fade_frames(bank, 250);
	ASSERT(slow > TICK * 60 && fast > 0);
	/* twice the tick rate, half the time, give or take a couple of ticks */
	ASSERT(fast * 2 + TICK * 2 > slow && fast * 2 < slow + TICK * 2);

	sfx_bank_free(bank);
	sfx_pool_quit();
	csf_free(out);
	RETURN_PASS;
}

testresult_t test_sfx_voices_steal(void)
{
	sfx_bank_t *bank;
	sfx_note_t a, b, c;

	setup_output();
	REQUIRE(sfx_pool_init(2) == 0);
	bank = make_bank(1, 1);
	REQUIRE(bank);

	a = sfx_note_on(bank, 1, NOTE_MIDC, 64, 32);
	b = sfx_note_on(bank, 1, NOTE_MIDC, 8, 32);
	ASSERT(a != SFX_NOTE_NONE && b != SFX_NOTE_NONE);
	mix(TICK * 3);

	/* full, and nothing gets stolen */
	ASSERT(sfx_pool_get_steal() == SFX_STEAL_NONE);
	ASSERT(sfx_note_on(bank, 1, NOTE_MIDC, 64, 32) == SFX_NOTE_NONE);

	/* the quiet one goes */
	sfx_pool_set_steal(SFX_STEAL_QUIETEST);
	c = sfx_note_on(bank, 1, NOTE_MIDC, 64, 32);
	ASSERT(c != SFX_NOTE_NONE);
	ASSERT(sfx_note_playing(a) && !sfx_note_playing(b));

	/* now a is the oldest */
	sfx_pool_set_steal(SFX_STEAL_OLDEST);
	b = sfx_note_on(bank, 1, NOTE_MIDC, 64, 32);
	ASSERT(b != SFX_NOTE_NONE);
	ASSERT(!sfx_note_playing(a) && sfx_note_playing(c) && sfx_note_playing(b));
	ASSERT(sfx_pool_voices_playing() == 2);

	sfx_bank_free(bank);
	sfx_pool_quit();
	csf_free(out);
	RETURN_PASS;
}

testresult_t test_sfx_voices_handles(void)
{
	sfx_bank_t *bank;
	sfx_note_t a, b;

	setup_output();
	REQUIRE(sfx_pool_init(0) < 0);
	REQUIRE(sfx_pool_init(SFX_POOL_MAX_VOICES + 1) < 0);
	REQUIRE(sfx_pool_init(4) == 0);
	REQUIRE(sfx_pool_init(4) < 0);
	bank = make_bank(1, 1);
	REQUIRE(bank);

	/* calls on handles that never were, or have ended, do nothing */
	sfx_note_off(SFX_NOTE_NONE);
	sfx_note_cut(0x12345678);
	sfx_note_set_volume(0x00010003, 10);
	ASSERT(!sfx_note_playing(SFX_NOTE_NONE));

	a = sfx_note_on(bank, 1, NOTE_MIDC, 64, 32);
	ASSERT(a != SFX_NOTE_NONE);
	mix(TICK * 2);
	sfx_note_cut(a);
	mix(TICK * 2);
	ASSERT(!sfx_note_playing(a));
	/* the slot gets reused, but the old handle stays dead */
	b = sfx_note_on(bank, 1, NOTE_MIDC, 64, 32);
	ASSERT(b != SFX_NOTE_NONE && b != a);
	ASSERT(!sfx_note_playing(a) && sfx_note_playing(b));

	sfx_note_set_volume(b, 20);
	sfx_note_set_pan(b, 10);
	sfx_note_set_pitch(b, 1.5f);
	sfx_note_set_cutoff(b, 50);
	sfx_note_set_resonance(b, 60);
	mix(TICK * 2);
	ASSERT(sfx_note_playing(b));

	/* freeing the bank stops its notes */
	sfx_bank_free(bank);
	ASSERT(!sfx_note_playing(b));
	ASSERT(sfx_pool_voices_playing() == 0);

	sfx_pool_quit();
	ASSERT(!sfx_pool_running());
	ASSERT(sfx_note_on(NULL, 1, NOTE_MIDC, 64, 32) == SFX_NOTE_NONE);
	csf_free(out);
	RETURN_PASS;
}
