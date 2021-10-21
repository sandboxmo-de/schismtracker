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

#define NEED_BYTESWAP
#define NEED_TIME
#include "headers.h"

#include "sndfile.h"
#include "song.h"
#include "slurp.h"

#include "fmt.h"
#include "dmoz.h"

#include "it_defs.h"

#include "snd_gm.h"
#include "midi.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

// ------------------------------------------------------------------------

char song_filename[PATH_MAX + 1];
char song_basename[NAME_MAX + 1];

// ------------------------------------------------------------------------
// replace any '\0' chars with spaces, mostly to make the string handling
// much easier.
// TODO | Maybe this should be done with the filenames and the song title
// TODO | as well? (though I've never come across any cases of either of
// TODO | these having null characters in them...)

static void _fix_names(song_t *qq)
{
	int c, n;

	for (n = 1; n < MAX_INSTRUMENTS; n++) {
		for (c = 0; c < 25; c++)
			if (qq->samples[n].name[c] == 0)
				qq->samples[n].name[c] = 32;
		qq->samples[n].name[25] = 0;

		if (!qq->instruments[n])
			continue;
		for (c = 0; c < 25; c++)
			if (qq->instruments[n]->name[c] == 0)
				qq->instruments[n]->name[c] = 32;
		qq->instruments[n]->name[25] = 0;
	}
}

// ------------------------------------------------------------------------
// file stuff

static void song_set_filename(const char *file)
{
	if (file && file[0]) {
		strncpy(song_filename, file, PATH_MAX);
		strncpy(song_basename, get_basename(file), NAME_MAX);
		song_filename[PATH_MAX] = '\0';
		song_basename[NAME_MAX] = '\0';
	} else {
		song_filename[0] = '\0';
		song_basename[0] = '\0';
	}
}

// clear patterns => clear filename and save flag
// clear orderlist => clear title, message, and channel settings
void song_new(int flags)
{
	int i;

	song_lock_audio();

	song_stop_unlocked(0);

	if ((flags & KEEP_PATTERNS) == 0) {
		song_set_filename(NULL);
//		status.flags &= ~SONG_NEEDS_SAVE;

		for (i = 0; i < MAX_PATTERNS; i++) {
			if (current_song->patterns[i]) {
				csf_free_pattern(current_song->patterns[i]);
				current_song->patterns[i] = NULL;
			}
			current_song->pattern_size[i] = 64;
			current_song->pattern_alloc_size[i] = 64;
		}
	}
	if ((flags & KEEP_SAMPLES) == 0) {
		for (i = 1; i < MAX_SAMPLES; i++) {
			if (current_song->samples[i].data) {
				csf_free_sample(current_song->samples[i].data);
			}
		}
		memset(current_song->samples, 0, sizeof(current_song->samples));
		for (i = 1; i < MAX_SAMPLES; i++) {
			current_song->samples[i].c5speed = 8363;
			current_song->samples[i].volume = 64 * 4;
			current_song->samples[i].global_volume = 64;
		}
	}
	if ((flags & KEEP_INSTRUMENTS) == 0) {
		for (i = 0; i < MAX_INSTRUMENTS; i++) {
			if (current_song->instruments[i]) {
				csf_free_instrument(current_song->instruments[i]);
				current_song->instruments[i] = NULL;
			}
		}
	}
	if ((flags & KEEP_ORDERLIST) == 0) {
		memset(current_song->orderlist, ORDER_LAST, sizeof(current_song->orderlist));
		memset(current_song->title, 0, sizeof(current_song->title));
		memset(current_song->message, 0, MAX_MESSAGE);

		for (i = 0; i < 64; i++) {
			current_song->channels[i].volume = 64;
			current_song->channels[i].panning = 128;
			current_song->channels[i].flags = 0;
			current_song->voices[i].volume = 256;
			current_song->voices[i].global_volume = current_song->channels[i].volume;
			current_song->voices[i].panning = current_song->channels[i].panning;
			current_song->voices[i].flags = current_song->channels[i].flags;
			current_song->voices[i].cutoff = 0x7F;
		}
	}

	current_song->repeat_count = 0;
	//song_stop();

	csf_forget_history(current_song);

	song_unlock_audio();

//	main_song_changed_cb();
}

