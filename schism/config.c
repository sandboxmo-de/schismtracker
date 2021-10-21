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

#include "it.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "config-parser.h"
#include "dmoz.h"
#include "osdefs.h"

/* --------------------------------------------------------------------- */
/* config settings */

char cfg_dir_modules[PATH_MAX + 1], cfg_dir_samples[PATH_MAX + 1], cfg_dir_instruments[PATH_MAX + 1],
	cfg_dir_dotschism[PATH_MAX + 1];

/* --------------------------------------------------------------------- */

#if defined(WIN32)
# define DOT_SCHISM "Schism Tracker"
#elif defined(MACOSX)
# define DOT_SCHISM "Library/Application Support/Schism Tracker"
#elif defined(GEKKO)
# define DOT_SCHISM "."
#else
# define DOT_SCHISM ".schism"
#endif

/* --------------------------------------------------------------------------------------------------------- */

void cfg_load(void)
{
	char *tmp;
//	int i;
	cfg_file_t cfg;

	tmp = dmoz_path_concat(cfg_dir_dotschism, "config");
	cfg_init(&cfg, tmp);
	free(tmp);

	/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//	tmp = get_home_directory();
//	cfg_get_string(&cfg, "Directories", "modules", cfg_dir_modules, PATH_MAX, tmp);
//	cfg_get_string(&cfg, "Directories", "samples", cfg_dir_samples, PATH_MAX, tmp);
//	cfg_get_string(&cfg, "Directories", "instruments", cfg_dir_instruments, PATH_MAX, tmp);
//	free(tmp);

	/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//	cfg_load_info(&cfg);
//	cfg_load_patedit(&cfg);
	cfg_load_audio(&cfg);
	cfg_load_midi(&cfg);
	cfg_load_disko(&cfg);
	cfg_load_dmoz(&cfg);

	/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	if (cfg_get_number(&cfg, "General", "classic_mode", 0))
		status.flags |= CLASSIC_MODE;
	else
		status.flags &= ~CLASSIC_MODE;
	if (cfg_get_number(&cfg, "General", "make_backups", 1))
		status.flags |= MAKE_BACKUPS;
	else
		status.flags &= ~MAKE_BACKUPS;
	if (cfg_get_number(&cfg, "General", "numbered_backups", 0))
		status.flags |= NUMBERED_BACKUPS;
	else
		status.flags &= ~NUMBERED_BACKUPS;

	if (cfg_get_number(&cfg, "General", "midi_like_tracker", 0))
		status.flags |= MIDI_LIKE_TRACKER;
	else
		status.flags &= ~MIDI_LIKE_TRACKER;

	/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	cfg_free(&cfg);
}

//void cfg_save(void)
//{
//	char *ptr;
//	cfg_file_t cfg;
//
//	ptr = dmoz_path_concat(cfg_dir_dotschism, "config");
//	cfg_init(&cfg, ptr);
//	free(ptr);
//
//	// this wart is here because Storlek is retarded
//	cfg_delete_key(&cfg, "Directories", "filename_pattern");
//
//	cfg_set_string(&cfg, "Directories", "modules", cfg_dir_modules);
//	cfg_set_string(&cfg, "Directories", "samples", cfg_dir_samples);
//	cfg_set_string(&cfg, "Directories", "instruments", cfg_dir_instruments);
//
////	cfg_save_info(&cfg);
////	cfg_save_patedit(&cfg);
//	cfg_save_audio(&cfg);
////	cfg_save_palette(&cfg);
//	cfg_save_disko(&cfg);
//	cfg_save_dmoz(&cfg);
//
//	cfg_write(&cfg);
//	cfg_free(&cfg);
//}

//void cfg_atexit_save(void)
//{
//	char *ptr;
//	cfg_file_t cfg;
//
//	ptr = dmoz_path_concat(cfg_dir_dotschism, "config");
//	cfg_init(&cfg, ptr);
//	free(ptr);
//
//	cfg_atexit_save_audio(&cfg);
//
//	cfg_set_number(&cfg, "General", "classic_mode", !!(status.flags & CLASSIC_MODE));
//	cfg_set_number(&cfg, "General", "make_backups", !!(status.flags & MAKE_BACKUPS));
//	cfg_set_number(&cfg, "General", "numbered_backups", !!(status.flags & NUMBERED_BACKUPS));
//
//	cfg_set_number(&cfg, "General", "midi_like_tracker", !!(status.flags & MIDI_LIKE_TRACKER));
//
//	/* hm... most of the time probably nothing's different, so saving the
//	config file here just serves to make the backup useless. maybe add a
//	'dirty' flag to the config parser that checks if any settings are
//	actually *different* from those in the file? */
//
//	cfg_write(&cfg);
//	cfg_free(&cfg);
//}

