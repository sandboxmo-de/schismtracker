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

#ifndef SCHISM_ENGINE_ALLOC_H_
#define SCHISM_ENGINE_ALLOC_H_

/* Memory for the engine comes from an allocator the host registers with
 * engine_allocator_register(). Every request carries a lifetime class, so a
 * host without a general-purpose heap can serve each class from its own
 * region and throw a whole region away at once. The default allocator uses
 * malloc() and friends and ignores the class.
 *
 * docs/engine-memory.md lists what the engine allocates in each class. */

#include "headers.h"

enum engine_mem_class {
	/* Fixed engine state that lives as long as the program: the mixing
	 * buffer, the OPL emulator, the song structure itself. */
	ENGINE_MEM_ENGINE,
	/* Song data: patterns, sample data, instruments, the edit history.
	 * Freed when the song is unloaded or replaced. */
	ENGINE_MEM_SONG,
	/* Temporaries: file buffers while a song loads, decompression and
	 * decoding buffers, formatted log lines. Freed before the operation
	 * that made them returns. */
	ENGINE_MEM_SCRATCH,

	ENGINE_MEM_CLASSES
};

/* flags for engine_allocator.flags */
enum {
	/* The allocator can't hold two songs at once. The engine unloads the
	 * current song before it starts loading the next one, so song_end
	 * comes before the next song_begin. A failed load leaves an empty
	 * song instead of the previous one. */
	ENGINE_ALLOC_ONE_SONG = (1 << 0),
};

/* Alignment the engine asks for when it doesn't need anything special. It's
 * what malloc() promises on the usual 32 and 64-bit platforms, and enough
 * for any scalar type. */
#define ENGINE_ALIGN_DEFAULT (2 * sizeof(void *))

struct engine_allocator {
	/* passed back to every callback */
	void *ctx;
	unsigned int flags;

	/* Return size bytes aligned to align (a power of two, at most
	 * ENGINE_ALIGN_DEFAULT unless the allocator says otherwise), or NULL.
	 * size is never 0. */
	void *(*alloc)(void *ctx, size_t size, size_t align, enum engine_mem_class cls);
	/* Resize ptr (never NULL) to size (never 0), keeping the contents up to
	 * the smaller of the two sizes. Return NULL and leave ptr alone if it
	 * can't be done. */
	void *(*realloc)(void *ctx, void *ptr, size_t size, size_t align, enum engine_mem_class cls);
	/* Free ptr (never NULL). cls is the class it was allocated with. */
	void (*free)(void *ctx, void *ptr, enum engine_mem_class cls);

	/* Bulk-lifetime notifications; any of these can be NULL.
	 *
	 * song_begin is called before the first SONG allocation for a new song
	 * and song_end after everything a song allocated in SONG has been
	 * freed. Without ENGINE_ALLOC_ONE_SONG two songs overlap while one
	 * loads; with it they never do.
	 *
	 * scratch_begin and scratch_end bracket an operation that makes
	 * SCRATCH allocations (loading a song or sample, an export). They can
	 * nest. At scratch_end every SCRATCH allocation made since the
	 * matching scratch_begin has been freed. */
	void (*song_begin)(void *ctx);
	void (*song_end)(void *ctx);
	void (*scratch_begin)(void *ctx);
	void (*scratch_end)(void *ctx);
};

/* Use this allocator from now on. NULL puts the libc default back. The
 * engine keeps a copy of the struct. Register before anything allocates:
 * memory is always freed through the allocator that's registered at the
 * time, so switching with live allocations is a bug. */
void engine_allocator_register(const struct engine_allocator *alloc);

/* the libc allocator, which is the default */
extern const struct engine_allocator engine_allocator_libc;

/* the flags of the registered allocator */
unsigned int engine_allocator_flags(void);

/* ------------------------------------------------------------------------ */
/* What engine code calls. All of these return NULL when the allocator does,
 * and never abort. engine_free(NULL, cls) does nothing. */

void *engine_alloc(size_t size, enum engine_mem_class cls) SCHISM_MALLOC SCHISM_ALLOC_SIZE(1);
void *engine_alloc_aligned(size_t size, size_t align, enum engine_mem_class cls) SCHISM_MALLOC SCHISM_ALLOC_SIZE(1);
void *engine_calloc(size_t nmemb, size_t size, enum engine_mem_class cls) SCHISM_MALLOC SCHISM_ALLOC_SIZE_EX(1, 2);
/* NULL ptr allocates; on failure ptr is still valid */
void *engine_realloc(void *ptr, size_t size, enum engine_mem_class cls) SCHISM_ALLOC_SIZE(2);
void engine_free(void *ptr, enum engine_mem_class cls);

char *engine_strdup(const char *s, enum engine_mem_class cls) SCHISM_MALLOC;
/* copies at most n bytes and always terminates */
char *engine_strndup(const char *s, size_t n, enum engine_mem_class cls) SCHISM_MALLOC;
/* like vasprintf; returns the length, or -1 with *out set to NULL */
int engine_vasprintf(char **out, enum engine_mem_class cls, const char *fmt, va_list ap);
int engine_asprintf(char **out, enum engine_mem_class cls, const char *fmt, ...)
	SCHISM_FORMAT_PRINTF(3, 4);

/* How many requests have failed since the program started. The load path
 * compares this before and after, so a failure the loader didn't report
 * still fails the load. */
unsigned long engine_alloc_failures(void);

void engine_song_begin(void);
void engine_song_end(void);
void engine_scratch_begin(void);
void engine_scratch_end(void);

#endif /* SCHISM_ENGINE_ALLOC_H_ */
