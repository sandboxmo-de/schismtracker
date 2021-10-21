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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "slurp.h"
#include "util.h"

#include "headers.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* The dup's are because fclose closes its file descriptor even if the FILE* was acquired with fdopen, and when
the control gets back to slurp, it closes the fd (again). It doesn't seem to exist on Amiga OS though, so... */
#ifndef HAVE_DUP
# define dup(fd) fd
#endif


#pragma pack(push, 1)
typedef struct mm_header {
	char zirconia[8]; // "ziRCONia"
	uint16_t hdrsize;

	uint16_t version;
	uint16_t blocks;
	uint32_t filesize;
	uint32_t blktable;
	uint8_t glb_comp;
	uint8_t fmt_comp;
} mm_header_t;

typedef struct mm_block {
	uint32_t unpk_size;
	uint32_t pk_size;
	uint32_t xor_chk;
	uint16_t sub_blk;
	uint16_t flags;
	uint16_t tt_entries;
	uint16_t num_bits;
} mm_block_t;

typedef struct mm_subblock {
	uint32_t unpk_pos;
	uint32_t unpk_size;
} mm_subblock_t;
#pragma pack(pop)


// only used internally
typedef struct mm_bit_buffer {
	uint32_t bits, buffer;
	uint8_t *src, *end;
} mm_bit_buffer_t;


enum {
	MM_COMP   = 0x0001,
	MM_DELTA  = 0x0002,
	MM_16BIT  = 0x0004,
	MM_STEREO = 0x0100, // unused?
	MM_ABS16  = 0x0200,
	MM_ENDIAN = 0x0400, // unused?
};


static const uint32_t mm_8bit_commands[8] = { 0x01, 0x03, 0x07, 0x0F, 0x1E, 0x3C, 0x78, 0xF8 };

static const uint32_t mm_8bit_fetch[8] = { 3, 3, 3, 3, 2, 1, 0, 0 };

static const uint32_t mm_16bit_commands[16] = {
	0x0001, 0x0003, 0x0007, 0x000F, 0x001E, 0x003C, 0x0078, 0x00F0,
	0x01F0, 0x03F0, 0x07F0, 0x0FF0, 0x1FF0, 0x3FF0, 0x7FF0, 0xFFF0,
};

static const uint32_t mm_16bit_fetch[16] = { 4, 4, 4, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };



static uint32_t get_bits(mm_bit_buffer_t *bb, uint32_t bits)
{
	uint32_t d;
	if (!bits) return 0;
	while (bb->bits < 24) {
		bb->buffer |= ((bb->src < bb->end) ? *bb->src++ : 0) << bb->bits;
		bb->bits += 8;
	}
	d = bb->buffer & ((1 << bits) - 1);
	bb->buffer >>= bits;
	bb->bits -= bits;
	return d;
}


