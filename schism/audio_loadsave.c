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

#include "bits.h"
#include "charset.h"
#include "it.h"
#include "song.h"
#include "slurp.h"
#include "version.h"
#include "osdefs.h"
#include "engine-alloc.h"
#include "str.h"

#include "fmt.h"
#include "dmoz.h"

#include "player/sndfile.h"
#include "player/snd_gm.h"

#include "midi.h"
#include "disko.h"
#include "engine-host.h"

// ------------------------------------------------------------------------

char song_filename[SCHISM_PATH_MAX + 1];
char song_basename[SCHISM_NAME_MAX + 1];

// ------------------------------------------------------------------------
// replace any '\0' chars with spaces, mostly to make the string handling
// much easier.

static inline SCHISM_ALWAYS_INLINE void _fix_name(char *buf, size_t max,
	size_t realsize)
{
	size_t c;

	for (c = 0; c < max; c++)
		if (!buf[c])
			buf[c] = 0x20;

	/* completely destroy anything after the max; this works around issues
	 * where someone deletes characters, runs into the \0, deletes it, and
	 * then the REST of the string appears :) */
	if (realsize > max)
		memset(buf + max, 0, realsize - max);

	/* kill the whitespace */
	str_rtrim(buf);
}

static void _fix_names(song_t *qq)
{
	int n;

	_fix_name(qq->title, 26, sizeof(qq->title));

	for (n = 1; n < MAX_SAMPLES; n++) {
		_fix_name(qq->samples[n].name, 26, sizeof(qq->samples[n].name));
		_fix_name(qq->samples[n].filename, 13, sizeof(qq->samples[n].filename));
	}

	for (n = 1; n < MAX_INSTRUMENTS; n++) {
		if (!qq->instruments[n])
			continue;

		_fix_name(qq->instruments[n]->name, 26, sizeof(qq->instruments[n]->name));
		_fix_name(qq->instruments[n]->filename, 13, sizeof(qq->instruments[n]->filename));
	}
}

// ------------------------------------------------------------------------
// file stuff