// ------------------------------------------------------------------------------------------------------------

#define LOAD_SONG(x) fmt_##x##_load_song,
static fmt_load_song_func load_song_funcs[] = {
#include "fmt-types.h"
	NULL,
};


const char *fmt_strerror(int n)
{
	switch (n) {
	case -LOAD_UNSUPPORTED:
		return "Unrecognised file type";
	case -LOAD_FORMAT_ERROR:
		return "File format error (corrupt?)";
	default:
		return strerror(errno);
	}
}

// IT uses \r in song messages; replace errant \n's
void message_convert_newlines(song_t *song) {
	int i = 0, len = strlen(song->message);
	for (i = 0; i < len; i++) {
		if (song->message[i] == '\n') {
			song->message[i] = '\r';
		}
	}
}

song_t *song_create_load(const char *file)
{
	fmt_load_song_func *func;
	int ok = 0, err = 0;

	slurp_t *s = slurp(file, NULL, 0);
	if (!s)
		return NULL;

	song_t *newsong = csf_allocate();

	if (current_song) {
		newsong->mix_flags = current_song->mix_flags;
		csf_set_wave_config(newsong,
			current_song->mix_frequency,
			current_song->mix_bits_per_sample,
			current_song->mix_channels);

		// loaders might override these
		newsong->row_highlight_major = current_song->row_highlight_major;
		newsong->row_highlight_minor = current_song->row_highlight_minor;
		csf_copy_midi_cfg(newsong, current_song);
	}

	for (func = load_song_funcs; *func && !ok; func++) {
		slurp_rewind(s);
		switch ((*func)(newsong, s, 0)) {
		case LOAD_SUCCESS:
			err = 0;
			ok = 1;
			break;
		case LOAD_UNSUPPORTED:
			err = -LOAD_UNSUPPORTED;
			continue;
		case LOAD_FORMAT_ERROR:
			err = -LOAD_FORMAT_ERROR;
			break;
		case LOAD_FILE_ERROR:
			err = errno;
			break;
		}
		if (err) {
			csf_free(newsong);
			unslurp(s);
			errno = err;
			return NULL;
		}
	}

	unslurp(s);

	if (err) {
		// awwww, nerts!
		csf_free(newsong);
		errno = err;
		return NULL;
	}

	newsong->stop_at_order = newsong->stop_at_row = -1;
	message_convert_newlines(newsong);
//	message_reset_selection();

	return newsong;
}

int song_load_unchecked(const char *file)
{
	const char *base = get_basename(file);
	int was_playing;
	song_t *newsong;

	// IT stops the song even if the new song can't be loaded
	if (1) {
		was_playing = (song_get_mode() == MODE_PLAYING);
	} else {
		was_playing = 0;
		song_stop();
	}

	newsong = song_create_load(file);
	if (!newsong) {
		return 0;
	}


	song_set_filename(file);

	song_lock_audio();
	csf_free(current_song);
	current_song = newsong;
	current_song->repeat_count = 0;
	max_channels_used = 0;
	_fix_names(current_song);
	song_stop_unlocked(0);
	song_unlock_audio();

	if (was_playing)
		song_start();

//	main_song_changed_cb();

//	status.flags &= ~SONG_NEEDS_SAVE;

	// print out some stuff
	const char *tid = current_song->tracker_id;

	char fmt[] = " %d patterns, %d samples, %d instruments";
	int n, nsmp, nins;
	song_sample_t *smp;
	song_instrument_t **ins;

	for (n = 0, smp = current_song->samples + 1, nsmp = 0; n < MAX_SAMPLES; n++, smp++)
		if (smp->data)
			nsmp++;
	for (n = 0, ins = current_song->instruments + 1, nins = 0; n < MAX_INSTRUMENTS; n++, ins++)
		if (*ins != NULL)
			nins++;

	if (!nins)
		*strrchr(fmt, ',') = 0; // cut off 'instruments'


	return 1;
}

