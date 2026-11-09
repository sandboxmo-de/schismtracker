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

#include "charset.h"

#include "engine-alloc.h"

#include <utf8proc.h>

static inline void *charset_map_to_utf8(const void *in, charset_t inset, utf8proc_option_t option)
{
	const uint8_t *utf8;
	uint8_t *alloc_ptr = NULL;

	if (inset == CHARSET_UTF8) {
		utf8 = in;
	} else {
		if (charset_iconv_engine(in, &alloc_ptr, inset, CHARSET_UTF8, SIZE_MAX))
			return NULL;
		utf8 = alloc_ptr;
	}

	/* This is what utf8proc_map() does, with the buffer coming from the
	 * engine instead of malloc(). */
	utf8proc_int32_t *buffer = NULL;
	utf8proc_ssize_t len = utf8proc_decompose(utf8, 0, NULL, 0, option);

	if (len >= 0)
		buffer = engine_alloc((size_t)len * sizeof(utf8proc_int32_t) + 1, ENGINE_MEM_SCRATCH);
	if (buffer) {
		len = utf8proc_decompose(utf8, 0, buffer, len, option);
		if (len >= 0)
			len = utf8proc_reencode(buffer, len, option);
		if (len < 0) {
			engine_free(buffer, ENGINE_MEM_SCRATCH);
			buffer = NULL;
		}
	}

	engine_free(alloc_ptr, ENGINE_MEM_SCRATCH);

	return buffer;
}

static inline void *charset_map_to_set(const void *in, charset_t inset, charset_t outset, utf8proc_option_t option)
{
	uint8_t *mapped_utf8 = charset_map_to_utf8(in, inset, option);
	if (!mapped_utf8)
		return NULL;

	/* now convert it back to the set we want */
	if (outset == CHARSET_UTF8)
		return mapped_utf8;

	uint8_t *mapped;

	int success = (charset_iconv_engine(mapped_utf8, &mapped, CHARSET_UTF8, outset, SIZE_MAX) == CHARSET_ERROR_SUCCESS);

	engine_free(mapped_utf8, ENGINE_MEM_SCRATCH);

	return (success ? mapped : NULL);
}

void *charset_compose_to_set_engine(const void *in, charset_t inset, charset_t outset)
{
	return charset_map_to_set(in, inset, outset, UTF8PROC_NULLTERM | UTF8PROC_COMPOSE);
}

void *charset_case_fold_to_set_engine(const void *in, charset_t inset, charset_t outset)
{
	return charset_map_to_set(in, inset, outset, UTF8PROC_NULLTERM | UTF8PROC_CASEFOLD | UTF8PROC_DECOMPOSE);
}
