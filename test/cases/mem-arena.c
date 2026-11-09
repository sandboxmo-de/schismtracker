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

#include "mem-arena.h"

/* the arena's memory; a union keeps it aligned */
static union {
	unsigned char bytes[64 * 1024];
	void *align_p;
	double align_d;
} arena_mem;

static struct mem_arena arena;
static struct engine_allocator alloc;

static void setup(size_t song, size_t scratch, size_t engine)
{
	mem_arena_init(&arena, arena_mem.bytes, sizeof(arena_mem.bytes), song, scratch, engine);
	mem_arena_get_allocator(&arena, &alloc);
}

#define A(size, cls) alloc.alloc(alloc.ctx, (size), ENGINE_ALIGN_DEFAULT, (cls))
#define F(ptr, cls) alloc.free(alloc.ctx, (ptr), (cls))
#define R(ptr, size, cls) alloc.realloc(alloc.ctx, (ptr), (size), ENGINE_ALIGN_DEFAULT, (cls))

#define SONG ENGINE_MEM_SONG
#define SCRATCH ENGINE_MEM_SCRATCH

testresult_t test_mem_arena_split(void)
{
	size_t total;

	ASSERT(mem_arena_init(&arena, arena_mem.bytes, sizeof(arena_mem.bytes), 0, 0, 0) == 0);
	total = arena.r[SONG].size + arena.r[SCRATCH].size + arena.r[ENGINE_MEM_ENGINE].size;
	ASSERT(total <= sizeof(arena_mem.bytes));
	ASSERT(arena.r[SONG].size == 4 * arena.r[ENGINE_MEM_ENGINE].size);
	ASSERT(arena.r[SCRATCH].size == 3 * arena.r[ENGINE_MEM_ENGINE].size);

	/* explicit sizes are kept; the one left out gets the rest */
	ASSERT(mem_arena_init(&arena, arena_mem.bytes, sizeof(arena_mem.bytes), 32768, 16384, 0) == 0);
	ASSERT(arena.r[SONG].size == 32768);
	ASSERT(arena.r[SCRATCH].size == 16384);
	ASSERT(arena.r[ENGINE_MEM_ENGINE].size == 16384);

	/* too big */
	ASSERT(mem_arena_init(&arena, arena_mem.bytes, sizeof(arena_mem.bytes), 65536, 16, 16) < 0);
	/* nothing left for the engine */
	ASSERT(mem_arena_init(&arena, arena_mem.bytes, sizeof(arena_mem.bytes), 32768, 32768, 0) < 0);

	RETURN_PASS;
}

testresult_t test_mem_arena_alignment(void)
{
	unsigned char *p, *q;

	setup(0, 0, 0);

	p = A(3, SONG);
	q = A(5, SONG);
	ASSERT(p && q);
	ASSERT(((uintptr_t)p % ENGINE_ALIGN_DEFAULT) == 0);
	ASSERT(((uintptr_t)q % ENGINE_ALIGN_DEFAULT) == 0);
	ASSERT(q >= p + 3);

	p = alloc.alloc(alloc.ctx, 100, 256, SONG);
	ASSERT(p && ((uintptr_t)p % 256) == 0);

	/* not a power of two */
	ASSERT(!alloc.alloc(alloc.ctx, 100, 24, SONG));

	RETURN_PASS;
}

testresult_t test_mem_arena_free_rolls_back(void)
{
	void *a, *b, *c;
	size_t top;

	setup(0, 0, 0);

	a = A(100, SONG);
	top = arena.r[SONG].top;
	b = A(100, SONG);
	F(b, SONG);
	/* freeing the newest block gives its space back */
	ASSERT(arena.r[SONG].top == top);
	c = A(100, SONG);
	ASSERT(c == b);

	/* freeing an older block doesn't, until the newer ones go too */
	b = A(100, SONG);
	F(c, SONG);
	ASSERT(arena.r[SONG].top > top);
	F(b, SONG);
	ASSERT(arena.r[SONG].top == top);
	F(a, SONG);
	ASSERT(arena.r[SONG].top == 0);
	ASSERT(arena.r[SONG].live == 0);

	RETURN_PASS;
}

testresult_t test_mem_arena_realloc(void)
{
	unsigned char *a, *b, *c;
	size_t i;

	setup(0, 0, 0);

	a = A(16, SONG);
	for (i = 0; i < 16; i++)
		a[i] = (unsigned char)i;

	/* the newest block grows in place */
	b = R(a, 1000, SONG);
	ASSERT(b == a);
	ASSERT(arena.r[SONG].top >= 1000);

	/* an older one moves, with its contents */
	c = A(16, SONG);
	b = R(a, 2000, SONG);
	ASSERT(b && b != a && b > c);
	for (i = 0; i < 16; i++)
		ASSERT(b[i] == i);

	/* too big: NULL, and the block is still there */
	ASSERT(!R(b, sizeof(arena_mem.bytes), SONG));
	ASSERT(b[15] == 15);

	RETURN_PASS;
}

