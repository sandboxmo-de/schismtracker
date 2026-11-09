/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
 * copyright (c) 2003-2005 Storlek <storlek@rigelseven.com>
 * copyright (c) 2005-2008 Mrs. Brisby <mrs.brisby@nimh.org>
 * copyright (c) 2009 Storlek & Mrs. Brisby
 * copyright (c) 2010-2012 Storlek
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

/* Un4seen MO3 loader.
 *
 * This is a C port of the MO3 loader in libopenmpt 0.8.9 (soundlib/Load_mo3.cpp),
 * which carries the following notice:
 *
 *   Load_mo3.cpp
 *   ------------
 *   Purpose: MO3 module loader.
 *   Authors: Johannes Schultz / OpenMPT Devs
 *            Based on documentation and the decompression routines from the
 *            open-source UNMO3 project (https://github.com/lclevy/unmo3).
 *            The modified decompression code has been relicensed to the BSD
 *            license with permission from Laurent Clévy.
 *   The OpenMPT source code is released under the BSD license.
 *
 * libopenmpt's license:
 *
 *   Copyright (c) 2004-2026, OpenMPT Project Developers and Contributors
 *   Copyright (c) 1997-2003, Olivier Lapicque
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *       * Neither the name of the OpenMPT project nor the
 *         names of its contributors may be used to endorse or promote products
 *         derived from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *   CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * MP3 and Ogg Vorbis samples are handed to whichever compressed sample
 * loader this build has (libavformat, Media Foundation, Core Audio or
 * GStreamer). Without one of those, such samples load as silence. */

#include "headers.h"
#include "bits.h"
#include "slurp.h"
#include "fmt.h"
#include "engine-alloc.h"
#include "log.h"
#include "str.h"

#include "player/sndfile.h"
#include "player/tables.h"

/* --------------------------------------------------------------------- */
/* bounded little-endian reader over a memory buffer */

struct mo3_reader {
	const uint8_t *data;
	size_t length;
	size_t pos;
};

static void mo3r_init(struct mo3_reader *r, const uint8_t *data, size_t length)
{
	r->data = data;
	r->length = length;
	r->pos = 0;
}

static int mo3r_can_read(const struct mo3_reader *r, size_t n)
{
	return (r->length - r->pos) >= n;
}

static int mo3r_byte(struct mo3_reader *r, uint8_t *b)
{
	if (r->pos >= r->length)
		return 0;
	*b = r->data[r->pos++];
	return 1;
}

/* these return zero when there isn't enough data left, and don't advance */
static uint8_t mo3r_u8(struct mo3_reader *r)
{
	uint8_t b = 0;
	mo3r_byte(r, &b);
	return b;
}

static uint16_t mo3r_u16(struct mo3_reader *r)
{
	uint16_t v;

	if (!mo3r_can_read(r, 2)) {
		r->pos = r->length;
		return 0;
	}
	v = r->data[r->pos] | (r->data[r->pos + 1] << 8);
	r->pos += 2;
	return v;
}

static uint32_t mo3r_u32(struct mo3_reader *r)
{
	uint32_t v;

	if (!mo3r_can_read(r, 4)) {
		r->pos = r->length;
		return 0;
	}
	v = (uint32_t)r->data[r->pos]
		| ((uint32_t)r->data[r->pos + 1] << 8)
		| ((uint32_t)r->data[r->pos + 2] << 16)
		| ((uint32_t)r->data[r->pos + 3] << 24);
	r->pos += 4;
	return v;
}

/* copies a NUL-terminated string into buf (truncating it to fit), and
 * skips past the terminator */
static void mo3r_string(struct mo3_reader *r, char *buf, size_t bufsize)
{
	size_t n = 0;
	uint8_t c;

	while (mo3r_byte(r, &c) && c) {
		if (buf && n + 1 < bufsize)
			buf[n++] = c;
	}
	if (buf && bufsize)
		buf[n] = '\0';
}

/* hands out a sub-reader for the next n bytes (or fewer, at the end) */
static struct mo3_reader mo3r_chunk(struct mo3_reader *r, size_t n)
{
	struct mo3_reader c;

	if (n > r->length - r->pos)
		n = r->length - r->pos;
	mo3r_init(&c, r->data + r->pos, n);
	r->pos += n;
	return c;
}

/* --------------------------------------------------------------------- */
/* the music data unpacker, and the two delta sample codecs */

/* Shifts the next control bit out of `data` into `carry`, reading a new
 * byte when the current one runs out. Bails out of the enclosing loop at
 * the end of the input. */
#define READ_CTRL_BIT \
	data <<= 1; \
	carry = (data > 0xFF); \
	data &= 0xFF; \
	if (data == 0) { \
		uint8_t next_byte_; \
		if (!mo3r_byte(r, &next_byte_)) \
			break; \
		data = next_byte_; \
		data <<= 1; \
		data += 1; \
		carry = (data > 0xFF); \
		data &= 0xFF; \
	}

/* Lengths are coded as pairs of control bits: the first bit of each pair
 * is the next bit of the value, and the second says whether another pair
 * follows. The leading 1 is implicit. */
#define DECODE_CTRL_BITS \
	{ \
		str_len++; \
		do { \
			READ_CTRL_BIT; \
			str_len = lshift_signed(str_len, 1) + carry; \
			READ_CTRL_BIT; \
		} while (carry); \
	}

/* Unpacks `size` bytes of LZ-compressed music data. Returns a buffer with
 * *got bytes in it; *got is less than size if the input was truncated or
 * corrupt. The reader is left just past the last byte used. */
static uint8_t *mo3_unpack(struct mo3_reader *r, uint32_t size, uint32_t *got)
{
	uint8_t *dst;
	uint32_t pos = 0, remain = size;
	uint16_t data = 0;
	int8_t carry = 0;
	int32_t str_len = 0, str_offset = 0;

	*got = 0;
	dst = engine_alloc(size ? size : 1, ENGINE_MEM_SCRATCH);
	if (!dst)
		return NULL;
	if (!size)
		return dst;

	/* first byte is stored as-is */
	dst[pos++] = mo3r_u8(r);
	remain--;

	while (remain) {
		READ_CTRL_BIT;
		if (!carry) {
			/* a literal byte */
			uint8_t b;
			if (!mo3r_byte(r, &b))
				break;
			dst[pos++] = b;
			remain--;
		} else {
			/* a back reference */
			uint8_t length_adjust = 0;
			uint32_t i;

			DECODE_CTRL_BITS;
			str_len -= 3;
			if (str_len < 0) {
				/* reuse the previous offset */
				str_len++;
			} else {
				uint8_t b;
				if (!mo3r_byte(r, &b))
					break;
				str_offset = lshift_signed(str_len, 8) | b;
				str_len = 0;
				str_offset = ~str_offset;
				if (str_offset < -1280)
					length_adjust++;
				length_adjust++;
				if (str_offset < -32000)
					length_adjust++;
			}
			if (str_offset >= 0 || -(int64_t)pos > str_offset)
				break;

			READ_CTRL_BIT;
			str_len = lshift_signed(str_len, 1) + carry;
			READ_CTRL_BIT;
			str_len = lshift_signed(str_len, 1) + carry;
			if (str_len == 0) {
				/* doesn't fit in two bits */
				DECODE_CTRL_BITS;
				str_len += 2;
			}
			str_len += length_adjust;

			if (str_len <= 0 || remain < (uint32_t)str_len)
				break;

			/* the source and destination can overlap, so go byte by byte */
			for (i = 0; i < (uint32_t)str_len; i++, pos++)
				dst[pos] = dst[pos + str_offset];
			remain -= str_len;
			str_len = 0;
		}
	}

	*got = pos;
	return dst;
}

/* The delta codecs below share a decoder skeleton between 8-bit and 16-bit
 * samples. `mask` keeps the arithmetic in the width of the sample type. */

static void mo3_unpack_delta(struct mo3_reader *r, void *dst, uint32_t length,
	int channels, int is16)
{
	const uint32_t mask = is16 ? 0xFFFF : 0xFF;
	const int shift = is16 ? 15 : 7;
	uint8_t dh = is16 ? 8 : 4, cl = 0;
	int8_t carry = 0;
	uint16_t data = 0;
	uint32_t val;
	int32_t previous = 0;
	int chn;

	for (chn = 0; chn < channels; chn++) {
		uint32_t p = chn;
		const uint32_t end = length * channels;

		while (p < end) {
			val = 0;
			if (is16 && dh < 5) {
				do {
					READ_CTRL_BIT;
					val = ((val << 1) + carry) & mask;
					READ_CTRL_BIT;
					val = ((val << 1) + carry) & mask;
					READ_CTRL_BIT;
				} while (carry);
			} else {
				do {
					READ_CTRL_BIT;
					val = ((val << 1) + carry) & mask;
					READ_CTRL_BIT;
				} while (carry);
			}
			cl = dh;
			while (cl > 0) {
				READ_CTRL_BIT;
				val = ((val << 1) + carry) & mask;
				cl--;
			}
			cl = 1;
			if (val >= 4) {
				cl = shift;
				while (((1u << cl) & val) == 0 && cl > 1)
					cl--;
			}
			dh = dh + cl;
			dh >>= 1;
			carry = val & 1; /* sign of the delta */
			val >>= 1;
			if (carry == 0)
				val = ~val & mask;
			val = (val + (uint32_t)previous) & mask;
			if (is16) {
				((int16_t *)dst)[p] = (int16_t)val;
				previous = (int16_t)val;
			} else {
				((int8_t *)dst)[p] = (int8_t)val;
				previous = (int8_t)val;
			}
			p += channels;
		}
	}
}

static void mo3_unpack_delta_prediction(struct mo3_reader *r, void *dst, uint32_t length,
	int channels, int is16)
{
	const uint32_t mask = is16 ? 0xFFFF : 0xFF;
	const int shift = is16 ? 15 : 7;
	const int32_t smin = is16 ? INT16_MIN : INT8_MIN;
	const int32_t smax = is16 ? INT16_MAX : INT8_MAX;
	uint8_t dh = is16 ? 8 : 4, cl = 0;
	int8_t carry = 0;
	uint16_t data = 0;
	int32_t next = 0;
	uint32_t val = 0;
	int32_t sval = 0, delta = 0, previous = 0;
	int chn;

	for (chn = 0; chn < channels; chn++) {
		uint32_t p = chn;
		const uint32_t end = length * channels;

		while (p < end) {
			val = 0;
			if (is16 && dh < 5) {
				do {
					READ_CTRL_BIT;
					val = ((val << 1) + carry) & mask;
					READ_CTRL_BIT;
					val = ((val << 1) + carry) & mask;
					READ_CTRL_BIT;
				} while (carry);
			} else {
				do {
					READ_CTRL_BIT;
					val = ((val << 1) + carry) & mask;
					READ_CTRL_BIT;
				} while (carry);
			}
			cl = dh;
			while (cl > 0) {
				READ_CTRL_BIT;
				val = ((val << 1) + carry) & mask;
				cl--;
			}
			cl = 1;
			if (val >= 4) {
				cl = shift;
				while (((1u << cl) & val) == 0 && cl > 1)
					cl--;
			}
			dh = dh + cl;
			dh >>= 1;
			carry = val & 1;
			val >>= 1;
			if (carry == 0)
				val = ~val & mask;

			delta = is16 ? (int16_t)val : (int8_t)val;
			val = (val + (uint32_t)next) & mask;
			if (is16) {
				((int16_t *)dst)[p] = (int16_t)val;
				sval = (int16_t)val;
			} else {
				((int8_t *)dst)[p] = (int8_t)val;
				sval = (int8_t)val;
			}
			p += channels;
			next = (sval * 2) + rshift_signed(delta, 1) - previous;
			next = CLAMP(next, smin, smax);
			previous = sval;
		}
	}
}

#undef READ_CTRL_BIT
#undef DECODE_CTRL_BITS

/* --------------------------------------------------------------------- */
/* file structures */

#define MO3_FILE_HEADER_SIZE 422
#define MO3_ENVELOPE_SIZE 106
#define MO3_INSTRUMENT_SIZE 826
#define MO3_SAMPLE_SIZE 41