/* ------------------------------------------------------------------------- */

//const struct save_format song_save_formats[] = {
//	{"IT", "Impulse Tracker", ".it", {.save_song = _save_it}},
//	{"S3M", "Scream Tracker 3", ".s3m", {.save_song = fmt_s3m_save_song}},
//	{"MOD", "Amiga ProTracker", ".mod", {.save_song = fmt_mod_save_song}},
//	{.label = NULL}
//};

//#define EXPORT_FUNCS(t) \
//	fmt_##t##_export_head, fmt_##t##_export_silence, fmt_##t##_export_body, fmt_##t##_export_tail
//
//const struct save_format song_export_formats[] = {
//	{"WAV", "WAV", ".wav", {.export = {EXPORT_FUNCS(wav), 0}}},
//	{"MWAV", "WAV multi-write", ".wav", {.export = {EXPORT_FUNCS(wav), 1}}},
//	{"AIFF", "Audio IFF", ".aiff", {.export = {EXPORT_FUNCS(aiff), 0}}},
//	{"MAIFF", "Audio IFF multi-write", ".aiff", {.export = {EXPORT_FUNCS(aiff), 1}}},
//	{.label = NULL}
//};
//// <distance> and maiff sounds like something you'd want to hug
//// <distance> .. dont ask
//
//const struct save_format sample_save_formats[] = {
//	{"ITS", "Impulse Tracker", ".its", {.save_sample = fmt_its_save_sample}},
//	//{"S3I", "Scream Tracker", ".s3i", {.save_sample = fmt_s3i_save_sample}},
//	{"AIFF", "Audio IFF", ".aiff", {.save_sample = fmt_aiff_save_sample}},
//	{"AU", "Sun/NeXT", ".au", {.save_sample = fmt_au_save_sample}},
//	{"WAV", "WAV", ".wav", {.save_sample = fmt_wav_save_sample}},
//	{"RAW", "Raw", ".raw", {.save_sample = fmt_raw_save_sample}},
//	{.label = NULL}
//};

#define LOAD_SAMPLE(x) fmt_##x##_load_sample,
static fmt_load_sample_func load_sample_funcs[] = {
#include "fmt-types.h"
	NULL,
};

#define LOAD_INSTRUMENT(x) fmt_##x##_load_instrument,
static fmt_load_instrument_func load_instrument_funcs[] = {
#include "fmt-types.h"
	NULL,
};


void song_clear_sample(int n)
{
	song_lock_audio();
	csf_destroy_sample(current_song, n);
	memset(current_song->samples + n, 0, sizeof(song_sample_t));
	current_song->samples[n].c5speed = 8363;
	current_song->samples[n].volume = 64 * 4;
	current_song->samples[n].global_volume = 64;
	song_unlock_audio();
}

void song_copy_sample(int n, song_sample_t *src)
{
	memcpy(current_song->samples + n, src, sizeof(song_sample_t));

	if (src->data) {
		unsigned long bytelength = src->length;
		if (src->flags & CHN_16BIT)
			bytelength *= 2;
		if (src->flags & CHN_STEREO)
			bytelength *= 2;

		current_song->samples[n].data = csf_allocate_sample(bytelength);
		memcpy(current_song->samples[n].data, src->data, bytelength);
	}
}