static void song_set_filename(const char *file)
{
	if (file && *file) {
		strncpy(song_filename, file, ARRAY_SIZE(song_filename) - 1);
		strncpy(song_basename, dmoz_path_get_basename(file), ARRAY_SIZE(song_basename) - 1);
		song_filename[ARRAY_SIZE(song_filename) - 1] = '\0';
		song_basename[ARRAY_SIZE(song_basename) - 1] = '\0';
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

	/* reset the quirks */
	BITARRAY_FILL(current_song->quirks);

	if ((flags & KEEP_PATTERNS) == 0) {
		song_set_filename(NULL);
		engine_flag_clear(SONG_NEEDS_SAVE);

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

		for (i = 0; i < MAX_CHANNELS; i++) {
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

	engine_hooks.song_changed();
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
static void message_convert_newlines(song_t *song) {
	int i = 0, len = strlen(song->message);
	for (i = 0; i < len; i++) {
		if (song->message[i] == '\n') {
			song->message[i] = '\r';
		}
	}
}

/* Mixer settings a newly loaded song takes over from the current one. The
 * MIDI configuration is static rather than on the stack because it's a few
 * kilobytes. */
struct load_settings {
	uint32_t mix_flags, mix_frequency, mix_bits, mix_channels;
	int row_highlight_major, row_highlight_minor;
};

static midi_config_t load_midi_cfg;

static void load_settings_save(struct load_settings *ls, song_t *from)
{
	ls->mix_flags = from->mix_flags;
	ls->mix_frequency = from->mix_frequency;
	ls->mix_bits = from->mix_bits_per_sample;
	ls->mix_channels = from->mix_channels;
	ls->row_highlight_major = from->row_highlight_major;
	ls->row_highlight_minor = from->row_highlight_minor;
	memcpy(&load_midi_cfg, &from->midi_config, sizeof(load_midi_cfg));
}

static void load_settings_apply(const struct load_settings *ls, song_t *to)
{
	to->mix_flags = ls->mix_flags;
	csf_set_wave_config(to, ls->mix_frequency, ls->mix_bits, ls->mix_channels);

	// loaders might override these
	to->row_highlight_major = ls->row_highlight_major;
	to->row_highlight_minor = ls->row_highlight_minor;
	memcpy(&to->midi_config, &load_midi_cfg, sizeof(load_midi_cfg));
}

/* Run the loaders on s until one takes it. song must be fresh from
 * csf_allocate() or csf_reinit(). Returns 0, or an errno value or
 * -LOAD_xxx for fmt_strerror(). Any allocation that failed while the
 * loaders ran fails the load, even if the loader didn't notice. */
static int load_song_slurp(song_t *song, slurp_t *s)
{
	unsigned long fails = engine_alloc_failures();
	fmt_load_song_func *func;
	int err = -LOAD_UNSUPPORTED;

	for (func = load_song_funcs; *func; func++) {
		slurp_rewind(s);
		switch ((*func)(song, s, 0)) {
		case LOAD_SUCCESS:
			err = 0;
			break;
		case LOAD_UNSUPPORTED:
			continue;
		case LOAD_FORMAT_ERROR:
			err = -LOAD_FORMAT_ERROR;
			break;
		case LOAD_FILE_ERROR:
			err = errno;
			break;
		}
		break;
	}

	if (engine_alloc_failures() != fails)
		err = ENOMEM;

	return err;
}

static void load_song_finish(song_t *song)
{
	song->stop_at_order = song->stop_at_row = -1;
	message_convert_newlines(song);
	engine_hooks.message_reset();
}

song_t *song_create_load(const char *file)
{
	struct load_settings ls;
	slurp_t s;
	song_t *newsong;
	int err;

	engine_scratch_begin();

	if (slurp(&s, file, NULL, 0) < 0) {
		err = errno;
		engine_scratch_end();
		errno = err;
		return NULL;
	}

	newsong = csf_allocate();
	if (!newsong) {
		unslurp(&s);
		engine_scratch_end();
		errno = ENOMEM;
		return NULL;
	}

	if (current_song) {
		load_settings_save(&ls, current_song);
		load_settings_apply(&ls, newsong);
	}

	err = load_song_slurp(newsong, &s);

	unslurp(&s);
	engine_scratch_end();

	if (err) {
		// awwww, nerts!
		csf_free(newsong);
		errno = err;
		return NULL;
	}

	load_song_finish(newsong);

	return newsong;
}

/* With ENGINE_ALLOC_ONE_SONG the current song is emptied and the file is
 * loaded into it, so the two never take up memory at the same time. The
 * audio lock is held throughout, since the mixer runs on current_song even
 * when it's stopped. If the load fails the song is left empty. */
static int song_load_in_place(const char *file)
{
	struct load_settings ls;
	slurp_t s;
	int err;

	engine_scratch_begin();

	if (slurp(&s, file, NULL, 0) < 0) {
		err = errno;
		engine_scratch_end();
		errno = err;
		return 0;
	}

	song_lock_audio();
	song_stop_unlocked(0);

	load_settings_save(&ls, current_song);
	csf_reinit(current_song);
	load_settings_apply(&ls, current_song);

	err = load_song_slurp(current_song, &s);

	unslurp(&s);
	engine_scratch_end();

	if (err) {
		csf_reinit(current_song);
		load_settings_apply(&ls, current_song);
		song_set_filename(NULL);
		max_channels_used = 0;
		song_init_modplug();
		song_unlock_audio();
		engine_hooks.song_changed();
		errno = err;
		return 0;
	}

	load_song_finish(current_song);

	song_unlock_audio();

	return 1;
}

int song_load_unchecked(const char *file)
{
	const char *base = dmoz_path_get_basename(file);
	int was_playing;
	song_t *newsong;

	// IT stops the song even if the new song can't be loaded
	if (engine_flag_test(PLAY_AFTER_LOAD)) {
		was_playing = (song_get_mode() == MODE_PLAYING);
	} else {
		was_playing = 0;
		song_stop();
	}

	log_nl();
	log_appendf_timestamp(2, "Loading %s", base);
	log_underline();

	if (engine_allocator_flags() & ENGINE_ALLOC_ONE_SONG) {
		if (!song_load_in_place(file)) {
			log_appendf(4, " %s", fmt_strerror(errno));
			return 0;
		}
		newsong = NULL;
	} else {
		newsong = song_create_load(file);
		if (!newsong) {
			log_appendf(4, " %s", fmt_strerror(errno));
			return 0;
		}
	}

	song_set_filename(file);

	song_lock_audio();
	if (newsong) {
		csf_free(current_song);
		current_song = newsong;
	}
	current_song->repeat_count = 0;
	max_channels_used = 0;
	_fix_names(current_song);
	song_stop_unlocked(0);
	song_init_modplug();
	song_unlock_audio();

	if (was_playing && engine_flag_test(PLAY_AFTER_LOAD))
		song_start();

	engine_hooks.song_changed();

	engine_flag_clear(SONG_NEEDS_SAVE);

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

	if (tid[0])
		log_appendf(5, " %s", tid);
	if (!nins)
		*strrchr(fmt, ',') = 0; // cut off 'instruments'
	log_appendf(5, fmt, csf_get_num_patterns(current_song), nsmp, nins);

	return 1;
}

/* ------------------------------------------------------------------------- */

#ifdef USE_FLAC

static int flac_enabled = 0;

static int flac_enabled_cb(void)
{
	return !!flac_enabled;
}

void audio_enable_flac(int enabled)
{
	flac_enabled = !!enabled;
}

#endif

const struct save_format song_save_formats[] = {
	{"IT", "Impulse Tracker", ".it", {.save_song = fmt_it_save_song}, NULL},
	{"S3M", "Scream Tracker 3", ".s3m", {.save_song = fmt_s3m_save_song}, NULL},
	{"MOD", "Amiga ProTracker", ".mod", {.save_song = fmt_mod_save_song}, NULL},
	{.label = NULL}
};

#define EXPORT_FUNCS(t) \
	fmt_##t##_export_head, fmt_##t##_export_silence, fmt_##t##_export_body, fmt_##t##_export_tail

const struct save_format song_export_formats[] = {
	{"WAV", "WAV", ".wav", {.export = {EXPORT_FUNCS(wav), 0}}, NULL},
	{"MWAV", "WAV multi-write", ".wav", {.export = {EXPORT_FUNCS(wav), 1}}, NULL},
	{"AIFF", "Audio IFF", ".aiff", {.export = {EXPORT_FUNCS(aiff), 0}}, NULL},
	{"MAIFF", "Audio IFF multi-write", ".aiff", {.export = {EXPORT_FUNCS(aiff), 1}}, NULL},
#ifdef USE_FLAC
	{"FLAC", "Free Lossless Audio Codec", ".flac", {.export = {EXPORT_FUNCS(flac), 0}}, flac_enabled_cb},
	{"MFLAC", "Free Lossless Audio Codec multi-write", ".flac", {.export = {EXPORT_FUNCS(flac), 1}}, flac_enabled_cb},
#endif
	{.label = NULL}
};
// <distance> and maiff sounds like something you'd want to hug
// <distance> .. dont ask

const struct save_format sample_save_formats[] = {
	{"ITS", "Impulse Tracker", ".its", {.save_sample = fmt_its_save_sample}, NULL},
	{"S3I", "Scream Tracker", ".s3i", {.save_sample = fmt_s3i_save_sample}, NULL},
	{"SBI", "Sound Blaster", ".sbi", {.save_sample = fmt_sbi_save_sample}, NULL},
	{"AIFF", "Audio IFF", ".aiff", {.save_sample = fmt_aiff_save_sample}, NULL},
	{"AU", "Sun/NeXT", ".au", {.save_sample = fmt_au_save_sample}, NULL},
	{"WAV", "WAV", ".wav", {.save_sample = fmt_wav_save_sample}, NULL},
#ifdef USE_FLAC
	{"FLAC", "Free Lossless Audio Codec", ".flac", {.save_sample = fmt_flac_save_sample}, flac_enabled_cb},
#endif
	{"RAW", "Raw", ".raw", {.save_sample = fmt_raw_save_sample}, NULL},
	{.label = NULL}
};

const struct save_format instrument_save_formats[] = {
	{"ITI", "Impulse Tracker", ".iti", {.save_instrument = fmt_iti_save_instrument}, NULL},
	{"XI", "Fasttracker II", ".xi", {.save_instrument = fmt_xi_save_instrument}, NULL},
	{.label = NULL}
};

static const struct save_format *get_save_format(const struct save_format *formats, const char *label)
{
	int n;

	if (!label) {
		// why would this happen, ever?
		log_appendf(4, "No file type given, very weird! (Try a different filename?)");
		return NULL;
	}

	for (n = 0; formats[n].label; n++)
		if (strcmp(formats[n].label, label) == 0 && (!formats[n].enabled || formats[n].enabled()))
			return formats + n;

	log_appendf(4, "Unknown save format %s", label);
	return NULL;
}


static char *mangle_filename(const char *in, const char *mid, const char *ext)
{
	const char *iext = dmoz_path_get_extension(in);

	const size_t baselen = iext - in;
	const size_t midlen = (mid) ? strlen(mid) : 0;
	const size_t extlen = (*iext) ? strlen(iext) : ((ext) ? strlen(ext) : 0);

	char *ret = engine_alloc(baselen + midlen + extlen + 1, ENGINE_MEM_SCRATCH); /* room for terminating \0 */

	if (!ret)
		return NULL;
	memcpy(ret, in, baselen);
	if (mid)
		memcpy(ret + baselen, mid, midlen);
	if (*iext)
		memcpy(ret + baselen + midlen, iext, extlen);
	else if (ext)
		memcpy(ret + baselen + midlen, ext, extlen);
	ret[baselen + midlen + extlen] = '\0';

	return ret;
}

int song_export(const char *filename, const char *type)
{
	const struct save_format *format = get_save_format(song_export_formats, type);
	const char *mid;
	char *mangle;
	int r;

	if (!format)
		return SAVE_INTERNAL_ERROR;

	mid = (format->f.export.multi && strcasestr(filename, "%c") == NULL) ? ".%c" : NULL;
	mangle = mangle_filename(filename, mid, format->ext);
	if (!mangle)
		return SAVE_INTERNAL_ERROR;

	log_nl();
	log_nl();
	log_appendf_timestamp(2, "Exporting to %s", format->name);
	log_underline();

	/* disko does the rest of the log messages itself */
	r = disko_export_song(mangle, format);
	engine_free(mangle, ENGINE_MEM_SCRATCH);
	switch (r) {
	case DW_OK:
		return SAVE_SUCCESS;
	case DW_ERROR:
		return SAVE_FILE_ERROR;
	default:
		return SAVE_INTERNAL_ERROR;
	}
}


int song_save(const char *filename, const char *type)
{
	disko_t fp;
	int ret, backup;
	const struct save_format *format = get_save_format(song_save_formats, type);
	char *mangle;

	if (!format)
		return SAVE_INTERNAL_ERROR;

	mangle = mangle_filename(filename, NULL, format->ext);
	if (!mangle)
		return SAVE_INTERNAL_ERROR;

	log_nl();
	log_appendf_timestamp(2, "Saving %s module", format->name);
	log_underline();

/* TODO: add or replace file extension as appropriate

From IT 2.10 update:
	- Automatic filename extension replacement on Ctrl-S, so that if you press
		Ctrl-S after loading a .MOD, .669, .MTM or .XM, the filename will be
		automatically modified to have a .IT extension.

In IT, if the filename given has no extension ("basename"), then the extension for the proper file type
(IT/S3M) will be appended to the name.
A filename with an extension is not modified, even if the extension is blank. (as in "basename.")

This brings up a rather odd bit of behavior: what happens when saving a file with the deliberately wrong
extension? Selecting the IT type and saving as "test.s3m" works just as one would expect: an IT file is
written, with the name "test.s3m". No surprises yet, but as soon as Ctrl-S is used, the filename is "fixed",
producing a second file called "test.it". The reverse happens when trying to save an S3M named "test.it" --
it's rewritten to "test.s3m".
Note that in these scenarios, Impulse Tracker *DOES NOT* check if an existing file by that name exists; it
will GLADLY overwrite the old "test.s3m" (or .it) with the renamed file that's being saved. Presumably this
is NOT intentional behavior.

Another note: if the file could not be saved for some reason or another, Impulse Tracker pops up a dialog
saying "Could not save file". This can be seen rather easily by trying to save a file with a malformed name,
such as "abc|def.it". This dialog is presented both when saving from F10 and Ctrl-S.
*/

	if (disko_open(&fp, mangle) < 0) {
		log_perror(mangle);
		engine_free(mangle, ENGINE_MEM_SCRATCH);
		return SAVE_FILE_ERROR;
	}

	ret = format->f.save_song(&fp, current_song);
	if (ret != SAVE_SUCCESS)
		disko_seterror(&fp, EINVAL);

	backup = (engine_flag_test(MAKE_BACKUPS)
			? engine_flag_test(NUMBERED_BACKUPS)
			? 65536 : 1 : 0);

	// this was not as successful as originally claimed!
	if (disko_close(&fp, backup) == DW_ERROR && ret == SAVE_SUCCESS)
		ret = SAVE_FILE_ERROR;

	switch (ret) {
	case SAVE_SUCCESS:
		engine_flag_clear(SONG_NEEDS_SAVE);
		/* kill any quirks */
		BITARRAY_FILL(current_song->quirks);
		if (charset_strcasecmp(song_filename, CHARSET_UTF8, mangle, CHARSET_UTF8))
			song_set_filename(mangle);
		log_appendf(5, " Done");
		break;
	case SAVE_FILE_ERROR:
		log_perror(mangle);
		break;
	case SAVE_INTERNAL_ERROR:
	default: // ???
		log_appendf(4, " Internal error saving song");
		break;
	}

	engine_free(mangle, ENGINE_MEM_SCRATCH);
	return ret;
}

int song_save_sample(const char *filename, const char *type, song_sample_t *smp, int num)
{
	static const char *err = "Error: Sample %d NOT saved! (%s)";
	disko_t fp;
	int ret;
	const struct save_format *format = get_save_format(sample_save_formats, type);
	const char *basename = filename ? dmoz_path_get_basename(filename) : NULL;

	if (!format)
		return SAVE_INTERNAL_ERROR;

	if (!filename || !filename[0] || !basename[0]) {
		ret = SAVE_NO_FILENAME;
	} else if (disko_open(&fp, filename) >= 0) {
		ret = format->f.save_sample(&fp, smp);
		if (ret != SAVE_SUCCESS)
			disko_seterror(&fp, EINVAL);

		// this was not as successful as originally claimed!
		if (disko_close(&fp, 0) == DW_ERROR && ret == SAVE_SUCCESS)
			ret = SAVE_FILE_ERROR;
	} else {
		ret = SAVE_FILE_ERROR;
	}

	switch (ret) {
	case SAVE_SUCCESS:
		status_text_flash("%s sample saved (sample %d)", format->name, num);
		break;
	case SAVE_UNSUPPORTED:
		status_text_flash(err, num, "Unsupported Data");
		break;
	case SAVE_FILE_ERROR:
		status_text_flash(err, num, "File Error");
		log_perror(basename);
		break;
	case SAVE_NO_FILENAME:
		status_text_flash(err, num, "No Filename?");
		break;
	case SAVE_INTERNAL_ERROR:
	default: // ???
		status_text_flash(err, num, "Internal error");
		log_appendf(4, "Internal error saving sample");
		break;
	}

	return ret;
}

// ------------------------------------------------------------------------

// All of the sample's fields are initially zeroed except the filename (which is set to the sample's
// basename and shouldn't be changed). A sample loader should not change anything in the sample until
// it is sure that it can accept the file.
// The title points to a buffer of 26 characters.

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
	song_lock_audio();

	csf_stop_sample(current_song, current_song->samples + n);

	memcpy(current_song->samples + n, src, sizeof(song_sample_t));

	if (src->data) {
		unsigned long bytelength = src->length;
		if (src->flags & CHN_16BIT)
			bytelength *= 2;
		if (src->flags & CHN_STEREO)
			bytelength *= 2;

		current_song->samples[n].data = csf_allocate_sample(bytelength);
		if (current_song->samples[n].data) {
			memcpy(current_song->samples[n].data, src->data, bytelength);
			csf_adjust_sample_loop(current_song->samples + n);
		} else {
			current_song->samples[n].length = 0;
		}
	}

	song_unlock_audio();
}

int song_load_instrument_ex(int target, const char *file, const char *libf, int n)
{
	slurp_t s;
	int r, x;

	song_lock_audio();

	/* 0. delete old samples */
	if (current_song->instruments[target]) {
		int sampmap[MAX_SAMPLES] = {0};

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
		int sampmap[MAX_SAMPLES] = {0};

		song_t *xl = song_create_load(libf);
		if (!xl) {
			log_appendf(4, "%s: %s", libf, fmt_strerror(errno));
			song_unlock_audio();
			return 0;
		}

		/* 1. find a place for all the samples */
		for (unsigned int j = 0; j < 128; j++) {
			x = xl->instruments[n]->sample_map[j];
			if (!sampmap[x]) {
				if (x > 0 && x < MAX_SAMPLES) {
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
	engine_scratch_begin();
	if (slurp(&s, file, NULL, 0) < 0) {
		log_perror(file);
		engine_scratch_end();
		song_unlock_audio();
		return 0;
	}

	r = 0;
	for (x = 0; load_instrument_funcs[x]; x++) {
		slurp_rewind(&s);
		r = load_instrument_funcs[x](&s, target);
		if (r) break;
	}

	unslurp(&s);
	engine_scratch_end();
	song_unlock_audio();

	return r;
}

int song_load_instrument(int n, const char* file)
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
	slurp_t s;
	fmt_load_sample_func *load;
	song_sample_t smp = {0};

	const char *base = dmoz_path_get_basename(file);

	engine_scratch_begin();
	if (slurp(&s, file, NULL, 0)) {
		log_perror(base);
		engine_scratch_end();
		return 0;
	}

	// set some default stuff
	song_lock_audio();
	csf_stop_sample(current_song, current_song->samples + n);
	strncpy(smp.name, base, 25);

	/* this was repeated multiple times in the sample loaders */
	smp.volume = 64 * 4;
	smp.global_volume = 64;

	for (load = load_sample_funcs; *load; load++) {
		slurp_rewind(&s);
		if ((*load)(&s, &smp))
			break;
	}

	if (!load) {
		unslurp(&s);
		engine_scratch_end();
		log_perror(base);
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

	unslurp(&s);
	engine_scratch_end();

	return 1;
}

// ------------------------------------------------------------------------

int song_save_instrument(const char *filename, const char *type, song_instrument_t *ins, int num)
{
	static const char *err = "Error: Instrument %d NOT saved! (%s)";

	disko_t fp;
	int ret;
	const struct save_format *format = get_save_format(instrument_save_formats, type);
	const char *basename = filename ? dmoz_path_get_basename(filename) : NULL;

	if (!format)
		return SAVE_INTERNAL_ERROR;

	if (!filename || !filename[0] || !basename[0]) {
		ret = SAVE_NO_FILENAME;
	} else if (disko_open(&fp, filename) >= 0) {
		ret = format->f.save_instrument(&fp, current_song, ins);
		if (ret != SAVE_SUCCESS)
			disko_seterror(&fp, EINVAL);

		// this was not as successful as originally claimed!
		if (disko_close(&fp, 0) == DW_ERROR && ret == SAVE_SUCCESS)
			ret = SAVE_FILE_ERROR;
	} else {
		ret = SAVE_FILE_ERROR;
	}

	switch (ret) {
	case SAVE_SUCCESS:
		status_text_flash("%s instrument saved (instrument %d)", format->name, num);
		break;
	case SAVE_UNSUPPORTED:
		status_text_flash(err, num, "Unsupported Data");
		break;
	case SAVE_FILE_ERROR:
		status_text_flash(err, num, "File Error");
		log_perror(basename);
		break;
	case SAVE_NO_FILENAME:
		status_text_flash(err, num, "No Filename?");
		break;
	case SAVE_INTERNAL_ERROR:
	default: // ???
		status_text_flash(err, num, "Internal error");
		log_appendf(4, "Internal error saving sample");
		break;
	}

	return ret;
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
		if (cur->data != NULL)
			continue;

		ii->expect_samples++;
		ii->sample_map[slot] = x;
		ii->basex = x + 1;
		return ii->sample_map[slot];
	}
	status_text_flash("Too many samples");
	return 0;
}

