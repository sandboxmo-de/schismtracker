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

/* Reference arena allocator; see include/mem-arena.h. */

#include "headers.h"

#include "mem-arena.h"

/* Every block has a small header in front of it, so free and realloc can
 * find its size and the block before it. */
struct arena_block {
	size_t start; /* region top before this block was allocated */
	size_t size;  /* bytes asked for */
	size_t prev;  /* header offset of the block before, or NO_BLOCK */
	size_t freed;
};

#define NO_BLOCK SIZE_MAX

/* the header size, rounded up so data after it stays aligned */
#define HDR_SIZE ((sizeof(struct arena_block) + ENGINE_ALIGN_DEFAULT - 1) & ~(ENGINE_ALIGN_DEFAULT - 1))

static inline struct arena_block *block_at(struct mem_arena_region *r, size_t off)
{
	return (struct arena_block *)(r->base + off);
}

static void region_set(struct mem_arena_region *r, unsigned char *base, size_t size)
{
	memset(r, 0, sizeof(*r));
	r->base = base;
	r->size = size;
	r->last = NO_BLOCK;
}

/* After a free, give back the space of freed blocks at the top. */
static void region_unwind(struct mem_arena_region *r)
{
	while (r->last != NO_BLOCK) {
		struct arena_block *b = block_at(r, r->last);

		if (!b->freed)
			break;

		r->top = b->start;
		r->last = b->prev;
	}
}

/* Drop every block that starts at or above mark, freed or not, and return
 * how many of them were never freed. */
static size_t region_cut(struct mem_arena_region *r, size_t mark)
{
	size_t n = 0;

	while (r->last != NO_BLOCK) {
		struct arena_block *b = block_at(r, r->last);

		if (b->start < mark)
			break;
		if (!b->freed)
			n++;
		r->last = b->prev;
	}

	if (r->last == NO_BLOCK) {
		r->top = 0;
	} else {
		/* the data follows the header directly */
		r->top = r->last + HDR_SIZE + block_at(r, r->last)->size;
	}
	r->live -= MIN(n, r->live);

	/* blocks under the cut may have been freed already */
	region_unwind(r);

	return n;
}

static void *region_alloc(struct mem_arena_region *r, size_t size, size_t align)
{
	uintptr_t base = (uintptr_t)r->base;
	uintptr_t data;
	size_t hdr_off, end;
	struct arena_block *b;

	if (align < ENGINE_ALIGN_DEFAULT)
		align = ENGINE_ALIGN_DEFAULT;
	if (align & (align - 1))
		goto fail;

	/* aligned address of the data, with room for the header in front */
	if (r->top > r->size || r->size - r->top < HDR_SIZE)
		goto fail;
	data = base + r->top + HDR_SIZE;
	data = (data + align - 1) & ~(uintptr_t)(align - 1);

	hdr_off = (size_t)(data - base) - HDR_SIZE;
	if (data - base > r->size || size > r->size - (size_t)(data - base))
		goto fail;
	end = (size_t)(data - base) + size;

	b = block_at(r, hdr_off);
	b->start = r->top;
	b->size = size;
	b->prev = r->last;
	b->freed = 0;

	r->top = end;
	r->last = hdr_off;
	r->live++;
	if (r->top > r->peak)
		r->peak = r->top;

	return (void *)data;

fail:
	r->fails++;
	return NULL;
}

static void region_free(struct mem_arena_region *r, void *ptr)
{
	struct arena_block *b = (struct arena_block *)((unsigned char *)ptr - HDR_SIZE);

	b->freed = 1;
	if (r->live)
		r->live--;
	region_unwind(r);
}

static void *region_realloc(struct mem_arena_region *r, void *ptr, size_t size, size_t align)
{
	struct arena_block *b = (struct arena_block *)((unsigned char *)ptr - HDR_SIZE);
	size_t data_off = (size_t)((unsigned char *)ptr - r->base);
	void *n;

	if ((size_t)((unsigned char *)b - r->base) == r->last) {
		/* the newest block: move the top */
		if (size > r->size - data_off) {
			r->fails++;
			return NULL;
		}
		b->size = size;
		r->top = data_off + size;
		if (r->top > r->peak)
			r->peak = r->top;
		return ptr;
	}

	n = region_alloc(r, size, align);
	if (!n)
		return NULL;
	memcpy(n, ptr, MIN(size, b->size));
	region_free(r, ptr);

	return n;
}

/* ------------------------------------------------------------------------ */

