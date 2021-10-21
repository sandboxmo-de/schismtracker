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

/* This is just a collection of some useful functions. None of these use any
extraneous libraries (i.e. GLib). */

#include "util.h"
#include <stdint.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "headers.h"

#include <errno.h>

#include <stdarg.h>

#include <math.h>

#ifdef WIN32
#include <windows.h>
#include <process.h>
#include <shlobj.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#endif

void ms_sleep(unsigned int ms)
{
#ifdef WIN32
	SleepEx(ms,FALSE);
#else
	usleep(ms*1000);
#endif
}

char *str_dup(const char *s)
{
	char *q;
	q = strdup(s);
	if (!q) {
		/* throw out of memory exception */
		perror("strdup");
		exit(255);
	}
	return q;
}

char *strn_dup(const char *s, size_t n)
{
	char *q;
	q = malloc(n + 1);
	if (!q) {
		/* throw out of memory exception */
		perror("strndup");
		exit(255);
	}
	memcpy(q, s, n);
	q[n] = '\0';
	return q;
}

void *mem_alloc(size_t amount)
{
	void *q;
	q = malloc(amount);
	if (!q) {
		/* throw out of memory exception */
		perror("malloc");
		exit(255);
	}
	return q;
}

void *mem_calloc(size_t nmemb, size_t size)
{
	void *q;
	q = calloc(nmemb, size);
	if (!q) {
		/* throw out of memory exception */
		perror("calloc");
		exit(255);
	}
	return q;
}

void *mem_realloc(void *orig, size_t amount)
{
	void *q;
	if (!orig) return mem_alloc(amount);
	q = realloc(orig, amount);
	if (!q) {
		/* throw out of memory exception */
		perror("malloc");
		exit(255);
	}
	return q;
}


/* --------------------------------------------------------------------- */
/* CONVERSION FUNCTIONS */

/* linear -> deciBell */
/* amplitude normalized to 1.0f. */
float dB(float amplitude)
{
	return 20.0f * log10f(amplitude);
}

/* deciBell -> linear */
float dB2_amp(float db)
{
	return powf(10.0f, db / 20.0f);
}

/* linear -> deciBell */
/* power normalized to 1.0f. */
float pdB(float power)
{
	return 10.0f * log10f(power);
}

/* deciBell -> linear */
float dB2_power(float db)
{
	return powf(10.0f, db / 10.0f);
}
/* linear -> deciBell */
/* amplitude normalized to 1.0f. */
/* Output scaled (and clipped) to 128 lines with noisefloor range. */
/* ([0..128] = [-noisefloor..0dB]) */
/* correction_dBs corrects the dB after converted, but before scaling.*/
short dB_s(int noisefloor, float amplitude, float correction_dBs)
{
	float db = dB(amplitude) + correction_dBs;
	return CLAMP((int)(128.f*(db+noisefloor))/noisefloor, 0, 127);
}

/* deciBell -> linear */
/* Input scaled to 128 lines with noisefloor range. */
/* ([0..128] = [-noisefloor..0dB]) */
/* amplitude normalized to 1.0f. */
/* correction_dBs corrects the dB after converted, but before scaling.*/
short dB2_amp_s(int noisefloor, int db, float correction_dBs)
{
	return dB2_amp((db*noisefloor/128.f)-noisefloor-correction_dBs);
}
/* linear -> deciBell */
/* power normalized to 1.0f. */
/* Output scaled (and clipped) to 128 lines with noisefloor range. */
/* ([0..128] = [-noisefloor..0dB]) */
/* correction_dBs corrects the dB after converted, but before scaling.*/
short pdB_s(int noisefloor, float power, float correction_dBs)
{
	float db = pdB(power)+correction_dBs;
	return CLAMP((int)(128.f*(db+noisefloor))/noisefloor, 0, 127);
}

/* deciBell -> linear */
/* Input scaled to 128 lines with noisefloor range. */
/* ([0..128] = [-noisefloor..0dB]) */
/* power normalized to 1.0f. */
/* correction_dBs corrects the dB after converted, but before scaling.*/
short dB2_power_s(int noisefloor, int db, float correction_dBs)
{
	return dB2_power((db*noisefloor/128.f)-noisefloor-correction_dBs);
}

/* --------------------------------------------------------------------- */
/* STRING HANDLING FUNCTIONS */

/* I was intending to get rid of this and use glibc's basename() instead,
but it doesn't do what I want (i.e. not bother with the string) and thanks
to the stupid libgen.h basename that's totally different, it'd cause some
possible portability issues. */
const char *get_basename(const char *filename)
{
	const char *base = strrchr(filename, DIR_SEPARATOR);
	if (base) {
		/* skip the slash */
		base++;
	}
	if (!(base && *base)) {
		/* well, there isn't one, so just return the filename */
		base = filename;
	}

	return base;
}

static const char *whitespace = " \t\v\r\n";

inline int ltrim_string(char *s)
{
	int ws = strspn(s, whitespace);
	int len = strlen(s) - ws;

	if (ws)
		memmove(s, s + ws, len + 1);
	return len;
}

inline int rtrim_string(char *s)
{
	int len = strlen(s) - 1;

	while (len > 0 && strchr(whitespace, s[len]))
		len--;
	len++;
	s[len] = '\0';
	return len;
}

int trim_string(char *s)
{
	ltrim_string(s);
	return rtrim_string(s);
}

static inline int readhex(const char *s, int w)
{
	int o = 0;

	while (w--) {
		o <<= 4;
		switch (*s) {
			case '0'...'9': o |= *s - '0';      break;
			case 'a'...'f': o |= *s - 'a' + 10; break;
			case 'A'...'F': o |= *s - 'A' + 10; break;
			default: return -1;
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
	char *dest = calloc(strlen(s) + 1, sizeof(char));
	char *d = dest;

	while (*s) {
		if (*s == '\\') {
			s++;
			switch (*s) {
			case '0'...'7':
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
