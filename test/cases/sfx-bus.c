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

#include "sfx-bus.h"
#include "mem.h"
#include "player/cmixer.h"

#include <math.h>

/* one 16-bit step in mixer units */
#define UNIT (1 << (16 - MIXING_ATTENUATION))

#define RATE 44100

static int32_t mixbuf[RATE * 2 * 2];

static void mix(uint32_t frames)
{
	uint32_t done, n;

	memset(mixbuf, 0, frames * 2 * sizeof(int32_t));
	/* in pieces the size the song mixer uses, so state carries between calls */
	for (done = 0; done < frames; done += n) {
		n = MIN(frames - done, 512);
		sfx_bus_mix(sfx_bus_get(), mixbuf + done * 2, n, 2, RATE);
	}
}

/* `cycles` periods of a sine over `frames` frames, so it loops cleanly */
static sfx_source_t *make_sine(uint32_t frames, uint32_t cycles, uint32_t rate, int16_t amp)
{
	int16_t *buf = mem_alloc(frames * sizeof(int16_t));
	sfx_source_t *src;
	uint32_t i;

	for (i = 0; i < frames; i++)
		buf[i] = (int16_t)lrint(amp * sin(2.0 * 3.14159265358979323846 * cycles * i / frames));
	src = sfx_source_from_pcm(buf, frames, SFX_PCM_S16, 1, rate);
	free(buf);

	return src;
}

/* sign changes in the left channel over frames [from, to) */
static uint32_t crossings(uint32_t from, uint32_t to)
{
	uint32_t i, n = 0;
	int last = 0;

	for (i = from; i < to; i++) {
		int s = (mixbuf[i * 2] > 0) - (mixbuf[i * 2] < 0);
		if (s && last && s != last)
			n++;
		if (s)
			last = s;
	}

	return n;
}

testresult_t test_sfx_bus_unity(void)
{
	int16_t pcm[2000];
	sfx_source_t *src;
	int i;

	for (i = 0; i < 2000; i++)
		pcm[i] = (int16_t)((i * 7919) % 65536 - 32768);

	REQUIRE(sfx_bus_init(4) == 0);
	src = sfx_source_from_pcm(pcm, 1000, SFX_PCM_S16, 2, RATE);
	REQUIRE(src);
	REQUIRE(sfx_play(src, 1.0f, 0.0f, 1.0f, 0) != SFX_VOICE_NONE);

	mix(1200);
	for (i = 0; i < 1000; i++) {
		ASSERT(mixbuf[i * 2] == pcm[i * 2] * UNIT);
		ASSERT(mixbuf[i * 2 + 1] == pcm[i * 2 + 1] * UNIT);
	}
	for (i = 1000; i < 1200; i++)
		ASSERT(mixbuf[i * 2] == 0 && mixbuf[i * 2 + 1] == 0);
	ASSERT(sfx_bus_voices_playing() == 0);

	sfx_source_free(src);
	sfx_bus_quit();
	RETURN_PASS;
}

testresult_t test_sfx_bus_pitch_loop(void)
{
	sfx_source_t *src;
	sfx_voice_t v;
	uint32_t n;

	REQUIRE(sfx_bus_init(4) == 0);
	/* 1000 Hz at 22050 Hz, one second, looped and played at 1.5x: 1500 Hz
	 * out, and the source runs out after 2/3 s without the loop */
	src = make_sine(22050, 1000, 22050, 8000);
	REQUIRE(src);
	v = sfx_play(src, 1.0f, 0.0f, 1.5f, 1);
	REQUIRE(v != SFX_VOICE_NONE);

	mix(RATE * 2);
	/* two sign changes per cycle */
	n = crossings(0, RATE);
	ASSERT_PRINTF(n >= 2998 && n <= 3002, "first second: %u crossings", n);
	n = crossings(RATE, RATE * 2);
	ASSERT_PRINTF(n >= 2998 && n <= 3002, "second second: %u crossings", n);
	ASSERT(sfx_is_playing(v));

	sfx_set_frequency(v, 44100);
	mix(RATE);
	n = crossings(0, RATE);
	ASSERT_PRINTF(n >= 3998 && n <= 4002, "at 44100 Hz: %u crossings", n);

	/* stopping fades out over the ramp, then the voice is gone */
	sfx_stop(v);
	mix(RATE / 10);
	ASSERT(!sfx_is_playing(v));
	ASSERT(sfx_bus_voices_playing() == 0);
	for (n = RATE / 250; n < RATE / 10; n++)
		ASSERT(mixbuf[n * 2] == 0);

	sfx_source_free(src);
	sfx_bus_quit();
	RETURN_PASS;
}