int mmcmp_unpack(uint8_t **data, size_t *length)
{
	size_t memlength;
	uint8_t *memfile;
	uint8_t *buffer;
	mm_header_t hdr;
	uint32_t *pblk_table;
	size_t filesize;
	uint32_t block, i;

	if (!data || !*data || !length || *length < 256) {
		return 0;
	}
	memlength = *length;
	memfile = *data;

	memcpy(&hdr, memfile, sizeof(hdr));
	hdr.hdrsize = bswapLE16(hdr.hdrsize);
	hdr.version = bswapLE16(hdr.version);
	hdr.blocks = bswapLE16(hdr.blocks);
	hdr.filesize = bswapLE32(hdr.filesize);
	hdr.blktable = bswapLE32(hdr.blktable);

	if (memcmp(hdr.zirconia, "ziRCONia", 8) != 0
	    || hdr.hdrsize < 14
	    || hdr.blocks == 0
	    || hdr.filesize < 16
	    || hdr.filesize > 0x8000000
	    || hdr.blktable >= memlength
	    || hdr.blktable + 4 * hdr.blocks > memlength) {
		return 0;
	}
	filesize = hdr.filesize;
	if ((buffer = calloc(1, (filesize + 31) & ~15)) == NULL)
		return 0;

	pblk_table = (uint32_t *) (memfile + hdr.blktable);

	for (block = 0; block < hdr.blocks; block++) {
		uint32_t pos = bswapLE32(pblk_table[block]);
		mm_subblock_t *psubblk = (mm_subblock_t *) (memfile + pos + 20);
		mm_block_t pblk;

		memcpy(&pblk, memfile + pos, sizeof(pblk));
		pblk.unpk_size = bswapLE32(pblk.unpk_size);
		pblk.pk_size = bswapLE32(pblk.pk_size);
		pblk.xor_chk = bswapLE32(pblk.xor_chk);
		pblk.sub_blk = bswapLE16(pblk.sub_blk);
		pblk.flags = bswapLE16(pblk.flags);
		pblk.tt_entries = bswapLE16(pblk.tt_entries);
		pblk.num_bits = bswapLE16(pblk.num_bits);

		if ((pos + 20 >= memlength)
		    || (pos + 20 + pblk.sub_blk * 8 >= memlength)) {
			break;
		}
		pos += 20 + pblk.sub_blk * 8;

		if (!(pblk.flags & MM_COMP)) {
			/* Data is not packed */
			for (i = 0; i < pblk.sub_blk; i++) {
				uint32_t unpk_pos = bswapLE32(psubblk->unpk_pos);
				uint32_t unpk_size = bswapLE32(psubblk->unpk_size);
				if ((unpk_pos > filesize)
				    || (unpk_pos + unpk_size > filesize)) {
					break;
				}
				memcpy(buffer + unpk_pos, memfile + pos, unpk_size);
				pos += unpk_size;
				psubblk++;
			}
		} else if (pblk.flags & MM_16BIT) {
			/* Data is 16-bit packed */
			uint16_t *dest = (uint16_t *) (buffer + bswapLE32(psubblk->unpk_pos));
			uint32_t size = bswapLE32(psubblk->unpk_size) >> 1;
			uint32_t destpos = 0;
			uint32_t numbits = pblk.num_bits;
			uint32_t subblk = 0, oldval = 0;

			mm_bit_buffer_t bb = {
				.bits = 0,
				.buffer = 0,
				.src = memfile + pos + pblk.tt_entries,
				.end = memfile + pos + pblk.pk_size,
			};

			while (subblk < pblk.sub_blk) {
				uint32_t newval = 0x10000;
				uint32_t d = get_bits(&bb, numbits + 1);

				if (d >= mm_16bit_commands[numbits]) {
					uint32_t fetch = mm_16bit_fetch[numbits];
					uint32_t newbits = get_bits(&bb, fetch)
						+ ((d - mm_16bit_commands[numbits]) << fetch);
					if (newbits != numbits) {
						numbits = newbits & 0x0F;
					} else {
						if ((d = get_bits(&bb, 4)) == 0x0F) {
							if (get_bits(&bb, 1))
								break;
							newval = 0xFFFF;
						} else {
							newval = 0xFFF0 + d;
						}
					}
				} else {
					newval = d;
				}
				if (newval < 0x10000) {
					newval = (newval & 1)
						? (uint32_t) (-(int32_t)((newval + 1) >> 1))
						: (uint32_t) (newval >> 1);
					if (pblk.flags & MM_DELTA) {
						newval += oldval;
						oldval = newval;
					} else if (!(pblk.flags & MM_ABS16)) {
						newval ^= 0x8000;
					}
					dest[destpos++] = bswapLE16((uint16_t) newval);
				}
				if (destpos >= size) {
					subblk++;
					destpos = 0;
					size = bswapLE32(psubblk[subblk].unpk_size) >> 1;
					dest = (uint16_t *)(buffer + bswapLE32(psubblk[subblk].unpk_pos));
				}
			}
		} else {
			/* Data is 8-bit packed */
			uint8_t *dest = buffer + bswapLE32(psubblk->unpk_pos);
			uint32_t size = bswapLE32(psubblk->unpk_size);
			uint32_t destpos = 0;
			uint32_t numbits = pblk.num_bits;
			uint32_t subblk = 0, oldval = 0;
			uint8_t *ptable = memfile + pos;

			mm_bit_buffer_t bb = {
				.bits = 0,
				.buffer = 0,
				.src = memfile + pos + pblk.tt_entries,
				.end = memfile + pos + pblk.pk_size,
			};

			while (subblk < pblk.sub_blk) {
				uint32_t newval = 0x100;
				uint32_t d = get_bits(&bb, numbits + 1);

				if (d >= mm_8bit_commands[numbits]) {
					uint32_t fetch = mm_8bit_fetch[numbits];
					uint32_t newbits = get_bits(&bb, fetch)
						+ ((d - mm_8bit_commands[numbits]) << fetch);
					if (newbits != numbits) {
						numbits = newbits & 0x07;
					} else {
						if ((d = get_bits(&bb, 3)) == 7) {
							if (get_bits(&bb, 1))
								break;
							newval = 0xFF;
						} else {
							newval = 0xF8 + d;
						}
					}
				} else {
					newval = d;
				}
				if (newval < 0x100) {
					int n = ptable[newval];
					if (pblk.flags & MM_DELTA) {
						n += oldval;
						oldval = n;
					}
					dest[destpos++] = (uint8_t) n;
				}
				if (destpos >= size) {
					subblk++;
					destpos = 0;
					size = bswapLE32(psubblk[subblk].unpk_size);
					dest = buffer + bswapLE32(psubblk[subblk].unpk_pos);
				}
			}
		}
	}
	*data = buffer;
	*length = filesize;
	return 1;
}

