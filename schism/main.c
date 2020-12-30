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

/* This is the first thing I *ever* did with SDL. :) */

/* Warning: spaghetti follows.... it just kind of ended up this way.
   In other news, TODO: clean this mess on a rainy day.
   and yes, that in fact rhymed :P */

#define NEED_TIME
#include "headers.h"

#include "event.h"

#include "it.h"
#include "disko.h"

#include "version.h"
#include "song.h"
#include "midi.h"
#include "dmoz.h"

#include "osdefs.h"

#include <errno.h>

#include "sdlmain.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef WIN32
# include <signal.h>
#endif

#include <getopt.h>

#if !defined(__amigaos4__) && !defined(GEKKO)
# define ENABLE_HOOKS 1
#endif

#define NATIVE_SCREEN_WIDTH     640
#define NATIVE_SCREEN_HEIGHT    400

/* --------------------------------------------------------------------- */
/* globals */

enum {
	EXIT_HOOK = 1,
	EXIT_SAVECFG = 4,
	EXIT_SDLQUIT = 16,
};
static int shutdown_process = 0;

static const char *audio_driver = NULL;

/* --------------------------------------------------------------------- */

/* If we're not not debugging, don't not dump core. (Have I ever mentioned
 * that NDEBUG is poorly named -- or that identifiers for settings in the
 * negative form are a bad idea?) */
#if defined(NDEBUG)
# define SDL_INIT_FLAGS SDL_INIT_TIMER
#else
# define SDL_INIT_FLAGS SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE
#endif

static void sdl_init(void)
{
	char *err;
	if (SDL_Init(SDL_INIT_FLAGS) == 0)
		return;
	err = SDL_GetError();
	fprintf(stderr, "SDL_Init: %s\n", err);
	exit(1);
}

/* --------------------------------------------------------------------- */

#if ENABLE_HOOKS
static void run_startup_hook(void)
{
	run_hook(cfg_dir_dotschism, "startup-hook", NULL);
}
static void run_exit_hook(void)
{
	run_hook(cfg_dir_dotschism, "exit-hook", NULL);
}
#endif

/* --------------------------------------------------------------------- */
/* arg parsing */

/* filename of song to load on startup, or NULL for none */
#ifdef MACOSX
char *initial_song = NULL;
#else
static char *initial_song = NULL;
#endif

#ifdef MACOSX
static int ibook_helper = -1;
#endif

/* initial module directory */
static char *initial_dir = NULL;

/* startup flags */
enum {
	SF_PLAY = 1, /* -p: start playing after loading initial_song */
	SF_HOOKS = 2, /* --no-hooks: don't run startup/exit scripts */
	SF_FONTEDIT = 4,
	SF_CLASSIC = 8,
	SF_NETWORK = 16,
};
static int startup_flags = SF_HOOKS | SF_NETWORK;

/* getopt ids */
#define SHORT_OPTIONS "a:v:fFpPh" // these should correspond to the characters below (trailing colon indicates argument)
enum {
	// short options
	O_SDL_AUDIODRIVER = 'a',
	O_SDL_VIDEODRIVER = 'v',
	O_FULLSCREEN = 'f', O_NO_FULLSCREEN = 'F',
	O_PLAY = 'p', O_NO_PLAY = 'P',
	O_HELP = 'h',
	// ids for long options with no corresponding short option
	O_VIDEO_YUVLAYOUT = 256,
	O_VIDEO_RESOLUTION,
	O_VIDEO_STRETCH, O_NO_VIDEO_STRETCH,
#if USE_OPENGL
	O_VIDEO_GLPATH,
#endif
	O_VIDEO_DEPTH,
#if HAVE_SYS_KD_H
	O_VIDEO_FBDEV,
#endif
#if USE_NETWORK
	O_NETWORK, O_NO_NETWORK,
#endif
#ifdef USE_X11
	O_DISPLAY,
#endif
	O_CLASSIC_MODE, O_NO_CLASSIC_MODE,
	O_FONTEDIT, O_NO_FONTEDIT,
#if ENABLE_HOOKS
	O_HOOKS, O_NO_HOOKS,
#endif
	O_DISKWRITE,
	O_DEBUG,
	O_VERSION,
};

#if defined(WIN32)
# define OPENGL_PATH "\\path\\to\\opengl.dll"
#elif defined(MACOSX)
# define OPENGL_PATH "/path/to/opengl.dylib"
#else
# define OPENGL_PATH "/path/to/opengl.so"
#endif

#define USAGE "Usage: %s [OPTIONS] [DIRECTORY] [FILE]\n"

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

