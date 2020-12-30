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

/* The all-important pattern editor. The code here is a general mess, so
 * don't look at it directly or, uh, you'll go blind or something. */

#include "headers.h"

#include <ctype.h>

#include "it.h"
#include "page.h"
#include "song.h"
#include "pattern-view.h"
#include "config-parser.h"
#include "midi.h"
#include "osdefs.h"

#include "sdlmain.h"
//#include "clippy.h"
#include "disko.h"

/* --------------------------------------------------------------------------------------------------------- */

#define ROW_IS_MAJOR(r) (current_song->row_highlight_major != 0 && (r) % current_song->row_highlight_major == 0)
#define ROW_IS_MINOR(r) (current_song->row_highlight_minor != 0 && (r) % current_song->row_highlight_minor == 0)
#define ROW_IS_HIGHLIGHT(r) (ROW_IS_MINOR(r) || ROW_IS_MAJOR(r))

/* this is actually used by pattern-view.c */
int show_default_volumes = 0;

/* --------------------------------------------------------------------- */
/* The (way too many) static variables */

static int midi_start_record = 0;

enum {
    TEMPLATE_OFF = 0,
    TEMPLATE_OVERWRITE,
    TEMPLATE_MIX_PATTERN_PRECEDENCE,
    TEMPLATE_MIX_CLIPBOARD_PRECEDENCE,
    TEMPLATE_NOTES_ONLY,
    TEMPLATE_MODE_MAX,
};
static int template_mode = TEMPLATE_OFF;

static const char *template_mode_names[] = {
	"",
	"Template, Overwrite",
	"Template, Mix - Pattern data precedence",
	"Template, Mix - Clipboard data precedence",
	"Template, Notes only",
};

/* only one widget, but MAN is it complicated :) */
static struct widget widgets_pattern[1];

/* pattern display position */
static int top_display_channel = 1;             /* one-based */
static int top_display_row = 0;         /* zero-based */

/* these three tell where the cursor is in the pattern */
static int current_row = 0;

static int keyjazz_noteoff = 0;       /* issue noteoffs when releasing note */
static int keyjazz_write_noteoff = 0; /* write noteoffs when releasing note */
static int keyjazz_repeat = 1;        /* insert multiple notes on key repeat */

/* this is, of course, what the current pattern is */
static int current_pattern = 0;

static int skip_value = 1;              /* aka cursor step */

static int link_effect_column = 0;
static int draw_divisions = 0;          /* = vertical lines between channels */


static int centralise_cursor = 0;
static int highlight_current_row = 0;

/* blah; other forwards */
static void pated_save(const char *descr);
static void pated_history_add2(int groupedf, const char *descr, int x, int y, int width, int height);
static void pated_history_add(const char *descr, int x, int y, int width, int height);

/* the current editing mask (what stuff is copied) */
static int edit_copy_mask = MASK_NOTE | MASK_INSTRUMENT | MASK_VOLUME;

/* playback mark (ctrl-f7) */
static int marked_pattern = -1, marked_row;

/* volume stuff (alt-i, alt-j, ctrl-j) */
static int volume_percent = 100;
static int fast_volume_percent = 67;
static int fast_volume_mode = 0;        /* toggled with ctrl-j */

enum {
    COPY_INST_OFF = 0, /* no search (IT style) */
    COPY_INST_UP = 1, /* search above the cursor for an instrument number */
    COPY_INST_UP_THEN_DOWN = 2, /* search both ways, up to row 0 first, then down */
    COPY_INST_SENTINEL = 3, /* non-value */
};
static int mask_copy_search_mode = COPY_INST_OFF;

/* If nonzero, home/end will move to the first/last row in the current channel
prior to moving to the first/last channel, i.e. operating in a 'z' pattern.
This is closer to FT2's behavior for the keys. */
static int invert_home_end = 0;

/* --------------------------------------------------------------------- */
/* undo and clipboard handling */
struct pattern_snap {
	song_note_t *data;
	int channels;
	int rows;