/* I hate this... */
#ifndef O_BINARY
# ifdef O_RAW
#  define O_BINARY O_RAW
# else
#  define O_BINARY 0
# endif
#endif

static void _slurp_closure_free(slurp_t *t)
{
	free(t->data);
}

/* --------------------------------------------------------------------- */

/* CHUNK is how much memory is allocated at once. Too large a number is a
 * waste of memory; too small means constantly realloc'ing.
 *
 * <mml> also, too large a number might take the OS more than an efficient number of reads to read in one
 *       hit -- which you could be processing/reallocing while waiting for the next bit
 * <mml> we had something for some proggy on the server that was sucking data off stdin
 * <mml> and had our resident c programmer and resident perl programmer competing for the fastest code
 * <mml> but, the c coder found that after a bunch of test runs with time, 64k worked out the best case
 * ...
 * <mml> but, on another system with a different block size, 64 blocks may still be efficient, but 64k
 *       might not be 64 blocks
 * (so maybe this should grab the block size from stat() instead...) */
#define CHUNK 65536

static int _slurp_stdio_pipe(slurp_t * t, int fd)
{
	int old_errno;
	FILE *fp;
	uint8_t *read_buf, *realloc_buf;
	size_t this_len;
	int chunks = 0;

	t->data = NULL;
	fp = fdopen(dup(fd), "rb");
	if (fp == NULL)
		return 0;

	do {
		chunks++;
		/* Have to cast away the const... */
		realloc_buf = realloc((void *) t->data, CHUNK * chunks);
		if (realloc_buf == NULL) {
			old_errno = errno;
			fclose(fp);
			free(t->data);
			errno = old_errno;
			return 0;
		}
		t->data = realloc_buf;
		read_buf = (void *) (t->data + (CHUNK * (chunks - 1)));
		this_len = fread(read_buf, 1, CHUNK, fp);
		if (this_len <= 0) {
			if (ferror(fp)) {
				old_errno = errno;
				fclose(fp);
				free(t->data);
				errno = old_errno;
				return 0;
			}
		}
		t->length += this_len;
	} while (this_len);
	fclose(fp);
	t->closure = _slurp_closure_free;
	return 1;
}

