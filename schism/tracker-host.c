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

void tracker_host_register(void)
{
	struct engine_host host = {
		.log = log_host_append,
		.log_underline = log_host_underline,
		.status_text = status_text_host,

		.exit = schism_exit,
#ifndef NDEBUG
		.assert_fail = tracker_assert_fail,
#endif
	};

	engine_host_register(&host);
}