	/* used by undo/history only */
	const char *snap_op;
	int snap_op_allocated;
	int x, y;
	int patternno;
};
static struct pattern_snap fast_save = {
	NULL, 0, 0,
	"Fast Pattern Save",
	0, 0, 0, -1
};
/* static int fast_save_validity = -1; */

static struct pattern_snap clipboard = {
	NULL, 0, 0,
	"Clipboard",
	0, 0, 0, -1
};
static struct pattern_snap undo_history[10];
static int undo_history_top = 0;

/* this function is stupid, it doesn't belong here */
void memused_get_pattern_saved(unsigned int *a, unsigned int *b)
{
	int i;
	if (b) {
		for (i = 0; i < 10; i++) {
			if (undo_history[i].data)
				*b = (*b) + undo_history[i].rows;
		}
	}
	if (a) {
		if (clipboard.data) (*a) = (*a) + clipboard.rows;
		if (fast_save.data) (*a) = (*a) + fast_save.rows;
	}
}



/* This is a bit weird... Impulse Tracker usually wraps at 74, but if
 * the line doesn't have any spaces in it (like in a solid line of
 * dashes or something) it gets wrapped at 75. I'm using 75 for
 * everything because it's always nice to have a bit extra space :) */
#define LINE_WRAP 75

/* --------------------------------------------------------------------- */

/* --------------------------------------------------------------------- */

void message_load_page(struct page *page)
{
}

void message_reset_selection(void)
{
}


/* --------------------------------------------------------------------- */
/* block selection handling */

static struct {
	int first_channel;
	int last_channel;
	int first_row;
	int last_row;
} selection = { 0, 0, 0, 0 };

/* if first_channel is zero, there's no selection, as the channel
 * numbers start with one. (same deal for last_channel, but i'm only
 * caring about one of them to be efficient.) */
#define SELECTION_EXISTS (selection.first_channel)

/* CHECK_FOR_SELECTION(optional return value)
will display an error dialog and cause the function to return if there is no block marked.
(The spaces around the text are to make it line up the same as Impulse Tracker) */
#define CHECK_FOR_SELECTION(q) do {\
	if (!SELECTION_EXISTS) {\
		q;\
	}\
} while(0)

/* --------------------------------------------------------------------- */
/* this is for the multiple track views stuff. */

struct track_view {
	int width;
	draw_channel_header_func draw_channel_header;
	draw_note_func draw_note;
	draw_mask_func draw_mask;
};

static const struct track_view track_views[] = {
#define TRACK_VIEW(n) {n, draw_channel_header_##n, draw_note_##n, draw_mask_##n}
	TRACK_VIEW(13),                 /* 5 channels */
	TRACK_VIEW(10),                 /* 6/7 channels */
	TRACK_VIEW(7),                  /* 9/10 channels */
	TRACK_VIEW(6),                  /* 10/12 channels */
	TRACK_VIEW(3),                  /* 18/24 channels */
	TRACK_VIEW(2),                  /* 24/36 channels */
	TRACK_VIEW(1),                  /* 36/64 channels */
#undef  TRACK_VIEW
};

#define NUM_TRACK_VIEWS ARRAY_SIZE(track_views)

static uint8_t track_view_scheme[64];
static int channel_multi_enabled = 0;
static int channel_multi[64];
static int visible_channels, visible_width;

static void recalculate_visible_area(void);

/* --------------------------------------------------------------------------------------------------------- */
/* options dialog */

static struct widget options_widgets[8];
static const int options_link_split[] = { 5, 6, -1 };
static int options_last_octave = 0;

static void options_change_base_octave(void)
{
	kbd_set_current_octave(options_widgets[0].d.thumbbar.value);
}

/* the base octave is changed directly when the thumbbar is changed.
 * anything else can wait until the dialog is closed. */