static void *arena_alloc(void *ctx, size_t size, size_t align, enum engine_mem_class cls)
{
	struct mem_arena *a = ctx;

	return region_alloc(&a->r[cls], size, align);
}

static void *arena_realloc(void *ctx, void *ptr, size_t size, size_t align, enum engine_mem_class cls)
{
	struct mem_arena *a = ctx;

	return region_realloc(&a->r[cls], ptr, size, align);
}

static void arena_free(void *ctx, void *ptr, enum engine_mem_class cls)
{
	struct mem_arena *a = ctx;

	region_free(&a->r[cls], ptr);
}

static void arena_song_begin(void *ctx)
{
	struct mem_arena *a = ctx;

	a->songs_open++;
}

static void arena_song_end(void *ctx)
{
	struct mem_arena *a = ctx;
	struct mem_arena_region *r = &a->r[ENGINE_MEM_SONG];

	if (a->songs_open > 0)
		a->songs_open--;
	if (a->songs_open)
		return;

	a->song_leaks += region_cut(r, 0);
}

static void arena_scratch_begin(void *ctx)
{
	struct mem_arena *a = ctx;
	struct mem_arena_region *r = &a->r[ENGINE_MEM_SCRATCH];

	if (a->scratch_depth < MEM_ARENA_MAX_SCRATCH_DEPTH)
		a->scratch_marks[a->scratch_depth] = r->top;
	a->scratch_depth++;
}

static void arena_scratch_end(void *ctx)
{
	struct mem_arena *a = ctx;
	struct mem_arena_region *r = &a->r[ENGINE_MEM_SCRATCH];

	if (a->scratch_depth <= 0)
		return;
	a->scratch_depth--;
	if (a->scratch_depth >= MEM_ARENA_MAX_SCRATCH_DEPTH)
		return;

	/* Anything allocated since scratch_begin starts at or above the mark.
	 * If the region shrank below the mark in between, blocks made after
	 * that sit under it and are left to be freed normally. */
	a->scratch_leaks += region_cut(r, a->scratch_marks[a->scratch_depth]);
}

/* ------------------------------------------------------------------------ */

int mem_arena_init(struct mem_arena *arena, void *mem, size_t size,
	size_t song, size_t scratch, size_t engine)
{
	unsigned char *p = mem;
	size_t fixed, rest;
	int shares = 0;

	memset(arena, 0, sizeof(*arena));

	/* keep each region aligned */
	song &= ~(ENGINE_ALIGN_DEFAULT - 1);
	scratch &= ~(ENGINE_ALIGN_DEFAULT - 1);
	engine &= ~(ENGINE_ALIGN_DEFAULT - 1);

	if (song > size || scratch > size - song || engine > size - song - scratch)
		return -1;
	fixed = song + scratch + engine;
	rest = size - fixed;

	/* ENGINE needs about the same amount whatever the song, so it gets a
	 * fixed size instead of a share. With SONG and SCRATCH both given it
	 * takes whatever they leave. */
	if (!engine) {
		if (song && scratch) {
			engine = rest & ~(ENGINE_ALIGN_DEFAULT - 1);
		} else {
			engine = MIN(MEM_ARENA_DEFAULT_ENGINE, rest / 2);
			engine &= ~(ENGINE_ALIGN_DEFAULT - 1);
		}
		rest -= engine;
	}

	if (!song) shares += 4;
	if (!scratch) shares += 3;

	if (shares) {
		/* SONG and SCRATCH get 4:3 of whatever's left */
		size_t unit = (rest / shares) & ~(ENGINE_ALIGN_DEFAULT - 1);

		if (!song) song = unit * 4;
		if (!scratch) scratch = unit * 3;
	}

	if (!song || !scratch || !engine)
		return -1;

	region_set(&arena->r[ENGINE_MEM_SONG], p, song);
	p += song;
	region_set(&arena->r[ENGINE_MEM_SCRATCH], p, scratch);
	p += scratch;
	region_set(&arena->r[ENGINE_MEM_ENGINE], p, engine);

	return 0;
}

void mem_arena_get_allocator(struct mem_arena *arena, struct engine_allocator *out)
{
	memset(out, 0, sizeof(*out));
	out->ctx = arena;
	out->flags = ENGINE_ALLOC_ONE_SONG;
	out->alloc = arena_alloc;
	out->realloc = arena_realloc;
	out->free = arena_free;
	out->song_begin = arena_song_begin;
	out->song_end = arena_song_end;
	out->scratch_begin = arena_scratch_begin;
	out->scratch_end = arena_scratch_end;
}
