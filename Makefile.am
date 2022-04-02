## vi:set bd=syn\ make: for elvis

AUTOMAKE_OPTIONS = foreign dist-bzip2 no-dist-gzip

bin_PROGRAMS = schismtracker

noinst_HEADERS = \
	include/cmixer.h		\
	include/dmoz.h			\
	include/event.h			\
	include/fmopl2.h			\
	include/fmopl3.h			\
	include/fmt.h			\
	include/headers.h		\
	include/it_defs.h		\
	include/log.h			\
	include/midi.h			\
	include/precomp_lut.h		\
	include/sample-edit.h		\
	include/sdlmain.h		\
	include/slurp.h			\
	include/sndfile.h		\
	include/snd_fm.h		\
	include/snd_gm.h		\
	include/song.h			\
	include/tables.h		\
	include/util.h

schismtracker_SOURCES = \
	schism/sample-edit.c		\
	schism/log.c		\
	schism/mplink.c			\
	fmt/compression.c		\
	fmt/generic.c			\
	fmt/it.c			\
	schism/util.c			\
	schism/slurp.c			\
	schism/main.c			\
	schism/midi-core.c		\
	schism/audio_playback.c		\
	schism/audio_loadsave.c		\
	player/csndfile.c		\
	player/mixutil.c		\
	player/equalizer.c		\
	player/mixer.c			\
	player/filters.c		\
	player/fmopl2.c			\
	player/fmopl3.c			\
	player/fmpatches.c		\
	player/sndmix.c			\
	player/opl-util.c		\
	player/snd_fm.c			\
	player/effects.c		\
	player/snd_gm.c			\
	player/tables.c

cflags_fmopl=-DHAS_YM3812=1 -DHAS_Y8950=0 -DHAS_YM3526=0

AM_CPPFLAGS = -D_USE_AUTOCONF -D_GNU_SOURCE -I$(srcdir)/include -I.
AM_CFLAGS = $(SDL_CFLAGS) $(cflags_version) $(cflags_win32)
AM_OBJCFLAGS = $(AM_CFLAGS)

schismtracker_LDADD = $(lib_asound) $(lib_win32) $(SDL_LIBS) $(LIBM)
