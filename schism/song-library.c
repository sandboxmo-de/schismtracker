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

/* The file browser's view into module and instrument files ("libraries"):
 * the instruments or samples inside one are listed like files in a
 * directory. Moved out of audio_loadsave.c since only the tracker uses it. */

#include "headers.h"

#include "song.h"
#include "dmoz.h"
#include "fmt.h"
#include "log.h"
#include "mem.h"
#include "osdefs.h"
#include "engine-alloc.h"

#include "player/sndfile.h"

// FIXME: unload the module when leaving the library 'directory'
static song_t *library = NULL;


// TODO: stat the file?
int dmoz_read_instrument_library(const char *path, dmoz_filelist_t *flist, SCHISM_UNUSED dmoz_dirlist_t *dlist)
{
	unsigned int j;
	int x;

	// why does this do this ? seems to be a no-op
	//
	// it's because we have a stupid "fake sample slot" that we
	// use for the sample/instrument preview "preload", which
	// just happens to now be hardcoded at slot 0.
	csf_stop_sample(current_song, current_song->samples + 0);

	if (library) {
		csf_free(library);
		library = NULL;
	}

	const char *base = dmoz_path_get_basename(path);
	library = song_create_load(path);
	if (!library) {
		log_appendf(4, "%s: %s", base, fmt_strerror(errno));
		return -1;
	}

	for (int n = 1; n < MAX_INSTRUMENTS; n++) {
		if (!library->instruments[n])
			continue;

		dmoz_file_t *file = dmoz_add_file(flist,
			str_dup(path), str_dup(base), NULL, n);
		file->title = engine_strdup(library->instruments[n]->name, ENGINE_MEM_ENGINE);

		int count[128] = {0};

		file->sampsize = 0;
		file->filesize = 0;
		file->instnum = n;
		for (j = 0; j < 128; j++) {
			x = library->instruments[n]->sample_map[j];
			if (!count[x]) {
				if (x > 0 && x < MAX_INSTRUMENTS) {
					file->filesize += library->samples[x].length;
					file->sampsize++;
				}
			}
			count[x]++;
		}

		file->type = TYPE_INST_ITI;
		file->description = "Fishcakes";
		// IT doesn't support this, despite it being useful.
		// Simply "unrecognized"
	}

	return 0;
}


int dmoz_read_sample_library(const char *path, dmoz_filelist_t *flist, SCHISM_UNUSED dmoz_dirlist_t *dlist)
{
	csf_stop_sample(current_song, current_song->samples + 0);

	if (library) {
		csf_free(library);
		library = NULL;
	}

	const char *base = dmoz_path_get_basename(path);

	struct stat st;
	if (os_stat(path, &st) < 0) {
		log_perror(path);
		return -1;
	}

	/* ask dmoz what type of file we have
	 * FIXME use slurp and read info funcs manually */
	dmoz_file_t info_file = {0};
	info_file.path = str_dup(path);
	info_file.filesize = st.st_size;
	dmoz_fill_ext_data(&info_file);

	/* free extra data we don't need */
	if (info_file.smp_filename != info_file.base &&
			info_file.smp_filename != info_file.title) {
		engine_free(info_file.smp_filename, ENGINE_MEM_ENGINE);
	}

	free(info_file.path);
	free(info_file.base);

	if (info_file.type & TYPE_EXT_DATA_MASK) {
		if (info_file.artist)
			engine_free(info_file.artist, ENGINE_MEM_ENGINE);
		engine_free(info_file.title, ENGINE_MEM_ENGINE);
	}

	if (info_file.type & TYPE_MODULE_MASK) {
		library = song_create_load(path);
	} else if (info_file.type & TYPE_INST_MASK) {
		/* temporarily set the current song to the library; song_load_instrument
		 * is hardcoded to it */
		song_lock_audio();
		song_t *tmp_ptr = current_song;
		library = current_song = csf_allocate();

		int ret = song_load_instrument(1, path);
		current_song = tmp_ptr;
		song_unlock_audio();
		if (!ret) {
			log_appendf(4, "song_load_instrument: %s failed with %d", path, ret);
			return 1;
		}
	} else {
		return 0;
	}

	for (int n = 1; n < MAX_SAMPLES; n++) {
		if (library->samples[n].length) {
			for (int c = 0; c < 25; c++) {
				if (library->samples[n].name[c] == 0)
					library->samples[n].name[c] = 32;
				library->samples[n].name[25] = 0;
			}
			dmoz_file_t *file = dmoz_add_file(flist, str_dup(path), str_dup(base), NULL, n);
			file->type = TYPE_SAMPLE_EXTD;
			file->description = "Impulse Tracker Sample"; /* FIXME: this lies for XI and PAT */
			file->filesize = library->samples[n].length*((library->samples[n].flags & CHN_STEREO) + 1)*((library->samples[n].flags & CHN_16BIT) + 1);
			file->smp_speed = library->samples[n].c5speed;
			file->smp_loop_start = library->samples[n].loop_start;
			file->smp_loop_end = library->samples[n].loop_end;
			file->smp_sustain_start = library->samples[n].sustain_start;
			file->smp_sustain_end = library->samples[n].sustain_end;
			file->smp_length = library->samples[n].length;
			file->smp_flags = library->samples[n].flags;
			file->smp_defvol = library->samples[n].volume>>2;
			file->smp_gblvol = library->samples[n].global_volume;
			file->smp_vibrato_speed = library->samples[n].vib_speed;
			file->smp_vibrato_depth = library->samples[n].vib_depth;
			file->smp_vibrato_rate = library->samples[n].vib_rate;
			// don't screw this up...
			if (((unsigned char)library->samples[n].name[23]) == 0xFF) {
				library->samples[n].name[23] = ' ';
			}
			file->title = engine_strdup(library->samples[n].name, ENGINE_MEM_ENGINE);
			file->sample = (song_sample_t *) library->samples + n;
		}
	}

	return 0;
}
