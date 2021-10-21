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

#include "song.h"
#include "midi.h"

#include "sdlmain.h"

static const char *audio_driver = NULL;

static void sdl_init(void)
{
	char *err;
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE) == 0)
		return;
	err = SDL_GetError();
	exit(1);
}

/* Path handling functions */

/* Normalize a path (remove /../ and stuff, condense multiple slashes, etc.)
this will return NULL if the path could not be normalized (not well-formed?).
the returned string must be free()'d. */
char *dmoz_path_normal(const char *path);

/* Return nonzero if the path is an absolute path (e.g. /bin, c:\progra~1, sd:/apps, etc.) */
int dmoz_path_is_absolute(const char *path);

/* Concatenate two paths, adding separators between them as necessary. The returned string must be free()'d.
The second version can be used if the string lengths are already known to avoid redundant strlen() calls.
Additionally, if 'b' is an absolute path (as determined by dmoz_path_is_absolute), ignore 'a' and return a
copy of 'b'. */
char *dmoz_path_concat(const char *a, const char *b);
char *dmoz_path_concat_len(const char *a, const char *b, int alen, int blen);

char *dmoz_path_normal(const char *path)
{
	char stub_char;
	char *result, *p, *q, *base, *dotdot;
	int rooted;

	/* The result cannot be larger than the input PATH. */
	result = strdup(path);

	rooted = dmoz_path_is_absolute(path);
	base = result + rooted;
	stub_char = rooted ? DIR_SEPARATOR : '.';

#ifdef WIN32
	/* Stupid hack -- fix up any initial slashes in the absolute part of the path.
	(The rest of them will be handled as the path components are processed.) */
	for (q = result; q < base; q++)
		if (*q == '/')
			*q = '\\';
#endif

	/* invariants:
		base points to the portion of the path we want to modify
		p points at beginning of path element we're considering.
		q points just past the last path element we wrote (no slash).
		dotdot points just past the point where .. cannot backtrack
		any further (no slash). */
	p = q = dotdot = base;

	while (*p) {
		if (IS_DIR_SEPARATOR(p[0])) {
			/* null element */
			p++;
		} else if (p[0] == '.' && (!p[1] || IS_DIR_SEPARATOR(p[1]))) {
			/* . and ./ */
			p += 1; /* don't count the separator in case it is nul */
		} else if (p[0] == '.' && p[1] == '.' && (!p[2] || IS_DIR_SEPARATOR(p[2]))) {
			/* .. and ../ */
			p += 2; /* skip `..' */
			if (q > dotdot) {       /* can backtrack */
				while (--q > dotdot && !IS_DIR_SEPARATOR(*q)) {
					/* nothing */
				}
			} else if (!rooted) {
				/* /.. is / but ./../ is .. */
				if (q != base)
					*q++ = DIR_SEPARATOR;
				*q++ = '.';
				*q++ = '.';
				dotdot = q;
			}
		} else {
			/* real path element */
			/* add separator if not at start of work portion of result */
			if (q != base)
				*q++ = DIR_SEPARATOR;
			while (*p && !IS_DIR_SEPARATOR(*p))
				*q++ = *p++;
		}
	}

	/* Empty string is really ``.'' or `/', depending on what we started with. */
	if (q == result)
		*q++ = stub_char;
	*q = '\0';

	return result;
}

int dmoz_path_is_absolute(const char *path)
{
	if (!path || !*path)
		return 0;
	if (isalpha(path[0]) && path[1] == ':')
		return IS_DIR_SEPARATOR(path[2]) ? 3 : 2;
	/* presumably, /foo (or \foo) is an absolute path on all platforms */
	if (!IS_DIR_SEPARATOR(path[0]))
		return 0;
	/* POSIX says to allow two leading slashes, but not more.
	(This also catches win32 \\share\blah\blah semantics) */
	return (IS_DIR_SEPARATOR(path[1]) && !IS_DIR_SEPARATOR(path[2])) ? 2 : 1;
}


/* See dmoz_path_concat_len. This function is a convenience for when the lengths aren't already known. */
char *dmoz_path_concat(const char *a, const char *b)
{
	return dmoz_path_concat_len(a, b, strlen(a), strlen(b));
}

/* Concatenate two paths. Additionally, if 'b' is an absolute path, ignore 'a' and return a copy of 'b'. */
char *dmoz_path_concat_len(const char *a, const char *b, int alen, int blen)
{
	char *ret;
	if (dmoz_path_is_absolute(b))
		return strdup(b);

	ret = mem_alloc(alen + blen + 2);

	if (alen) {
		char last = a[alen - 1];

		strcpy(ret, a);

		/* need a slash? */
		if (last != DIR_SEPARATOR)
			strcat(ret, DIR_SEPARATOR_STR);
	}
	strcat(ret, b);

	return ret;
}

static char *initial_song = NULL;
/* initial module directory */
static char *initial_dir = NULL;

static int is_directory(const char *filename)
{
	struct stat buf;

	if (stat(filename, &buf) == -1) {
		/* Well, at least we tried. */
		return 0;
	}

	return S_ISDIR(buf.st_mode);
}

static char *get_current_directory(void)
{
	char buf[PATH_MAX + 1];

	/* hmm. fall back to the current dir */
	if (getcwd(buf, PATH_MAX))
		return str_dup(buf);
	return str_dup(".");
}

static void parse_only_initial_song(int argc, char **argv)
{
	char *cwd = get_current_directory();
	for (; optind < argc; optind++) {
		char *arg = argv[optind];
		char *tmp = dmoz_path_concat(cwd, arg);
		if (!tmp) {
			perror(arg);
			continue;
		}
		char *norm = dmoz_path_normal(tmp);
		free(tmp);
		if (is_directory(arg)) {
			free(initial_dir);
			initial_dir = norm;
		} else {
			free(initial_song);
			initial_song = norm;
		}
	}
	free(cwd);
}

int main(int argc, char **argv)
{
	parse_only_initial_song(argc, argv);

	song_initialise();
	load_audio();
	load_midi();

	sdl_init();

	midi_engine_start();
	audio_init(audio_driver);
	song_init_modplug();

	if (initial_song) {
		if (song_load_unchecked(initial_song)) {
			song_start();
		}
		free(initial_song);
	}

	/* poll once */
	midi_engine_poll_ports();

	//	event_loop();
	SDL_Event event;
	while (SDL_WaitEvent(&event)) {
		continue;
	}

	while(1){}
	volatile int j;
	j = 1 / 0;
	while(1){
	}
	return 0; /* blah */
}
