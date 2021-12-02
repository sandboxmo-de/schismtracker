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

#include "slurp.h"
#include "sndfile.h"
#include "song.h"

#include "sdram.h"

#include "dmoz.h"
#include "fmt.h"

#include "it_defs.h"

#include "midi.h"
#include "snd_gm.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void _fix_names(song_t *qq) {
  int c, n;

  for (n = 1; n < MAX_INSTRUMENTS; n++) {
    for (c = 0; c < 25; c++)
      if (qq->samples[n].name[c] == 0)
        qq->samples[n].name[c] = 32;
    qq->samples[n].name[25] = 0;

    if (!qq->instruments[n])
      continue;
    for (c = 0; c < 25; c++)
      if (qq->instruments[n]->name[c] == 0)
        qq->instruments[n]->name[c] = 32;
    qq->instruments[n]->name[25] = 0;
  }
}

// clear patterns => clear filename and save flag
// clear orderlist => clear title, message, and channel settings
void song_new(int flags) {
  int i;

  song_lock_audio();

  song_stop_unlocked(0);

  if ((flags & KEEP_PATTERNS) == 0) {
    //		song_set_filename(NULL);
    //		status.flags &= ~SONG_NEEDS_SAVE;

    for (i = 0; i < MAX_PATTERNS; i++) {
      if (current_song->patterns[i]) {
        csf_free_pattern(current_song->patterns[i]);
        current_song->patterns[i] = NULL;
      }
      current_song->pattern_size[i] = 64;
      current_song->pattern_alloc_size[i] = 64;
    }
  }
  if ((flags & KEEP_SAMPLES) == 0) {
    for (i = 1; i < MAX_SAMPLES; i++) {
      if (current_song->samples[i].data) {
        csf_free_sample(current_song->samples[i].data);
      }
    }
    memset(current_song->samples, 0, sizeof(current_song->samples));
    for (i = 1; i < MAX_SAMPLES; i++) {
      current_song->samples[i].c5speed = 8363;
      current_song->samples[i].volume = 64 * 4;
      current_song->samples[i].global_volume = 64;
    }
  }
  if ((flags & KEEP_INSTRUMENTS) == 0) {
    for (i = 0; i < MAX_INSTRUMENTS; i++) {
      if (current_song->instruments[i]) {
        csf_free_instrument(current_song->instruments[i]);
        current_song->instruments[i] = NULL;
      }
    }
  }
  if ((flags & KEEP_ORDERLIST) == 0) {
    memset(current_song->orderlist, ORDER_LAST, sizeof(current_song->orderlist));
    memset(current_song->title, 0, sizeof(current_song->title));
    memset(current_song->message, 0, MAX_MESSAGE);

    for (i = 0; i < 64; i++) {
      current_song->channels[i].volume = 64;
      current_song->channels[i].panning = 128;
      current_song->channels[i].flags = 0;
      current_song->voices[i].volume = 256;
      current_song->voices[i].global_volume = current_song->channels[i].volume;
      current_song->voices[i].panning = current_song->channels[i].panning;
      current_song->voices[i].flags = current_song->channels[i].flags;
      current_song->voices[i].cutoff = 0x7F;
    }
  }

  current_song->repeat_count = 0;
  // song_stop();

  csf_forget_history(current_song);

  song_unlock_audio();

  //	main_song_changed_cb();
}

// ------------------------------------------------------------------------------------------------------------

// #define LOAD_SONG(x) fmt_##x##_load_song,

int fmt_it_load_song(song_t *song, slurp_t *fp, unsigned int lflags);
static fmt_load_song_func load_song_funcs[] = {
    fmt_it_load_song,
    // #include "fmt-types.h"
    NULL,
};

const char *fmt_strerror(int n) {
  switch (n) {
  case -LOAD_UNSUPPORTED:
    return "Unrecognised file type";
  case -LOAD_FORMAT_ERROR:
    return "File format error (corrupt?)";
  default:
    return strerror(errno);
  }
}

// IT uses \r in song messages; replace errant \n's
void message_convert_newlines(song_t *song) {
  int i = 0, len = strlen(song->message);
  for (i = 0; i < len; i++) {
    if (song->message[i] == '\n') {
      song->message[i] = '\r';
    }
  }
}

static slurp_t t = {0};
song_t *song_create_load(const char *file) {
  slurp_t *s = &t;
  song_t *newsong;
  if (current_song) {
    newsong = current_song;
  } else {
    newsong = reserve_space(sizeof(song_t));
    memset(newsong, 0, sizeof(song_t));
  }
  fmt_load_song_func *func;
  int ok = 0, err = 0;
  uint8_t *mmdata;
  size_t mmlen;
  bool loaded = load_file(file, &s->data, &s->length);

  if (!loaded)
    return NULL;

  // s->pos = 0;

  mmdata = s->data;
  mmlen = s->length;
  if (mmcmp_unpack(&mmdata, &mmlen)) {
    // clean up the existing data
    if (s->data && s->closure) {
      s->closure(s);
    }
    // and put the new stuff in
    s->length = mmlen;
    s->data = mmdata;
    s->closure = NULL;
    // s->closure = _slurp_closure_free;
  }

  if (current_song) {
    newsong->mix_flags = current_song->mix_flags;
    csf_set_wave_config(newsong, current_song->mix_frequency, current_song->mix_bits_per_sample,
                        current_song->mix_channels);

    // loaders might override these
    newsong->row_highlight_major = current_song->row_highlight_major;
    newsong->row_highlight_minor = current_song->row_highlight_minor;
    csf_copy_midi_cfg(newsong, current_song);
  }

  for (func = load_song_funcs; *func && !ok; func++) {
    slurp_rewind(s);
    switch ((*func)(newsong, s, 0)) {
    case LOAD_SUCCESS:
      err = 0;
      ok = 1;
      break;
    case LOAD_UNSUPPORTED:
      err = -LOAD_UNSUPPORTED;
      continue;
    case LOAD_FORMAT_ERROR:
      err = -LOAD_FORMAT_ERROR;
      break;
    case LOAD_FILE_ERROR:
      err = errno;
      break;
    }
    if (err) {
      //   csf_free(newsong);
      //   // unslurp(s);
      //   if (s->data && s->closure) {
      //     s->closure(s);
      //   }
      errno = err;
      return NULL;
    }
  }

  //   // unslurp(s);
  //   if (s->data && s->closure) {
  //     s->closure(s);
  //   }

  if (err) {
    // awwww, nerts!
    // csf_free(newsong);
    errno = err;
    return NULL;
  }

  newsong->stop_at_order = newsong->stop_at_row = -1;
  message_convert_newlines(newsong);
  //	message_reset_selection();

  return newsong;
}