int song_load_instrument_ex(int target, const char *file, const char *libf, int n)
{
	slurp_t *s;
	int r, x;

	song_lock_audio();

	/* 0. delete old samples */
	if (current_song->instruments[target]) {
		int sampmap[MAX_SAMPLES] = {};

		/* init... */
		for (unsigned int j = 0; j < 128; j++) {
			x = current_song->instruments[target]->sample_map[j];
			sampmap[x] = 1;
		}
		/* mark... */
		for (unsigned int q = 0; q < MAX_INSTRUMENTS; q++) {
			if ((int) q == target) continue;
			if (!current_song->instruments[q]) continue;
			for (unsigned int j = 0; j < 128; j++) {
				x = current_song->instruments[q]->sample_map[j];
				sampmap[x] = 0;
			}
		}
		/* sweep! */
		for (int j = 1; j < MAX_SAMPLES; j++) {
			if (!sampmap[j]) continue;

			csf_destroy_sample(current_song, j);
			memset(current_song->samples + j, 0, sizeof(current_song->samples[j]));
		}
		/* now clear everything "empty" so we have extra slots */
		for (int j = 1; j < MAX_SAMPLES; j++) {
			if (csf_sample_is_empty(current_song->samples + j)) sampmap[j] = 0;
		}
	}

	if (libf) { /* file is ignored */
		int sampmap[MAX_SAMPLES] = {};

		song_t *xl = song_create_load(libf);
		if (!xl) {
			song_unlock_audio();
			return 0;
		}

		/* 1. find a place for all the samples */
		for (unsigned int j = 0; j < 128; j++) {
			x = xl->instruments[n]->sample_map[j];
			if (!sampmap[x]) {
				if (x > 0 && x < MAX_INSTRUMENTS) {
					for (int k = 1; k < MAX_SAMPLES; k++) {
						if (current_song->samples[k].length) continue;
						sampmap[x] = k;
						//song_sample *smp = (song_sample *)song_get_sample(k);

						for (int c = 0; c < 25; c++) {
							if (xl->samples[x].name[c] == 0)
								xl->samples[x].name[c] = 32;
						}
						xl->samples[x].name[25] = 0;

						song_copy_sample(k, &xl->samples[x]);
						break;
					}
				}
			}
		}

		/* transfer the instrument */
		current_song->instruments[target] = xl->instruments[n];
		xl->instruments[n] = NULL; /* dangle */

		/* and rewrite! */
		for (unsigned int k = 0; k < 128; k++) {
			current_song->instruments[target]->sample_map[k] = sampmap[
					current_song->instruments[target]->sample_map[k]
			];
		}

		song_unlock_audio();
		return 1;
	}

	/* okay, load an ITI file */
	s = slurp(file, NULL, 0);
	if (!s) {
		song_unlock_audio();
		return 0;
	}

	r = 0;
	for (x = 0; load_instrument_funcs[x]; x++) {
		r = load_instrument_funcs[x](s->data, s->length, target);
		if (r) break;
	}

	unslurp(s);
	song_unlock_audio();

	return r;
}

int song_load_instrument(int n, const char *file)
{
	return song_load_instrument_ex(n,file,NULL,-1);
}

int song_preload_sample(dmoz_file_t *file)
{
	// 0 is our "hidden sample"
#define FAKE_SLOT 0
	//csf_stop_sample(current_song, current_song->samples + FAKE_SLOT);
	if (file->sample) {
		song_sample_t *smp = song_get_sample(FAKE_SLOT);

		song_lock_audio();
		csf_destroy_sample(current_song, FAKE_SLOT);
		song_copy_sample(FAKE_SLOT, file->sample);
		strncpy(smp->name, file->title, 25);
		smp->name[25] = 0;
		strncpy(smp->filename, file->base, 12);
		smp->filename[12] = 0;
		song_unlock_audio();
		return FAKE_SLOT;
	}
	// WARNING this function must return 0 or KEYJAZZ_NOINST
	return song_load_sample(FAKE_SLOT, file->path) ? FAKE_SLOT : KEYJAZZ_NOINST;
#undef FAKE_SLOT
}

int song_load_sample(int n, const char *file)
{
	fmt_load_sample_func *load;
	song_sample_t smp = {};

	const char *base = get_basename(file);
	slurp_t *s = slurp(file, NULL, 0);

	if (s == NULL) {
		return 0;
	}

	// set some default stuff
	song_lock_audio();
	csf_stop_sample(current_song, current_song->samples + n);
	strncpy(smp.name, base, 25);

	for (load = load_sample_funcs; *load; load++) {
		if ((*load)(s->data, s->length, &smp)) {
			break;
		}
	}

	if (!load) {
		unslurp(s);
		song_unlock_audio();
		return 0;
	}

	// this is after the loaders because i don't trust them, even though i wrote them ;)
	strncpy(smp.filename, base, 12);
	smp.filename[12] = 0;
	smp.name[25] = 0;

	csf_destroy_sample(current_song, n);
	if (((unsigned char)smp.name[23]) == 0xFF) {
		// don't load embedded samples
		// (huhwhat?!)
		smp.name[23] = ' ';
	}
	memcpy(&(current_song->samples[n]), &smp, sizeof(song_sample_t));
	song_unlock_audio();

	unslurp(s);

	return 1;
}