testresult_t test_mem_arena_out_of_memory(void)
{
	void *p;
	size_t top;

	setup(0, 0, 0);

	p = A(1000, SONG);
	ASSERT(p);
	top = arena.r[SONG].top;
	ASSERT(!A(arena.r[SONG].size, SONG));
	ASSERT(arena.r[SONG].fails == 1);
	ASSERT(arena.r[SONG].top == top);
	/* other regions don't care */
	ASSERT(A(1000, SCRATCH));

	RETURN_PASS;
}

testresult_t test_mem_arena_song_end(void)
{
	setup(0, 0, 0);

	alloc.song_begin(alloc.ctx);
	ASSERT(A(1000, SONG));
	ASSERT(A(1000, SONG));
	ASSERT(arena.r[SONG].peak >= 2000);

	/* left over at song_end: counted, and the region is emptied */
	alloc.song_end(alloc.ctx);
	ASSERT(arena.r[SONG].top == 0);
	ASSERT(arena.r[SONG].live == 0);
	ASSERT(arena.song_leaks == 2);
	ASSERT(arena.r[SONG].peak >= 2000);

	RETURN_PASS;
}

testresult_t test_mem_arena_scratch_nesting(void)
{
	void *keep, *a, *b;
	size_t top0, top1;

	setup(0, 0, 0);

	keep = A(100, SCRATCH);
	top0 = arena.r[SCRATCH].top;

	alloc.scratch_begin(alloc.ctx);
	a = A(100, SCRATCH);
	top1 = arena.r[SCRATCH].top;

	alloc.scratch_begin(alloc.ctx);
	b = A(100, SCRATCH);
	F(b, SCRATCH);
	ASSERT(A(100, SCRATCH)); /* never freed */
	alloc.scratch_end(alloc.ctx);
	ASSERT(arena.r[SCRATCH].top == top1);
	ASSERT(arena.scratch_leaks == 1);

	F(a, SCRATCH);
	alloc.scratch_end(alloc.ctx);
	ASSERT(arena.r[SCRATCH].top == top0);
	ASSERT(arena.scratch_leaks == 1);

	/* a block from before the bracket, freed inside it */
	alloc.scratch_begin(alloc.ctx);
	a = A(100, SCRATCH);
	F(keep, SCRATCH);
	alloc.scratch_end(alloc.ctx);
	ASSERT(arena.r[SCRATCH].top == 0);
	ASSERT(arena.r[SCRATCH].live == 0);

	/* a block from before the bracket, grown in place inside it */
	keep = A(100, SCRATCH);
	alloc.scratch_begin(alloc.ctx);
	ASSERT(R(keep, 5000, SCRATCH) == keep);
	alloc.scratch_end(alloc.ctx);
	ASSERT(arena.r[SCRATCH].top >= 5000);
	ASSERT(arena.r[SCRATCH].live == 1);

	/* the region shrinks under the mark, and a new block lands there */
	alloc.scratch_begin(alloc.ctx);
	F(keep, SCRATCH);
	a = A(100, SCRATCH);
	alloc.scratch_end(alloc.ctx);
	ASSERT(arena.r[SCRATCH].live == 1);
	F(a, SCRATCH);
	ASSERT(arena.r[SCRATCH].top == 0);

	RETURN_PASS;
}

testresult_t test_mem_arena_engine_wrappers(void)
{
	char *s;
	int n;

	setup(0, 0, 0);
	engine_allocator_register(&alloc);

	s = engine_strdup("hello", SCRATCH);
	ASSERT(s && !strcmp(s, "hello"));
	ASSERT(arena.r[SCRATCH].live == 1);
	engine_free(s, SCRATCH);

	n = engine_asprintf(&s, SCRATCH, "%d-%s", 42, "x");
	ASSERT(n == 4 && !strcmp(s, "42-x"));
	engine_free(s, SCRATCH);
	ASSERT(arena.r[SCRATCH].top == 0);

	s = engine_calloc(10, 10, ENGINE_MEM_ENGINE);
	ASSERT(s && !s[99]);
	engine_free(s, ENGINE_MEM_ENGINE);

	/* overflow in calloc fails without asking the allocator */
	ASSERT(!engine_calloc(SIZE_MAX / 2, 4, ENGINE_MEM_ENGINE));

	engine_allocator_register(NULL);

	RETURN_PASS;
}
