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
#include "util.h"
#include "it.h"
#include "osdefs.h"
#include "events.h"
#include "page.h"
#include "widget.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

#define kMyDriversKeyboardClassName     "AppleADBKeyboard"
#define kfnSwitchError                  200
#define kfnAppleMode            0
#define kfntheOtherMode         1

#ifndef kIOHIDFKeyModeKey
#define kIOHIDFKeyModeKey    "HIDFKeyMode"
#endif

int macosx_ibook_fnswitch(int setting)
{
	kern_return_t kr;
	mach_port_t mp;
	io_service_t so;
	/*io_name_t sn;*/
	io_connect_t dp;
	io_iterator_t it;
	CFDictionaryRef classToMatch;
	/*CFNumberRef   fnMode;*/
	unsigned int res, dummy;

	kr = IOMasterPort(bootstrap_port, &mp);
	if (kr != KERN_SUCCESS) return -1;

	classToMatch = IOServiceMatching(kIOHIDSystemClass);
	if (classToMatch == NULL) {
		return -1;
	}
	kr = IOServiceGetMatchingServices(mp, classToMatch, &it);
	if (kr != KERN_SUCCESS) return -1;

	so = IOIteratorNext(it);
	IOObjectRelease(it);

	if (!so) return -1;

	kr = IOServiceOpen(so, mach_task_self(), kIOHIDParamConnectType, &dp);
	if (kr != KERN_SUCCESS) return -1;

	kr = IOHIDGetParameter(dp, CFSTR(kIOHIDFKeyModeKey), sizeof(res),
						&res, (IOByteCount *) &dummy);
	if (kr != KERN_SUCCESS) {
		IOServiceClose(dp);
		return -1;
	}

	if (setting == kfnAppleMode || setting == kfntheOtherMode) {
		dummy = setting;
		kr = IOHIDSetParameter(dp, CFSTR(kIOHIDFKeyModeKey),
					&dummy, sizeof(dummy));
		if (kr != KERN_SUCCESS) {
			IOServiceClose(dp);
			return -1;
		}
	}

	IOServiceClose(dp);
	/* old setting... */
	return res;
}

/* ------------------------------------------------------------------------ */

/* this gets stored at startup as the initial value of fnswitch before
 * we tamper with it, so we can restore it on shutdown */
static int ibook_helper = -1;

int macosx_event(schism_event_t *event)
{
	switch (event->type) {
	case SCHISM_WINDOWEVENT_FOCUS_GAINED:
		macosx_ibook_fnswitch(1);
		return 1;
	case SCHISM_WINDOWEVENT_FOCUS_LOST:
		macosx_ibook_fnswitch(ibook_helper);
		return 1;
	case SCHISM_KEYDOWN:
	case SCHISM_KEYUP:
		switch (status.fix_numlock_setting) {
		case NUMLOCK_GUESS:
			/* why is this checking for ibook_helper? */
			if (ibook_helper != -1) {
				if (ACTIVE_PAGE.selected_widget > -1
					&& ACTIVE_PAGE.selected_widget < ACTIVE_PAGE.total_widgets
					&& ACTIVE_PAGE_WIDGET.accept_text) {
					/* text is more likely? */
					event->key.mod |= SCHISM_KEYMOD_NUM;
				} else {
					event->key.mod &= ~SCHISM_KEYMOD_NUM;
				}
			} /* otherwise honor it */
			break;
		default:
			/* other cases are handled in schism/main.c */
			break;
		}

		switch (event->key.scancode) {
		case SCHISM_SCANCODE_KP_ENTER:
			/* On portables, the regular Insert key
			 * isn't available. This is equivalent to
			 * pressing Fn-Return, which just so happens
			 * to be a "de facto" Insert in mac land.
			 * However, on external keyboards this causes
			 * a real keypad enter to get eaten by this
			 * function as well. IMO it's more important
			 * for now that portable users can actually
			 * have an Insert key.
			 *
			 *   - paper */
			event->key.sym = SCHISM_KEYSYM_INSERT;
			break;
		default:
			break;
		};
		return 1;
	default:
		break;
	}
	return 1;
}

void macosx_ibook_init(void)
{
	/* macosx_ibook_fnswitch only sets the value if it's one of (0, 1) */
	ibook_helper = macosx_ibook_fnswitch(-1);
}

void macosx_ibook_quit(void)
{
	/* return back to default */
	if (ibook_helper != -1)
		macosx_ibook_fnswitch(ibook_helper);
}