int song_load_unchecked(const char *file) {
  // const char *base = get_basename(file);
  int was_playing;
  song_t *newsong;

  // IT stops the song even if the new song can't be loaded
  if (1) {
    was_playing = (song_get_mode() == MODE_PLAYING);
  } else {
    was_playing = 0;
    song_stop();
  }

  newsong = song_create_load(file);
  if (!newsong) {
    return 0;
  }

  //	song_set_filename(file);

  song_lock_audio();
  //   csf_free(current_song);
  current_song = newsong;
  current_song->repeat_count = 0;
  max_channels_used = 0;
  _fix_names(current_song);
  song_stop_unlocked(0);
  song_unlock_audio();

  if (was_playing)
    song_start();

  //	main_song_changed_cb();

  //	status.flags &= ~SONG_NEEDS_SAVE;

  // print out some stuff
  //   const char *tid = current_song->tracker_id;

  char fmt[] = " %d patterns, %d samples, %d instruments";
  int n, nsmp, nins;
  song_sample_t *smp;
  song_instrument_t **ins;

  for (n = 0, smp = current_song->samples + 1, nsmp = 0; n < MAX_SAMPLES; n++, smp++)
    if (smp->data)
      nsmp++;
  for (n = 0, ins = current_song->instruments + 1, nins = 0; n < MAX_INSTRUMENTS; n++, ins++)
    if (*ins != NULL)
      nins++;

  if (!nins)
    *strrchr(fmt, ',') = 0; // cut off 'instruments'

  return 1;
}

/* ------------------------------------------------------------------------- */

// #define LOAD_SAMPLE(x) fmt_##x##_load_sample,
// static fmt_load_sample_func load_sample_funcs[] = {
// #include "fmt-types.h"
//     NULL,
// };

// #define LOAD_INSTRUMENT(x) fmt_##x##_load_instrument,
// static fmt_load_instrument_func load_instrument_funcs[] = {
// #include "fmt-types.h"
//     NULL,
// };

void song_clear_sample(int n) {
  song_lock_audio();
  csf_destroy_sample(current_song, n);
  memset(current_song->samples + n, 0, sizeof(song_sample_t));
  current_song->samples[n].c5speed = 8363;
  current_song->samples[n].volume = 64 * 4;
  current_song->samples[n].global_volume = 64;
  song_unlock_audio();
}

void song_copy_sample(int n, song_sample_t *src) {
  memcpy(current_song->samples + n, src, sizeof(song_sample_t));

  if (src->data) {
    unsigned long bytelength = src->length;
    if (src->flags & CHN_16BIT)
      bytelength *= 2;
    if (src->flags & CHN_STEREO)
      bytelength *= 2;

    current_song->samples[n].data = csf_allocate_sample(bytelength);
    memcpy(current_song->samples[n].data, src->data, bytelength);
  }
}

void song_create_host_instrument(int smp, int ins) {
  if (csf_instrument_is_empty(current_song->instruments[smp]))
    ins = smp;
  else if (!csf_instrument_is_empty(current_song->instruments[ins]))
    ins = csf_first_blank_instrument(current_song, 0);

  if (ins > 0) {
    song_init_instrument_from_sample(ins, smp);
    //		status_text_flash("Sample assigned to Instrument %d", ins);
  } else {
    //		status_text_flash("Error: No available Instruments!");
  }
}

// ------------------------------------------------------------------------
// instrument loader

song_instrument_t *instrument_loader_init(struct instrumentloader *ii, int slot) {
  ii->expect_samples = 0;
  ii->inst = song_get_instrument(slot);
  ii->slot = slot;
  ii->basex = 1;
  memset(ii->sample_map, 0, sizeof(ii->sample_map));
  return ii->inst;
}

int instrument_loader_abort(struct instrumentloader *ii) {
  int n;
  song_wipe_instrument(ii->slot);
  for (n = 0; n < MAX_SAMPLES; n++) {
    if (ii->sample_map[n]) {
      song_clear_sample(ii->sample_map[n] - 1);
      ii->sample_map[n] = 0;
    }
  }
  return 0;
}

int instrument_loader_sample(struct instrumentloader *ii, int slot) {
  int x;

  if (!slot)
    return 0;
  if (ii->sample_map[slot])
    return ii->sample_map[slot];
  for (x = ii->basex; x < MAX_SAMPLES; x++) {
    song_sample_t *cur = (current_song->samples + x);

    //              if (!csf_sample_is_empty(current_song->samples + x))
    //                      continue;
    if (cur->data != NULL)
      continue;

    ii->expect_samples++;
    ii->sample_map[slot] = x;
    ii->basex = x + 1;
    return ii->sample_map[slot];
  }
  //	status_text_flash("Too many samples");
  return 0;
}