testresult_t test_sfx_bus_pan(void)
{
	int16_t pcm[100];
	sfx_source_t *src;
	int i;

	for (i = 0; i < 100; i++)
		pcm[i] = 10000;

	REQUIRE(sfx_bus_init(4) == 0);
	src = sfx_source_from_pcm(pcm, 100, SFX_PCM_S16, 1, RATE);
	REQUIRE(src);

	sfx_play(src, 1.0f, -1.0f, 1.0f, 0);
	mix(50);
	ASSERT(mixbuf[0] == 10000 * UNIT && mixbuf[1] == 0);
	sfx_stop_all();
	mix(RATE / 10);

	sfx_play(src, 0.5f, 0.5f, 1.0f, 0);
	mix(50);
	ASSERT(mixbuf[0] == 2500 * UNIT && mixbuf[1] == 5000 * UNIT);
	sfx_stop_all();
	mix(RATE / 10);

	sfx_bus_set_volume(0.5f);
	sfx_play(src, 1.0f, 0.0f, 1.0f, 0);
	mix(50);
	ASSERT(mixbuf[0] == 5000 * UNIT && mixbuf[1] == 5000 * UNIT);

	sfx_source_free(src);
	ASSERT(sfx_bus_voices_playing() == 0);
	sfx_bus_quit();
	RETURN_PASS;
}

testresult_t test_sfx_bus_delay_duration(void)
{
	int16_t pcm[RATE];
	struct sfx_play_args args = {
		.volume = 1.0f,
		.loop = 1,
		.delay = 1000.0 / RATE,
		.duration = 2000.0 / RATE,
	};
	sfx_source_t *src;
	int i;

	for (i = 0; i < RATE; i++)
		pcm[i] = 1000;

	REQUIRE(sfx_bus_init(4) == 0);
	src = sfx_source_from_pcm(pcm, RATE, SFX_PCM_S16, 1, RATE);
	REQUIRE(src);
	REQUIRE(sfx_play_ex(src, &args) != SFX_VOICE_NONE);

	mix(RATE / 4);
	ASSERT(mixbuf[999 * 2] == 0);
	ASSERT(mixbuf[1000 * 2] == 1000 * UNIT);
	ASSERT(mixbuf[2999 * 2] == 1000 * UNIT);
	/* then the fade, then nothing */
	ASSERT(mixbuf[3000 * 2] < 1000 * UNIT);
	ASSERT(mixbuf[(3000 + RATE / 500) * 2] == 0);
	ASSERT(sfx_bus_voices_playing() == 0);

	sfx_source_free(src);
	sfx_bus_quit();
	RETURN_PASS;
}

testresult_t test_sfx_bus_handles(void)
{
	int16_t pcm[10] = {0};
	sfx_source_t *src;
	sfx_voice_t a, b, c;

	REQUIRE(sfx_play(NULL, 1, 0, 1, 0) == SFX_VOICE_NONE);
	REQUIRE(sfx_bus_init(0) < 0);
	REQUIRE(sfx_bus_init(2) == 0);
	REQUIRE(sfx_bus_init(2) < 0);
	src = sfx_source_from_pcm(pcm, 10, SFX_PCM_S16, 1, RATE);
	REQUIRE(src);

	a = sfx_play(src, 1, 0, 1, 1);
	b = sfx_play(src, 1, 0, 1, 1);
	c = sfx_play(src, 1, 0, 1, 1);
	ASSERT(a != SFX_VOICE_NONE && b != SFX_VOICE_NONE && a != b);
	ASSERT(c == SFX_VOICE_NONE); /* pool full, nothing stolen */

	/* a voice that hasn't been mixed yet stops at once */
	sfx_stop(a);
	ASSERT(!sfx_is_playing(a));
	c = sfx_play(src, 1, 0, 1, 1);
	ASSERT(c != SFX_VOICE_NONE && c != a); /* same slot, new generation */
	ASSERT(!sfx_is_playing(a));
	sfx_set_volume(a, 0.5f); /* stale handle, does nothing */

	/* freeing the source ends its voices */
	sfx_source_free(src);
	ASSERT(sfx_bus_voices_playing() == 0);
	ASSERT(!sfx_is_playing(b) && !sfx_is_playing(c));

	sfx_bus_quit();
	ASSERT(!sfx_bus_running());
	RETURN_PASS;
}