void pattern_editor_display_options(void)
{
	struct dialog *dialog;

	if (options_widgets[0].width == 0) {
		/* haven't built it yet */
		create_thumbbar(options_widgets + 0, 40, 23, 2, 7, 1, 1, options_change_base_octave, 0, 8);
		create_thumbbar(options_widgets + 1, 40, 26, 3, 0, 2, 2, NULL, 0, 16);
		create_thumbbar(options_widgets + 2, 40, 29, 5, 1, 3, 3, NULL, 0, 32);
		create_thumbbar(options_widgets + 3, 40, 32, 17, 2, 4, 4, NULL, 0, 128);
		/* Although patterns as small as 1 row can be edited properly (as of c759f7a0166c), I have
		discovered it's a bit annoying to hit 'home' here expecting to get 32 rows but end up with
		just one row instead. so I'll allow editing these patterns, but not really provide a way to
		set the size, at least until I decide how to present the option nonintrusively. */
		create_thumbbar(options_widgets + 4, 40, 35, 22, 3, 5, 5, NULL, 32, 200);
		create_togglebutton(options_widgets + 5, 40, 38, 8, 4, 7, 6, 6, 6,
				    NULL, "Link", 3, options_link_split);
		create_togglebutton(options_widgets + 6, 52, 38, 9, 4, 7, 5, 5, 5,
				    NULL, "Split", 3, options_link_split);
//		create_button(options_widgets + 7, 35, 41, 8, 5, 0, 7, 7, 7, dialog_yes_NULL, "Done", 3);
	}

	options_last_octave = kbd_get_current_octave();
	options_widgets[0].d.thumbbar.value = options_last_octave;
	options_widgets[1].d.thumbbar.value = skip_value;
	options_widgets[2].d.thumbbar.value = current_song->row_highlight_minor;
	options_widgets[3].d.thumbbar.value = current_song->row_highlight_major;
	options_widgets[4].d.thumbbar.value = song_get_pattern(current_pattern, NULL);
	togglebutton_set(options_widgets, link_effect_column ? 5 : 6, 0);

//	dialog = dialog_create_custom(10, 18, 60, 26, options_widgets, 8, options_selected_widget,
//				      options_draw_const, NULL);
//	dialog->action_yes = options_close;
//	if (status.flags & CLASSIC_MODE) {
//		dialog->action_cancel = options_close;
//	} else {
//		dialog->action_cancel = options_close_cancel;
//	}
//	dialog->data = dialog;
}

/* --------------------------------------------------------------------------------------------------------- */
/* pattern length dialog */
static struct widget length_edit_widgets[4];

void pattern_editor_length_edit(void)
{
	struct dialog *dialog;

	create_thumbbar(length_edit_widgets + 0, 34, 24, 22, 0, 1, 1, NULL, 32, 200);
	length_edit_widgets[0].d.thumbbar.value = song_get_pattern(current_pattern, NULL );
	create_thumbbar(length_edit_widgets + 1, 34, 27, 26, 0, 2, 2, NULL, 0, 199);
	create_thumbbar(length_edit_widgets + 2, 34, 28, 26, 1, 3, 3, NULL, 0, 199);
	length_edit_widgets[1].d.thumbbar.value
		= length_edit_widgets[2].d.thumbbar.value
		= current_pattern;

//	create_button(length_edit_widgets + 3, 35, 31, 8, 2, 3, 3, 3, 0, dialog_yes_NULL, "OK", 4);
//
//	dialog = dialog_create_custom(15, 19, 51, 15, length_edit_widgets, 4, 0,
//				      length_edit_draw_const, NULL);
//	dialog->action_yes = length_edit_close;
//	dialog->action_cancel = length_edit_cancel;
}

/* --------------------------------------------------------------------------------------------------------- */
/* settings */

