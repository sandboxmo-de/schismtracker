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

/* The engine's end of include/engine-host.h: the registered callbacks, their
 * defaults, and the log and status functions the engine calls, which format
 * the text and pass it on. */

#include "headers.h"

#include "engine-host.h"
#include "log.h"

#include <errno.h>
#include <stdarg.h>

/* ------------------------------------------------------------------------ */
/* defaults */

static void default_log(SCHISM_UNUSED int color, SCHISM_UNUSED int flags,
	SCHISM_UNUSED const char *text) {}
static void default_void(void) {}
static void default_text(SCHISM_UNUSED const char *text) {}
static int default_post_event(SCHISM_UNUSED const schism_event_t *event) { return 0; }
static void default_vis_resize(SCHISM_UNUSED uint32_t frames) {}
static void default_vis_feed(SCHISM_UNUSED const void *buf, SCHISM_UNUSED uint32_t frames,
	SCHISM_UNUSED uint32_t bits, SCHISM_UNUSED uint32_t channels) {}
static void default_midi_key(SCHISM_UNUSED struct key_event *k) {}
static void default_midi_activity(SCHISM_UNUSED struct midi_port *port,
	SCHISM_UNUSED const unsigned char *data, SCHISM_UNUSED uint32_t len,
	SCHISM_UNUSED int outgoing) {}
static int default_midi_monitor_active(void) { return 0; }
static const char *default_config_dir(void) { return NULL; }
static void default_diskwrite_begin(SCHISM_UNUSED size_t est_frames, SCHISM_UNUSED uint32_t rate) {}
static void default_diskwrite_progress(SCHISM_UNUSED size_t frames,
	SCHISM_UNUSED size_t est_frames, SCHISM_UNUSED uint32_t rate) {}
static void default_diskwrite_end(SCHISM_UNUSED int canceled) {}

static void default_exit(int status)
{
	exit(status);
}

static void default_assert_fail(const char *msg, const char *exp, const char *file, int line)
{
	fprintf(stderr, "Assertion failed at %s:%d: %s (%s)\n", file, line, msg, exp);
	abort();
}

static const struct engine_host default_hooks = {
	.log = default_log,
	.log_underline = default_void,
	.status_text = default_text,
	.redraw = default_void,

	.song_changed = default_void,
	.song_mode_changed = default_void,
	.message_reset = default_void,
	.memused_changed = default_void,
	.audio_driver_changed = default_void,

	.playback_reset = default_void,
	.playback_stopped = default_void,
	.post_event = default_post_event,

	.vis_resize = default_vis_resize,
	.vis_feed = default_vis_feed,

	.midi_key = default_midi_key,
	.midi_activity = default_midi_activity,
	.midi_monitor_active = default_midi_monitor_active,
	.config_dir = default_config_dir,

	.diskwrite_begin = default_diskwrite_begin,
	.diskwrite_progress = default_diskwrite_progress,
	.diskwrite_end = default_diskwrite_end,

	.exit = default_exit,
	.assert_fail = default_assert_fail,
};

struct engine_host engine_hooks = default_hooks;

void engine_host_register(const struct engine_host *host)
{
	engine_hooks = *host;

#define DEFAULT(name) if (!engine_hooks.name) engine_hooks.name = default_hooks.name
	DEFAULT(log);
	DEFAULT(log_underline);
	DEFAULT(status_text);
	DEFAULT(redraw);
	DEFAULT(song_changed);
	DEFAULT(song_mode_changed);
	DEFAULT(message_reset);
	DEFAULT(memused_changed);
	DEFAULT(audio_driver_changed);
	DEFAULT(playback_reset);
	DEFAULT(playback_stopped);
	DEFAULT(post_event);
	DEFAULT(vis_resize);
	DEFAULT(vis_feed);
	DEFAULT(midi_key);
	DEFAULT(midi_activity);
	DEFAULT(midi_monitor_active);
	DEFAULT(config_dir);
	DEFAULT(diskwrite_begin);
	DEFAULT(diskwrite_progress);
	DEFAULT(diskwrite_end);
	DEFAULT(exit);
	DEFAULT(assert_fail);
#undef DEFAULT
}

/* ------------------------------------------------------------------------ */
/* engine state */

static int engine_flags = 0;

int engine_flag_test(int mask)
{
	return engine_flags & mask;
}

void engine_flag_set(int mask)
{
	engine_flags |= mask;
}

void engine_flag_clear(int mask)
{
	engine_flags &= ~mask;
}

void engine_flag_toggle(int mask)
{
	engine_flags ^= mask;
}

/* ------------------------------------------------------------------------ */
/* log and status line (declared in log.h) */

static void vlog(int color, int flags, const char *format, va_list ap)
{
	char *ptr;

	if (vasprintf(&ptr, format, ap) == -1 || !ptr) {
		perror("asprintf");
		exit(255);
	}

	engine_hooks.log(color, flags, ptr);
	free(ptr);
}

void log_appendf(int color, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vlog(color, 0, format, ap);
	va_end(ap);
}

void log_appendf_timestamp(int color, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vlog(color, ENGINE_LOG_TIMESTAMP, format, ap);
	va_end(ap);
}

void log_append_timestamp(int color, const char *text)
{
	engine_hooks.log(color, ENGINE_LOG_TIMESTAMP, text);
}

void log_nl(void)
{
	engine_hooks.log(ENGINE_LOG_DEFAULT_COLOR, 0, "");
}

void log_underline(void)
{
	engine_hooks.log_underline();
}

void log_perror(const char *prefix)
{
	char *e = strerror(errno);
	perror(prefix);
	log_appendf(4, "%s: %s", prefix, e);
}

void status_text_flash(const char *format, ...)
{
	char *ptr;
	va_list ap;

	va_start(ap, format);
	if (vasprintf(&ptr, format, ap) == -1)
		abort();
	va_end(ap);

	engine_hooks.status_text(ptr);
	free(ptr);
}

/* ------------------------------------------------------------------------ */

#ifndef NDEBUG
void schism_assert_fail(const char *msg, const char *exp, const char *file, int line)
{
	engine_hooks.assert_fail(msg, exp, file, line);

	/* in case the host's handler came back */
	abort();
}
#endif