enum {
	MO3_LINEAR_SLIDES    = 0x0001,
	MO3_IS_S3M           = 0x0002,
	MO3_S3M_FAST_SLIDES  = 0x0004,
	MO3_IS_MTM           = 0x0008, /* really "not XM"; MTM if none of the others are set */
	MO3_S3M_AMIGA_LIMITS = 0x0010,
	MO3_IS_MOD           = 0x0080,
	MO3_IS_IT            = 0x0100,
	MO3_INSTRUMENT_MODE  = 0x0200,
	MO3_IT_COMPAT_GXX    = 0x0400,
	MO3_IT_OLD_FX        = 0x0800,
	MO3_MODPLUG_MODE     = 0x10000,
	MO3_MOD_VBLANK       = 0x80000,
	MO3_HAS_PLUGINS      = 0x100000,
	MO3_EXT_FILTER_RANGE = 0x200000,
};

enum {
	MO3_ENV_ENABLED = 0x01,
	MO3_ENV_SUSTAIN = 0x02,
	MO3_ENV_LOOP    = 0x04,
	MO3_ENV_FILTER  = 0x10,
	MO3_ENV_CARRY   = 0x20,
};

enum {
	MO3_INS_PLAY_ON_MIDI = 0x01,
};

enum {
	MO3_SMP_16BIT              = 0x01,
	MO3_SMP_LOOP               = 0x10,
	MO3_SMP_PINGPONG_LOOP      = 0x20,
	MO3_SMP_SUSTAIN            = 0x100,
	MO3_SMP_SUSTAIN_PINGPONG   = 0x200,
	MO3_SMP_STEREO             = 0x400,
	MO3_SMP_COMPRESSION_MPEG   = 0x1000,
	MO3_SMP_COMPRESSION_OGG    = 0x1000 | 0x2000,
	MO3_SMP_SHARED_OGG         = 0x1000 | 0x2000 | 0x4000,
	MO3_SMP_DELTA              = 0x2000,
	MO3_SMP_DELTA_PREDICTION   = 0x4000,
	MO3_SMP_OPL                = 0x8000,
	MO3_SMP_COMPRESSION_MASK   = 0x1000 | 0x2000 | 0x4000 | 0x8000,
};

enum mo3_type {
	MO3_TYPE_XM,
	MO3_TYPE_IT,
	MO3_TYPE_S3M,
	MO3_TYPE_MOD,
	MO3_TYPE_MTM,
};

struct mo3_file_header {
	uint8_t num_channels;
	uint16_t num_orders;
	uint16_t restart_pos;
	uint16_t num_patterns;
	uint16_t num_tracks;
	uint16_t num_instruments;
	uint16_t num_samples;
	uint8_t default_speed;
	uint8_t default_tempo;
	uint32_t flags;
	uint8_t global_vol;
	uint8_t pan_separation;
	int8_t sample_volume;
	uint8_t chn_volume[64];
	uint8_t chn_pan[64];
	uint8_t sfx_macros[16];
	uint8_t fixed_macros[128][2];
};

struct mo3_envelope {
	uint8_t flags;
	uint8_t num_nodes;
	uint8_t sustain_start;
	uint8_t sustain_end;
	uint8_t loop_start;
	uint8_t loop_end;
	int16_t points[25][2];
};

struct mo3_instrument {
	uint32_t flags;
	uint16_t sample_map[120][2];
	struct mo3_envelope vol_env;
	struct mo3_envelope pan_env;
	struct mo3_envelope pitch_env;
	uint8_t vib_type, vib_sweep, vib_depth, vib_rate;
	uint16_t fadeout;
	uint8_t midi_channel;
	uint8_t midi_bank;
	uint8_t midi_patch;
	uint8_t midi_bend;
	uint8_t global_vol;
	uint16_t panning;
	uint8_t nna, pps, ppc, dct, dca;
	uint16_t vol_swing;
	uint16_t pan_swing;
	uint8_t cutoff;
	uint8_t resonance;
};

struct mo3_sample {
	uint32_t freq_finetune;
	int8_t transpose;
	uint8_t default_volume;
	uint16_t panning;
	uint32_t length;
	uint32_t loop_start;
	uint32_t loop_end;
	uint16_t flags;
	uint8_t vib_type, vib_sweep, vib_depth, vib_rate;
	uint8_t global_vol;
	uint32_t sustain_start;
	uint32_t sustain_end;
	int32_t compressed_size;
	uint16_t encoder_delay; /* MP3: bytes of decoded output to skip. Ogg: size of shared header */

	/* not part of the file */
	int16_t shared_header;
	struct mo3_reader chunk;
	int have_chunk;
};

static int mo3_read_file_header(struct mo3_reader *r, struct mo3_file_header *h)
{
	int i;

	if (!mo3r_can_read(r, MO3_FILE_HEADER_SIZE))
		return 0;
	h->num_channels = mo3r_u8(r);
	h->num_orders = mo3r_u16(r);
	h->restart_pos = mo3r_u16(r);
	h->num_patterns = mo3r_u16(r);
	h->num_tracks = mo3r_u16(r);
	h->num_instruments = mo3r_u16(r);
	h->num_samples = mo3r_u16(r);
	h->default_speed = mo3r_u8(r);
	h->default_tempo = mo3r_u8(r);
	h->flags = mo3r_u32(r);
	h->global_vol = mo3r_u8(r);
	h->pan_separation = mo3r_u8(r);
	h->sample_volume = (int8_t)mo3r_u8(r);
	for (i = 0; i < 64; i++)
		h->chn_volume[i] = mo3r_u8(r);
	for (i = 0; i < 64; i++)
		h->chn_pan[i] = mo3r_u8(r);
	for (i = 0; i < 16; i++)
		h->sfx_macros[i] = mo3r_u8(r);
	for (i = 0; i < 128; i++) {
		h->fixed_macros[i][0] = mo3r_u8(r);
		h->fixed_macros[i][1] = mo3r_u8(r);
	}
	return 1;
}

static void mo3_read_envelope(struct mo3_reader *r, struct mo3_envelope *e)
{
	int i;

	e->flags = mo3r_u8(r);
	e->num_nodes = mo3r_u8(r);
	e->sustain_start = mo3r_u8(r);
	e->sustain_end = mo3r_u8(r);
	e->loop_start = mo3r_u8(r);
	e->loop_end = mo3r_u8(r);
	for (i = 0; i < 25; i++) {
		e->points[i][0] = (int16_t)mo3r_u16(r);
		e->points[i][1] = (int16_t)mo3r_u16(r);
	}
}

static int mo3_read_instrument(struct mo3_reader *r, struct mo3_instrument *ins)
{
	int i;

	if (!mo3r_can_read(r, MO3_INSTRUMENT_SIZE))
		return 0;
	ins->flags = mo3r_u32(r);
	for (i = 0; i < 120; i++) {
		ins->sample_map[i][0] = mo3r_u16(r);
		ins->sample_map[i][1] = mo3r_u16(r);
	}
	mo3_read_envelope(r, &ins->vol_env);
	mo3_read_envelope(r, &ins->pan_env);
	mo3_read_envelope(r, &ins->pitch_env);
	ins->vib_type = mo3r_u8(r);
	ins->vib_sweep = mo3r_u8(r);
	ins->vib_depth = mo3r_u8(r);
	ins->vib_rate = mo3r_u8(r);
	ins->fadeout = mo3r_u16(r);
	ins->midi_channel = mo3r_u8(r);
	ins->midi_bank = mo3r_u8(r);
	ins->midi_patch = mo3r_u8(r);
	ins->midi_bend = mo3r_u8(r);
	ins->global_vol = mo3r_u8(r);
	ins->panning = mo3r_u16(r);
	ins->nna = mo3r_u8(r);
	ins->pps = mo3r_u8(r);
	ins->ppc = mo3r_u8(r);
	ins->dct = mo3r_u8(r);
	ins->dca = mo3r_u8(r);
	ins->vol_swing = mo3r_u16(r);
	ins->pan_swing = mo3r_u16(r);
	ins->cutoff = mo3r_u8(r);
	ins->resonance = mo3r_u8(r);
	return 1;
}

static int mo3_read_sample(struct mo3_reader *r, struct mo3_sample *s)
{
	if (!mo3r_can_read(r, MO3_SAMPLE_SIZE))
		return 0;
	s->freq_finetune = mo3r_u32(r);
	s->transpose = (int8_t)mo3r_u8(r);
	s->default_volume = mo3r_u8(r);
	s->panning = mo3r_u16(r);
	s->length = mo3r_u32(r);
	s->loop_start = mo3r_u32(r);
	s->loop_end = mo3r_u32(r);
	s->flags = mo3r_u16(r);
	s->vib_type = mo3r_u8(r);
	s->vib_sweep = mo3r_u8(r);
	s->vib_depth = mo3r_u8(r);
	s->vib_rate = mo3r_u8(r);
	s->global_vol = mo3r_u8(r);
	s->sustain_start = mo3r_u32(r);
	s->sustain_end = mo3r_u32(r);
	s->compressed_size = (int32_t)mo3r_u32(r);
	s->encoder_delay = mo3r_u16(r);
	s->shared_header = 0;
	s->have_chunk = 0;
	return 1;
}

/* The container header is "MO3", a version byte, and the unpacked size of
 * the music data. Version 5 adds the packed size after that. */
static int mo3_check_header(const uint8_t *hdr, uint32_t *music_size)
{
	uint32_t size;

	if (memcmp(hdr, "MO3", 3) != 0)
		return 0;
	size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
	/* the LZ scheme can blow a few bytes up to an enormous size, so put a
	 * sane upper limit on it */
	if (size <= MO3_FILE_HEADER_SIZE || size >= 0x20000000)
		return 0;
	if (hdr[3] > 5)
		return 0;
	if (music_size)
		*music_size = size;
	return 1;
}

static enum mo3_type mo3_get_type(uint32_t flags)
{
	if (flags & MO3_IS_IT)
		return MO3_TYPE_IT;
	else if (flags & MO3_IS_S3M)
		return MO3_TYPE_S3M;
	else if (flags & MO3_IS_MOD)
		return MO3_TYPE_MOD;
	else if (flags & MO3_IS_MTM)
		return MO3_TYPE_MTM;
	return MO3_TYPE_XM;
}

/* --------------------------------------------------------------------- */

int fmt_mo3_read_info(dmoz_file_t *file, slurp_t *fp)
{
	static const char *const descriptions[] = {
		[MO3_TYPE_XM] = "MO3 (FastTracker 2)",
		[MO3_TYPE_IT] = "MO3 (Impulse Tracker)",
		[MO3_TYPE_S3M] = "MO3 (Scream Tracker 3)",
		[MO3_TYPE_MOD] = "MO3 (Protracker)",
		[MO3_TYPE_MTM] = "MO3 (MultiTracker)",
	};
	static const int types[] = {
		[MO3_TYPE_XM] = TYPE_MODULE_XM,
		[MO3_TYPE_IT] = TYPE_MODULE_IT,
		[MO3_TYPE_S3M] = TYPE_MODULE_S3M,
		[MO3_TYPE_MOD] = TYPE_MODULE_MOD,
		[MO3_TYPE_MTM] = TYPE_MODULE_MOD,
	};
	uint8_t hdr[12];
	uint32_t music_size, got;
	uint8_t *buf, *music;
	size_t length;
	struct mo3_reader r, m;
	struct mo3_file_header fh;
	char title[33];
	enum mo3_type type;

	if (slurp_read(fp, hdr, 8) != 8 || !mo3_check_header(hdr, &music_size))
		return 0;

	/* Only the title and the file header are needed here. They come first
	 * in the music data, so unpack just the start of it. A literal byte
	 * costs nine bits at worst, which bounds how much input that takes. */
	length = slurp_length(fp);
	if (length <= 8)
		return 0;
	length = MIN(length - 8, 4 + 65536 * 9 / 8 + 16);
	buf = engine_alloc(length, ENGINE_MEM_SCRATCH);
	if (!buf)
		return 0;
	length = slurp_read(fp, buf, length);
	mo3r_init(&r, buf, length);
	if (hdr[3] >= 5)
		mo3r_u32(&r);

	music = mo3_unpack(&r, MIN(music_size, 65536), &got);
	engine_free(buf, ENGINE_MEM_SCRATCH);
	if (!music)
		return 0;

	mo3r_init(&m, music, got);
	mo3r_string(&m, title, sizeof(title));
	mo3r_string(&m, NULL, 0);
	if (!mo3_read_file_header(&m, &fh)) {
		engine_free(music, ENGINE_MEM_SCRATCH);
		return 0;
	}
	engine_free(music, ENGINE_MEM_SCRATCH);

	type = mo3_get_type(fh.flags);
	file->description = descriptions[type];
	file->title = engine_strdup(title, ENGINE_MEM_ENGINE);
	file->type = types[type];
	return 1;
}