#define CFG_SET_PE(v) cfg_set_number(cfg, "Pattern Editor", #v, v)
void cfg_save_patedit(cfg_file_t *cfg)
{
	int n;
	char s[65];

	CFG_SET_PE(link_effect_column);
	CFG_SET_PE(draw_divisions);
	CFG_SET_PE(centralise_cursor);
	CFG_SET_PE(highlight_current_row);
	CFG_SET_PE(edit_copy_mask);
	CFG_SET_PE(volume_percent);
	CFG_SET_PE(fast_volume_percent);
	CFG_SET_PE(fast_volume_mode);
	CFG_SET_PE(keyjazz_noteoff);
	CFG_SET_PE(keyjazz_write_noteoff);
	CFG_SET_PE(keyjazz_repeat);
	CFG_SET_PE(mask_copy_search_mode);
	CFG_SET_PE(invert_home_end);

	cfg_set_number(cfg, "Pattern Editor", "crayola_mode", !!(status.flags & CRAYOLA_MODE));
	for (n = 0; n < 64; n++)
		s[n] = track_view_scheme[n] + 'a';
	s[64] = 0;

	cfg_set_string(cfg, "Pattern Editor", "track_view_scheme", s);
	for (n = 0; n < 64; n++)
		s[n] = (channel_multi[n]) ? 'M' : '-';
	s[64] = 0;
	cfg_set_string(cfg, "Pattern Editor", "channel_multi", s);
}

#define CFG_GET_PE(v,d) v = cfg_get_number(cfg, "Pattern Editor", #v, d)
void cfg_load_patedit(cfg_file_t *cfg)
{
	int n, r = 0;
	char s[65];

	CFG_GET_PE(link_effect_column, 0);
	CFG_GET_PE(draw_divisions, 1);
	CFG_GET_PE(centralise_cursor, 0);
	CFG_GET_PE(highlight_current_row, 0);
	CFG_GET_PE(edit_copy_mask, MASK_NOTE | MASK_INSTRUMENT | MASK_VOLUME);
	CFG_GET_PE(volume_percent, 100);
	CFG_GET_PE(fast_volume_percent, 67);
	CFG_GET_PE(fast_volume_mode, 0);
	CFG_GET_PE(keyjazz_noteoff, 0);
	CFG_GET_PE(keyjazz_write_noteoff, 0);
	CFG_GET_PE(keyjazz_repeat, 1);
	CFG_GET_PE(mask_copy_search_mode, 0);
	CFG_GET_PE(invert_home_end, 0);

	if (cfg_get_number(cfg, "Pattern Editor", "crayola_mode", 0))
		status.flags |= CRAYOLA_MODE;
	else
		status.flags &= ~CRAYOLA_MODE;

	cfg_get_string(cfg, "Pattern Editor", "track_view_scheme", s, 64, "a");

	/* "decode" the track view scheme */
	for (n = 0; n < 64; n++) {
		if (s[n] == '\0') {
			/* end of the string */
			break;
		} else if (s[n] >= 'a' && s[n] <= 'z') {
			s[n] -= 'a';
		} else if (s[n] >= 'A' && s[n] <= 'Z') {
			s[n] -= 'A';
		} else {
			log_appendf(4, "Track view scheme corrupted; using default");
			n = 64;
			r = 0;
			break;
		}
		r = s[n];
	}
	memcpy(track_view_scheme, s, n);
	if (n < 64)
		memset(track_view_scheme + n, r, 64 - n);

	cfg_get_string(cfg, "Pattern Editor", "channel_multi", s, 64, "");
	memset(channel_multi, 0, sizeof(channel_multi));
	channel_multi_enabled = 0;
	for (n = 0; n < 64; n++) {
		if (!s[n])
			break;
		channel_multi[n] = ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z')) ? 1 : 0;
		if (channel_multi[n])
			channel_multi_enabled = 1;
	}

	recalculate_visible_area();
//	pattern_editor_reposition();
	if (status.current_page == PAGE_PATTERN_EDITOR)
		status.flags |= NEED_UPDATE;
}

enum roll_dir { ROLL_DOWN = -1, ROLL_UP = +1 };
/* --------------------------------------------------------------------------------------------------------- */
/* Row shifting operations */

