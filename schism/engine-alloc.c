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

/* The engine's side of include/engine-alloc.h: the registered allocator and
 * the wrappers engine code calls. The libc default is in mem-libc.c. */

#include "headers.h"

#include "engine-alloc.h"

static struct engine_allocator allocator = {0};
static int allocator_set = 0;
static unsigned long failures = 0;

void engine_allocator_register(const struct engine_allocator *alloc)
{
	allocator = alloc ? *alloc : engine_allocator_libc;
	allocator_set = 1;
}

static inline const struct engine_allocator *get_allocator(void)
{
	if (!allocator_set) {
		allocator = engine_allocator_libc;
		allocator_set = 1;
	}
	return &allocator;
}

unsigned int engine_allocator_flags(void)
{
	return get_allocator()->flags;
}

unsigned long engine_alloc_failures(void)
{
	return failures;
}

/* ------------------------------------------------------------------------ */

void *engine_alloc_aligned(size_t size, size_t align, enum engine_mem_class cls)
{
	const struct engine_allocator *a = get_allocator();
	void *p;

	if (!size)
		size = 1;
	if (!align)
		align = ENGINE_ALIGN_DEFAULT;

	p = a->alloc(a->ctx, size, align, cls);
	if (!p)
		failures++;

	return p;
}

void *engine_alloc(size_t size, enum engine_mem_class cls)
{
	return engine_alloc_aligned(size, ENGINE_ALIGN_DEFAULT, cls);
}

void *engine_calloc(size_t nmemb, size_t size, enum engine_mem_class cls)
{
	void *p;

	if (size && nmemb > SIZE_MAX / size) {
		failures++;
		return NULL;
	}

	p = engine_alloc(nmemb * size, cls);
	if (p)
		memset(p, 0, nmemb * size);

	return p;
}

void *engine_realloc(void *ptr, size_t size, enum engine_mem_class cls)
{
	const struct engine_allocator *a = get_allocator();
	void *p;

	if (!ptr)
		return engine_alloc(size, cls);
	if (!size)
		size = 1;

	p = a->realloc(a->ctx, ptr, size, ENGINE_ALIGN_DEFAULT, cls);
	if (!p)
		failures++;

	return p;
}

void engine_free(void *ptr, enum engine_mem_class cls)
{
	const struct engine_allocator *a = get_allocator();

	if (ptr)
		a->free(a->ctx, ptr, cls);
}

char *engine_strndup(const char *s, size_t n, enum engine_mem_class cls)
{
	char *q;
	size_t len = 0;

	while (len < n && s[len])
		len++;

	q = engine_alloc(len + 1, cls);
	if (q) {
		memcpy(q, s, len);
		q[len] = '\0';
	}

	return q;
}

char *engine_strdup(const char *s, enum engine_mem_class cls)
{
	return engine_strndup(s, SIZE_MAX, cls);
}

int engine_vasprintf(char **out, enum engine_mem_class cls, const char *fmt, va_list ap)
{
	va_list ap2;
	int len;

	*out = NULL;

	va_copy(ap2, ap);
	len = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);

	if (len < 0)
		return -1;

	*out = engine_alloc((size_t)len + 1, cls);
	if (!*out)
		return -1;

	vsnprintf(*out, (size_t)len + 1, fmt, ap);

	return len;
}

int engine_asprintf(char **out, enum engine_mem_class cls, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = engine_vasprintf(out, cls, fmt, ap);
	va_end(ap);

	return len;
}

/* ------------------------------------------------------------------------ */

void engine_song_begin(void)
{
	const struct engine_allocator *a = get_allocator();

	if (a->song_begin)
		a->song_begin(a->ctx);
}

void engine_song_end(void)
{
	const struct engine_allocator *a = get_allocator();

	if (a->song_end)
		a->song_end(a->ctx);
}

void engine_scratch_begin(void)
{
	const struct engine_allocator *a = get_allocator();

	if (a->scratch_begin)
		a->scratch_begin(a->ctx);
}

void engine_scratch_end(void)
{
	const struct engine_allocator *a = get_allocator();

	if (a->scratch_end)
		a->scratch_end(a->ctx);
}
