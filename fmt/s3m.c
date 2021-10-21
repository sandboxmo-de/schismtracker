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

#define NEED_BYTESWAP
#include "headers.h"
#include "slurp.h"
#include "fmt.h"

#include "sndfile.h"

#include "log.h"

/* --------------------------------------------------------------------- */

int fmt_s3m_read_info(dmoz_file_t *file, const uint8_t *data, size_t length)
{
	if (!(length > 48 && memcmp(data + 44, "SCRM", 4) == 0))
		return 0;

	file->description = "Scream Tracker 3";
	/*file->extension = str_dup("s3m");*/
	file->title = strn_dup((const char *)data, 27);
	file->type = TYPE_MODULE_S3M;
	return 1;
}

/* --------------------------------------------------------------------------------------------------------- */

enum {
	S3I_TYPE_NONE = 0,
	S3I_TYPE_PCM = 1,
	S3I_TYPE_ADMEL = 2,
	S3I_TYPE_CONTROL = 0xff, // only internally used for saving
};


/* misc flags for loader (internal) */
#define S3M_UNSIGNED 1
#define S3M_CHANPAN 2 // the FC byte

int fmt_s3m_load_song(song_t *song, slurp_t *fp, unsigned int lflags)
{
	uint16_t nsmp, nord, npat;
	int misc = S3M_UNSIGNED | S3M_CHANPAN; // temporary flags, these are both generally true
	int n;
	song_note_t *note;
	/* junk variables for reading stuff into */
	uint16_t tmp;
	uint8_t c;
	uint32_t tmplong;
	uint8_t b[4];
	/* parapointers */
	uint16_t para_smp[MAX_SAMPLES];
	uint16_t para_pat[MAX_PATTERNS];
	uint32_t para_sdata[MAX_SAMPLES] = { 0 };
	uint32_t smp_flags[MAX_SAMPLES] = { 0 };
	song_sample_t *sample;
	uint16_t trkvers;
	uint16_t flags;
	uint16_t special;
	uint16_t reserved;
	uint32_t adlib = 0; // bitset
	uint16_t gus_addresses = 0;
	char any_samples = 0;
	int uc;
	const char *tid = NULL;

	/* check the tag */
	slurp_seek(fp, 44, SEEK_SET);
	slurp_read(fp, b, 4);
	if (memcmp(b, "SCRM", 4) != 0)
		return LOAD_UNSUPPORTED;

	/* read the title */
	slurp_rewind(fp);
	slurp_read(fp, song->title, 25);
	song->title[25] = 0;

	/* skip the last three bytes of the title, the supposed-to-be-0x1a byte,
	the tracker ID, and the two useless reserved bytes */
	slurp_seek(fp, 7, SEEK_CUR);

	slurp_read(fp, &nord, 2);
	slurp_read(fp, &nsmp, 2);
	slurp_read(fp, &npat, 2);
	nord = bswapLE16(nord);
	nsmp = bswapLE16(nsmp);
	npat = bswapLE16(npat);

	if (nord > MAX_ORDERS || nsmp > MAX_SAMPLES || npat > MAX_PATTERNS)
		return LOAD_FORMAT_ERROR;

	song->flags = SONG_ITOLDEFFECTS;
	slurp_read(fp, &flags, 2);  /* flags (don't really care) */
	flags = bswapLE16(flags);
	slurp_read(fp, &trkvers, 2);
	trkvers = bswapLE16(trkvers);
	slurp_read(fp, &tmp, 2);  /* file format info */
	if (tmp == bswapLE16(1))
		misc &= ~S3M_UNSIGNED;     /* signed samples (ancient s3m) */

	slurp_seek(fp, 4, SEEK_CUR); /* skip the tag */

	song->initial_global_volume = slurp_getc(fp) << 1;
	// In the case of invalid data, ST3 uses the speed/tempo value that's set in the player prior to
	// loading the song, but that's just crazy.
	song->initial_speed = slurp_getc(fp) ?: 6;
	song->initial_tempo = slurp_getc(fp);
	if (song->initial_tempo <= 32) {
		// (Yes, 32 is ignored by Scream Tracker.)
		song->initial_tempo = 125;
	}
	song->mixing_volume = slurp_getc(fp);
	if (song->mixing_volume & 0x80) {
		song->mixing_volume ^= 0x80;
	} else {
		song->flags |= SONG_NOSTEREO;
	}
	uc = slurp_getc(fp); /* ultraclick removal (useless) */

	if (slurp_getc(fp) != 0xfc)
		misc &= ~S3M_CHANPAN;     /* stored pan values */

	/* Extended Schism Tracker version information */
	slurp_read(fp, &reserved, 2);
	reserved = bswapLE16(reserved);
	/* Impulse Tracker hides its edit timer in the next four bytes. */
	slurp_seek(fp, 6, SEEK_CUR);
	slurp_read(fp, &special, 2); // field not used by st3
	special = bswapLE16(special);

	/* channel settings */
	for (n = 0; n < 32; n++) {
		/* Channel 'type': 0xFF is a disabled channel, which shows up as (--) in ST3.
		Any channel with the high bit set is muted.
		00-07 are L1-L8, 08-0F are R1-R8, 10-18 are adlib channels A1-A9.
		Hacking at a file with a hex editor shows some perhaps partially-implemented stuff:
		types 19-1D show up in ST3 as AB, AS, AT, AC, and AH; 20-2D are the same as 10-1D
		except with 'B' insted of 'A'. None of these appear to produce any sound output,
		apart from 19 which plays adlib instruments briefly before cutting them. (Weird!)
		Also, 1E/1F and 2E/2F display as "??"; and pressing 'A' on a disabled (--) channel
		will change its type to 1F.
		Values past 2F seem to display bits of the UI like the copyright and help, strange!
		These out-of-range channel types will almost certainly hang or crash ST3 or
		produce other strange behavior. Simply put, don't do it. :) */
		c = slurp_getc(fp);
		if (c & 0x80) {
			song->channels[n].flags |= CHN_MUTE;
			// ST3 doesn't even play effects in muted channels -- throw them out?
			c &= ~0x80;
		}
		if (c < 0x08) {
			// L1-L8 (panned to 3 in ST3)
			song->channels[n].panning = 14;
		} else if (c < 0x10) {
			// R1-R8 (panned to C in ST3)
			song->channels[n].panning = 50;
		} else if (c < 0x19) {
			// A1-A9
			song->channels[n].panning = 32;
			adlib |= 1 << n;
		} else {
			// Disabled 0xff/0x7f, or broken
			song->channels[n].panning = 32;
			song->channels[n].flags |= CHN_MUTE;
		}
		song->channels[n].volume = 64;
	}
	for (; n < 64; n++) {
		song->channels[n].panning = 32;
		song->channels[n].volume = 64;
		song->channels[n].flags = CHN_MUTE;
	}

	// Schism Tracker before 2018-11-12 played AdLib instruments louder than ST3. Compensate by lowering the sample mixing volume.
	if (adlib && trkvers >= 0x4000 && trkvers < 0x4D33) {
		song->mixing_volume = song->mixing_volume * 2274 / 4096;
	}

	/* orderlist */
	slurp_read(fp, song->orderlist, nord);
	memset(song->orderlist + nord, ORDER_LAST, MAX_ORDERS - nord);

	/* load the parapointers */
	slurp_read(fp, para_smp, 2 * nsmp);
	slurp_read(fp, para_pat, 2 * npat);

	/* default pannings */
	if (misc & S3M_CHANPAN) {
		for (n = 0; n < 32; n++) {
			c = slurp_getc(fp);
			if ((c & 0x20) && (!(adlib & (1 << n)) || trkvers > 0x1320))
				song->channels[n].panning = ((c & 0xf) << 2) + 2;
		}
	}

	//mphack - fix the pannings
	for (n = 0; n < 64; n++)
		song->channels[n].panning *= 4;

	/* samples */
	for (n = 0, sample = song->samples + 1; n < nsmp; n++, sample++) {
		uint8_t type;

		slurp_seek(fp, (para_smp[n]) << 4, SEEK_SET);

		type = slurp_getc(fp);
		slurp_read(fp, sample->filename, 12);
		sample->filename[12] = 0;

		slurp_read(fp, b, 3); // data pointer for pcm, irrelevant otherwise
		switch (type) {
		case S3I_TYPE_PCM:
			para_sdata[n] = b[1] | (b[2] << 8) | (b[0] << 16);
			slurp_read(fp, &tmplong, 4);
			sample->length = bswapLE32(tmplong);
			slurp_read(fp, &tmplong, 4);
			sample->loop_start = bswapLE32(tmplong);
			slurp_read(fp, &tmplong, 4);
			sample->loop_end = bswapLE32(tmplong);
			sample->volume = slurp_getc(fp) * 4; //mphack
			slurp_getc(fp);      /* unused byte */
			slurp_getc(fp);      /* packing info (never used) */
			c = slurp_getc(fp);  /* flags */
			if (c & 1)
				sample->flags |= CHN_LOOP;
			smp_flags[n] = (SF_LE
				| ((misc & S3M_UNSIGNED) ? SF_PCMU : SF_PCMS)
				| ((c & 4) ? SF_16 : SF_8)
				| ((c & 2) ? SF_SS : SF_M));
			if (sample->length)
				any_samples = 1;
			break;

		default:
			//printf("s3m: mystery-meat sample type %d\n", type);
		case S3I_TYPE_NONE:
			slurp_seek(fp, 12, SEEK_CUR);
			sample->volume = slurp_getc(fp) * 4; //mphack
			slurp_seek(fp, 3, SEEK_CUR);
			break;

		case S3I_TYPE_ADMEL:
			slurp_read(fp, sample->adlib_bytes, 12);
			sample->volume = slurp_getc(fp) * 4; //mphack
			// next byte is "dsk", what is that?
			slurp_seek(fp, 3, SEEK_CUR);
			sample->flags |= CHN_ADLIB;
			// dumb hackaround that ought to some day be fixed:
			sample->length = 1;
			sample->data = csf_allocate_sample(1);
			break;
		}

		slurp_read(fp, &tmplong, 4);
		sample->c5speed = bswapLE32(tmplong);
		if (type == S3I_TYPE_ADMEL) {
			if (sample->c5speed < 1000 || sample->c5speed > 0xFFFF) {
				sample->c5speed = 8363;
			}
		}
		slurp_seek(fp, 4, SEEK_CUR);        /* unused space */
		int16_t gus_address;
		slurp_read(fp, &gus_address, 2);
		gus_addresses |= gus_address;
		slurp_seek(fp, 6, SEEK_CUR);
		slurp_read(fp, sample->name, 25);
		sample->name[25] = 0;
		sample->vib_type = 0;
		sample->vib_rate = 0;
		sample->vib_depth = 0;
		sample->vib_speed = 0;
		sample->global_volume = 64;
	}

	/* sample data */
	if (!(lflags & LOAD_NOSAMPLES)) {
		for (n = 0, sample = song->samples + 1; n < nsmp; n++, sample++) {
			if (!sample->length || (sample->flags & CHN_ADLIB))
				continue;
			slurp_seek(fp, para_sdata[n] << 4, SEEK_SET);
			csf_read_sample(sample, smp_flags[n], fp->data + fp->pos, fp->length - fp->pos);
		}
	}

	// Mixing volume is not used with the GUS driver; relevant for PCM + OPL tracks
	if (gus_addresses > 1)
		song->mixing_volume = 48;

	if (!(lflags & LOAD_NOPATTERNS)) {
		for (n = 0; n < npat; n++) {
			int row = 0;
			long end;

			para_pat[n] = bswapLE16(para_pat[n]);
			if (!para_pat[n])
				continue;

			slurp_seek(fp, para_pat[n] << 4, SEEK_SET);
			slurp_read(fp, &tmp, 2);
			end = (para_pat[n] << 4) + bswapLE16(tmp) + 2;

			song->patterns[n] = csf_allocate_pattern(64);

			while (row < 64 && slurp_tell(fp) < end) {
				int mask = slurp_getc(fp);
				uint8_t chn = (mask & 31);

				if (mask == EOF) {
					log_appendf(4, " Warning: Pattern %d: file truncated", n);
					break;
				}
				if (!mask) {
					/* done with the row */
					row++;
					continue;
				}
				note = song->patterns[n] + 64 * row + chn;
				if (mask & 32) {
					/* note/instrument */
					note->note = slurp_getc(fp);
					note->instrument = slurp_getc(fp);
					//if (note->instrument > 99)
					//      note->instrument = 0;
					switch (note->note) {
					default:
						// Note; hi=oct, lo=note
						note->note = (note->note >> 4) * 12 + (note->note & 0xf) + 13;
						break;
					case 255:
						note->note = NOTE_NONE;
						break;
					case 254:
						note->note = (adlib & (1 << chn)) ? NOTE_OFF : NOTE_CUT;
						break;
					}
				}
				if (mask & 64) {
					/* volume */
					note->voleffect = VOLFX_VOLUME;
					note->volparam = slurp_getc(fp);
					if (note->volparam == 255) {
						note->voleffect = VOLFX_NONE;
						note->volparam = 0;
					} else if (note->volparam >= 128 && note->volparam <= 192) {
						// ModPlug (or was there any earlier tracker using this command?)
						note->voleffect = VOLFX_PANNING;
						note->volparam -= 128;
					} else if (note->volparam > 64) {
						// some weirdly saved s3m?
						note->volparam = 64;
					}
				}
				if (mask & 128) {
					note->effect = slurp_getc(fp);
					note->param = slurp_getc(fp);
					csf_import_s3m_effect(note, 0);
					if (note->effect == FX_SPECIAL) {
						// mimic ST3's SD0/SC0 behavior
						if (note->param == 0xd0) {
							note->note = NOTE_NONE;
							note->instrument = 0;
							note->voleffect = VOLFX_NONE;
							note->volparam = 0;
							note->effect = FX_NONE;
							note->param = 0;
						} else if (note->param == 0xc0) {
							note->effect = FX_NONE;
							note->param = 0;
						}
					}
				}
				/* ... next note, same row */
			}
		}
	}

	/* MPT identifies as ST3.20 in the trkvers field, but it puts zeroes for the 'special' field, only ever
	 * sets flags 0x10 and 0x40, writes multiples of 16 orders, always saves channel pannings, and writes
	 * zero into the ultraclick removal field. (ST3.2x always puts either 16, 24, or 32 there, older versions put 0).
	 * Velvet Studio also pretends to be ST3, but writes zeroes for 'special'. ultraclick, and flags, and
	 * does NOT save channel pannings. Also, it writes a fairly recognizable LRRL pattern for the channels,
	 * but I'm not checking that. (yet?) */
	if (trkvers == 0x1320) {
		if (special == 0 && uc == 0 && (flags & ~0x50) == 0
		    && misc == (S3M_UNSIGNED | S3M_CHANPAN) && (nord % 16) == 0) {
			tid = "Modplug Tracker";
		} else if (special == 0 && uc == 0 && flags == 0 && misc == (S3M_UNSIGNED)) {
			tid = "Velvet Studio";
		} else if (uc != 16 && uc != 24 && uc != 32) {
			// sure isn't scream tracker
			tid = "Unknown tracker";
		}
	}
	if (!tid) {
		switch (trkvers >> 12) {
		case 1:
			if (gus_addresses > 1)
				tid = "Scream Tracker %d.%02x (GUS)";
			else if (gus_addresses == 1 || !any_samples || trkvers == 0x1300)
				tid = "Scream Tracker %d.%02x (SB)"; // could also be a GUS file with a single sample
			else
				tid = "Unknown tracker";
			break;
		case 2:
			tid = "Imago Orpheus %d.%02x";
			break;
		case 3:
			if (trkvers <= 0x3214) {
				tid = "Impulse Tracker %d.%02x";
			} else {
				tid = NULL;
				sprintf(song->tracker_id, "Impulse Tracker 2.14p%d", trkvers - 0x3214);
			}
			break;
		case 4:
			tid = NULL;
			strcpy(song->tracker_id, "Schism Tracker ");
//			ver_decode_cwtv(trkvers, reserved, song->tracker_id + strlen(song->tracker_id));
			break;
		case 5:
			if (trkvers >= 0x5129 && reserved)
				sprintf(song->tracker_id, "OpenMPT %d.%02x.%02x.%02x", (trkvers & 0xf00) >> 8, trkvers & 0xff, (reserved >> 8) & 0xff, reserved & 0xff);
			else
				tid = "OpenMPT %d.%02x";
			break;
		}
	}
	if (tid)
		sprintf(song->tracker_id, tid, (trkvers & 0xf00) >> 8, trkvers & 0xff);

//      if (ferror(fp)) {
//              return LOAD_FILE_ERROR;
//      }
	/* done! */
	return LOAD_SUCCESS;
}

