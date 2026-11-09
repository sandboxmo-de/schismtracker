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

/* The tracker's side of include/engine-host.h: which UI functions the engine
 * gets to call. */

#include "headers.h"

#include "engine-host.h"
#include "it.h"
#include "page.h"
#include "fakemem.h"
#include "config.h"
#include "timer.h"
#include "disko.h"
#include "dialog.h"
#include "widget.h"
#include "vgamem.h"
#include "log.h"

static void tracker_redraw(void)
{
	status.flags |= NEED_UPDATE;
}

/* the waterfall page and the FFT view in the top right corner want the
 * mixed audio */
static void tracker_vis_feed(const void *buf, uint32_t frames, uint32_t bits, uint32_t channels)
{
	if (status.current_page != PAGE_WATERFALL && status.vis_style != VIS_FFT)
		return;

	if (!buf) {
		vis_work_8m(NULL, 0);
		return;
	}

	switch (bits) {
#define BITSCASE(BITS) case BITS: if (channels == 2) { vis_work_##BITS##s(buf, frames / 2); } else { vis_work_##BITS##m(buf, frames); } break;
	BITSCASE(8)
	BITSCASE(16)
	BITSCASE(32)
#undef BITSCASE
	}
}

/* the MIDI page shows the last event that went in or out */
static void tracker_midi_activity(struct midi_port *port, const unsigned char *data,
	uint32_t len, int outgoing)
{
	status.last_midi_real_len = len;
	status.last_midi_len = MIN(sizeof(status.last_midi_event), len);
	memcpy(status.last_midi_event, data, status.last_midi_len);
	status.last_midi_port = port;
	status.last_midi_tick = timer_ticks();
	status.flags |= MIDI_EVENT_CHANGED;
	if (outgoing)
		status.flags |= NEED_UPDATE;
}

static int tracker_midi_monitor_active(void)
{
	return (status.current_page == PAGE_MIDI);
}

static const char *tracker_config_dir(void)
{
	return cfg_dir_dotschism;
}

/* ------------------------------------------------------------------------ */
/* song export progress dialog */

static struct widget diskodlg_widgets[1];
static int diskodlg_active = 0;
static size_t diskodlg_frames, diskodlg_est;
static uint32_t diskodlg_rate;
static int prgh;

static void diskodlg_draw(void)
{
	int sec, pos;
	char buf[32];

	if (!diskodlg_active) {
		/* what are we doing here?! */
		dialog_destroy_all();
		log_appendf(4, "disk export dialog was eaten by a grue!");
		return;
	}

	sec = diskodlg_frames / diskodlg_rate;
	pos = diskodlg_frames * 64 / diskodlg_est;
	snprintf(buf, 32, "Exporting song...%6d:%02d", sec / 60, sec % 60);
	buf[31] = '\0';
	draw_text(buf, 27, 27, 0, 2);
	draw_fill_chars(24, 30, 55, 30, DEFAULT_FG, 0);
	draw_vu_meter(24, 30, 32, pos, prgh, prgh);
	draw_box(23, 29, 56, 31, BOX_THIN | BOX_INNER | BOX_INSET);
}

static void diskodlg_cancel(SCHISM_UNUSED void *ignored)
{
	disko_export_cancel();
}

static void diskodlg_setup(void);

// this needs to be done to work around stupid inconsistent key-up code
static void diskodlg_reset(SCHISM_UNUSED void *ignored)
{
	diskodlg_setup();
}

static void diskodlg_setup(void)
{
	struct dialog *d = dialog_create_custom(22, 25, 36, 8, diskodlg_widgets, 0, 0, diskodlg_draw, NULL);
	d->action_yes = diskodlg_reset;
	d->action_no = diskodlg_reset;
	d->action_cancel = diskodlg_cancel;

	uint32_t r = (rand() >> 8) & 63;
	if (r <= 7) {
		prgh = 6;
	} else if (r <= 18) {
		prgh = 3;
	} else if (r <= 31) {
		prgh = 5;
	} else {
		prgh = 4;
	}
}

static void tracker_diskwrite_begin(size_t est_frames, uint32_t rate)
{
	diskodlg_active = 1;
	diskodlg_frames = 0;
	diskodlg_est = est_frames;
	diskodlg_rate = rate;
	diskodlg_setup();
}

static void tracker_diskwrite_progress(size_t frames, size_t est_frames, uint32_t rate)
{
	diskodlg_frames = frames;
	diskodlg_est = est_frames;
	diskodlg_rate = rate;
	status.flags |= NEED_UPDATE;
}

/* if it was canceled, the dialog has already closed itself */
static void tracker_diskwrite_end(int canceled)
{
	diskodlg_active = 0;
	if (!canceled)
		dialog_destroy();
}

/* ------------------------------------------------------------------------ */

void tracker_host_register(void)
{
	struct engine_host host = {
		.log = log_host_append,
		.log_underline = log_host_underline,
		.status_text = status_text_host,
		.redraw = tracker_redraw,

		.song_changed = main_song_changed_cb,
		.song_mode_changed = main_song_mode_changed_cb,
		.message_reset = message_reset_selection,
		.memused_changed = memused_songchanged,
		.audio_driver_changed = preferences_audio_driver_changed,

		.playback_reset = pattern_editor_playback_reset,
		.playback_stopped = pattern_editor_playback_stopped,
		.post_event = events_push_event,

		.vis_resize = vis_set_size,
		.vis_feed = tracker_vis_feed,

		.midi_key = handle_key,
		.midi_activity = tracker_midi_activity,
		.midi_monitor_active = tracker_midi_monitor_active,
		.config_dir = tracker_config_dir,

		.diskwrite_begin = tracker_diskwrite_begin,
		.diskwrite_progress = tracker_diskwrite_progress,
		.diskwrite_end = tracker_diskwrite_end,

		.exit = schism_exit,
#ifndef NDEBUG
		.assert_fail = tracker_assert_fail,
#endif
	};

	engine_host_register(&host);
}