/* --------------------------------------------------------------------- */
/* pattern data */

/* libopenmpt's internal effect and volume column numbering, which the
 * track decoder below produces before the per-format conversion */
enum {
	M3_CMD_NONE, M3_CMD_ARPEGGIO, M3_CMD_PORTAMENTOUP, M3_CMD_PORTAMENTODOWN,
	M3_CMD_TONEPORTAMENTO, M3_CMD_VIBRATO, M3_CMD_TONEPORTAVOL, M3_CMD_VIBRATOVOL,
	M3_CMD_TREMOLO, M3_CMD_PANNING8, M3_CMD_OFFSET, M3_CMD_VOLUMESLIDE,
	M3_CMD_POSITIONJUMP, M3_CMD_VOLUME, M3_CMD_PATTERNBREAK, M3_CMD_RETRIG,
	M3_CMD_SPEED, M3_CMD_TEMPO, M3_CMD_TREMOR, M3_CMD_MODCMDEX,
	M3_CMD_S3MCMDEX, M3_CMD_CHANNELVOLUME, M3_CMD_CHANNELVOLSLIDE, M3_CMD_GLOBALVOLUME,
	M3_CMD_GLOBALVOLSLIDE, M3_CMD_KEYOFF, M3_CMD_FINEVIBRATO, M3_CMD_PANBRELLO,
	M3_CMD_XFINEPORTAUPDOWN, M3_CMD_PANNINGSLIDE, M3_CMD_SETENVPOSITION, M3_CMD_MIDI,
	M3_CMD_SMOOTHMIDI, M3_CMD_DELAYCUT, M3_CMD_XPARAM, M3_CMD_FINETUNE,
	M3_CMD_FINETUNE_SMOOTH, M3_CMD_DUMMY,
};

enum {
	M3_VOL_NONE, M3_VOL_VOLUME, M3_VOL_PANNING, M3_VOL_VOLSLIDEUP,
	M3_VOL_VOLSLIDEDOWN, M3_VOL_FINEVOLUP, M3_VOL_FINEVOLDOWN, M3_VOL_VIBRATOSPEED,
	M3_VOL_VIBRATODEPTH, M3_VOL_PANSLIDELEFT, M3_VOL_PANSLIDERIGHT, M3_VOL_TONEPORTAMENTO,
	M3_VOL_PORTAUP, M3_VOL_PORTADOWN, M3_VOL_PLAYCONTROL, M3_VOL_OFFSET,
};

/* libopenmpt's special notes */
#define M3_NOTE_FADE 253
#define M3_NOTE_CUT 254
#define M3_NOTE_OFF 255

struct mo3_cell {
	uint8_t note, instr, volcmd, vol, command, param;
};

/* MO3 track command -> effect, for the commands that need no special care */
static const uint8_t mo3_effect_trans[] = {
	M3_CMD_NONE,             M3_CMD_NONE,             M3_CMD_NONE,             M3_CMD_ARPEGGIO,
	M3_CMD_PORTAMENTOUP,     M3_CMD_PORTAMENTODOWN,   M3_CMD_TONEPORTAMENTO,   M3_CMD_VIBRATO,
	M3_CMD_TONEPORTAVOL,     M3_CMD_VIBRATOVOL,       M3_CMD_TREMOLO,          M3_CMD_PANNING8,
	M3_CMD_OFFSET,           M3_CMD_VOLUMESLIDE,      M3_CMD_POSITIONJUMP,     M3_CMD_VOLUME,
	M3_CMD_PATTERNBREAK,     M3_CMD_MODCMDEX,         M3_CMD_TEMPO,            M3_CMD_TREMOR,
	M3_CMD_NONE,             M3_CMD_NONE,             M3_CMD_GLOBALVOLUME,     M3_CMD_GLOBALVOLSLIDE,
	M3_CMD_KEYOFF,           M3_CMD_SETENVPOSITION,   M3_CMD_PANNINGSLIDE,     M3_CMD_NONE,
	M3_CMD_RETRIG,           M3_CMD_XFINEPORTAUPDOWN, M3_CMD_XFINEPORTAUPDOWN, M3_CMD_NONE,
	M3_CMD_NONE,             M3_CMD_SPEED,            M3_CMD_VOLUMESLIDE,      M3_CMD_PORTAMENTODOWN,
	M3_CMD_PORTAMENTOUP,     M3_CMD_TREMOR,           M3_CMD_RETRIG,           M3_CMD_FINEVIBRATO,
	M3_CMD_CHANNELVOLUME,    M3_CMD_CHANNELVOLSLIDE,  M3_CMD_PANNINGSLIDE,     M3_CMD_S3MCMDEX,
	M3_CMD_TEMPO,            M3_CMD_GLOBALVOLSLIDE,   M3_CMD_PANBRELLO,        M3_CMD_MIDI,
	M3_CMD_NONE,             M3_CMD_NONE,             M3_CMD_NONE,             M3_CMD_DUMMY,
	M3_CMD_NONE,             M3_CMD_XPARAM,           M3_CMD_SMOOTHMIDI,       M3_CMD_DELAYCUT,
	M3_CMD_FINETUNE,         M3_CMD_FINETUNE_SMOOTH,
};

/* Impulse Tracker's Gx volume column speeds */
static const uint8_t it_porta_vol_cmd[10] = {0x00, 0x01, 0x04, 0x08, 0x10, 0x20, 0x40, 0x60, 0x80, 0xFF};

/* Applies one track command to a cell. This follows libopenmpt exactly,
 * including which column a command ends up in. */
static void mo3_apply_command(struct mo3_cell *m, const uint8_t cmd[2], enum mo3_type type,
	uint8_t note_offset)
{
	int i;

	switch (cmd[0]) {
	case 0x01:
		m->note = cmd[1];
		if (m->note < 120)
			m->note += note_offset;
		else if (m->note == 0xFF)
			m->note = M3_NOTE_OFF;
		else if (m->note == 0xFE)
			m->note = M3_NOTE_CUT;
		else
			m->note = M3_NOTE_FADE;
		break;
	case 0x02:
		m->instr = cmd[1] + 1;
		break;
	case 0x06:
		/* tone portamento */
		if (m->volcmd == M3_VOL_NONE && type == MO3_TYPE_XM && !(cmd[1] & 0x0F)) {
			m->volcmd = M3_VOL_TONEPORTAMENTO;
			m->vol = cmd[1] >> 4;
			break;
		} else if (m->volcmd == M3_VOL_NONE && type == MO3_TYPE_IT) {
			for (i = 0; i < 10; i++) {
				if (it_porta_vol_cmd[i] == cmd[1]) {
					m->volcmd = M3_VOL_TONEPORTAMENTO;
					m->vol = i;
					break;
				}
			}
			if (m->volcmd != M3_VOL_NONE)
				break;
		}
		m->command = M3_CMD_TONEPORTAMENTO;
		m->param = cmd[1];
		break;
	case 0x07:
		/* vibrato */
		if (m->volcmd == M3_VOL_NONE && cmd[1] < 10 && type == MO3_TYPE_IT) {
			m->volcmd = M3_VOL_VIBRATODEPTH;
			m->vol = cmd[1];
		} else {
			m->command = M3_CMD_VIBRATO;
			m->param = cmd[1];
		}
		break;
	case 0x0B:
		/* panning */
		if (m->volcmd == M3_VOL_NONE) {
			if (type == MO3_TYPE_IT && cmd[1] == 0xFF) {
				m->volcmd = M3_VOL_PANNING;
				m->vol = 64;
				break;
			}
			if ((type == MO3_TYPE_IT && !(cmd[1] & 0x03))
			    || (type == MO3_TYPE_XM && !(cmd[1] & 0x0F))) {
				m->volcmd = M3_VOL_PANNING;
				m->vol = cmd[1] / 4;
				break;
			}
		}
		m->command = M3_CMD_PANNING8;
		m->param = cmd[1];
		break;
	case 0x0F:
		/* volume */
		if (type != MO3_TYPE_MOD && m->volcmd == M3_VOL_NONE && cmd[1] <= 64) {
			m->volcmd = M3_VOL_VOLUME;
			m->vol = cmd[1];
		} else {
			m->command = M3_CMD_VOLUME;
			m->param = cmd[1];
		}
		break;
	case 0x10:
		/* pattern break; BCD everywhere except IT */
		m->command = M3_CMD_PATTERNBREAK;
		m->param = cmd[1];
		if (type != MO3_TYPE_IT)
			m->param = ((m->param >> 4) * 10) + (m->param & 0x0F);
		break;
	case 0x12:
		/* combined tempo and speed */
		m->command = (cmd[1] < 0x20) ? M3_CMD_SPEED : M3_CMD_TEMPO;
		m->param = cmd[1];
		break;
	case 0x14:
	case 0x15:
		/* XM volume column volume slides */
		if (cmd[1] & 0xF0) {
			m->volcmd = (cmd[0] == 0x14) ? M3_VOL_VOLSLIDEUP : M3_VOL_FINEVOLUP;
			m->vol = cmd[1] >> 4;
		} else {
			m->volcmd = (cmd[0] == 0x14) ? M3_VOL_VOLSLIDEDOWN : M3_VOL_FINEVOLDOWN;
			m->vol = cmd[1] & 0x0F;
		}
		break;
	case 0x1B:
		/* XM volume column panning slides */
		if (cmd[1] & 0xF0) {
			m->volcmd = M3_VOL_PANSLIDERIGHT;
			m->vol = cmd[1] >> 4;
		} else {
			m->volcmd = M3_VOL_PANSLIDELEFT;
			m->vol = cmd[1] & 0x0F;
		}
		break;
	case 0x1D:
		/* XM extra fine portamento up */
		m->command = M3_CMD_XFINEPORTAUPDOWN;
		m->param = 0x10 | cmd[1];
		break;
	case 0x1E:
		/* XM extra fine portamento down */
		m->command = M3_CMD_XFINEPORTAUPDOWN;
		m->param = 0x20 | cmd[1];
		break;
	case 0x1F:
	case 0x20:
		/* XM volume column vibrato */
		m->volcmd = (cmd[0] == 0x1F) ? M3_VOL_VIBRATOSPEED : M3_VOL_VIBRATODEPTH;
		m->vol = cmd[1];
		break;
	case 0x22:
		/* IT / S3M volume slide; this also turns a preceding G or H into L or K */
		if (m->command == M3_CMD_TONEPORTAMENTO)
			m->command = M3_CMD_TONEPORTAVOL;
		else if (m->command == M3_CMD_VIBRATO)
			m->command = M3_CMD_VIBRATOVOL;
		else
			m->command = M3_CMD_VOLUMESLIDE;
		m->param = cmd[1];
		break;
	case 0x30:
		/* IT volume column volume slides */
		m->vol = cmd[1] % 10;
		if (cmd[1] < 10)
			m->volcmd = M3_VOL_FINEVOLUP;
		else if (cmd[1] < 20)
			m->volcmd = M3_VOL_FINEVOLDOWN;
		else if (cmd[1] < 30)
			m->volcmd = M3_VOL_VOLSLIDEUP;
		else if (cmd[1] < 40)
			m->volcmd = M3_VOL_VOLSLIDEDOWN;
		break;
	case 0x31:
	case 0x32:
		/* IT volume column portamento */
		m->volcmd = (cmd[0] == 0x31) ? M3_VOL_PORTADOWN : M3_VOL_PORTAUP;
		m->vol = cmd[1];
		break;
	case 0x34:
		/* other IT volume column commands (OpenMPT extensions) */
		if (cmd[1] >= 223 && cmd[1] <= 232) {
			m->volcmd = M3_VOL_OFFSET;
			m->vol = cmd[1] - 223;
		}
		break;
	default:
		if (cmd[0] < ARRAY_SIZE(mo3_effect_trans)) {
			m->command = mo3_effect_trans[cmd[0]];
			m->param = cmd[1];
		}
		break;
	}
}

