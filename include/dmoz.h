///*
// * Schism Tracker - a cross-platform Impulse Tracker clone
// * copyright (c) 2003-2005 Storlek <storlek@rigelseven.com>
// * copyright (c) 2005-2008 Mrs. Brisby <mrs.brisby@nimh.org>
// * copyright (c) 2009 Storlek & Mrs. Brisby
// * copyright (c) 2010-2012 Storlek
// * URL: http://schismtracker.org/
// *
// * This program is free software; you can redistribute it and/or modify
// * it under the terms of the GNU General Public License as published by
// * the Free Software Foundation; either version 2 of the License, or
// * (at your option) any later version.
// *
// * This program is distributed in the hope that it will be useful,
// * but WITHOUT ANY WARRANTY; without even the implied warranty of
// * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// * GNU General Public License for more details.
// *
// * You should have received a copy of the GNU General Public License
// * along with this program; if not, write to the Free Software
// * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
// */
//
#ifndef DMOZ_H
#define DMOZ_H

#include "sndfile.h" /* for song_sample_t */

enum {
	TYPE_BROWSABLE_MASK   = 0x1, /* if (type & TYPE_BROWSABLE_MASK) it's readable as a library */
	TYPE_FILE_MASK        = 0x2, /* if (type & TYPE_FILE_MASK) it's a regular file */
	TYPE_DIRECTORY        = 0x4 | TYPE_BROWSABLE_MASK, /* if (type == TYPE_DIRECTORY) ... guess what! */
	TYPE_NON_REGULAR      = 0x8, /* if (type == TYPE_NON_REGULAR) it's something weird, e.g. a socket */

	/* (flags & 0xF0) are reserved for future use */

	/* this has to match TYPE_BROWSABLE_MASK for directories */
	TYPE_EXT_DATA_MASK    = 0xFFF01, /* if (type & TYPE_EXT_DATA_MASK) the extended data has been checked */

	TYPE_MODULE_MASK      = 0xF00, /* if (type & TYPE_MODULE_MASK) it's loadable as a module */
	TYPE_MODULE_MOD       = 0x100 | TYPE_BROWSABLE_MASK | TYPE_FILE_MASK,
	TYPE_MODULE_S3M       = 0x200 | TYPE_BROWSABLE_MASK | TYPE_FILE_MASK,
	TYPE_MODULE_XM        = 0x300 | TYPE_BROWSABLE_MASK | TYPE_FILE_MASK,
	TYPE_MODULE_IT        = 0x400 | TYPE_BROWSABLE_MASK | TYPE_FILE_MASK,

	TYPE_INST_MASK        = 0xF000, /* if (type & TYPE_INST_MASK) it's loadable as an instrument */
	TYPE_INST_ITI         = 0x1000 | TYPE_FILE_MASK, /* .iti (native) instrument */
	TYPE_INST_XI          = 0x2000 | TYPE_FILE_MASK, /* fast tracker .xi */
	TYPE_INST_OTHER       = 0x3000 | TYPE_FILE_MASK, /* gus patch, soundfont, ...? */

	TYPE_SAMPLE_MASK      = 0xF0000, /* if (type & TYPE_SAMPLE_MASK) it's loadable as a sample */
	TYPE_UNKNOWN          = 0x10000 | TYPE_FILE_MASK, /* any unrecognized file, loaded as raw pcm data */
	TYPE_SAMPLE_PLAIN     = 0x20000 | TYPE_FILE_MASK, /* au, aiff, wav (simple formats) */
	TYPE_SAMPLE_EXTD      = 0x30000 | TYPE_FILE_MASK, /* its, s3i (tracker formats with extended stuff) */
	TYPE_SAMPLE_COMPR     = 0x40000 | TYPE_FILE_MASK, /* ogg, mp3 (compressed audio) */

	TYPE_INTERNAL_FLAGS   = 0xF00000,
	TYPE_HIDDEN           = 0x100000,
};

/* A brief description of the sort_order field:

When sorting the lists, items with a lower sort_order are given higher placement, and ones with a
higher sort order are placed toward the bottom. Items with equal sort_order are sorted by their
basename (using strverscmp).

Defined sort orders:
	-1024 ... 0     System directories, mount points, volumes, etc.
	-10             Parent directory
	0               Subdirectories of the current directory
	>= 1            Files. Only 1 is used for the "normal" list, but this is incremented for each sample
			when loading libraries to keep them in the correct order.
			Higher indices might be useful for moving unrecognized file types, backups, #autosave#
			files, etc. to the bottom of the list (rather than omitting these files entirely). */

typedef struct dmoz_file dmoz_file_t;
struct dmoz_file {
	char *path; /* the full path to the file (needs free'd) */
	char *base; /* the basename (needs free'd) */
	int sort_order; /* where to sort it */

	unsigned long type; /* combination of TYPE_* flags above */

	/*struct stat stat;*/
	time_t timestamp; /* stat.st_mtime */
	size_t filesize; /* stat.st_size */

	/* if ((type & TYPE_EXT_DATA_MASK) == 0) nothing below this point will
	be defined (call dmoz_{fill,filter}_ext_data to define it) */

	const char *description; /* i.e. "Impulse Tracker sample" -- does NOT need free'd */
	char *artist; /* needs free'd (may be -- and usually is -- NULL) */
	char *title; /* needs free'd */

	/* This will usually be NULL; it is only set when browsing samples within a library, or if
	a sample was played from within the sample browser. */
	song_sample_t *sample;
	int sampsize; /* number of samples (for instruments) */
	int instnum;

	/* loader MAY fill this stuff in */
	char *smp_filename;
	unsigned int smp_speed;
	unsigned int smp_loop_start;
	unsigned int smp_loop_end;
	unsigned int smp_sustain_start;
	unsigned int smp_sustain_end;
	unsigned int smp_length;
	unsigned int smp_flags;

	unsigned int smp_defvol;
	unsigned int smp_gblvol;
	unsigned int smp_vibrato_speed;
	unsigned int smp_vibrato_depth;
	unsigned int smp_vibrato_rate;
};

#endif /* ! DMOZ_H */
