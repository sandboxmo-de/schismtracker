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

		.exit = schism_exit,
#ifndef NDEBUG
		.assert_fail = tracker_assert_fail,
#endif
	};

	engine_host_register(&host);
}
