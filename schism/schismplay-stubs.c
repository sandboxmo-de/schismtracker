/*
 * Schism Tracker - a cross-platform Impulse Tracker clone
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

/* Stand-ins for the UI symbols that the engine files (audio_playback.c,
 * audio_loadsave.c, disko.c, mplink.c, midi-core.c, player/) still refer to.
 * Only schismplay links this file. The list and the reasons for each entry
 * are in docs/engine-ui-coupling.md.
 *
 * Nothing here draws or reads input. Log lines and status messages go to
 * stderr when schismplay_verbose is set, and are dropped otherwise. */

#include "headers.h"

#include "it.h"
#include "page.h"
#include "dialog.h"
#include "vgamem.h"
#include "events.h"
#include "fakemem.h"
#include "config.h"
#include "log.h"
#include "song.h"
#include "osdefs.h"

#include <errno.h>
#include <stdarg.h>

int schismplay_verbose = 0;

/* ------------------------------------------------------------------------ */
/* Global tracker state (page.c, page_patedit.c, page_instruments.c, config.c)
 *
 * The engine reads a few bits of `status.flags` (MIDI_LIKE_TRACKER,
 * CLASSIC_MODE, DISKWRITER_ACTIVE, ...). All zero matches a fresh config. */

struct tracker_status status = {0};

/* audio_playback.c calls every page's song_mode_changed_cb; with no pages
 * loaded they're all NULL. */
struct page pages[PAGE_MAX] = {0};

int instrument_list_subpage = PAGE_INSTRUMENT_LIST_GENERAL;

int playback_tracing = 0;
int midi_playback_tracing = 0;
int midi_last_bend_hit[MAX_CHANNELS] = {0};

/* midi-core.c only uses this for the per-port config, which schismplay
 * never loads or saves. */
char *cfg_dir_dotschism = NULL;

int sample_get_current(void) { return 1; }
int instrument_get_current(void) { return 1; }

void set_page(SCHISM_UNUSED int new_page) {}
void main_song_changed_cb(void) {}
void message_reset_selection(void) {}
void preferences_audio_driver_changed(void) {}
void song_vars_sync_stereo(void) {}
void memused_songchanged(void) {}
void handle_key(SCHISM_UNUSED struct key_event *k) {}

/* ------------------------------------------------------------------------ */
/* Visualiser feed (page_waterfall.c). The audio callback only calls these
 * when the waterfall page or the FFT view is active, which never happens
 * here, apart from vis_set_size on buffer changes. */

void vis_set_size(SCHISM_UNUSED uint32_t size) {}
void vis_work_32s(SCHISM_UNUSED const int32_t *in, SCHISM_UNUSED size_t inlen) {}
void vis_work_32m(SCHISM_UNUSED const int32_t *in, SCHISM_UNUSED size_t inlen) {}
void vis_work_16s(SCHISM_UNUSED const int16_t *in, SCHISM_UNUSED size_t inlen) {}
void vis_work_16m(SCHISM_UNUSED const int16_t *in, SCHISM_UNUSED size_t inlen) {}
void vis_work_8s(SCHISM_UNUSED const int8_t *in, SCHISM_UNUSED size_t inlen) {}
void vis_work_8m(SCHISM_UNUSED const int8_t *in, SCHISM_UNUSED size_t inlen) {}

/* ------------------------------------------------------------------------ */
/* Events (events.c). The audio callback posts SCHISM_EVENT_PLAYBACK so the
 * UI can redraw; schismplay polls the song position instead. */

int events_push_event(SCHISM_UNUSED const schism_event_t *event)
{
	return 1;
}

/* ------------------------------------------------------------------------ */
/* Dialogs and drawing (dialog.c, vgamem.c), used by disko.c for the export
 * progress box and by audio_loadsave.c for overwrite prompts.
 *
 * disko.c sets the cancel/yes/no callbacks on the dialog it gets back, so
 * dialog_create_custom has to hand out real storage. The prompts from
 * dialog_create are only reached from editor commands. */

static struct dialog dummy_dialog;

struct dialog *dialog_create(SCHISM_UNUSED int type, SCHISM_UNUSED const char *text,
	SCHISM_UNUSED void (*action_yes)(void *data), SCHISM_UNUSED void (*action_no)(void *data),
	SCHISM_UNUSED int default_widget, SCHISM_UNUSED void *data)
{
	memset(&dummy_dialog, 0, sizeof(dummy_dialog));
	return &dummy_dialog;
}

struct dialog *dialog_create_custom(SCHISM_UNUSED int x, SCHISM_UNUSED int y,
	SCHISM_UNUSED int w, SCHISM_UNUSED int h, SCHISM_UNUSED struct widget *dialog_widgets,
	SCHISM_UNUSED int dialog_total_widgets, SCHISM_UNUSED int dialog_selected_widget,
	SCHISM_UNUSED void (*draw_const)(void), SCHISM_UNUSED void *data)
{
	memset(&dummy_dialog, 0, sizeof(dummy_dialog));
	return &dummy_dialog;
}

void dialog_destroy(void) {}
void dialog_destroy_all(void) {}

int draw_text(SCHISM_UNUSED const char *text, SCHISM_UNUSED int x, SCHISM_UNUSED int y,
	SCHISM_UNUSED uint32_t fg, SCHISM_UNUSED uint32_t bg)
{
	return 0;
}

void draw_fill_chars(SCHISM_UNUSED int xs, SCHISM_UNUSED int ys, SCHISM_UNUSED int xe,
	SCHISM_UNUSED int ye, SCHISM_UNUSED uint32_t fg, SCHISM_UNUSED uint32_t bg) {}
void draw_box(SCHISM_UNUSED int xs, SCHISM_UNUSED int ys, SCHISM_UNUSED int xe,
	SCHISM_UNUSED int ye, SCHISM_UNUSED uint32_t flags) {}
void draw_vu_meter(SCHISM_UNUSED int x, SCHISM_UNUSED int y, SCHISM_UNUSED int val,
	SCHISM_UNUSED int width, SCHISM_UNUSED int color, SCHISM_UNUSED int peak_color) {}

/* ------------------------------------------------------------------------ */
/* Log and status line (page_log.c, status.c) */

static void vlog(const char *prefix, const char *format, va_list ap)
{
	if (!schismplay_verbose)
		return;

	fputs(prefix, stderr);
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
}

void log_appendf(SCHISM_UNUSED int color, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vlog("", format, ap);
	va_end(ap);
}

void log_appendf_timestamp(SCHISM_UNUSED int color, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vlog("", format, ap);
	va_end(ap);
}

void log_append_timestamp(SCHISM_UNUSED int color, const char *text)
{
	if (schismplay_verbose)
		fprintf(stderr, "%s\n", text);
}

void log_nl(void) {}
void log_underline(void) {}

/* errors are worth showing even when quiet */
void log_perror(const char *prefix)
{
	fprintf(stderr, "%s: %s\n", prefix, strerror(errno));
}

void status_text_flash(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vlog("status: ", format, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* macOS keyboard quirk (ibook-support.c), referenced from osdefs.m.
 * -1 tells osdefs.m there's no fn-key setting to restore on exit. */

#ifdef SCHISM_MACOSX
int macosx_ibook_fnswitch(SCHISM_UNUSED int setting)
{
	return -1;
}
#endif