/* --------------------------------------------------------------------------------------------------------- */

/* IT displays some of these slightly differently
most notably "Only 100 patterns supported" which doesn't follow the general pattern,
and the channel limits (IT entirely refuses to save data in channels > 16 at all).
Also, the Adlib and sample count warnings of course do not exist in IT at all. */

enum {
	WARN_MAXPATTERNS,
	WARN_CHANNELVOL,
	WARN_LINEARSLIDES,
	WARN_SAMPLEVOL,
	WARN_LOOPS,
	WARN_SAMPLEVIB,
	WARN_INSTRUMENTS,
	WARN_PATTERNLEN,
	WARN_MAXCHANNELS,
	WARN_MAXPCM,
	WARN_MAXADLIB,
	WARN_PCMADLIBMIX,
	WARN_MUTED,
	WARN_NOTERANGE,
	WARN_VOLEFFECTS,
	WARN_MAXSAMPLES,

	MAX_WARN
};

#pragma pack(push, 1)
struct s3m_header {
	char title[28];
	char eof; // 0x1a
	char type; // 16
	uint8_t x[2]; // junk
	uint16_t ordnum, smpnum, patnum; // ordnum should be even
	uint16_t flags, cwtv, ffi; // 0, 0x4nnn, 2 for unsigned
	char scrm[4]; // "SCRM"
	uint8_t gv, is, it, mv, uc, dp; // gv is half range of IT, uc should be 8/12/16, dp is 252
	uint16_t reserved; // extended version information is stored here
	uint32_t reserved2; // Impulse Tracker hides its edit timer here
	uint8_t junk[4]; // last 2 bytes are "special", which means "more junk"
};

struct s3i_header {
	uint8_t type;
	char filename[12];
	union {
		struct {
			uint8_t memseg[3];
			uint32_t length;
			uint32_t loop_start;
			uint32_t loop_end;
		} pcm;
		struct {
			uint8_t zero[3];
			uint8_t data[12];
		} admel;
	};
	uint8_t vol;
	uint8_t x; // "dsk" for adlib
	uint8_t pack; // 0
	uint8_t flags; // 1=loop 2=stereo 4=16-bit / zero for adlib
	uint32_t c5speed;
	uint8_t junk[12];
	char name[28];
	char tag[4]; // SCRS/SCRI/whatever
};
#pragma pack(pop)
