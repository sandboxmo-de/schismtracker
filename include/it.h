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

#ifndef IT_H
#define IT_H

#include <stdio.h>
#include <stdint.h>

#include <sys/types.h>

#include "util.h"

/* each page with configurable settings has a function to load/save them... */
#include "config-parser.h" /* FIXME: shouldn't need this here */

void cfg_load_midi(void);

void cfg_load_audio(void);

void cfg_load_disko(cfg_file_t *cfg);

void cfg_load_dmoz(cfg_file_t *cfg);

/* --------------------------------------------------------------------- */

#endif /* ! IT_H */

