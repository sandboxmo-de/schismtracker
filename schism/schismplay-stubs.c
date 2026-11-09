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
 * Nothing here draws or reads input. */

#include "headers.h"

#include "it.h"
#include "page.h"
#include "events.h"
#include "fakemem.h"
#include "config.h"
#include "song.h"
#include "osdefs.h"

/* ------------------------------------------------------------------------ */
/* Global tracker state (page.c). sys/macosx/osdefs.m's key handler reads
 * status.fix_numlock_setting; it's never called here. */

struct tracker_status status = {0};

/* sys/macosx/osdefs.m's key handler looks at the active page; it's never
 * called here. */
struct page pages[PAGE_MAX] = {0};

/* ------------------------------------------------------------------------ */
/* macOS keyboard quirk (ibook-support.c), referenced from osdefs.m.
 * -1 tells osdefs.m there's no fn-key setting to restore on exit. */

#ifdef SCHISM_MACOSX
int macosx_ibook_fnswitch(SCHISM_UNUSED int setting)
{
	return -1;
}
#endif