static void schism_shutdown(void)
{
#if ENABLE_HOOKS
	if (shutdown_process & EXIT_HOOK)
		run_exit_hook();
#endif
	if (shutdown_process & EXIT_SAVECFG)
		cfg_atexit_save();

#ifdef MACOSX
	if (ibook_helper != -1)
		macosx_ibook_fnswitch(ibook_helper);
#endif
	if (shutdown_process & EXIT_SDLQUIT) {
		song_lock_audio();
		song_stop_unlocked(1);
		song_unlock_audio();

		// Clear to black on exit (nicer on Wii; I suppose it won't hurt elsewhere)
//		video_refresh();
//		video_blit();
//		video_shutdown();
		/*
		If this is the atexit() handler, why are we calling SDL_Quit?

		Simple, SDL's documentation says always call SDL_Quit. :) In
		fact, in the examples they recommend writing atexit(SDL_Quit)
		directly after SDL_Init. I'm not sure exactly why, but I don't
		see a reason *not* to do it...
			/ Storlek
		*/
		SDL_Quit();
	}
	os_sysexit();
}

int main(int argc, char **argv)
{
	os_sysinit(&argc, &argv);

	/* this needs to be done very early, because the version is used in the help text etc.
	Also, this needs to happen before any locale stuff is initialized
	(but we don't do that at all yet, anyway) */
//	ver_init();

	/* FIXME: make a config option for this, and stop abusing frickin' environment variables! */
//	put_env_var("SCHISM_VIDEO_ASPECT", "full");

//	vis_init();
	atexit(schism_shutdown);

//	video_fullscreen(0);

	tzset(); // localtime_r wants this
	srand(time(NULL));
	parse_only_initial_song(argc, argv);

#ifdef USE_DLTRICK_ALSA
	alsa_dlinit();
#endif

	cfg_init_dir();

#if ENABLE_HOOKS
	if (startup_flags & SF_HOOKS) {
		run_startup_hook();
		shutdown_process |= EXIT_HOOK;
	}
#endif
#ifdef MACOSX
	ibook_helper = macosx_ibook_fnswitch(1);
#endif

	/* Eh. */
	log_append2(0, 3, 0, schism_banner(0));
	log_nl();
	log_nl();

	song_initialise();
	cfg_load();

	if (!(startup_flags & SF_NETWORK)) {
		status.flags |= NO_NETWORK;
	}

	shutdown_process |= EXIT_SAVECFG;

	sdl_init();
	shutdown_process |= EXIT_SDLQUIT;
	os_sdlinit();

	midi_engine_start();
	log_nl();
	audio_init(audio_driver);
	song_init_modplug();

#ifndef WIN32
	signal(SIGINT, exit);
	signal(SIGQUIT, exit);
	signal(SIGTERM, exit);
#endif

	volume_setup();

//	load_pages();
//	main_song_changed_cb();

	if (initial_song && !initial_dir) {
		initial_dir = get_parent_directory(initial_song);
		if (!initial_dir) {
			initial_dir = get_current_directory();
		}
	}
	if (initial_dir) {
		strncpy(cfg_dir_modules, initial_dir, PATH_MAX);
		cfg_dir_modules[PATH_MAX] = 0;
		strncpy(cfg_dir_samples, initial_dir, PATH_MAX);
		cfg_dir_samples[PATH_MAX] = 0;
		strncpy(cfg_dir_instruments, initial_dir, PATH_MAX);
		cfg_dir_instruments[PATH_MAX] = 0;
		free(initial_dir);
	}

	if (startup_flags & SF_FONTEDIT) {
		status.flags |= STARTUP_FONTEDIT;
//		set_page(PAGE_FONT_EDIT);
		free(initial_song);
	} else if (initial_song) {
//		set_page(PAGE_LOG);
		if (song_load_unchecked(initial_song)) {
//			if (diskwrite_to) {
//				// make a guess?
//				const char *multi = strcasestr(diskwrite_to, "%c");
//				const char *driver = (strcasestr(diskwrite_to, ".aif")
//						      ? (multi ? "MAIFF" : "AIFF")
//						      : (multi ? "MWAV" : "WAV"));
//				if (song_export(diskwrite_to, driver) != SAVE_SUCCESS)
//					exit(1); // ?
//			} else if (startup_flags & SF_PLAY) {
				song_start();
//				set_page(PAGE_INFO);
//			}
		}
		free(initial_song);
	} else {
//		set_page(PAGE_ABOUT);
	}

#if HAVE_NICE
	if (nice(1) == -1) {
	}
#endif
	/* poll once */
	midi_engine_poll_ports();

	//	event_loop();
	SDL_Event event;
	while (SDL_WaitEvent(&event)) {
		continue;
	}
	while(1){}
	fprintf(stdout, "SDL_Init:\n");
	volatile int j;
	j = 1 / 0;
	while(1){
	}
	return 0; /* blah */
}