static int _slurp_stdio(slurp_t * t, int fd)
{
	int old_errno;
	FILE *fp;
	size_t got = 0, need, len;

	if (t->length == 0) {
		/* Hrmph. Probably a pipe or something... gotta do it the REALLY ugly way. */
		return _slurp_stdio_pipe(t, fd);
	}

	fp = fdopen(dup(fd), "rb");

	if (!fp)
		return 0;

	t->data = (uint8_t *) malloc(t->length);
	if (t->data == NULL) {
		old_errno = errno;
		fclose(fp);
		errno = old_errno;
		return 0;
	}

	/* Read the WHOLE thing -- fread might not get it all at once,
	 * so keep trying until it returns zero. */
	need = t->length;
	do {
		len = fread(t->data + got, 1, need, fp);
		if (len <= 0) {
			if (ferror(fp)) {
				old_errno = errno;
				fclose(fp);
				free(t->data);
				errno = old_errno;
				return 0;
			}

			if (need > 0) {
				/* short file */
				need = 0;
				t->length = got;
			}
		} else {
			got += len;
			need -= len;
		}
	} while (need > 0);

	fclose(fp);
	t->closure = _slurp_closure_free;
	return 1;
}

static long file_size(const char *filename)
{
	struct stat buf;

	if (stat(filename, &buf) < 0) {
		return EOF;
	}
	if (S_ISDIR(buf.st_mode)) {
		errno = EISDIR;
		return EOF;
	}
	return buf.st_size;
}

/* --------------------------------------------------------------------- */

static slurp_t *_slurp_open(const char *filename, struct stat * buf, size_t size)
{
	slurp_t *t;
	int fd, old_errno;

	if (buf && S_ISDIR(buf->st_mode)) {
		errno = EISDIR;
		return NULL;
	}

	t = (slurp_t *) mem_alloc(sizeof(slurp_t));
	if (t == NULL)
		return NULL;
	t->pos = 0;

	if (strcmp(filename, "-") == 0) {
		if (_slurp_stdio(t, STDIN_FILENO))
			return t;
		free(t);
		return NULL;
	}

	if (size <= 0) {
		size = (buf ? buf->st_size : file_size(filename));
	}

	fd = open(filename, O_RDONLY | O_BINARY);

	if (fd < 0) {
		free(t);
		return NULL;
	}

	t->length = size;

	if (_slurp_stdio(t, fd)) {
		close(fd);
		return t;
	}

	old_errno = errno;
	close(fd);
	free(t);
	errno = old_errno;
	return NULL;
}

slurp_t *slurp(const char *filename, struct stat * buf, size_t size)
{
	slurp_t *t = _slurp_open(filename, buf, size);
	uint8_t *mmdata;
	size_t mmlen;

	if (!t) {
		return NULL;
	}

	mmdata = t->data;
	mmlen = t->length;
	if (mmcmp_unpack(&mmdata, &mmlen)) {
		// clean up the existing data
		if (t->data && t->closure) {
			t->closure(t);
		}
		// and put the new stuff in
		t->length = mmlen;
		t->data = mmdata;
		t->closure = _slurp_closure_free;
	}

	// TODO re-add PP20 unpacker, possibly also handle other formats?

	return t;
}


void unslurp(slurp_t * t)
{
	if (!t)
		return;
	if (t->data && t->closure) {
		t->closure(t);
	}
	free(t);
}

/* --------------------------------------------------------------------- */

int slurp_seek(slurp_t *t, long offset, int whence)
{
	switch (whence) {
	default:
	case SEEK_SET:
		break;
	case SEEK_CUR:
		offset += t->pos;
		break;
	case SEEK_END:
		offset += t->length;
		break;
	}
	if (offset < 0 || (size_t) offset > t->length)
		return -1;
	t->pos = offset;
	return 0;
}

long slurp_tell(slurp_t *t)
{
	return (long) t->pos;
}

size_t slurp_read(slurp_t *t, void *ptr, size_t count)
{
	count = slurp_peek(t, ptr, count);
	t->pos += count;
	return count;
}

size_t slurp_peek(slurp_t *t, void *ptr, size_t count)
{
	size_t bytesleft = t->length - t->pos;
	if (count > bytesleft) {
		// short read -- fill in any extra bytes with zeroes
		size_t tail = count - bytesleft;
		count = bytesleft;
		memset(ptr + count, 0, tail);
	}
	if (count)
		memcpy(ptr, t->data + t->pos, count);
	return count;
}

int slurp_getc(slurp_t *t)
{
	return (t->pos < t->length) ? t->data[t->pos++] : EOF;
}

int slurp_eof(slurp_t *t)
{
	return t->pos >= t->length;
}