/* stats for the warnings at the end of loading */
struct mo3_lost {
	uint32_t effects;     /* effects Schism has no equivalent for */
	uint32_t volfx;       /* volume column commands Schism has no equivalent for */
	uint32_t merged;      /* XM cells where one of the two effects didn't fit */
	uint32_t notes;       /* notes outside Schism's range */
};

/* letters for IT/S3M effects, in Schism's import numbering (A = 1) */
static uint8_t mo3_it_letter(uint8_t command)
{
	switch (command) {
	case M3_CMD_SPEED:           return 'A' - 0x40;
	case M3_CMD_POSITIONJUMP:    return 'B' - 0x40;
	case M3_CMD_VOLUMESLIDE:     return 'D' - 0x40;
	case M3_CMD_PORTAMENTODOWN:  return 'E' - 0x40;
	case M3_CMD_PORTAMENTOUP:    return 'F' - 0x40;
	case M3_CMD_TONEPORTAMENTO:  return 'G' - 0x40;
	case M3_CMD_VIBRATO:         return 'H' - 0x40;
	case M3_CMD_TREMOR:          return 'I' - 0x40;
	case M3_CMD_ARPEGGIO:        return 'J' - 0x40;
	case M3_CMD_VIBRATOVOL:      return 'K' - 0x40;
	case M3_CMD_TONEPORTAVOL:    return 'L' - 0x40;
	case M3_CMD_CHANNELVOLUME:   return 'M' - 0x40;
	case M3_CMD_CHANNELVOLSLIDE: return 'N' - 0x40;
	case M3_CMD_OFFSET:          return 'O' - 0x40;
	case M3_CMD_PANNINGSLIDE:    return 'P' - 0x40;
	case M3_CMD_RETRIG:          return 'Q' - 0x40;
	case M3_CMD_TREMOLO:         return 'R' - 0x40;
	case M3_CMD_S3MCMDEX:        return 'S' - 0x40;
	case M3_CMD_TEMPO:           return 'T' - 0x40;
	case M3_CMD_FINEVIBRATO:     return 'U' - 0x40;
	case M3_CMD_GLOBALVOLUME:    return 'V' - 0x40;
	case M3_CMD_GLOBALVOLSLIDE:  return 'W' - 0x40;
	case M3_CMD_PANNING8:        return 'X' - 0x40;
	case M3_CMD_PANBRELLO:       return 'Y' - 0x40;
	case M3_CMD_MIDI:            return 'Z' - 0x40;
	case M3_CMD_SMOOTHMIDI:      return '\\' - 0x40;
	default:                     return 0;
	}
}

/* Effects shared by the MOD and XM effect numbering. Returns 0 and leaves
 * *effect alone if there's no MOD/XM spelling for the command. */
static int mo3_mod_effect(const struct mo3_cell *m, uint8_t *effect, uint8_t *param, int xm)
{
	*param = m->param;
	switch (m->command) {
	case M3_CMD_ARPEGGIO:         *effect = 0x0; break;
	case M3_CMD_PORTAMENTOUP:     *effect = 0x1; break;
	case M3_CMD_PORTAMENTODOWN:   *effect = 0x2; break;
	case M3_CMD_TONEPORTAMENTO:   *effect = 0x3; break;
	case M3_CMD_VIBRATO:          *effect = 0x4; break;
	case M3_CMD_TONEPORTAVOL:     *effect = 0x5; break;
	case M3_CMD_VIBRATOVOL:       *effect = 0x6; break;
	case M3_CMD_TREMOLO:          *effect = 0x7; break;
	case M3_CMD_PANNING8:         *effect = 0x8; break;
	case M3_CMD_OFFSET:           *effect = 0x9; break;
	case M3_CMD_VOLUMESLIDE:      *effect = 0xA; break;
	case M3_CMD_POSITIONJUMP:     *effect = 0xB; break;
	case M3_CMD_VOLUME:           *effect = 0xC; break;
	case M3_CMD_PATTERNBREAK:
		/* back to BCD, since the importer converts it again */
		*effect = 0xD;
		*param = ((m->param / 10) << 4) | (m->param % 10);
		break;
	case M3_CMD_MODCMDEX:         *effect = 0xE; break;
	case M3_CMD_SPEED:
	case M3_CMD_TEMPO:            *effect = 0xF; break;
	/* the rest are XM (and OpenMPT) extensions, spelled the way
	 * csf_import_mod_effect expects them */
	case M3_CMD_GLOBALVOLUME:     *effect = 'G' - 55; break;
	case M3_CMD_GLOBALVOLSLIDE:   *effect = 'H' - 55; break;
	case M3_CMD_KEYOFF:           *effect = 'K' - 55; break;
	case M3_CMD_SETENVPOSITION:   *effect = 'L' - 55; break;
	case M3_CMD_CHANNELVOLUME:    *effect = 'M' - 55; break;
	case M3_CMD_CHANNELVOLSLIDE:  *effect = 'N' - 55; break;
	case M3_CMD_PANNINGSLIDE:     *effect = 'P' - 55; break;
	case M3_CMD_RETRIG:           *effect = 'R' - 55; break;
	case M3_CMD_TREMOR:           *effect = 'T' - 55; break;
	case M3_CMD_XFINEPORTAUPDOWN: *effect = 'X' - 55; break;
	case M3_CMD_PANBRELLO:        *effect = 'Y' - 55; break;
	case M3_CMD_MIDI:
	case M3_CMD_SMOOTHMIDI:       *effect = 'Z' - 55; break;
	default:
		return 0;
	}
	(void)xm;
	return 1;
}

/* XM volume column byte for a volume command */
static uint8_t mo3_xm_volume_byte(const struct mo3_cell *m)
{
	switch (m->volcmd) {
	case M3_VOL_VOLUME:         return 0x10 + MIN(m->vol, 64);
	case M3_VOL_VOLSLIDEDOWN:   return 0x60 | (m->vol & 0x0F);
	case M3_VOL_VOLSLIDEUP:     return 0x70 | (m->vol & 0x0F);
	case M3_VOL_FINEVOLDOWN:    return 0x80 | (m->vol & 0x0F);
	case M3_VOL_FINEVOLUP:      return 0x90 | (m->vol & 0x0F);
	case M3_VOL_VIBRATOSPEED:   return 0xA0 | (m->vol & 0x0F);
	case M3_VOL_VIBRATODEPTH:   return 0xB0 | (m->vol & 0x0F);
	case M3_VOL_PANNING:        return 0xC0 | ((m->vol >> 2) & 0x0F);
	case M3_VOL_PANSLIDELEFT:   return 0xD0 | (m->vol & 0x0F);
	case M3_VOL_PANSLIDERIGHT:  return 0xE0 | (m->vol & 0x0F);
	case M3_VOL_TONEPORTAMENTO: return 0xF0 | (m->vol & 0x0F);
	default:                    return 0;
	}
}

/* Turns a decoded cell into a Schism note, going through the same import
 * path the loader for the original format uses. */
static void mo3_convert_cell(const struct mo3_cell *m, song_note_t *note, enum mo3_type type,
	struct mo3_lost *lost)
{
	uint8_t effect = 0, param = 0;

	memset(note, 0, sizeof(*note));

	switch (m->note) {
	case 0:
		break;
	case M3_NOTE_OFF:
		note->note = NOTE_OFF;
		break;
	case M3_NOTE_CUT:
		note->note = NOTE_CUT;
		break;
	case M3_NOTE_FADE:
		note->note = NOTE_FADE;
		break;
	default:
		if (NOTE_IS_NOTE(m->note)) {
			note->note = m->note;
		} else {
			lost->notes++;
		}
		break;
	}
	note->instrument = m->instr;

	switch (type) {
	case MO3_TYPE_XM:
		/* XM: rebuild the raw XM columns and let the XM importer sort out
		 * where everything goes */
		if (note->note == NOTE_OFF)
			note->instrument = 0; /* the XM loader does this too */
		if (m->command != M3_CMD_NONE && !mo3_mod_effect(m, &effect, &param, 1)) {
			lost->effects++;
			effect = param = 0;
		}
		if (m->volcmd != M3_VOL_NONE) {
			note->volparam = mo3_xm_volume_byte(m);
			if (!note->volparam)
				lost->volfx++;
		}
		note->effect = effect;
		note->param = param;
		lost->merged += xm_import_note_effects(note);
		break;

	case MO3_TYPE_MOD:
	case MO3_TYPE_MTM:
		if (m->command != M3_CMD_NONE && !mo3_mod_effect(m, &effect, &param, 0)) {
			lost->effects++;
			effect = param = 0;
		}
		if (type == MO3_TYPE_MTM) {
			/* same filtering as the MTM loader */
			if (effect == 0xA && (param & 0xF0)) {
				param &= 0xF0;
			} else if (effect == 0x8) {
				effect = param = 0;
			} else if (effect == 0xE) {
				switch (param >> 4) {
				case 0x0: case 0x3: case 0x4: case 0x6: case 0x7: case 0xF:
					effect = param = 0;
					break;
				default:
					break;
				}
			}
		}
		note->effect = effect;
		note->param = param;
		if (note->effect || note->param)
			csf_import_mod_effect(note, 0);
		if (m->volcmd == M3_VOL_VOLUME) {
			/* MTM gets a volume column in libopenmpt; MOD never does */
			if (note->voleffect == VOLFX_NONE) {
				note->voleffect = VOLFX_VOLUME;
				note->volparam = MIN(m->vol, 64);
			}
		} else if (m->volcmd != M3_VOL_NONE) {
			lost->volfx++;
		}
		break;

	case MO3_TYPE_IT:
	case MO3_TYPE_S3M:
		if (m->command == M3_CMD_PATTERNBREAK) {
			/* already decimal */
			note->effect = FX_PATTERNBREAK;
			note->param = m->param;
		} else if (m->command == M3_CMD_VOLUME) {
			note->effect = FX_VOLUME;
			note->param = m->param;
		} else if (m->command == M3_CMD_KEYOFF) {
			note->effect = FX_KEYOFF;
			note->param = m->param;
		} else if (m->command == M3_CMD_SETENVPOSITION) {
			note->effect = FX_SETENVPOSITION;
			note->param = m->param;
		} else if (m->command == M3_CMD_XFINEPORTAUPDOWN || m->command == M3_CMD_MODCMDEX) {
			/* not IT commands, but there's a direct translation */
			note->effect = (m->command == M3_CMD_XFINEPORTAUPDOWN) ? 'X' - 55 : 0x0E;
			note->param = m->param;
			csf_import_mod_effect(note, 1);
		} else if (m->command != M3_CMD_NONE) {
			note->effect = mo3_it_letter(m->command);
			note->param = m->param;
			if (note->effect) {
				csf_import_s3m_effect(note, type == MO3_TYPE_IT);
			} else {
				lost->effects++;
				note->param = 0;
			}
		}

		/* IT's volume column numbering is the same as Schism's, minus the
		 * OpenMPT extensions on the end */
		if (m->volcmd != M3_VOL_NONE) {
			if (m->volcmd <= M3_VOL_PORTADOWN) {
				note->voleffect = m->volcmd;
				note->volparam = m->vol;
			} else {
				lost->volfx++;
			}
		}

		if (type == MO3_TYPE_S3M && note->effect == FX_SPECIAL) {
			/* mimic ST3's SD0/SC0 like the S3M loader does */
			if (note->param == 0xD0) {
				memset(note, 0, sizeof(*note));
			} else if (note->param == 0xC0) {
				note->effect = FX_NONE;
				note->param = 0;
			}
		}
		break;
	}
}

static int mo3_load_patterns(song_t *song, struct mo3_reader *music, const struct mo3_file_header *fh,
	enum mo3_type type, uint32_t lflags)
{
	struct mo3_reader track_chunk, patlen_chunk;
	struct mo3_reader *tracks;
	struct mo3_lost lost = {0};
	uint8_t note_offset;
	uint32_t lost_patterns = 0;
	int pat, n;

	track_chunk = mo3r_chunk(music, (size_t)fh->num_patterns * fh->num_channels * 2);
	patlen_chunk = mo3r_chunk(music, (size_t)fh->num_patterns * 2);

