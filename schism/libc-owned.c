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

/* Helpers whose results the caller frees with free().
 *
 * The tracker's pages, the config parser and the platform code have always
 * freed what these return with free(), so they keep handing out libc
 * memory, whatever allocator the engine is using. Engine code doesn't call
 * them. It uses the _engine variants in charset.c, charset_unicode.c and
 * disko.c instead, which return SCRATCH memory from the engine allocator.
 * The wrappers here copy that into a malloc() block.
 *
 * scripts/check-engine-boundary.sh fails if a core engine object refers to
 * any of the functions in this file. */

#include "headers.h"

#include "charset.h"
#include "disko.h"
#include "engine-alloc.h"
#include "mem.h"
#include "str.h"

/* ------------------------------------------------------------------------ */
/* moving SCRATCH memory to the libc heap */

static void *scratch_to_libc(void *p, size_t len)
{
	void *q = malloc(len ? len : 1);

	if (q && len)
		memcpy(q, p, len);
	engine_free(p, ENGINE_MEM_SCRATCH);

	return q;
}

int disko_memclose(disko_t *ds, int keep_buffer)
{
	if (disko_memclose_engine(ds, keep_buffer) != DW_OK)
		return DW_ERROR;

	if (keep_buffer && ds->data) {
		ds->data = scratch_to_libc(ds->data, ds->length);
		ds->allocated = ds->length;
		if (!ds->data) {
			errno = ENOMEM;
			return DW_ERROR;
		}
	}

	return DW_OK;
}

charset_error_t charset_iconv(const void *in, void *out, charset_t inset, charset_t outset, size_t insize)
{
	void *p, *q;
	size_t len;
	charset_error_t rc;

	rc = charset_iconv_engine_len(in, &p, inset, outset, insize, &len);
	if (rc != CHARSET_ERROR_SUCCESS)
		return rc;

	q = scratch_to_libc(p, len);
	if (!q)
		return CHARSET_ERROR_NOMEM;

	memcpy(out, &q, sizeof(void *));
	return CHARSET_ERROR_SUCCESS;
}

/* The engine variants work through UTF-8, so do the folding there and the
 * conversion to outset with the libc charset_iconv() above. */
static void *map_to_set_libc(void *utf8, charset_t outset)
{
	void *out = NULL;

	if (!utf8)
		return NULL;

	if (outset == CHARSET_UTF8) {
		out = scratch_to_libc(utf8, strlen(utf8) + 1);
	} else {
		if (charset_iconv(utf8, &out, CHARSET_UTF8, outset, SIZE_MAX) != CHARSET_ERROR_SUCCESS)
			out = NULL;
		engine_free(utf8, ENGINE_MEM_SCRATCH);
	}

	return out;
}

void *charset_compose_to_set(const void *in, charset_t inset, charset_t outset)
{
	return map_to_set_libc(charset_compose_to_set_engine(in, inset, CHARSET_UTF8), outset);
}

void *charset_case_fold_to_set(const void *in, charset_t inset, charset_t outset)
{
	return map_to_set_libc(charset_case_fold_to_set_engine(in, inset, CHARSET_UTF8), outset);
}

/* ------------------------------------------------------------------------ */
/* string helpers from str.h that allocate */

/* break the string 's' with the character 'c', placing the two parts in 'first' and 'second'.
return: 1 if the string contained the character (and thus could be split), 0 if not.
the pointers returned in first/second should be free()'d by the caller. */
int str_break(const char *s, char c, char **first, char **second)
{
	const char *p = strchr(s, c);
	if (!p)
		return 0;
	*first = mem_alloc(p - s + 1);
	memcpy(*first, s, p - s);
	(*first)[p - s] = 0;
	*second = str_dup(p + 1);
	return 1;
}

/* adapted from glib. in addition to the normal c escapes, this also escapes the hashmark and semicolon
 * (comment characters). if space is true, the first/last character is also escaped if it is a space. */
char *str_escape(const char *s, int space)
{
	/* Each source byte needs maximally four destination chars (\777) */
	char *dest = calloc(4 * strlen(s) + 1, sizeof(char));
	char *d = dest;

	if (space && *s == ' ') {
		*d++ = '\\';
		*d++ = '0';
		*d++ = '4';
		*d++ = '0';
		s++;
	}

	while (*s) {
		switch (*s) {
		case '\a':
			*d++ = '\\';
			*d++ = 'a';
			break;
		case '\b':
			*d++ = '\\';
			*d++ = 'b';
			break;
		case '\f':
			*d++ = '\\';
			*d++ = 'f';
			break;
		case '\n':
			*d++ = '\\';
			*d++ = 'n';
			break;
		case '\r':
			*d++ = '\\';
			*d++ = 'r';
			break;
		case '\t':
			*d++ = '\\';
			*d++ = 't';
			break;
		case '\v':
			*d++ = '\\';
			*d++ = 'v';
			break;
		case '\\': case '"':
			*d++ = '\\';
			*d++ = *s;
			break;

		default:
			if (*s < ' ' || *s >= 127 || (space && *s == ' ' && s[1] == '\0')) {
		case '#': case ';':
				*d++ = '\\';
				*d++ = '0' + ((((uint8_t) *s) >> 6) & 7);
				*d++ = '0' + ((((uint8_t) *s) >> 3) & 7);
				*d++ = '0' + ( ((uint8_t) *s)       & 7);
			} else {
				*d++ = *s;
			}
			break;
		}
		s++;
	}

	*d = 0;
	return dest;
}

