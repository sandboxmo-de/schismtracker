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

/* It's lo-og, lo-og, it's big, it's heavy, it's wood!
 * It's lo-og, lo-og, it's better than bad, it's good! */

#include "headers.h"

#include "log.h"
#include "util.h"

#include <stdarg.h>
#include <errno.h>

struct log_line {
	int color;
	const char *text;
	int bios_font;
	/* Set this flag if the text should be free'd when it is scrolled offscreen.
	DON'T set it if the text is going to be modified after it is added to the log (e.g. for displaying
	status information for module loaders like IT); in that case, change the text pointer to some
	constant value such as "". Also don't try changing must_free after adding a line to the log, since
	there's a chance that the line scrolled offscreen, and it'd never get free'd. (Also, ignore this
	comment since there's currently no interface for manipulating individual lines in the log after
	adding them.) */
	int must_free;
};

/* --------------------------------------------------------------------- */

#define NUM_LINES 1000
static struct log_line lines[NUM_LINES];
static int top_line = 0;
static int last_line = -1;

/* --------------------------------------------------------------------- */

inline void log_append2(int bios_font, int color, int must_free, const char *text)
{
	if (last_line < NUM_LINES - 1) {
		last_line++;
	} else {
		if (lines[0].must_free)
			free((void *) lines[0].text);
		memmove(lines, lines + 1, last_line * sizeof(struct log_line));
	}
	lines[last_line].text = text;
	lines[last_line].color = color;
	lines[last_line].must_free = must_free;
	lines[last_line].bios_font = bios_font;
	top_line = CLAMP(last_line - 32, 0, NUM_LINES-32);

//	if (status.current_page == PAGE_LOG)
//		status.flags |= NEED_UPDATE;
}
inline void log_append(int color, int must_free, const char *text)
{
	log_append2(0, color, must_free, text);
}
inline void log_nl(void)
{
	log_append(0,0,"");
}
void log_appendf(int color, const char *format, ...)
{
	char *ptr;
	va_list ap;

	va_start(ap, format);
	if (vasprintf(&ptr, format, ap) == -1) {
		perror("asprintf");
		exit(255);
	}
	va_end(ap);

	if (!ptr) {
		perror("asprintf");
		exit(255);
	}

	log_append(color, 1, ptr);
}

void log_underline(int chars)
{
	char buf[75];

	chars = CLAMP(chars, 0, (int) sizeof(buf) - 1);
	buf[chars--] = '\0';
	do
		buf[chars] = 0x81;
	while (chars--);
	log_appendf(2, "%s", buf);
}

void log_perror(const char *prefix)
{
	char *e = strerror(errno);
	perror(prefix);
	log_appendf(4, "%s: %s", prefix, e);
}

