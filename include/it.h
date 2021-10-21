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

#ifndef IT_H
#define IT_H

#include <stdio.h>
#include <stdint.h>

#include <sys/types.h>

#include "util.h"
#include "log.h"

/* --------------------------------------------------------------------- */
/* structs 'n enums */

/* tracker_status flags
   eventual TODO: split this into two sections or something so we don't run
   out of bits to toggle... and while we're at it, namespace them with
   CFG_ (for the configuration stuff -- bits that are accessible through the
   interface in some way) and uh, something else for the internal status flags
   like IS_VISIBLE or whatever */
enum {
	/* if this flag is set, the screen will be redrawn */
//	NEED_UPDATE = (1 << 0),

	/* is the current palette "backwards"? (used to make the borders look right) */
//	INVERTED_PALETTE = (1 << 1),

	DIR_MODULES_CHANGED = (1 << 2),
	DIR_SAMPLES_CHANGED = (1 << 3),
	DIR_INSTRUMENTS_CHANGED = (1 << 4),

	/* these refer to the window's state.
	 * (they're rather useless on the console ;) */
//	IS_FOCUSED = (1 << 5),
//	IS_VISIBLE = (1 << 6),
//	WM_AVAILABLE = (1 << 7),

	/* if this is set, some stuff behaves differently
	 * (grep the source files for what stuff ;) */
	CLASSIC_MODE = (1 << 8),

	/* make a backup file (song.it~) when saving a module? */
	MAKE_BACKUPS = (1 << 9),
	NUMBERED_BACKUPS = (1 << 10), /* song.it.3~ */

//	LAZY_REDRAW = (1 << 11),

	/* this is here if anything is "changed" and we need to whine to
	the user if they quit */
	SONG_NEEDS_SAVE = (1 << 12),

	/* if the software mouse pointer moved.... */
//	SOFTWARE_MOUSE_MOVED = (1 << 13),

	/* pasting is done by setting a flag here, the main event loop then synthesizes
	the various events... after we return */
//	CLIPPY_PASTE_SELECTION = (1 << 14),
//	CLIPPY_PASTE_BUFFER = (1 << 15),

	/* if the disko is active */
	DISKWRITER_ACTIVE = (1 << 16),
	DISKWRITER_ACTIVE_PATTERN = (1 << 17), /* recording only a single pattern */

	/* mark... set by midi core when received new midi event */
//	MIDI_EVENT_CHANGED = (1 << 18),

	/* poop */
//	ACCIDENTALS_AS_FLATS = (1 << 19),

	/* fontedit */
//	STARTUP_FONTEDIT = (1 << 20),

	/* key hacks -- should go away when keyboard redefinition is possible */
//	META_IS_CTRL = (1 << 21),
//	ALTGR_IS_ALT = (1 << 22),

	/* holding shift (used on pattern editor for weird template thing) */
//	SHIFT_KEY_DOWN = (1 << 23),

	/* Devi Ever's hack */
//	CRAYOLA_MODE = (1 << 25),

	/* holding caps */
//	CAPS_PRESSED = (1 << 26),

	NO_NETWORK = (1 << 27),
//	NO_MOUSE = (1 << 28),

	/* Play MIDI events using the same semantics as tracker samples */
	MIDI_LIKE_TRACKER = (1 << 29),

	/* if true, don't stop playing on load, and start playing new song afterward
	(but only if the last song was already playing before loading) */
	PLAY_AFTER_LOAD = (1 << 30),
};

struct tracker_status {
	int flags;
};

/* --------------------------------------------------------------------- */
/* global crap */

extern struct tracker_status status;

/* --------------------------------------------------------------------- */
/* settings (config.c) */

extern char cfg_dir_modules[], cfg_dir_samples[], cfg_dir_instruments[];
extern char cfg_dir_dotschism[]; /* the full path to ~/.schism */

void cfg_init_dir(void);
void cfg_load(void);
void cfg_save(void);
void cfg_atexit_save(void); /* this only saves a handful of settings, not everything */

/* each page with configurable settings has a function to load/save them... */
#include "config-parser.h" /* FIXME: shouldn't need this here */

void cfg_load_midi(cfg_file_t *cfg);
void cfg_save_midi(cfg_file_t *cfg);

void cfg_load_patedit(cfg_file_t *cfg);
void cfg_save_patedit(cfg_file_t *cfg);

void cfg_load_info(cfg_file_t *cfg);
void cfg_save_info(cfg_file_t *cfg);

void cfg_load_audio(cfg_file_t *cfg);
void cfg_save_audio(cfg_file_t *cfg);
void cfg_atexit_save_audio(cfg_file_t *cfg);

void cfg_load_disko(cfg_file_t *cfg);
void cfg_save_disko(cfg_file_t *cfg);

void cfg_load_dmoz(cfg_file_t *cfg);
void cfg_save_dmoz(cfg_file_t *cfg);

/* --------------------------------------------------------------------- */

#endif /* ! IT_H */