static inline int readhex(const char *s, int w)
{
	int o = 0;

	while (w--) {
		o <<= 4;
		switch (*s) {
			case '0': case '1': case '2':
			case '3': case '4': case '5':
			case '6': case '7': case '8':
			case '9':
				o |= *s - '0';
				break;
			case 'a': case 'b': case 'c':
			case 'd': case 'e': case 'f':
				o |= *s - 'a' + 10;
				break;
			case 'A': case 'B': case 'C':
			case 'D': case 'E': case 'F':
				o |= *s - 'A' + 10;
				break;
			default:
				return -1;
		}
		s++;
	}
	return o;
}

/* opposite of str_escape. (this is glib's 'compress' function renamed more clearly) */
char *str_unescape(const char *s)
{
	const char *end;
	int hex;
	char *dest = mem_calloc(strlen(s) + 1, sizeof(char));
	char *d = dest;

	while (*s) {
		if (*s == '\\') {
			s++;
			switch (*s) {
			case '0': case '1': case '2':
			case '3': case '4': case '5':
			case '6': case '7':
				*d = 0;
				end = s + 3;
				while (s < end && *s >= '0' && *s <= '7') {
					*d = *d * 8 + *s - '0';
					s++;
				}
				d++;
				s--;
				break;
			case 'a':
				*d++ = '\a';
				break;
			case 'b':
				*d++ = '\b';
				break;
			case 'f':
				*d++ = '\f';
				break;
			case 'n':
				*d++ = '\n';
				break;
			case 'r':
				*d++ = '\r';
				break;
			case 't':
				*d++ = '\t';
				break;
			case 'v':
				*d++ = '\v';
				break;
			case '\0': // trailing backslash?
				*d++ = '\\';
				s--;
				break;
			case 'x':
				hex = readhex(s + 1, 2);
				if (hex >= 0) {
					*d++ = hex;
					s += 2;
					break;
				}
				/* fall through */
			default: /* Also handles any other char, like \" \\ \; etc. */
				*d++ = *s;
				break;
			}
		} else {
			*d++ = *s;
		}
		s++;
	}
	*d = 0;

	return dest;
}

char *str_concat(const char *s, ...)
{
	/* As it turns out, concatenating strings isn't hard, but doing it
	 * *fast* is another story. Here we go over the list of strings not
	 * once, not twice, but thrice, because it will generally be faster
	 * than reading over the whole string many times. (here we do it twice,
	 * the first time to get the length, and the next time to do the
	 * actual copying, similar to str_dup) */
	va_list ap, ap2, ap3;
	char *out = NULL;
	size_t len;
	size_t i;
	const char *n;

	va_start(ap, s);
	va_copy(ap2, ap);
	va_copy(ap3, ap);

	/* first check how many strings we have */
	for (i = 0, n = s; n; n = va_arg(ap, const char *), i++);

	SCHISM_VLA_ALLOC(size_t, lens, i);

	len = 0;

	/* then find their lengths */
	for (i = 0, n = s; n; n = va_arg(ap2, const char *), i++) {
		lens[i] = strlen(n);
		len += lens[i];
	}

	out = mem_alloc(len + 1);

	len = 0;

	/* finally, do the copying */
	for (i = 0, n = s; n; n = va_arg(ap3, const char *), i++) {
		memcpy(out + len, n, lens[i]);
		len += lens[i];
	}

	out[len] = '\0';

	va_end(ap);
	va_end(ap2);
	va_end(ap3);

	return out;
}

/* if len is zero, this function calls strlen to get the input's
 * length.
 *
 * The input will be free'd if the input isn't a null pointer,
 * so make sure you initialize your strings properly ;)
 *
 * returns 0 on fail or 1 on success */
int str_realloc(char **output, const char *input, size_t len)
{
	if (*output)
		free(*output);
	*output = (len) ? strn_dup(input, len) : str_dup(input);
	if (!*output)
		return 0;
	return 1;
}
