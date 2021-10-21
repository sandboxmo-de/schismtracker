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
