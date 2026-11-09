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

#include "headers.h"

#include "mem.h"
#include "str.h"
#include "util.h"
#include "bits.h"

/* --------------------------------------------------------------------- */
/* FORMATTING FUNCTIONS */

char *str_date_from_tm(struct tm *tm, char buf[27], str_date_format_t format)
{
	static const char *month_str[12] = {
		"January",
		"February",
		"March",
		"April",
		"May",
		"June",
		"July",
		"August",
		"September",
		"October",
		"November",
		"December",
	};

	switch (format) {
	default:
	case STR_DATE_FORMAT_DEFAULT:
	case STR_DATE_FORMAT_MMMMDYYYY:
		snprintf(buf, 27, "%s %d, %d", month_str[tm->tm_mon], tm->tm_mday, 1900 + tm->tm_year);
		break;
	case STR_DATE_FORMAT_DMMMMYYYY:
		snprintf(buf, 27, "%d %s %d", tm->tm_mday, month_str[tm->tm_mon], 1900 + tm->tm_year);
		break;
	case STR_DATE_FORMAT_YYYYMMMMDD:
		snprintf(buf, 27, "%d %s %02d", 1900 + tm->tm_year, month_str[tm->tm_mon], tm->tm_mday);
		break;
	case STR_DATE_FORMAT_MDYYYY:
		snprintf(buf, 27, "%d/%d/%d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year);
		break;
	case STR_DATE_FORMAT_DMYYYY:
		snprintf(buf, 27, "%d/%d/%d", tm->tm_mday, tm->tm_mon + 1, 1900 + tm->tm_year);
		break;
	case STR_DATE_FORMAT_YYYYMD:
		snprintf(buf, 27, "%d/%d/%d", 1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday);
		break;
	case STR_DATE_FORMAT_MMDDYYYY:
		snprintf(buf, 27, "%02d/%02d/%d", tm->tm_mon + 1, tm->tm_mday, 1900 + tm->tm_year);
		break;
	case STR_DATE_FORMAT_DDMMYYYY:
		snprintf(buf, 27, "%02d/%02d/%d", tm->tm_mday, tm->tm_mon + 1, 1900 + tm->tm_year);
		break;
	case STR_DATE_FORMAT_YYYYMMDD:
		snprintf(buf, 27, "%d/%02d/%02d", 1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday);
		break;
	case STR_DATE_FORMAT_ISO8601:
		snprintf(buf, 27, "%04d-%02d-%02d", 1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday);
		break;
	}

	return buf;
}

char *str_time_from_tm(struct tm *tm, char buf[27], str_time_format_t format)
{
	switch (format) {
	default:
	case STR_TIME_FORMAT_DEFAULT:
	case STR_TIME_FORMAT_12HR:
		snprintf(buf, 27, "%d:%02d%s", (tm->tm_hour % 12) ? (tm->tm_hour % 12) : 12, tm->tm_min, tm->tm_hour < 12 ? "am" : "pm");
		break;
	case STR_TIME_FORMAT_24HR:
		// so much easier
		snprintf(buf, 27, "%d:%02d", tm->tm_hour, tm->tm_min);
		break;
	}

	return buf;
}

char *str_from_date(time_t when, char buf[27], str_date_format_t format)
{
	struct tm tmr;

	/* DO NOT change this back to localtime(). If some backward platform
	 * doesn't have localtime_r, it needs to be implemented separately. */
	localtime_r(&when, &tmr);

	return str_date_from_tm(&tmr, buf, format);
}

char *str_from_time(time_t when, char buf[27], str_time_format_t format)
{
	struct tm tmr;

	localtime_r(&when, &tmr);

	return str_time_from_tm(&tmr, buf, format);
}

char *str_from_num99(int32_t n, char buf[3])
{
	static const char *qv = "HIJKLMNOPQRSTUVWXYZ";
	if (n < 0) {
		/* This is a bug */
	} else if (n < 100) {
		snprintf(buf, 3, "%02d", n);
	} else if (n <= 256) {
		n -= 100;
		snprintf(buf, 3, "%c%d",
			qv[(n/10)], (n % 10));
	}
	return buf;
}

/* _MAX_ULTOSTR_BASE10_COUNT == 11 (Win32) */
char *str_from_num(int digits, uint32_t n, char buf[11])
{
	if (digits > 0) {
		char fmt[] = "%03" PRIu32;

		SCHISM_RUNTIME_ASSERT(digits >= 0 && digits <= 9,
			"# of digits must fit in one decimal digit");
		digits %= 10;

		fmt[2] = '0' + digits;
		snprintf(buf, digits + 1, fmt, n);
		buf[digits] = 0;
	} else {
		snprintf(buf, 11, "%" PRIu32, n);
	}
	return buf;
}

/* _MAX_LTOSTR_BASE10_COUNT == 12 (Win32) */
char *str_from_num_signed(int digits, int32_t n, char buf[12])
{
	if (digits > 0) {
		char fmt[] = "%03" PRId32;

		SCHISM_RUNTIME_ASSERT(digits >= 0 && digits <= 9,
			"# of digits must fit in one decimal digit");
		digits %= 10;

		fmt[2] = '0' + digits;
		snprintf(buf, digits + 1, fmt, n);
		buf[digits] = 0;
	} else {
		snprintf(buf, 12, "%" PRId32, n);
	}
	return buf;
}

/* hm, this ought to take in a delimiter */
char *str_from_num_thousands(int32_t n, char buf[15])
{
	char *ptr;
	int place;
	int negative;
	int places;
	uint32_t un;

	negative = (n < 0);
	un = babs32(n);
	places = bplacesu32(un);

	/* place max: 10
	 * ((place / 3) - 1) max: 3
	 * negative max: 1
	 *
	 * total max: 14. */

	/* work backwards */
	ptr = buf + places + negative + ((places - 1) / 3);
	*ptr = '\0';

	for (place = 0; /* nothing */; /* nothing */) {
		*--ptr = (un % 10) + '0';
		un /= 10;

		if (!un) break;

		place++;

		if (!(place % 3))
			*--ptr = ',';
	}

	if (negative)
		*--ptr = '-';

	return buf;
}

/* --------------------------------------------------------------------- */
/* STRING HANDLING FUNCTIONS */

static const char whitespace[] = " \t\v\r\n";

int str_ltrim(char *s)
{
	int ws = strspn(s, whitespace);
	int len = strlen(s) - ws;

	if (ws)
		memmove(s, s + ws, len + 1);
	return len;
}

int str_rtrim(char *s)
{
	ptrdiff_t len = (ptrdiff_t)strlen(s) - 1;

	while (len >= 0 && memchr(whitespace, s[len], sizeof(whitespace) - 1))
		len--;
	len++;

	s[len] = '\0';
	return len;
}

int str_trim(char *s)
{
	str_ltrim(s);
	return str_rtrim(s);
}


/* blecch */
int str_get_num_lines(const char *text)
{
	const char *ptr = text;
	int n = 0;

	if (!text)
		return 0;
	for (;;) {
		ptr = strpbrk(ptr, "\015\012");
		if (!ptr)
			return n;
		/* CRLF? */
		if (ptr[0] == 13 && ptr[1] == 10)
			ptr += 2;
		else
			ptr++;
		n++;
	}
}

/* --------------------------------------------------------------------- */


/* --------------------------------------------------------------------- */
/* Functions for working with Pascal strings.
 * Pascal strings are basically just a stream of bytes, like C strings.
 * However, the first byte is used as the length.
 * This means they are limited to a length of 255 in their very nature.
 * These kinds of strings are used fairly often in the wild, with
 * larger types serving as the length (see the Win32 BSTR type). */

void str_to_pascal(const char *cstr, unsigned char pstr[256], int *truncated)
{
	const size_t len = strlen(cstr);

	if (truncated)
		*truncated = (len > 255);

	pstr[0] = MIN(len, 255);
	if (pstr[0] > 0)
		memcpy(&pstr[1], cstr, pstr[0]);
}

void str_from_pascal(const unsigned char pstr[256], char cstr[256])
{
	if (pstr[0] > 0)
		memcpy(cstr, pstr + 1, pstr[0]);
	cstr[pstr[0]] = 0;
}

/* --------------------------------------------------------------------- */


/* --------------------------------------------------------------------- */

char *mem_mem(const char *haystack, size_t haystacksz, const char *needle, size_t needlesz)
{
	char *x;

	if (!haystacksz || !needlesz)
		return NULL;

	/* Easy */
	if (needlesz == 1)
		return memchr(haystack, *needle, haystacksz);

	while (haystacksz > 0 && (x = memchr(haystack, *needle, haystacksz))) {
		haystacksz -= (x - haystack);
		haystack = x;

		if (haystacksz < needlesz)
			return NULL;

		if (memcmp(x, needle, needlesz) == 0)
			return x;

		haystack++;
		haystacksz--;
	}

	return NULL;
}
