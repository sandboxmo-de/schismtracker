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

/* The default engine allocator: malloc, realloc and free, with the class
 * ignored. This is the only file in the core engine that's allowed to call
 * the libc heap (scripts/check-engine-boundary.sh). */

#include "headers.h"

#include "engine-alloc.h"

/* malloc's alignment covers ENGINE_ALIGN_DEFAULT. Anything stricter is
 * refused, because free() has to work on whatever we hand out. The engine
 * never asks for more. */

static void *libc_alloc(SCHISM_UNUSED void *ctx, size_t size, size_t align,
	SCHISM_UNUSED enum engine_mem_class cls)
{
	if (align > ENGINE_ALIGN_DEFAULT)
		return NULL;

	return malloc(size);
}

static void *libc_realloc(SCHISM_UNUSED void *ctx, void *ptr, size_t size, size_t align,
	SCHISM_UNUSED enum engine_mem_class cls)
{
	if (align > ENGINE_ALIGN_DEFAULT)
		return NULL;

	return realloc(ptr, size);
}

static void libc_free(SCHISM_UNUSED void *ctx, void *ptr, SCHISM_UNUSED enum engine_mem_class cls)
{
	free(ptr);
}

const struct engine_allocator engine_allocator_libc = {
	.ctx = NULL,
	.flags = 0,
	.alloc = libc_alloc,
	.realloc = libc_realloc,
	.free = libc_free,
};