	tracks = engine_calloc(fh->num_tracks ? fh->num_tracks : 1, sizeof(*tracks), ENGINE_MEM_SCRATCH);
	if (!tracks)
		return 0;
	for (n = 0; n < fh->num_tracks; n++) {
		uint32_t len = mo3r_u32(music);
		/* a track can't really be bigger than this (65535 rows, 15 events
		 * a row, plus some slack) */
		if (len >= 0x200000) {
			engine_free(tracks, ENGINE_MEM_SCRATCH);
			return 0;
		}
		tracks[n] = mo3r_chunk(music, len);
	}

	if (type == MO3_TYPE_MTM)
		note_offset = 13 + 1;
	else if (type != MO3_TYPE_IT)
		note_offset = 12 + 1;
	else
		note_offset = 1;

	for (pat = 0; pat < fh->num_patterns; pat++) {
		const uint16_t rows = mo3r_u16(&patlen_chunk);
		song_note_t *pdata = NULL;
		int chn;

		/* libopenmpt skips patterns it can't create without consuming
		 * their track numbers; do the same so the rest line up */
		if ((lflags & LOAD_NOPATTERNS) || rows == 0 || rows > 1024)
			continue;

		if (pat < MAX_PATTERNS) {
			pdata = song->patterns[pat] = csf_allocate_pattern(rows);
			if (pdata)
				song->pattern_size[pat] = song->pattern_alloc_size[pat] = rows;
		} else {
			lost_patterns++;
		}

		for (chn = 0; chn < fh->num_channels; chn++) {
			const uint16_t track_index = mo3r_u16(&track_chunk);
			struct mo3_reader track;
			uint32_t row = 0;

			if (track_index >= fh->num_tracks)
				continue;
			track = tracks[track_index];
			track.pos = 0;

			while (row < rows) {
				const uint8_t b = mo3r_u8(&track);
				uint8_t num_commands, rep, c;
				struct mo3_cell m = {0};
				song_note_t note;
				uint32_t target;

				if (!b)
					break;

				num_commands = b & 0x0F;
				rep = b >> 4;
				for (c = 0; c < num_commands; c++) {
					uint8_t cmd[2] = {0, 0};
					if (mo3r_can_read(&track, 2)) {
						cmd[0] = mo3r_u8(&track);
						cmd[1] = mo3r_u8(&track);
					} else {
						track.pos = track.length;
					}
					mo3_apply_command(&m, cmd, type, note_offset);
				}

				mo3_convert_cell(&m, &note, type, &lost);

				target = MIN(row + rep, rows);
				for (; row < target; row++) {
					if (pdata && chn < MAX_CHANNELS)
						pdata[row * MAX_CHANNELS + chn] = note;
				}
			}
		}
	}

	engine_free(tracks, ENGINE_MEM_SCRATCH);

	if (lost_patterns)
		log_appendf(4, " Warning: Too many patterns in song (%" PRIu32 " skipped)", lost_patterns);
	if (lost.effects)
		log_appendf(4, " Warning: %" PRIu32 " unsupported effect%s dropped", lost.effects, lost.effects == 1 ? "" : "s");
	if (lost.volfx)
		log_appendf(4, " Warning: %" PRIu32 " unsupported volume column effect%s dropped", lost.volfx, lost.volfx == 1 ? "" : "s");
	if (lost.merged)
		log_appendf(4, " Warning: %" PRIu32 " effect%s dropped", lost.merged, lost.merged == 1 ? "" : "s");
	if (lost.notes)
		log_appendf(4, " Warning: %" PRIu32 " out-of-range note%s dropped", lost.notes, lost.notes == 1 ? "" : "s");

	return 1;
}

/* --------------------------------------------------------------------- */
/* instruments */

static void mo3_convert_envelope(const struct mo3_envelope *src, song_envelope_t *env,
	int shift, enum mo3_type type, uint32_t *flags, uint32_t enable_flag, uint32_t sustain_flag,
	uint32_t loop_flag, uint32_t carry_flag)
{
	int n, nodes;

	if (src->flags & MO3_ENV_ENABLED) *flags |= enable_flag;
	if (src->flags & MO3_ENV_SUSTAIN) *flags |= sustain_flag;
	if (src->flags & MO3_ENV_LOOP) *flags |= loop_flag;
	if (src->flags & MO3_ENV_CARRY) *flags |= carry_flag;

	nodes = MIN(src->num_nodes, 25);
	for (n = 0; n < nodes; n++) {
		int32_t tick = src->points[n][0];
		int32_t value = rshift_signed(src->points[n][1], shift);

		/* ticks are 16-bit unsigned in libopenmpt */
		tick &= 0xFFFF;
		if (n > 0 && tick < env->ticks[n - 1])
			tick = (env->ticks[n - 1] + 1) & 0xFFFF;
		env->ticks[n] = tick;
		env->values[n] = CLAMP(value, 0, 64);
	}

	if (type == MO3_TYPE_XM) {
		env->nodes = nodes;
		env->sustain_start = env->sustain_end = MIN(src->sustain_start, nodes);
		env->loop_start = MIN(src->loop_start, nodes);
		env->loop_end = MIN(src->loop_end, nodes);
		if (nodes == 0)
			*flags &= ~enable_flag;
	} else {
		/* same clamping as the IT instrument loader */
		env->nodes = CLAMP(nodes, 2, 25);
		env->loop_start = MIN(src->loop_start, env->nodes);
		env->loop_end = CLAMP(src->loop_end, env->loop_start, env->nodes);
		env->sustain_start = MIN(src->sustain_start, env->nodes);
		env->sustain_end = CLAMP(src->sustain_end, env->sustain_start, env->nodes);
	}
}

static uint8_t mo3_sample_index(uint16_t smp)
{
	/* stored zero-based, with 0xFFFF for none (which wraps to 0) */
	uint16_t s = (uint16_t)(smp + 1);
	return (s < MAX_SAMPLES) ? (uint8_t)s : 0;
}

static void mo3_convert_instrument(const struct mo3_instrument *mi, song_instrument_t *ins,
	enum mo3_type type)
{
	int n;

	if (type == MO3_TYPE_XM) {
		for (n = 0; n < 120; n++)
			ins->note_map[n] = n + 1;
		for (n = 0; n < 96; n++)
			ins->sample_map[n + 12] = mo3_sample_index(mi->sample_map[n][1]);
	} else {
		for (n = 0; n < 120; n++) {
			int note = (uint8_t)(mi->sample_map[n][0] + NOTE_FIRST);
			/* map invalid notes to themselves, like the IT loader */
			if (!NOTE_IS_NOTE(note))
				note = n + NOTE_FIRST;
			ins->note_map[n] = note;
			ins->sample_map[n] = mo3_sample_index(mi->sample_map[n][1]);
		}
	}

	mo3_convert_envelope(&mi->vol_env, &ins->vol_env, 0, type, &ins->flags,
		ENV_VOLUME, ENV_VOLSUSTAIN, ENV_VOLLOOP, ENV_VOLCARRY);
	mo3_convert_envelope(&mi->pan_env, &ins->pan_env, 0, type, &ins->flags,
		ENV_PANNING, ENV_PANSUSTAIN, ENV_PANLOOP, ENV_PANCARRY);
	mo3_convert_envelope(&mi->pitch_env, &ins->pitch_env, 5, type, &ins->flags,
		ENV_PITCH, ENV_PITCHSUSTAIN, ENV_PITCHLOOP, ENV_PITCHCARRY);
	if (mi->pitch_env.flags & MO3_ENV_FILTER)
		ins->flags |= ENV_FILTER;

	ins->fadeout = mi->fadeout;

	if (type == MO3_TYPE_XM) {
		ins->global_volume = 128;
		ins->panning = 128;
		ins->pitch_pan_center = 60;
		ins->midi_channel_mask = ((mi->flags & MO3_INS_PLAY_ON_MIDI) && mi->midi_channel < 17)
			? (1 << MIN(mi->midi_channel, 15)) : 0;
		ins->midi_program = MIN(mi->midi_patch, 127);
		xm_fix_instrument_envelopes(ins);
		return;
	}

	/* only IT has instrument global volume */
	ins->global_volume = (type == MO3_TYPE_IT) ? MIN(mi->global_vol, 128) : 128;
	if (mi->panning <= 256) {
		ins->panning = mi->panning;
		ins->flags |= ENV_SETPANNING;
	}
	ins->nna = mi->nna % 4;
	ins->dct = mi->dct % 4;
	ins->dca = mi->dca % 3;
	ins->vol_swing = MIN(mi->vol_swing, 100);
	ins->pan_swing = MIN(mi->pan_swing, 256) / 4;
	ins->pitch_pan_separation = CLAMP((int8_t)mi->pps, -32, 32);
	ins->pitch_pan_center = MIN(mi->ppc, 119);
	ins->ifc = mi->cutoff;
	ins->ifr = mi->resonance;

	/* The MIDI channel is stored as IT's value minus one, and plugin
	 * assignments (128 and up) the same way. Older encoders wrote "no
	 * channel" and channel 1 identically, without the play-on-MIDI flag.
	 * Rebuild IT's value and handle it like the IT instrument loader. */
	if ((mi->flags & MO3_INS_PLAY_ON_MIDI) || mi->midi_channel > 0) {
		const int mch = mi->midi_channel + 1;
		ins->midi_channel_mask = (mch > 16) ? (0x10000 + mch) : (1 << (mch - 1));
	}
	ins->midi_program = mi->midi_patch;
	ins->midi_bank = mi->midi_bank;
}

/* --------------------------------------------------------------------- */
/* samples */

static const uint8_t mo3_xm_autovib[8] = {
	VIB_SINE, VIB_SQUARE, VIB_RAMP_DOWN, VIB_RAMP_DOWN, VIB_RANDOM,
	VIB_SINE, VIB_SINE, VIB_SINE,
};

static void mo3_convert_sample(const struct mo3_sample *ms, song_sample_t *smp, enum mo3_type type,
	int frequency_is_hertz)
{
	static const uint8_t it_vib[4] = {VIB_SINE, VIB_RAMP_DOWN, VIB_SQUARE, VIB_RANDOM};

	if (type == MO3_TYPE_IT || type == MO3_TYPE_S3M) {
		if (frequency_is_hertz)
			smp->c5speed = ms->freq_finetune;
		else
			smp->c5speed = (uint32_t)(8363.0 * pow(2.0, ((int32_t)ms->freq_finetune + 1408) / 1536.0) + 0.5);
	} else {
		int8_t finetune = (int8_t)ms->freq_finetune;
		if (type != MO3_TYPE_MTM)
			finetune = (int8_t)(finetune - 128);
		smp->c5speed = transpose_to_frequency(ms->transpose, finetune);
	}

	smp->volume = MIN(ms->default_volume, 64) * 4;
	if (ms->panning <= 256) {
		smp->panning = ms->panning;
		smp->flags |= CHN_PANNING;
	}
	smp->length = ms->length;
	smp->loop_start = ms->loop_start;
	smp->loop_end = ms->loop_end;
	smp->sustain_start = ms->sustain_start;
	smp->sustain_end = ms->sustain_end;
	if (ms->flags & MO3_SMP_LOOP)
		smp->flags |= CHN_LOOP;
	if (ms->flags & MO3_SMP_PINGPONG_LOOP)
		smp->flags |= CHN_PINGPONGLOOP;
	if (ms->flags & MO3_SMP_SUSTAIN)
		smp->flags |= CHN_SUSTAINLOOP;
	if (ms->flags & MO3_SMP_SUSTAIN_PINGPONG)
		smp->flags |= CHN_PINGPONGSUSTAIN;

	smp->vib_type = it_vib[ms->vib_type & 3];
	smp->vib_rate = ms->vib_sweep;
	smp->vib_depth = ms->vib_depth;
	smp->vib_speed = ms->vib_rate;

	smp->global_volume = (type == MO3_TYPE_IT) ? MIN(ms->global_vol, 64) : 64;
}

/* Copies decoded PCM into a sample, converting between 8 and 16 bits as
 * needed. `src` is 16-bit, interleaved, `frames` long. */
static void mo3_store_pcm(song_sample_t *smp, const int16_t *src, uint32_t frames, int channels)
{
	const uint32_t n = frames * channels;
	uint32_t i;

	if (smp->flags & CHN_16BIT) {
		memcpy(smp->data, src, n * 2);
	} else {
		for (i = 0; i < n; i++) {
			int32_t v = rshift_signed(src[i] + 128, 8);
			smp->data[i] = (signed char)MIN(v, 127);
		}
	}
}

