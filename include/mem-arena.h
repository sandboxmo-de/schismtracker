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

#ifndef SCHISM_MEM_ARENA_H_
#define SCHISM_MEM_ARENA_H_

/* A bump allocator for hosts without a general-purpose heap. The caller
 * hands over one block of memory, which is split into an ENGINE, a SONG and
 * a SCRATCH region, one per lifetime class (include/engine-alloc.h).
 *
 * Inside a region, memory is handed out from the bottom up. Freeing the
 * most recent block gives its space back; freeing anything else only marks
 * it, and the space comes back once everything above it has been freed
 * too. realloc of the most recent block grows or shrinks it in place. The
 * SONG region is emptied when the last open song ends, and the SCRATCH
 * region goes back to where it was at the matching scratch_begin. The
 * ENGINE region is never emptied.
 *
 * Nothing here calls the libc heap. */

#include "headers.h"

#include "engine-alloc.h"

#define MEM_ARENA_MAX_SCRATCH_DEPTH 16

struct mem_arena_region {
	unsigned char *base;
	size_t size;
	size_t top;     /* bytes in use from base */
	size_t last;    /* offset of the newest block's header, or SIZE_MAX */
	size_t peak;    /* highest top seen */
	size_t live;    /* blocks allocated and not yet freed */
	size_t fails;   /* requests that didn't fit */
};

struct mem_arena {
	struct mem_arena_region r[ENGINE_MEM_CLASSES];
	int songs_open;
	int scratch_depth;
	/* SCRATCH top at each open scratch_begin */
	size_t scratch_marks[MEM_ARENA_MAX_SCRATCH_DEPTH];
	/* song_end or scratch_end found blocks that were never freed */
	size_t song_leaks, scratch_leaks;
};

/* What the ENGINE region gets when the caller leaves it to mem_arena_init.
 * The engine's own ENGINE memory is about 190 KB on a 64-bit build
 * (song_engine_mem_minimum() in song.h gives the exact figure), and live
 * audio output and the effects bus add some tens of KB on top. */
#define MEM_ARENA_DEFAULT_ENGINE (256 * 1024)

/* Split size bytes at mem into the three regions. Regions given as 0 share
 * whatever the others leave. If ENGINE is one of them it takes
 * MEM_ARENA_DEFAULT_ENGINE, or half of what's left if that's less, and
 * SONG and SCRATCH split the rest 4:3. A region that's the only one given
 * as 0 gets everything that's left.
 *
 * Returns 0 on success, -1 if the sizes don't fit or a region would be
 * empty. mem should be aligned to ENGINE_ALIGN_DEFAULT. */
int mem_arena_init(struct mem_arena *arena, void *mem, size_t size,
	size_t song, size_t scratch, size_t engine);

/* Fill in an allocator that serves the engine from arena. It sets
 * ENGINE_ALLOC_ONE_SONG, since the SONG region can only be emptied when no
 * song is left in it. */
void mem_arena_get_allocator(struct mem_arena *arena, struct engine_allocator *out);

#endif /* SCHISM_MEM_ARENA_H_ */
