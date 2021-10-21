## vi:set bd=syn\ make: for elvis

AUTOMAKE_OPTIONS = foreign dist-bzip2 no-dist-gzip

## NOTE: helptexts should be in the same order as the enum in page.h
sysfiles = \
	sys/fd.org/autopackage.apspec	\
	sys/fd.org/itf.desktop		\
	sys/fd.org/schism.desktop	\
	sys/win32/schism.nsis		\
	sys/sdl/README
scripts = \
	scripts/bin2h.sh		\
	scripts/genhelp.py		\
	scripts/itf2half.py		\
	scripts/half2itf.py		\
	scripts/itcfg.py		\
	scripts/itmidicfg.py		\
	scripts/lutgen.c		\
	scripts/palette.py

bin_PROGRAMS = schismtracker

noinst_HEADERS = \
	include/auto/logoit.h		\
	include/auto/logoschism.h	\
	include/auto/schismico.h	\
	include/cmixer.h		\
	include/config-parser.h		\
	include/dmoz.h			\
	include/event.h			\
	include/fmopl2.h			\
	include/fmopl3.h			\
	include/fmt.h			\
	include/fmt-types.h		\
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
	include/util.h			\
	sys/win32/wine-ddraw.h

desktopdir = $(datadir)/applications

appicondir = $(datadir)/pixmaps

noinst_SCRIPTS = $(scripts)

if USE_WIN32
files_win32 = \
	sys/win32/slurp-win32.c		\
	sys/win32/midi-win32mm.c	\
	sys/win32/localtime_r.c
cflags_win32=-I$(srcdir)/sys/win32
lib_win32=-lwinmm
endif

if HAVE_WINDRES

## use today's date if we didn't get a commit date from git
if HAVE_GIT
wrcflags_version = -DWRC_VERSION=0,`echo '$(PACKAGE_VERSION)' | sed 's/\(....\)\(..\)\(..\).*/\1,\2,\3/'`
else
wrcflags_version = -DWRC_VERSION=0,`date +%Y%m%d | sed 's/\(....\)\(..\)\(..\).*/\1,\2,\3/'`
endif

## --use-temp-file is needed to work around stupid bugs
WRCFLAGS = --use-temp-file -I. -I$(srcdir) $(cflags_version) $(wrcflags_version)
.rc.$(OBJEXT):
	$(WINDRES) $(WRCFLAGS) -i $< -o $@
files_windres=sys/win32/schismres.rc
sys/win32/schismres.$(OBJEXT): config.h Makefile.am
endif HAVE_WINDRES


## Replacement functions for crappy systems
files_stdlib =
if NEED_ASPRINTF
files_stdlib += sys/stdlib/asprintf.c
endif
if NEED_VASPRINTF
files_stdlib += sys/stdlib/vasprintf.c
endif
if NEED_MEMCMP
files_stdlib += sys/stdlib/memcmp.c
endif
if NEED_STRPTIME
files_stdlib += sys/stdlib/strptime.c
endif
if NEED_MKSTEMP
files_stdlib += sys/stdlib/mkstemp.c
endif


## aaaaaaaaahhhhhhhhhhhhhhhhhhh!!!!!!!1
schismtracker_SOURCES = \
	schism/sample-edit.c		\
	schism/dmoz.c			\
	schism/log.c		\
	schism/mplink.c			\
	fmt/compression.c		\
	fmt/generic.c			\
	fmt/mmcmp.c			\
	fmt/669.c			\
	fmt/aiff.c			\
	fmt/ams.c			\
	fmt/au.c			\
	fmt/f2r.c			\
	fmt/far.c			\
	fmt/imf.c			\
	fmt/it.c			\
	fmt/iti.c			\
	fmt/its.c			\
	fmt/liq.c			\
	fmt/mdl.c			\
	fmt/med.c			\
	fmt/mf.c			\
	fmt/mid.c                       \
	fmt/mod.c			\
	fmt/mt2.c			\
	fmt/mtm.c			\
	fmt/mus.c			\
	fmt/ntk.c			\
	fmt/okt.c			\
	fmt/pat.c			\
	fmt/raw.c			\
	fmt/s3i.c			\
	fmt/s3m.c			\
	fmt/sfx.c			\
	fmt/stm.c			\
	fmt/ult.c			\
	fmt/wav.c			\
	fmt/xi.c			\
	fmt/xm.c			\
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
	player/tables.c			\
	$(files_win32)			\
	$(files_stdlib)

# have version.o rely on all files
schism/version.$(OBJEXT): $(filter-out schism/version.$(OBJEXT),$(schismtracker_OBJECTS)) $(HEADERS)

cflags_fmopl=-DHAS_YM3812=1 -DHAS_Y8950=0 -DHAS_YM3526=0

AM_CPPFLAGS = -D_USE_AUTOCONF -D_GNU_SOURCE -I$(srcdir)/include -I.
AM_CFLAGS = $(SDL_CFLAGS) $(cflags_version) $(cflags_win32)
AM_OBJCFLAGS = $(AM_CFLAGS)

schismtracker_DEPENDENCIES = $(files_windres)
schismtracker_LDADD = $(lib_asound) $(lib_win32) $(SDL_LIBS) $(LIBM)