/* Runs a compressed stream through whichever sample decoder can take it.
 * On success, *pcm holds 16-bit interleaved data that the caller frees. */
static int mo3_decode_external(const uint8_t *data, size_t length, int16_t **pcm, uint32_t *frames,
	int *channels, const char **decoder)
{
	static const struct {
		const char *name;
		fmt_load_sample_func func;
	} decoders[] = {
#ifdef USE_AVFORMAT
		{"libavformat", fmt_avformat_load_sample},
#endif
#ifdef USE_MEDIAFOUNDATION
		{"Media Foundation", fmt_win32mf_load_sample},
#endif
#ifdef SCHISM_MACOSX
		{"Core Audio", fmt_macosxca_load_sample},
#endif
#ifdef USE_GSTREAMER
		{"GStreamer", fmt_gstreamer_load_sample},
#endif
		{NULL, NULL},
	};
	size_t i;

	for (i = 0; decoders[i].func; i++) {
		song_sample_t tmp = {0};
		slurp_t fp;
		uint32_t n, k;

		if (slurp_memstream(&fp, data, length) < 0)
			return 0;
		if (!decoders[i].func(&fp, &tmp) || !tmp.data || !tmp.length) {
			unslurp(&fp);
			if (tmp.data)
				csf_free_sample(tmp.data);
			continue;
		}
		unslurp(&fp);

		*channels = (tmp.flags & CHN_STEREO) ? 2 : 1;
		*frames = tmp.length;
		n = tmp.length * *channels;
		*pcm = engine_alloc(n * sizeof(int16_t), ENGINE_MEM_SCRATCH);
		if (!*pcm) {
			csf_free_sample(tmp.data);
			return 0;
		}
		if (tmp.flags & CHN_16BIT) {
			memcpy(*pcm, tmp.data, n * 2);
		} else {
			for (k = 0; k < n; k++)
				(*pcm)[k] = (int16_t)(tmp.data[k] * 256);
		}
		csf_free_sample(tmp.data);
		*decoder = decoders[i].name;
		return 1;
	}

	return 0;
}

/* -- MPEG -- */

/* Looks at the first MPEG frame. Old encoders left the LAME/Xing info frame
 * in, and the encoder delay in the sample header counts it, so it has to
 * be stripped by hand. Returns 1 if it's an info frame and sets its size
 * and sample count (times two, like libopenmpt). */