static void snap_copy(struct pattern_snap *s, int x, int y, int width, int height)
{
	song_note_t *pattern;
	int row, total_rows, len;

	memused_songchanged();
	s->channels = width;
	s->rows = height;

	total_rows = song_get_pattern(current_pattern, &pattern);
	s->data = mem_alloc(len = (sizeof(song_note_t) * s->channels * s->rows));

	if (s->rows > total_rows) {
		memset(s->data, 0,  len);
	}

	s->x = x; s->y = y;
	if (x == 0 && width == 64) {
		if (height >total_rows) height = total_rows;
		memcpy(s->data, pattern + 64 * y, (width*height*sizeof(song_note_t)));
	} else {
		for (row = 0; row < s->rows && row < total_rows; row++) {
			memcpy(s->data + s->channels * row,
			       pattern + 64 * (row + s->y) + s->x,
			       s->channels * sizeof(song_note_t));
		}
	}
}

static void pated_save(const char *descr)
{
	int total_rows;

	total_rows = song_get_pattern(current_pattern, NULL);
	pated_history_add(descr,0,0,64,total_rows);
}
static void pated_history_add(const char *descr, int x, int y, int width, int height)
{
	pated_history_add2(0, descr, x, y, width, height);
}

static void pated_history_add2(int groupedf, const char *descr, int x, int y, int width, int height)
{
	int j;

	j = undo_history_top;
	if (groupedf
	&& undo_history[j].patternno == current_pattern
	&& undo_history[j].x == x && undo_history[j].y == y
	&& undo_history[j].channels == width
	&& undo_history[j].rows == height
	&& undo_history[j].snap_op
	&& strcmp(undo_history[j].snap_op, descr) == 0) {

		/* do nothing; use the previous bit of history */

	} else {
		j = (undo_history_top + 1) % 10;
		free(undo_history[j].data);
		snap_copy(&undo_history[j], x, y, width, height);
		undo_history[j].snap_op = str_dup(descr);
		undo_history[j].snap_op_allocated = 1;
		undo_history[j].patternno = current_pattern;
		undo_history_top = j;
	}
}
static void fast_save_update(void)
{
	int total_rows;

	free(fast_save.data);
	fast_save.data = NULL;

	total_rows = song_get_pattern(current_pattern, NULL);

	snap_copy(&fast_save, 0, 0, 64, total_rows);
}

/* --------------------------------------------------------------------- */

void update_current_row(void)
{
	char buf[4];

	draw_text(numtostr(3, current_row, buf), 12, 7, 5, 0);
	draw_text(numtostr(3, song_get_rows_in_pattern(current_pattern), buf), 16, 7, 5, 0);
}

int get_current_channel(void)
{
//	return current_channel;
	return 0;
}

void set_current_channel(int channel)
{
//	current_channel = CLAMP(channel, 0, 64);
}

int get_current_row(void)
{
	return current_row;
}

void set_current_row(int row)
{
	int total_rows = song_get_rows_in_pattern(current_pattern);

	current_row = CLAMP(row, 0, total_rows);
//	pattern_editor_reposition();
	status.flags |= NEED_UPDATE;
}

/* --------------------------------------------------------------------- */

void update_current_pattern(void)
{
	char buf[4];

	draw_text(numtostr(3, current_pattern, buf), 12, 6, 5, 0);
	draw_text(numtostr(3, csf_get_num_patterns(current_song) - 1, buf), 16, 6, 5, 0);
}

int get_current_pattern(void)
{
	return current_pattern;
}

static void _pattern_update_magic(void)
{
	song_sample_t *s;
	int i;

	for (i = 1; i <= 99; i++) {
		s = song_get_sample(i);
		if (!s) continue;
		if (((unsigned char)s->name[23]) != 0xFF) continue;
		if (((unsigned char)s->name[24]) != current_pattern) continue;
		disko_writeout_sample(i,current_pattern,1);
		break;
	}
}

