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

#ifndef SCHISM_ENGINE_HOST_H_
#define SCHISM_ENGINE_HOST_H_

/* The engine (player/, fmt/, and the song, playback, disk writer, MIDI and
 * backend code in schism/ and sys/) talks to whatever program it's built
 * into through the callbacks below, and never calls UI code directly.
 *
 * A program fills in a struct engine_host and passes it to
 * engine_host_register() before it initialises anything else. Members it
 * leaves NULL get a default that does nothing, apart from exit and
 * assert_fail, which exit and abort. docs/engine.md lists who registers what.
 *
 * Unless a comment says otherwise, a callback is only called from the thread
 * that called into the engine. */

#include "headers.h"

#include "events.h" /* schism_event_t, struct key_event */

struct midi_port;

/* flags for engine_host.log */
enum {
	/* put the date and time at the end of the line */
	ENGINE_LOG_TIMESTAMP = (1 << 0),
};

/* the colour log_nl() uses for its empty line */
#define ENGINE_LOG_DEFAULT_COLOR 3

struct engine_host {
	/* ---- messages ---- */

	/* Append one line to the log. text is UTF-8, color is one of the 16
	 * palette entries, flags are ENGINE_LOG_*. The engine keeps ownership of
	 * text. */
	void (*log)(int color, int flags, const char *text);
	/* Underline the last line appended with log. */
	void (*log_underline)(void);
	/* Show a short message on the status line. */
	void (*status_text)(const char *text);
	/* Something the host might be showing changed (the file list, the MIDI
	 * monitor) and the screen should be redrawn. */
	void (*redraw)(void);

	/* ---- song notifications ---- */

	/* A song was loaded, or the current one cleared. */
	void (*song_changed)(void);
	/* Playback started, stopped or was paused. */
	void (*song_mode_changed)(void);
	/* A newly loaded song replaced the song message. */
	void (*message_reset)(void);
	/* Sample data was added, removed or resized. */
	void (*memused_changed)(void);
	/* audio_init() opened a device, possibly a different one. */
	void (*audio_driver_changed)(void);

	/* ---- playback feedback ---- */

	/* The play state is being reset, on start, stop and loop. Called with
	 * the audio lock held. */
	void (*playback_reset)(void);
	/* Playback stopped, either from song_stop() or because the song ended.
	 * Can be called from the audio thread, with the audio lock held. */
	void (*playback_stopped)(void);
	/* Hand an event to the host's main loop: SCHISM_EVENT_PLAYBACK when the
	 * play position moves, SCHISM_EVENT_UPDATE_IPMIDI, and incoming MIDI
	 * (SCHISM_EVENT_MIDI_*). Called from the audio and MIDI threads. Returns
	 * nonzero if the event was queued. */
	int (*post_event)(const schism_event_t *event);

	/* ---- visualiser feed, from the audio thread ---- */

	/* The mixing buffer is now this many frames long. */
	void (*vis_resize)(uint32_t frames);
	/* Mixed audio, after conversion to the output format: 8 bits is signed.
	 * buf is NULL with frames 0 when nothing was played. */
	void (*vis_feed)(const void *buf, uint32_t frames, uint32_t bits, uint32_t channels);

	/* ---- MIDI ---- */

	/* A MIDI note, pitch bend or controller event, translated into a key
	 * event for the host's input handling (midi_engine_handle_event()). */
	void (*midi_key)(struct key_event *k);
	/* MIDI data went in or out, for a MIDI monitor. port is the input port
	 * (which some drivers leave NULL), or NULL when outgoing is set. Called
	 * with the MIDI record lock held, possibly from a MIDI or timer thread. */
	void (*midi_activity)(struct midi_port *port, const unsigned char *data,
		uint32_t len, int outgoing);
	/* Nonzero if the host is showing a MIDI monitor. While it is, outgoing
	 * data is passed to midi_activity, and incoming data is echoed to the
	 * outputs. */
	int (*midi_monitor_active)(void);
	/* Directory holding the host's config file, where the MIDI port
	 * settings are read from. NULL if there isn't one. */
	const char *(*config_dir)(void);

	/* ---- song export progress ---- */

	/* An export started. est_frames is a guess at its length; rate is the
	 * export sample rate. The host can stop the export with
	 * disko_export_cancel(). */
	void (*diskwrite_begin)(size_t est_frames, uint32_t rate);
	/* Called after each block disko_sync() writes. */
	void (*diskwrite_progress)(size_t frames, size_t est_frames, uint32_t rate);
	/* The export finished, failed or was canceled. */
	void (*diskwrite_end)(int canceled);

	/* ---- fatal errors ---- */

	/* The engine can't go on (no audio device could be opened). Shut down
	 * and exit with this status. Must not return. */
	void (*exit)(int status);
	/* A SCHISM_RUNTIME_ASSERT failed. Must not return. */
	void (*assert_fail)(const char *msg, const char *exp, const char *file, int line);
};

/* Copy the callbacks in. NULL members get the defaults. */
void engine_host_register(const struct engine_host *host);

/* the registered callbacks; every member is non-NULL */
extern struct engine_host engine_hooks;

/* ------------------------------------------------------------------------ */
/* Engine state that used to live in the UI's `status.flags`. The engine
 * reads these, and the host sets them from its configuration. */

enum {
	/* if this is set, some stuff behaves differently
	 * (grep the source files for what stuff ;) */
	CLASSIC_MODE = (1 << 0),

	/* make a backup file (song.it~) when saving a module? */
	MAKE_BACKUPS = (1 << 1),
	NUMBERED_BACKUPS = (1 << 2), /* song.it.3~ */

	/* this is here if anything is "changed" and we need to whine to
	the user if they quit */
	SONG_NEEDS_SAVE = (1 << 3),

	/* if the disko is active */
	DISKWRITER_ACTIVE = (1 << 4),
	DISKWRITER_ACTIVE_PATTERN = (1 << 5), /* recording only a single pattern */

	/* don't open network MIDI ports */
	NO_NETWORK = (1 << 6),

	/* Play MIDI events using the same semantics as tracker samples */
	MIDI_LIKE_TRACKER = (1 << 7),

	/* if true, don't stop playing on load, and start playing new song afterward
	(but only if the last song was already playing before loading) */
	PLAY_AFTER_LOAD = (1 << 8),
};

/* returns flags & mask */
int engine_flag_test(int mask);
void engine_flag_set(int mask);
void engine_flag_clear(int mask);
void engine_flag_toggle(int mask);

#endif /* SCHISM_ENGINE_HOST_H_ */