static int mo3_mpeg_info_frame(const uint8_t *d, size_t len, uint32_t *frame_size, uint32_t *num_samples)
{
	static const uint16_t samples_per_frame[2][3] = {{384, 1152, 1152}, {384, 1152, 576}};
	static const uint16_t bit_rates[2][3][15] = {
		{
			{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
			{0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384},
			{0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320},
		},
		{
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
			{0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160},
			{0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160},
		},
	};
	static const uint16_t sampling_rates[4][3] = {
		{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000},
	};
	static const uint8_t coefficients[2][3] = {{12, 144, 144}, {12, 144, 72}};
	static const uint8_t side_info_size[2][2] = {{17, 32}, {9, 17}};
	uint8_t h[4] = {0}, frame[36] = {0};
	uint8_t version, mpeg1, layer, bitrate, rate, padding;
	uint32_t lame_offset, i;
	int stereo;

	memcpy(h, d, MIN(len, 4));
	if (!(h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && (h[1] & 0x18) != 0x08
	      && (h[1] & 0x06) != 0x00 && (h[2] & 0x0C) != 0x0C && (h[2] & 0xF0) != 0xF0))
		return 0;

	version = (h[1] & 0x18) >> 3;
	mpeg1 = (version == 3) ? 0 : 1;
	layer = 3 - ((h[1] & 0x06) >> 1);
	bitrate = (h[2] & 0xF0) >> 4;
	rate = (h[2] & 0x0C) >> 2;
	padding = (h[2] & 0x02) >> 1;
	stereo = ((h[3] & 0xC0) >> 6) != 3;

	*frame_size = (uint16_t)(((coefficients[mpeg1][layer] * (bit_rates[mpeg1][layer][bitrate] * 1000)
		/ sampling_rates[version][rate]) + padding) * (layer == 0 ? 4 : 1));
	*num_samples = samples_per_frame[mpeg1][layer] * (stereo ? 2 : 1);

	lame_offset = side_info_size[mpeg1][stereo ? 1 : 0];
	if (*frame_size < lame_offset + 8)
		return 0;
	if (len > 4)
		memcpy(frame, d + 4, MIN(len - 4, lame_offset + 4));
	/* the first two bytes might be a CRC */
	for (i = 2; i < lame_offset; i++)
		if (frame[i] != 0)
			return 0;
	return !memcmp(frame + lame_offset, "Info", 4) || !memcmp(frame + lame_offset, "Xing", 4);
}

static int mo3_load_mpeg(song_sample_t *smp, const struct mo3_sample *ms, const uint8_t *data,
	size_t length, const char **decoder)
{
	uint32_t frame_size, frame_samples, frames, delay_frames;
	uint16_t encoder_delay = ms->encoder_delay;
	int16_t *pcm;
	int channels;

	if (mo3_mpeg_info_frame(data, length, &frame_size, &frame_samples)
	    && encoder_delay >= frame_samples * 2) {
		/* the info frame makes no sound, but counts towards the delay */
		encoder_delay -= frame_samples * 2;
		frame_size = MIN(frame_size, length);
		data += frame_size;
		length -= frame_size;
	}

	if (!mo3_decode_external(data, length, &pcm, &frames, &channels, decoder))
		return 0;

	/* MP3 samples always come out as 16-bit, with the stream's channels */
	smp->flags &= ~(CHN_16BIT | CHN_STEREO);
	smp->flags |= CHN_16BIT | ((channels == 2) ? CHN_STEREO : 0);

	/* encoder delay is in bytes of decoded output */
	delay_frames = 0;
	if (encoder_delay > 0 && encoder_delay < frames * channels * 2)
		delay_frames = encoder_delay / (channels * 2);
	frames -= delay_frames;
	frames = MIN(frames, ms->length);

	smp->length = frames;
	smp->data = csf_allocate_sample(frames * channels * 2);
	if (smp->data)
		memcpy(smp->data, pcm + delay_frames * channels, frames * channels * 2);
	engine_free(pcm, ENGINE_MEM_SCRATCH);
	return smp->data != NULL;
}

/* -- Ogg Vorbis -- */

static uint32_t mo3_ogg_crc(const uint8_t *d, size_t n, uint32_t crc)
{
	size_t i;
	int b;

	for (i = 0; i < n; i++) {
		crc ^= (uint32_t)d[i] << 24;
		for (b = 0; b < 8; b++)
			crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
	}
	return crc;
}

/* Finds the next valid Ogg page at or after *pos, skipping junk. */
static int mo3_ogg_next_page(const uint8_t *d, size_t len, size_t *pos, size_t *page_start, size_t *page_size)
{
	while (*pos + 4 <= len) {
		size_t p = *pos, header_size, data_size = 0, i;
		uint8_t hdr[27];
		uint32_t crc, stored;

		if (memcmp(d + p, "OggS", 4) != 0) {
			(*pos)++;
			continue;
		}
		if (p + 27 > len || p + 27 + d[p + 26] > len) {
			*pos += 4;
			continue;
		}
		header_size = 27 + d[p + 26];
		for (i = 0; i < d[p + 26]; i++)
			data_size += d[p + 27 + i];
		if (p + header_size + data_size > len) {
			*pos += 4;
			continue;
		}

		memcpy(hdr, d + p, 27);
		stored = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | ((uint32_t)hdr[25] << 24);
		memset(hdr + 22, 0, 4);
		crc = mo3_ogg_crc(hdr, 27, 0);
		crc = mo3_ogg_crc(d + p + 27, header_size - 27 + data_size, crc);
		if (crc != stored) {
			*pos += 4;
			continue;
		}

		*page_start = p;
		*page_size = header_size + data_size;
		*pos = p + *page_size;
		return 1;
	}
	return 0;
}

static uint32_t mo3_ogg_serial(const uint8_t *page)
{
	return page[14] | (page[15] << 8) | (page[16] << 16) | ((uint32_t)page[17] << 24);
}

/* Glues a shared header onto a sample's Ogg data. The header pages get the
 * sample's own bitstream serial numbers, so that stricter Ogg demuxers
 * accept the result. */
static uint8_t *mo3_ogg_merge(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len,
	size_t *out_len)
{
	uint32_t data_serials[16], head_serials[16];
	size_t num_data = 0, num_head = 0, pos, start, size, out = 0;
	uint8_t *buf;

	pos = 0;
	while (mo3_ogg_next_page(data, data_len, &pos, &start, &size)) {
		uint32_t s = mo3_ogg_serial(data + start);
		size_t i;
		for (i = 0; i < num_data && data_serials[i] != s; i++);
		if (i == num_data && num_data < ARRAY_SIZE(data_serials))
			data_serials[num_data++] = s;
	}

	buf = engine_alloc(head_len + data_len + 1, ENGINE_MEM_SCRATCH);
	if (!buf)
		return NULL;
	pos = 0;
	while (mo3_ogg_next_page(head, head_len, &pos, &start, &size)) {
		uint32_t s = mo3_ogg_serial(head + start), new_serial, crc;
		uint8_t *page = buf + out;
		size_t i;

		for (i = 0; i < num_head && head_serials[i] != s; i++);
		if (i == num_head && num_head < ARRAY_SIZE(head_serials))
			head_serials[num_head++] = s;

		if (i < num_data) {
			new_serial = data_serials[i];
		} else {
			/* no matching stream in the data; pick an unused serial */
			size_t extra = i - num_data, k;
			for (new_serial = 1; new_serial < 0xFFFFFFFF; new_serial++) {
				int used = 0;
				for (k = 0; k < num_data; k++)
					used |= (data_serials[k] == new_serial);
				if (!used && extra-- == 0)
					break;
			}
		}

		memcpy(page, head + start, size);
		page[14] = new_serial & 0xFF;
		page[15] = (new_serial >> 8) & 0xFF;
		page[16] = (new_serial >> 16) & 0xFF;
		page[17] = (new_serial >> 24) & 0xFF;
		memset(page + 22, 0, 4);
		crc = mo3_ogg_crc(page, size, 0);
		page[22] = crc & 0xFF;
		page[23] = (crc >> 8) & 0xFF;
		page[24] = (crc >> 16) & 0xFF;
		page[25] = (crc >> 24) & 0xFF;
		out += size;
	}

	memcpy(buf + out, data, data_len);
	*out_len = out + data_len;
	return buf;
}

/* FFmpeg trims the end of a Vorbis stream against the last granule position
 * in a way that loses the final short block of these samples (libvorbis
 * keeps it). The sample header has the real length and the decoded data
 * gets cut to it anyway, so move the last granule position out of the way. */
static void mo3_ogg_extend_last_granule(uint8_t *d, size_t len)
{
	size_t pos = 0, start, size, last = 0, last_size = 0;
	uint64_t granule;
	uint32_t crc;
	int i, found = 0;

	while (mo3_ogg_next_page(d, len, &pos, &start, &size)) {
		last = start;
		last_size = size;
		found = 1;
	}
	if (!found)
		return;

	granule = 0;
	for (i = 7; i >= 0; i--)
		granule = (granule << 8) | d[last + 6 + i];
	if (granule >= UINT64_C(0x7FFFFFFFFFFF0000))
		return; /* -1 (no position) or something absurd */
	granule += 8192;
	for (i = 0; i < 8; i++)
		d[last + 6 + i] = (granule >> (8 * i)) & 0xFF;

	memset(d + last + 22, 0, 4);
	crc = mo3_ogg_crc(d + last, last_size, 0);
	for (i = 0; i < 4; i++)
		d[last + 22 + i] = (crc >> (8 * i)) & 0xFF;
}

static int mo3_load_vorbis(song_sample_t *smp, const uint8_t *data, size_t length,
	const uint8_t *head, size_t head_len, const char **decoder)
{
	uint8_t *stream;
	int16_t *pcm;
	uint32_t frames;
	int channels, ok, want_channels;

	if (head) {
		stream = mo3_ogg_merge(head, head_len, data, length, &length);
	} else {
		stream = engine_alloc(length + 1, ENGINE_MEM_SCRATCH);
		if (stream)
			memcpy(stream, data, length);
	}
	if (!stream)
		return 0;
	mo3_ogg_extend_last_granule(stream, length);
	ok = mo3_decode_external(stream, length, &pcm, &frames, &channels, decoder);
	engine_free(stream, ENGINE_MEM_SCRATCH);
	if (!ok)
		return 0;

	/* Vorbis samples keep the length and format from the sample header.
	 * Decoded data with the wrong channel count is thrown away, which is
	 * what libopenmpt does as well. */
	want_channels = (smp->flags & CHN_STEREO) ? 2 : 1;
	smp->data = csf_allocate_sample(smp->length * want_channels * ((smp->flags & CHN_16BIT) ? 2 : 1));
	if (!smp->data) {
		engine_free(pcm, ENGINE_MEM_SCRATCH);
		return 0;
	}
	if (channels == want_channels)
		mo3_store_pcm(smp, pcm, MIN(frames, smp->length), channels);
	engine_free(pcm, ENGINE_MEM_SCRATCH);
	return 1;
}

/* Fills in silence for a sample that couldn't be decoded, so the song
 * still loads with the loop points and so on intact. */
static void mo3_silence_sample(song_sample_t *smp)
{
	uint32_t bytes = smp->length * ((smp->flags & CHN_16BIT) ? 2 : 1) * ((smp->flags & CHN_STEREO) ? 2 : 1);

	if (smp->data || !smp->length)
		return;
	smp->data = csf_allocate_sample(bytes);
	if (!smp->data)
		smp->length = 0;
}

/* --------------------------------------------------------------------- */

static void mo3_set_macro(char *dst, const char *fmt, int a, int b)
{
	snprintf(dst, MAX_MIDI_MACRO, fmt, a, b);
}

int fmt_mo3_load_song(song_t *song, slurp_t *fp, uint32_t lflags)
{
	static const char *const format_names[] = {
		[MO3_TYPE_XM] = "FastTracker 2",
		[MO3_TYPE_IT] = "Impulse Tracker",
		[MO3_TYPE_S3M] = "Scream Tracker 3",
		[MO3_TYPE_MOD] = "Protracker",
		[MO3_TYPE_MTM] = "MultiTracker",
	};
	uint8_t hdr[8];
	uint32_t music_size, got, compressed_size = 0;
	uint8_t version;
	uint8_t *file_data = NULL, *music = NULL;
	size_t file_length, sample_pos;
	struct mo3_reader src, m;
	struct mo3_file_header fh;
	struct mo3_sample *samples = NULL;
	struct mo3_instrument *xm_vibrato = NULL;
	enum mo3_type type;
	char made_with[64] = "";
	uint16_t cwtv = 0, cmwt = 0;
	int is_sample_mode, frequency_is_hertz, any_macros = 0;
	int num_ins, num_smp, n, ret = LOAD_FORMAT_ERROR;
	int unsupported = 0, silenced = 0, plugins = 0, ompt = 0;
	const char *decoder_used = NULL;

	if (slurp_read(fp, hdr, 8) != 8 || !mo3_check_header(hdr, &music_size))
		return LOAD_UNSUPPORTED;
	version = hdr[3];

	file_length = slurp_length(fp);
	file_data = engine_alloc(file_length + 1, ENGINE_MEM_SCRATCH);
	if (!file_data)
		return LOAD_FILE_ERROR;
	slurp_rewind(fp);
	if (slurp_read(fp, file_data, file_length) != file_length) {
		engine_free(file_data, ENGINE_MEM_SCRATCH);
		return LOAD_FILE_ERROR;
	}

	mo3r_init(&src, file_data, file_length);
	src.pos = 8;
	if (version >= 5) {
		compressed_size = mo3r_u32(&src);
		if (!mo3r_can_read(&src, compressed_size))
			goto fail;
	}

	music = mo3_unpack(&src, music_size, &got);
	if (!music)
		goto fail;
	mo3r_init(&m, music, got);

	mo3r_string(&m, song->title, sizeof(song->title));
	mo3r_string(&m, song->message, MAX_MESSAGE + 1);

	if (!mo3_read_file_header(&m, &fh)
	    || fh.num_channels == 0 || fh.num_channels > 127
	    || fh.restart_pos > fh.num_orders
	    || fh.num_instruments >= 256
	    || fh.num_samples >= 4000)
		goto fail;

	type = mo3_get_type(fh.flags);

	/* song settings */
	song->initial_speed = fh.default_speed ? fh.default_speed : 6;
	song->initial_tempo = fh.default_tempo ? fh.default_tempo : 125;
	song->initial_tempo = CLAMP(song->initial_tempo, 31, 255);

	switch (type) {
	case MO3_TYPE_IT:
		song->initial_global_volume = MIN(fh.global_vol, 128);
		song->pan_separation = fh.pan_separation;
		if (!(fh.flags & MO3_IT_OLD_FX))
			song->flags |= SONG_ITOLDEFFECTS;
		if (!(fh.flags & MO3_IT_COMPAT_GXX))
			song->flags |= SONG_COMPATGXX;
		break;
	case MO3_TYPE_S3M:
		song->initial_global_volume = MIN(fh.global_vol, 64) * 2;
		song->pan_separation = 128;
		song->flags |= SONG_ITOLDEFFECTS;
		break;
	case MO3_TYPE_XM:
		song->initial_global_volume = 128;
		song->pan_separation = 128;
		song->flags |= SONG_ITOLDEFFECTS | SONG_COMPATGXX | SONG_INSTRUMENTMODE;
		break;
	case MO3_TYPE_MOD:
	case MO3_TYPE_MTM:
		song->initial_global_volume = 128;
		song->pan_separation = (type == MO3_TYPE_MOD) ? 64 : 128;
		song->flags |= SONG_ITOLDEFFECTS | SONG_COMPATGXX;
		break;
	}
	if (fh.flags & MO3_LINEAR_SLIDES)
		song->flags |= SONG_LINEARSLIDES;

	is_sample_mode = (type != MO3_TYPE_XM && !(fh.flags & MO3_INSTRUMENT_MODE));
	if (!is_sample_mode)
		song->flags |= SONG_INSTRUMENTMODE;

	/* this is how libopenmpt derives the sample pre-amp, which is the same
	 * thing as Schism's mixing volume */
	{
		int preamp;
		if (fh.sample_volume < 0)
			preamp = fh.sample_volume + 52;
		else
			preamp = (int)exp(fh.sample_volume * 3.1 / 20.0) + 51;
		song->mixing_volume = CLAMP(preamp, 0, 128);
	}

	for (n = 0; n < MAX_CHANNELS; n++) {
		song_channel_t *chn = song->channels + n;

		chn->volume = 64;
		chn->panning = 128;
		chn->flags = 0;
		if (n >= fh.num_channels) {
			chn->flags |= CHN_MUTE;
			continue;
		}
		if (type == MO3_TYPE_IT)
			chn->volume = MIN(fh.chn_volume[n], 64);
		if (type != MO3_TYPE_XM) {
			if (fh.chn_pan[n] == 127)
				chn->flags |= CHN_SURROUND;
			else if (fh.chn_pan[n] == 255)
				chn->panning = 256;
			else
				chn->panning = fh.chn_pan[n];
		}
	}
	if (fh.num_channels > MAX_CHANNELS)
		log_appendf(4, " Warning: Song has %d channels, only %d loaded", fh.num_channels, MAX_CHANNELS);

	/* MIDI macros from the header */
	for (n = 0; n < 16; n++)
		if (fh.sfx_macros[n])
			any_macros = 1;
	for (n = 0; n < 128; n++)
		if (fh.fixed_macros[n][1])
			any_macros = 1;
	if (any_macros) {
		for (n = 0; n < 16; n++) {
			if (fh.sfx_macros[n])
				mo3_set_macro(song->midi_config.sfx[n], "F0F0%02Xz", fh.sfx_macros[n] - 1, 0);
			else
				song->midi_config.sfx[n][0] = '\0';
		}
		for (n = 0; n < 128; n++) {
			if (fh.fixed_macros[n][1])
				mo3_set_macro(song->midi_config.zxx[n], "F0F0%02X%02X",
					fh.fixed_macros[n][1] - 1, fh.fixed_macros[n][0]);
			else
				song->midi_config.zxx[n][0] = '\0';
		}
	}

	/* orders */
	{
		const int separators = (type != MO3_TYPE_MOD && type != MO3_TYPE_XM);
		for (n = 0; n < fh.num_orders; n++) {
			uint8_t o = mo3r_u8(&m);
			if (n >= MAX_ORDERS)
				continue;
			if (separators && (o == 0xFF || o == 0xFE))
				song->orderlist[n] = (o == 0xFF) ? ORDER_LAST : ORDER_SKIP;
			else
				song->orderlist[n] = (o >= MAX_PATTERNS) ? ORDER_SKIP : o;
		}
		if (fh.num_orders > MAX_ORDERS)
			log_appendf(4, " Warning: Too many orders in the order list (%d skipped)", fh.num_orders - MAX_ORDERS);
	}

	if (!mo3_load_patterns(song, &m, &fh, type, lflags))
		goto fail;

	/* instruments; their headers are there even in sample mode */
	num_ins = fh.num_instruments;
	if (type == MO3_TYPE_XM) {
		xm_vibrato = engine_calloc(num_ins ? num_ins : 1, sizeof(*xm_vibrato), ENGINE_MEM_SCRATCH);
		if (!xm_vibrato)
			goto fail;
	}
	for (n = 1; n <= num_ins; n++) {
		struct mo3_instrument mi;
		char name[64], filename[64];
		song_instrument_t *ins;

		mo3r_string(&m, name, sizeof(name));
		filename[0] = '\0';
		if (version >= 5)
			mo3r_string(&m, filename, sizeof(filename));
		if (!mo3_read_instrument(&m, &mi))
			goto fail;
		if (is_sample_mode)
			continue;
		if (xm_vibrato)
			xm_vibrato[n - 1] = mi;
		if (n >= MAX_INSTRUMENTS) {
			if (n == MAX_INSTRUMENTS)
				log_appendf(4, " Warning: Too many instruments in file");
			continue;
		}

		ins = song->instruments[n] = csf_allocate_instrument();
		if (!ins)
			goto fail;
		strncpy(ins->name, name, sizeof(ins->name) - 1);
		strncpy(ins->filename, filename, sizeof(ins->filename) - 1);
		mo3_convert_instrument(&mi, ins, type);

		if (mi.midi_channel >= 128)
			plugins = 1;
	}

	/* sample headers */
	num_smp = fh.num_samples;
	frequency_is_hertz = (version >= 5 || !(fh.flags & MO3_LINEAR_SLIDES));
	samples = engine_calloc(num_smp ? num_smp : 1, sizeof(*samples), ENGINE_MEM_SCRATCH);
	if (!samples)
		goto fail;
	for (n = 1; n <= num_smp; n++) {
		struct mo3_sample *ms = samples + n - 1;
		char name[64], filename[64];

		mo3r_string(&m, name, sizeof(name));
		filename[0] = '\0';
		if (version >= 5)
			mo3r_string(&m, filename, sizeof(filename));
		if (!mo3_read_sample(&m, ms))
			goto fail;
		if (version >= 5 && (ms->flags & MO3_SMP_COMPRESSION_MASK) == MO3_SMP_SHARED_OGG)
			ms->shared_header = (int16_t)mo3r_u16(&m);

		if (n < MAX_SAMPLES) {
			song_sample_t *smp = song->samples + n;
			strncpy(smp->name, name, sizeof(smp->name) - 1);
			strncpy(smp->filename, filename, sizeof(smp->filename) - 1);
			mo3_convert_sample(ms, smp, type, frequency_is_hertz);
		} else if (n == MAX_SAMPLES) {
			log_appendf(4, " Warning: Too many samples in file");
		}
	}

	/* XM keeps auto-vibrato in the instrument; hand it to every sample the
	 * instrument uses, like the XM loader does */
	if (xm_vibrato) {
		for (n = 1; n <= num_ins && n < MAX_INSTRUMENTS; n++) {
			const struct mo3_instrument *mi = xm_vibrato + n - 1;
			song_instrument_t *ins = song->instruments[n];
			int vtype, vsweep, vdepth, vrate, k;

			if (!ins)
				continue;
			vtype = mo3_xm_autovib[mi->vib_type & 7];
			vsweep = mi->vib_sweep;
			vdepth = MIN(mi->vib_depth, 32);
			vrate = MIN(mi->vib_rate, 64);
			if (vrate | vdepth) {
				if (vsweep) {
					int32_t s = _muldivr(vdepth, 256, vsweep);
					vsweep = CLAMP(s, 0, 255);
				} else {
					vsweep = 255;
				}
			}
			for (k = 0; k < 120; k++) {
				const int s = ins->sample_map[k];
				if (s && s <= num_smp) {
					song->samples[s].vib_type = vtype;
					song->samples[s].vib_rate = vsweep;
					song->samples[s].vib_depth = vdepth;
					song->samples[s].vib_speed = vrate;
				}
			}
		}
	}

	/* plugin data; Schism has no use for it */
	if ((fh.flags & MO3_HAS_PLUGINS) && mo3r_can_read(&m, 1)) {
		const uint8_t plugin_flags = mo3r_u8(&m);
		plugins = 1;
		if (plugin_flags & 1) {
			/* channel plugin assignments, one per channel */
			m.pos += MIN(4 * (size_t)fh.num_channels, m.length - m.pos);
		}
		while (mo3r_can_read(&m, 1)) {
			uint32_t len;
			if (!mo3r_u8(&m))
				break;
			len = mo3r_u32(&m);
			if (len >= music_size || music_size - len < m.pos)
				goto fail;
			mo3r_chunk(&m, len);
		}
	}

	/* extra chunks */
	while (mo3r_can_read(&m, 8)) {
		const uint32_t id = mo3r_u32(&m);
		const uint32_t len = mo3r_u32(&m);
		struct mo3_reader chunk;

		if (len >= music_size || music_size - len < m.pos)
			goto fail;
		chunk = mo3r_chunk(&m, len);

		switch (id) {
		case 0x53524556: /* VERS */
			switch (type) {
			case MO3_TYPE_IT:
				cwtv = mo3r_u16(&chunk);
				cmwt = mo3r_u16(&chunk);
				break;
			case MO3_TYPE_S3M:
				cwtv = mo3r_u16(&chunk);
				break;
			case MO3_TYPE_XM: {
				size_t l = MIN(chunk.length, 32);
				memcpy(made_with, chunk.data, l);
				made_with[l] = '\0';
				str_rtrim(made_with);
				break;
			}
			case MO3_TYPE_MTM: {
				uint8_t v = mo3r_u8(&chunk);
				snprintf(made_with, sizeof(made_with), "MultiTracker %d.%d", v >> 4, v & 0x0F);
				break;
			}
			default:
				break;
			}
			break;
		case 0x49485250: /* PRHI */
			song->row_highlight_minor = mo3r_u8(&chunk);
			song->row_highlight_major = mo3r_u8(&chunk);
			break;
		case 0x4944494D: { /* MIDI */
			slurp_t mfp;
			if (slurp_memstream(&mfp, chunk.data, chunk.length) >= 0) {
				if (it_read_midi_config(&song->midi_config, &mfp))
					song->flags |= SONG_EMBEDMIDICFG;
				unslurp(&mfp);
			}
			break;
		}
		case 0x54504D4F: /* OMPT */
			/* pattern and channel names, extended instrument and song
			 * properties; nothing Schism can store */
			ompt = 1;
			break;
		default:
			break;
		}
	}

	if ((type == MO3_TYPE_IT && cwtv >= 0x0100 && cwtv < 0x0214)
	    || (type == MO3_TYPE_S3M && cwtv >= 0x3100 && cwtv < 0x3214)
	    || (type == MO3_TYPE_S3M && cwtv >= 0x1300 && cwtv < 0x1320)) {
		/* old IT and ST3 versions ignore MIDI data */
		for (n = 0; n < 128; n++)
			song->midi_config.zxx[n][0] = '\0';
	}

	if (type == MO3_TYPE_IT && cmwt)
		snprintf(made_with, sizeof(made_with), "Impulse Tracker %d.%02x", cmwt >> 8, cmwt & 0xFF);
	if (made_with[0])
		snprintf(song->tracker_id, sizeof(song->tracker_id), "MO3 v%d (%s)", version, made_with);
	else
		snprintf(song->tracker_id, sizeof(song->tracker_id), "MO3 v%d (%s)", version, format_names[type]);

	if (type != MO3_TYPE_IT && type != MO3_TYPE_S3M && fh.restart_pos)
		csf_insert_restart_pos(song, fh.restart_pos);

	if (lflags & LOAD_NOSAMPLES) {
		ret = LOAD_SUCCESS;
		goto done;
	}

	/* Sample data. Before version 5 the packed size isn't stored, so the
	 * samples start wherever the unpacker stopped reading. */
	if (version < 5) {
		if (got != music_size)
			goto fail;
		sample_pos = src.pos;
	} else {
		sample_pos = 12 + (size_t)compressed_size;
	}
	src.pos = MIN(sample_pos, src.length);

	/* first pass: raw samples get read in place, compressed ones are just
	 * located, since an Ogg sample can borrow the header of a later one */
	for (n = 1; n <= num_smp; n++) {
		struct mo3_sample *ms = samples + n - 1;
		const uint32_t compression = ms->flags & MO3_SMP_COMPRESSION_MASK;

		if (!compression && ms->compressed_size == 0) {
			const int bps = ((ms->flags & MO3_SMP_16BIT) ? 2 : 1) * ((ms->flags & MO3_SMP_STEREO) ? 2 : 1);
			const size_t want = (size_t)ms->length * bps;
			const size_t have = MIN(want, src.length - src.pos);

			if (n < MAX_SAMPLES && ms->length) {
				song_sample_t *smp = song->samples + n;
				slurp_t sfp;
				uint8_t *tmp = engine_calloc(want ? want : 1, 1, ENGINE_MEM_SCRATCH);

				if (!tmp)
					goto fail;
				memcpy(tmp, src.data + src.pos, have);
				if (slurp_memstream(&sfp, tmp, want) >= 0) {
					csf_read_sample(smp, SF_LE | SF_PCMS
						| ((ms->flags & MO3_SMP_16BIT) ? SF_16 : SF_8)
						| ((ms->flags & MO3_SMP_STEREO) ? SF_SS : SF_M), &sfp);
					unslurp(&sfp);
				}
				engine_free(tmp, ENGINE_MEM_SCRATCH);
			}
			src.pos += have;
		} else if (ms->compressed_size > 0) {
			ms->chunk = mo3r_chunk(&src, ms->compressed_size);
			ms->have_chunk = 1;
		}
	}

	/* second pass: everything else */
	for (n = 1; n <= num_smp && n < MAX_SAMPLES; n++) {
		struct mo3_sample *ms = samples + n - 1;
		song_sample_t *smp = song->samples + n;
		const uint32_t compression = ms->flags & MO3_SMP_COMPRESSION_MASK;
		const char *decoder = NULL;

		if (ms->compressed_size < 0 && n + ms->compressed_size > 0) {
			/* a copy of an earlier sample */
			const song_sample_t *orig = song->samples + n + ms->compressed_size;
			uint32_t bytes;

			smp->length = orig->length;
			smp->flags = (smp->flags & ~(CHN_16BIT | CHN_STEREO | CHN_ADLIB))
				| (orig->flags & (CHN_16BIT | CHN_STEREO | CHN_ADLIB));
			memcpy(smp->adlib_bytes, orig->adlib_bytes, sizeof(smp->adlib_bytes));
			if (orig->data) {
				bytes = orig->length * ((orig->flags & CHN_16BIT) ? 2 : 1) * ((orig->flags & CHN_STEREO) ? 2 : 1);
				if (orig->flags & CHN_ADLIB)
					bytes = 1;
				smp->data = csf_allocate_sample(bytes);
				if (smp->data)
					memcpy(smp->data, orig->data, bytes);
				else
					smp->length = 0;
			}
			continue;
		}

		if (!ms->length || !ms->have_chunk)
			continue;

		if (ms->flags & MO3_SMP_16BIT)
			smp->flags |= CHN_16BIT;
		if (ms->flags & MO3_SMP_STEREO)
			smp->flags |= CHN_STEREO;

		if (compression == MO3_SMP_DELTA || compression == MO3_SMP_DELTA_PREDICTION) {
			const int channels = (smp->flags & CHN_STEREO) ? 2 : 1;
			const int is16 = (smp->flags & CHN_16BIT) ? 1 : 0;
			struct mo3_reader r = ms->chunk;
			/* at best, MO3 packs a sample point into two bits */
			const uint64_t max_length = (uint64_t)ms->chunk.length * (4 / channels);

			if (smp->length > max_length)
				smp->length = max_length;
			if (smp->length > MAX_SAMPLE_LENGTH)
				smp->length = MAX_SAMPLE_LENGTH;

			smp->data = csf_allocate_sample(smp->length * channels * (is16 ? 2 : 1));
			if (smp->data) {
				r.pos = 0;
				if (compression == MO3_SMP_DELTA)
					mo3_unpack_delta(&r, smp->data, smp->length, channels, is16);
				else
					mo3_unpack_delta_prediction(&r, smp->data, smp->length, channels, is16);
			} else {
				smp->length = 0;
			}
		} else if (compression == MO3_SMP_COMPRESSION_OGG || compression == MO3_SMP_SHARED_OGG) {
			const uint16_t shared_size = ms->encoder_delay;
			const int shared_index = (n + ms->shared_header > 0) ? n + ms->shared_header : n;
			const int shared = (shared_index != n && shared_index > 0 && shared_index <= num_smp
				&& shared_size > 0);
			const uint8_t *head = NULL;

			if (shared) {
				const struct mo3_sample *hs = samples + shared_index - 1;
				if (!hs->have_chunk || hs->chunk.length < shared_size) {
					unsupported = 1;
					mo3_silence_sample(smp);
					silenced++;
					continue;
				}
				head = hs->chunk.data;
			}
			if (mo3_load_vorbis(smp, ms->chunk.data, ms->chunk.length, head, shared_size, &decoder)) {
				decoder_used = decoder;
			} else {
				unsupported = 1;
				mo3_silence_sample(smp);
				silenced++;
			}
		} else if (compression == MO3_SMP_COMPRESSION_MPEG) {
			if (mo3_load_mpeg(smp, ms, ms->chunk.data, ms->chunk.length, &decoder)) {
				decoder_used = decoder;
			} else {
				unsupported = 1;
				mo3_silence_sample(smp);
				silenced++;
			}
		} else if (compression == MO3_SMP_OPL) {
			if (ms->chunk.length >= 12) {
				memcpy(smp->adlib_bytes, ms->chunk.data, 12);
				smp->flags |= CHN_ADLIB;
				smp->flags &= ~(CHN_16BIT | CHN_STEREO);
				smp->length = 1;
				smp->data = csf_allocate_sample(1);
			}
		} else {
			unsupported = 1;
			mo3_silence_sample(smp);
			silenced++;
		}
	}

	if (decoder_used)
		log_appendf(5, " Compressed samples decoded with %s", decoder_used);
	if (unsupported)
		log_appendf(4, " Warning: %d compressed sample%s could not be decoded and will be silent",
			silenced, silenced == 1 ? "" : "s");

	ret = LOAD_SUCCESS;

done:
	if (plugins)
		log_appendf(4, " Warning: Plugin data ignored");
	if (ompt)
		log_appendf(4, " Warning: OpenMPT extensions ignored");
	if (fh.flags & MO3_S3M_FAST_SLIDES && type == MO3_TYPE_S3M)
		log_appendf(4, " Warning: Fast volume slides are not supported");
	if (fh.flags & MO3_MOD_VBLANK)
		log_appendf(4, " Warning: VBlank timing is not supported");

fail:
	engine_free(samples, ENGINE_MEM_SCRATCH);
	engine_free(xm_vibrato, ENGINE_MEM_SCRATCH);
	engine_free(music, ENGINE_MEM_SCRATCH);
	engine_free(file_data, ENGINE_MEM_SCRATCH);
	return ret;
}