void set_current_pattern(int n)
{
	int total_rows;
	char undostr[64];

	if (!playback_tracing || !(song_get_mode() & (MODE_PLAYING|MODE_PATTERN_LOOP))) {
		_pattern_update_magic();
	}

	current_pattern = CLAMP(n, 0, 199);
	total_rows = song_get_rows_in_pattern(current_pattern);

	if (current_row > total_rows)
		current_row = total_rows;

	if (SELECTION_EXISTS) {
		if (selection.first_row > total_rows) {
			selection.first_row = selection.last_row = total_rows;
		} else if (selection.last_row > total_rows) {
			selection.last_row = total_rows;
		}
	}

	/* save pattern */
	sprintf(undostr, "Pattern %d", current_pattern);
	pated_save(undostr);
	fast_save_update();

//	pattern_editor_reposition();
//	pattern_selection_system_copyout();

	status.flags |= NEED_UPDATE;
}

/* --------------------------------------------------------------------- */

void play_song_from_mark_orderpan(void)
{
	if (marked_pattern == -1) {
		song_start_at_order(get_current_order(), current_row);
	} else {
		song_start_at_pattern(marked_pattern, marked_row);
	}
}
void play_song_from_mark(void)
{
	int new_order;

	if (marked_pattern != -1) {
		song_start_at_pattern(marked_pattern, marked_row);
		return;
	}

	new_order = get_current_order();
	while (new_order < 255) {
		if (current_song->orderlist[new_order] == current_pattern) {
			set_current_order(new_order);
			song_start_at_order(new_order, current_row);
			return;
		}
		new_order++;
	}
	new_order = 0;
	while (new_order < 255) {
		if (current_song->orderlist[new_order] == current_pattern) {
			set_current_order(new_order);
			song_start_at_order(new_order, current_row);
			return;
		}
		new_order++;
	}
	song_start_at_pattern(current_pattern, current_row);
}

/* --------------------------------------------------------------------- */

static void recalculate_visible_area(void)
{
	int n, last = 0, new_width;

	visible_width = 0;
	for (n = 0; n < 64; n++) {
		if (track_view_scheme[n] >= NUM_TRACK_VIEWS) {
			/* shouldn't happen, but might (e.g. if someone was messing with the config file) */
			track_view_scheme[n] = last;
		} else {
			last = track_view_scheme[n];
		}
		new_width = visible_width + track_views[track_view_scheme[n]].width;

		if (new_width > 72)
			break;
		visible_width = new_width;
		if (draw_divisions)
			visible_width++;
	}

	if (draw_divisions) {
		/* a division after the last channel would look pretty dopey :) */
		visible_width--;
	}
	visible_channels = n;

	/* don't allow anything past channel 64 */
	if (top_display_channel > 64 - visible_channels + 1)
		top_display_channel = 64 - visible_channels + 1;
}

/* --------------------------------------------------------------------- */

#if 0
static int note_is_empty(song_note_t *p)
{
	if (!p->note && p->voleffect == VOLFX_NONE && !p->effect && !p->param)
		return 1;
	return 0;
}
#endif

void pattern_editor_load_page(struct page *page)
{
//	int i;
//	for (i = 0; i < 10; i++) {
//		memset(&undo_history[i],0,sizeof(struct pattern_snap));
//		undo_history[i].snap_op = "Empty";
//		undo_history[i].snap_op_allocated = 0;
//	}
//	page->title = "Pattern Editor (F2)";
//	page->playback_update = pattern_editor_playback_update;
//	page->song_changed_cb = pated_song_changed;
//	page->pre_handle_key = _fix_f7;
//	page->total_widgets = 1;
//	page->clipboard_paste = pattern_selection_system_paste;
//	page->widgets = widgets_pattern;
//	page->help_index = HELP_PATTERN_EDITOR;
//
//	create_other(widgets_pattern + 0, 0, pattern_editor_handle_key_cb, pattern_editor_redraw);
}