void song_create_host_instrument(int smp, int ins)
{
	if (csf_instrument_is_empty(current_song->instruments[smp]))
		ins = smp;
	else if (!csf_instrument_is_empty(current_song->instruments[ins]))
		ins = csf_first_blank_instrument(current_song, 0);

	if (ins > 0) {
		song_init_instrument_from_sample(ins, smp);
//		status_text_flash("Sample assigned to Instrument %d", ins);
	} else {
//		status_text_flash("Error: No available Instruments!");
	}
}

// ------------------------------------------------------------------------
// song information

const char *song_get_filename(void)
{
	return song_filename;
}

const char *song_get_basename(void)
{
	return song_basename;
}

// ------------------------------------------------------------------------
// sample library browsing

// FIXME: unload the module when leaving the library 'directory'
static song_t *library = NULL;


// TODO: stat the file?
int dmoz_read_instrument_library(const char *path, dmoz_filelist_t *flist, UNUSED dmoz_dirlist_t *dlist)
{
	unsigned int j;
	int x;

	csf_stop_sample(current_song, current_song->samples + 0);
	csf_free(library);

	const char *base = get_basename(path);
	library = song_create_load(path);
	if (!library) {
		return -1;
	}

	for (int n = 1; n < MAX_INSTRUMENTS; n++) {
		if (!library->instruments[n])
			continue;

		dmoz_file_t *file = dmoz_add_file(flist,
			str_dup(path), str_dup(base), NULL, n);
		file->title = str_dup(library->instruments[n]->name);

		int count[128] = {};

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


int dmoz_read_sample_library(const char *path, dmoz_filelist_t *flist, UNUSED dmoz_dirlist_t *dlist)
{
	csf_stop_sample(current_song, current_song->samples + 0);
	csf_free(library);

	const char *base = get_basename(path);
	library = song_create_load(path);
	if (!library) {
		/* FIXME: try loading as an instrument before giving up */
		errno = ENOTDIR;
		return -1;
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
			file->description = "Fishcakes"; // FIXME - what does IT say?
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
			file->title = str_dup(library->samples[n].name);
			file->sample = (song_sample_t *) library->samples + n;
		}
	}

	return 0;
}

// ------------------------------------------------------------------------
// instrument loader

song_instrument_t *instrument_loader_init(struct instrumentloader *ii, int slot)
{
	ii->expect_samples = 0;
	ii->inst = song_get_instrument(slot);
	ii->slot = slot;
	ii->basex = 1;
	memset(ii->sample_map, 0, sizeof(ii->sample_map));
	return ii->inst;
}

int instrument_loader_abort(struct instrumentloader *ii)
{
	int n;
	song_wipe_instrument(ii->slot);
	for (n = 0; n < MAX_SAMPLES; n++) {
		if (ii->sample_map[n]) {
			song_clear_sample(ii->sample_map[n]-1);
			ii->sample_map[n] = 0;
		}
	}
	return 0;
}

int instrument_loader_sample(struct instrumentloader *ii, int slot)
{
	int x;

	if (!slot) return 0;
	if (ii->sample_map[slot]) return ii->sample_map[slot];
	for (x = ii->basex; x < MAX_SAMPLES; x++) {
		song_sample_t *cur = (current_song->samples + x);

//              if (!csf_sample_is_empty(current_song->samples + x))
//                      continue;
		if (cur->data != NULL)
			continue;

		ii->expect_samples++;
		ii->sample_map[slot] = x;
		ii->basex = x + 1;
		return ii->sample_map[slot];
	}
//	status_text_flash("Too many samples");
	return 0;
}

