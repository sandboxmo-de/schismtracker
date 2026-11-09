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

/* schismplay: plays a module through the audio backends, or renders it to a
 * WAV file through the same disk writer schismtracker's --diskwrite uses.
 * It has no UI; it registers its own engine callbacks below. */

#include "headers.h"

#include "it.h"
#include "song.h"
#include "config.h"
#include "config-parser.h"
#include "disko.h"
#include "dmoz.h"
#include "fmt.h"
#include "timer.h"
#include "mt.h"
#include "cpu.h"
#include "atomic.h"
#include "ieee-float.h"
#include "util.h"
#include "version.h"
#include "osdefs.h"
#include "engine-host.h"
#include "engine-alloc.h"
#include "mem-arena.h"
#include "sfx-bus.h"
#include "sfx-voices.h"
#include "mem.h"

#include <signal.h>
#include <math.h>

static int verbose = 0;

static const char *opt_output = NULL;
static const char *opt_driver = NULL;
static const char *opt_device = NULL;
static int opt_rate = 44100;
static int opt_bits = 16;
static int opt_channels = 2;
static int opt_interp = SRCMODE_LINEAR;
static double opt_length = 0; /* seconds, 0 = to the end */
static int opt_seeded = 0;
static uint32_t opt_seed = 0;
static int opt_loop = 0;
static int opt_quiet = 0;
static const char *opt_driver_spec = NULL;
static const char *opt_arena = NULL;
static int opt_mem_stats = 0;
static size_t opt_mem_limit = 0;

/* sound effects to play at given times (--sfx, --sfx-sample) */
struct sfx_cue {
	const char *file; /* NULL for a song sample */
	int sample;
	double at;
	float volume;
	float pan;
	uint32_t rate;
	double length; /* loop for this long; 0 plays once */
	sfx_source_t *src;
};
static struct sfx_cue *sfx_cues = NULL;
static int sfx_ncues = 0;
static int opt_sfx_only = 0;
static int opt_sfx_voices = 64;
static float opt_sfx_volume = 1.0f;
static double opt_sfx_stress = 0;

/* the effects voice pool (--sfx-bank, --sfx-note) */
struct sfx_note_cue {
	int bank;
	int index;
	int is_sample; /* index is a sample even if the bank has instruments */
	int note;
	double at;
	int volume, pan;
	double duration; /* note-off after this long; 0 for none */
	int index_in_args;
};
static const char **sfx_bank_files = NULL;
static int sfx_nbank_files = 0;
static int opt_sfx_bank_song = 0;
static sfx_bank_t **sfx_banks = NULL;
static int sfx_nbanks = 0;
static struct sfx_note_cue *sfx_notes = NULL;
static int sfx_nnotes = 0;
static int opt_sfx_pool = 0; /* voices; 0 = only if banks are given */
static int opt_sfx_steal = SFX_STEAL_NONE;
static int opt_sfx_tick_rate = 125;
static float opt_sfx_pool_volume = 1.0f;
static int opt_sfx_pool_interp = -1;
static double opt_sfx_pool_stress = 0;

static int opening_audio = 0;
static int audio_opened = 0;
static volatile sig_atomic_t interrupted = 0;

/* ------------------------------------------------------------------------ */

static void shutdown_all(void)
{
	song_lock_audio();
	song_stop_unlocked(1);
	song_unlock_audio();

	/* Close the audio device before anything it might call into goes away.
	 * The MIDI engine is never started here, so there's nothing to stop. */
	if (audio_opened)
		audio_quit();
	free_audio_device_list();

#ifdef USE_FLAC
	flac_quit();
#endif
#ifdef USE_AVFORMAT
	avformat_quit();
#endif
#ifdef USE_ZLIB
	gzip_quit();
#endif
#ifdef USE_BZIP2
	bzip2_quit();
#endif
#ifdef USE_LZMA
	xz_quit();
#endif
#ifdef USE_ZSTD
	zstd_quit();
#endif

	/* nothing plays any more, so the song can go */
	csf_free(current_song);
	current_song = NULL;
	engine_free((void *)opt_driver, ENGINE_MEM_ENGINE);
	engine_free((void *)opt_device, ENGINE_MEM_ENGINE);
	opt_driver = opt_device = NULL;

	dmoz_quit();
	localtime_r_quit();
	timer_quit();
	atm_quit();
	mt_quit();

	os_sysexit();
}

/* ------------------------------------------------------------------------ */
/* engine callbacks */

/* Log lines and status messages go to stderr with -v. log_perror() prints
 * errors itself, so those show up either way. */
static void host_log(SCHISM_UNUSED int color, SCHISM_UNUSED int flags, const char *text)
{
	if (verbose)
		fprintf(stderr, "%s\n", text);
}

static void host_status_text(const char *text)
{
	if (verbose)
		fprintf(stderr, "status: %s\n", text);
}

/* audio_init() calls this when no driver works at all */
static void host_exit(int x)
{
	if (opening_audio) {
		fprintf(stderr, "schismplay: couldn't open an audio device\n");
		x = 1;
	}

	shutdown_all();
	exit(x);
}

static void host_assert_fail(const char *msg, const char *exp, const char *file, int line)
{
	fprintf(stderr, "schismplay: assertion failed at %s:%d: %s (%s)\n", file, line, msg, exp);
	abort();
}

/* schismtracker's export dialog takes a number from rand() here, for the
 * colour of its progress bar. Do the same, so that without --seed the
 * mixer's share of the rand() sequence, and so the output, is the same as
 * schismtracker's for a given time(). */
static void host_diskwrite_begin(SCHISM_UNUSED size_t est_frames, SCHISM_UNUSED uint32_t rate)
{
	(void)rand();
}

/* how much of a render has been mixed, for starting pool notes on time */
static size_t render_frames = 0;

static void host_diskwrite_progress(size_t frames, SCHISM_UNUSED size_t est_frames, SCHISM_UNUSED uint32_t rate)
{
	render_frames = frames;
}

static const struct engine_host host = {
	.log = host_log,
	.status_text = host_status_text,
	.diskwrite_begin = host_diskwrite_begin,
	.diskwrite_progress = host_diskwrite_progress,
	.exit = host_exit,
	.assert_fail = host_assert_fail,
};

static void on_signal(SCHISM_UNUSED int sig)
{
	interrupted = 1;
}

/* ------------------------------------------------------------------------ */

static void usage(FILE *f, const char *argv0)
{
	fprintf(f,
		"Usage: %s [OPTIONS] FILE\n"
		"       %s [OPTIONS] --sfx-only [--sfx=...] [FILE]\n"
		"Play a module, or render it to a WAV file with -o.\n"
		"\n"
		"  -o, --output=FILE         render to FILE instead of playing\n"
		"  -r, --rate=HZ             sample rate (default 44100)\n"
		"  -b, --bits=8|16|24|32     bits per sample (default 16)\n"
		"  -c, --channels=1|2        output channels (default 2)\n"
		"  -i, --interpolation=MODE  nearest, linear (default), cubic or polyphase\n"
		"  -l, --length=SECONDS      stop after this much audio\n"
		"  -s, --seed=NUMBER         fixed seed for the mixer's random numbers\n"
		"  -d, --driver=DRIVER[:DEVICE]\n"
		"                            audio driver for playback\n"
		"      --loop                keep playing at the end of the song\n"
		"      --arena=SIZE[,SONG,SCRATCH,ENGINE]\n"
		"                            run the engine on the arena allocator over\n"
		"                            one block of SIZE bytes (K, M or G suffix);\n"
		"                            regions left out or 0 share the rest 4:3:1\n"
		"      --mem-stats           print peak memory per lifetime class after\n"
		"                            loading and after playing or rendering\n"
		"      --mem-limit=SIZE      fail engine allocations past SIZE bytes in\n"
		"                            use (libc allocator; for testing)\n"
		"      --sfx=FILE@SECONDS[:VOL[:PAN[:RATE[:LENGTH]]]]\n"
		"                            play a sound effect at that song time\n"
		"                            (repeatable; see docs/sfx-bus.md)\n"
		"      --sfx-sample=N@SECONDS[:VOL[:PAN[:RATE[:LENGTH]]]]\n"
		"                            the same with sample N of the song\n"
		"      --sfx-only            play or render the effects without the song\n"
		"      --sfx-voices=N        size of the effects voice pool (default 64)\n"
		"      --sfx-volume=V        effects master volume (default 1.0)\n"
		"      --sfx-stress=SECONDS  start and stop effects at random while\n"
		"                            playing, as a threading test\n"
		"      --sfx-bank=FILE       load a module, instrument or sample file as\n"
		"                            an effects bank (repeatable, numbered from 0)\n"
		"      --sfx-bank-song       make the song bank 0 (files then start at 1)\n"
		"      --sfx-note=B:I:NOTE@SECONDS[:VOL[:PAN[:DURATION]]]\n"
		"                            play instrument I (sN for sample N) of bank B\n"
		"                            in the effects voice pool; NOTE is C-5, C#5\n"
		"                            or 1-120, VOL and PAN 0-64, and DURATION\n"
		"                            seconds until the note-off (see\n"
		"                            docs/sfx-voices.md)\n"
		"      --sfx-pool=N          voices in the effects voice pool (default 64)\n"
		"      --sfx-steal=none|oldest|quietest\n"
		"                            what a note does when the pool is full\n"
		"      --sfx-tick-rate=TEMPO the pool's tick rate as an IT tempo (125)\n"
		"      --sfx-pool-volume=V   the pool's group volume (default 1.0)\n"
		"      --sfx-pool-interp=MODE  the pool's interpolation (default: -i)\n"
		"      --sfx-pool-stress=SECONDS  random note-on/off/cut/setters on the\n"
		"                            pool while playing, as a threading test\n"
		"  -q, --quiet               don't show the play position\n"
		"  -v, --verbose             show loader and engine messages\n"
		"      --version\n"
		"  -h, --help\n",
		argv0, argv0);
}

static int parse_int(const char *s, const char *what, long lo, long hi)
{
	char *end;
	long v = strtol(s, &end, 0);

	if (!*s || *end || v < lo || v > hi) {
		fprintf(stderr, "schismplay: bad %s '%s'\n", what, s);
		exit(2);
	}

	return (int)v;
}

static int parse_interp(const char *s)
{
	static const char *names[] = {"nearest", "linear", "cubic", "polyphase"};
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(names); i++)
		if (!strcasecmp(s, names[i]))
			return i;

	return parse_int(s, "interpolation mode", 0, NUM_SRC_MODES - 1);
}

enum {
	O_LOOP = 256,
	O_VERSION,
	O_ARENA,
	O_MEM_STATS,
	O_MEM_LIMIT,
	O_SFX,
	O_SFX_SAMPLE,
	O_SFX_ONLY,
	O_SFX_VOICES,
	O_SFX_VOLUME,
	O_SFX_STRESS,
	O_SFX_BANK,
	O_SFX_BANK_SONG,
	O_SFX_NOTE,
	O_SFX_POOL,
	O_SFX_STEAL,
	O_SFX_TICK_RATE,
	O_SFX_POOL_VOLUME,
	O_SFX_POOL_INTERP,
	O_SFX_POOL_STRESS,
};

/* "123", "64K", "16M", "1G" */
static int parse_size(const char *s, size_t *out)
{
	char *end;
	unsigned long long v;

	if (!*s)
		return 0;
	v = strtoull(s, &end, 0);
	switch (*end) {
	case 'k': case 'K': v <<= 10; end++; break;
	case 'm': case 'M': v <<= 20; end++; break;
	case 'g': case 'G': v <<= 30; end++; break;
	default: break;
	}
	if (*end || v > SIZE_MAX)
		return 0;

	*out = (size_t)v;
	return 1;
}

static double parse_double(const char *s, const char *what)
{
	char *end;
	double v = strtod(s, &end);

	if (!*s || *end) {
		fprintf(stderr, "schismplay: bad %s '%s'\n", what, s);
		exit(2);
	}

	return v;
}

/* FILE@SECONDS[:VOL[:PAN[:RATE[:LENGTH]]]], or N@... for a song sample */
static void add_sfx_cue(char *arg, int is_sample)
{
	static const char *what[] = {"effect time", "effect volume", "effect pan",
		"effect rate", "effect length"};
	struct sfx_cue *c;
	char *at = strrchr(arg, '@'), *field, *next;
	int i;

	if (!at || at == arg) {
		fprintf(stderr, "schismplay: effect '%s' needs @SECONDS\n", arg);
		exit(2);
	}
	*at = '\0';

	sfx_cues = mem_realloc(sfx_cues, (sfx_ncues + 1) * sizeof(*sfx_cues));
	c = sfx_cues + sfx_ncues++;
	memset(c, 0, sizeof(*c));
	c->volume = 1.0f;

	if (is_sample)
		c->sample = parse_int(arg, "sample number", 1, MAX_SAMPLES - 1);
	else
		c->file = arg;

	for (i = 0, field = at + 1; field && i < 5; i++, field = next) {
		double v;

		next = strchr(field, ':');
		if (next)
			*next++ = '\0';
		v = parse_double(field, what[i]);
		switch (i) {
		case 0: c->at = v; break;
		case 1: c->volume = (float)v; break;
		case 2: c->pan = (float)v; break;
		case 3: c->rate = (uint32_t)MAX(v, 0); break;
		case 4: c->length = v; break;
		}
	}
	if (field || c->at < 0 || c->length < 0) {
		fprintf(stderr, "schismplay: bad effect time or too many fields\n");
		exit(2);
	}
}

/* "C-5", "C#5", "c5" or a number 1-120 */
static int parse_note(const char *s)
{
	static const char names[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
	char nm[3];
	int i;

	if (s[0] >= '0' && s[0] <= '9')
		return parse_int(s, "note", NOTE_FIRST, NOTE_LAST);

	nm[0] = (char)toupper((unsigned char)s[0]);
	nm[1] = (s[1] == '#' || s[1] == '-') ? s[1] : '-';
	nm[2] = '\0';
	for (i = 0; i < 12; i++) {
		if (names[i * 2] == nm[0] && names[i * 2 + 1] == nm[1]) {
			const char *oct = (s[1] == '#' || s[1] == '-') ? s + 2 : s + 1;
			int o = parse_int(oct, "note octave", 0, 9);
			return 12 * o + i + NOTE_FIRST;
		}
	}
	fprintf(stderr, "schismplay: bad note '%s'\n", s);
	exit(2);
}

/* B:I:NOTE@SECONDS[:VOL[:PAN[:DURATION]]] */
static void add_sfx_note(char *arg)
{
	struct sfx_note_cue *c;
	char *at = strrchr(arg, '@'), *f[3], *field, *next;
	int i;

	if (!at) {
		fprintf(stderr, "schismplay: note '%s' needs @SECONDS\n", arg);
		exit(2);
	}
	*at = '\0';

	f[0] = arg;
	for (i = 1; i < 3; i++) {
		f[i] = strchr(f[i - 1], ':');
		if (!f[i]) {
			fprintf(stderr, "schismplay: note needs BANK:INSTRUMENT:NOTE\n");
			exit(2);
		}
		*f[i]++ = '\0';
	}

	sfx_notes = mem_realloc(sfx_notes, (sfx_nnotes + 1) * sizeof(*sfx_notes));
	c = sfx_notes + sfx_nnotes++;
	memset(c, 0, sizeof(*c));
	c->volume = c->pan = SFX_DEFAULT;
	c->bank = parse_int(f[0], "bank number", 0, 9999);
	if (f[1][0] == 's' || f[1][0] == 'S') {
		c->is_sample = 1;
		c->index = parse_int(f[1] + 1, "sample number", 1, MAX_SAMPLES - 1);
	} else {
		c->index = parse_int(f[1], "instrument number", 1, MAX_INSTRUMENTS - 1);
	}
	c->note = parse_note(f[2]);

	for (i = 0, field = at + 1; field && i < 4; i++, field = next) {
		next = strchr(field, ':');
		if (next)
			*next++ = '\0';
		/* an empty field keeps the default */
		if (i > 0 && !*field)
			continue;
		switch (i) {
		case 0: c->at = parse_double(field, "note time"); break;
		case 1: c->volume = parse_int(field, "note volume", 0, 64); break;
		case 2: c->pan = parse_int(field, "note pan", 0, 64); break;
		case 3: c->duration = parse_double(field, "note duration"); break;
		}
	}
	if (field || c->at < 0 || c->duration < 0) {
		fprintf(stderr, "schismplay: bad note time or too many fields\n");
		exit(2);
	}
}

static const char *parse_options(int argc, char **argv)
{
	static const struct option long_options[] = {
		{"output", 1, NULL, 'o'},
		{"rate", 1, NULL, 'r'},
		{"bits", 1, NULL, 'b'},
		{"channels", 1, NULL, 'c'},
		{"interpolation", 1, NULL, 'i'},
		{"length", 1, NULL, 'l'},
		{"seed", 1, NULL, 's'},
		{"driver", 1, NULL, 'd'},
		{"loop", 0, NULL, O_LOOP},
		{"arena", 1, NULL, O_ARENA},
		{"mem-stats", 0, NULL, O_MEM_STATS},
		{"mem-limit", 1, NULL, O_MEM_LIMIT},
		{"sfx", 1, NULL, O_SFX},
		{"sfx-sample", 1, NULL, O_SFX_SAMPLE},
		{"sfx-only", 0, NULL, O_SFX_ONLY},
		{"sfx-voices", 1, NULL, O_SFX_VOICES},
		{"sfx-volume", 1, NULL, O_SFX_VOLUME},
		{"sfx-stress", 1, NULL, O_SFX_STRESS},
		{"sfx-bank", 1, NULL, O_SFX_BANK},
		{"sfx-bank-song", 0, NULL, O_SFX_BANK_SONG},
		{"sfx-note", 1, NULL, O_SFX_NOTE},
		{"sfx-pool", 1, NULL, O_SFX_POOL},
		{"sfx-steal", 1, NULL, O_SFX_STEAL},
		{"sfx-tick-rate", 1, NULL, O_SFX_TICK_RATE},
		{"sfx-pool-volume", 1, NULL, O_SFX_POOL_VOLUME},
		{"sfx-pool-interp", 1, NULL, O_SFX_POOL_INTERP},
		{"sfx-pool-stress", 1, NULL, O_SFX_POOL_STRESS},
		{"quiet", 0, NULL, 'q'},
		{"verbose", 0, NULL, 'v'},
		{"version", 0, NULL, O_VERSION},
		{"help", 0, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	char *end;
	int opt;

	while ((opt = getopt_long(argc, argv, "o:r:b:c:i:l:s:d:qvh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'o':
			opt_output = optarg;
			break;
		case 'r':
			opt_rate = parse_int(optarg, "sample rate", 4000, 192000);
			break;
		case 'b':
			opt_bits = parse_int(optarg, "bit depth", 8, 32);
			if (opt_bits != 8 && opt_bits != 16 && opt_bits != 24 && opt_bits != 32) {
				fprintf(stderr, "schismplay: bits must be 8, 16, 24 or 32\n");
				exit(2);
			}
			break;
		case 'c':
			opt_channels = parse_int(optarg, "channel count", 1, 2);
			break;
		case 'i':
			opt_interp = parse_interp(optarg);
			break;
		case 'l':
			opt_length = strtod(optarg, &end);
			if (!*optarg || *end || opt_length <= 0) {
				fprintf(stderr, "schismplay: bad length '%s'\n", optarg);
				exit(2);
			}
			break;
		case 's':
			opt_seed = strtoul(optarg, &end, 0);
			if (!*optarg || *end) {
				fprintf(stderr, "schismplay: bad seed '%s'\n", optarg);
				exit(2);
			}
			opt_seeded = 1;
			break;
		case 'd':
			/* parsed once the allocator is set up */
			opt_driver_spec = optarg;
			break;
		case O_LOOP:
			opt_loop = 1;
			break;
		case O_ARENA:
			opt_arena = optarg;
			break;
		case O_MEM_STATS:
			opt_mem_stats = 1;
			break;
		case O_MEM_LIMIT:
			if (!parse_size(optarg, &opt_mem_limit) || !opt_mem_limit) {
				fprintf(stderr, "schismplay: bad size '%s'\n", optarg);
				exit(2);
			}
			break;
		case O_SFX:
		case O_SFX_SAMPLE:
			add_sfx_cue(optarg, opt == O_SFX_SAMPLE);
			break;
		case O_SFX_ONLY:
			opt_sfx_only = 1;
			break;
		case O_SFX_VOICES:
			opt_sfx_voices = parse_int(optarg, "voice count", 1, SFX_MAX_VOICES);
			break;
		case O_SFX_VOLUME:
			opt_sfx_volume = (float)parse_double(optarg, "effects volume");
			break;
		case O_SFX_STRESS:
			opt_sfx_stress = parse_double(optarg, "stress time");
			break;
		case O_SFX_BANK:
			sfx_bank_files = mem_realloc(sfx_bank_files, (sfx_nbank_files + 1) * sizeof(*sfx_bank_files));
			sfx_bank_files[sfx_nbank_files++] = optarg;
			break;
		case O_SFX_BANK_SONG:
			opt_sfx_bank_song = 1;
			break;
		case O_SFX_NOTE:
			add_sfx_note(optarg);
			break;
		case O_SFX_POOL:
			opt_sfx_pool = parse_int(optarg, "pool size", 1, SFX_POOL_MAX_VOICES);
			break;
		case O_SFX_STEAL:
			if (!strcasecmp(optarg, "none")) {
				opt_sfx_steal = SFX_STEAL_NONE;
			} else if (!strcasecmp(optarg, "oldest")) {
				opt_sfx_steal = SFX_STEAL_OLDEST;
			} else if (!strcasecmp(optarg, "quietest")) {
				opt_sfx_steal = SFX_STEAL_QUIETEST;
			} else {
				fprintf(stderr, "schismplay: bad steal policy '%s'\n", optarg);
				exit(2);
			}
			break;
		case O_SFX_TICK_RATE:
			opt_sfx_tick_rate = parse_int(optarg, "tick rate", 1, 10000);
			break;
		case O_SFX_POOL_VOLUME:
			opt_sfx_pool_volume = (float)parse_double(optarg, "pool volume");
			break;
		case O_SFX_POOL_INTERP:
			opt_sfx_pool_interp = parse_interp(optarg);
			break;
		case O_SFX_POOL_STRESS:
			opt_sfx_pool_stress = parse_double(optarg, "stress time");
			break;
		case 'q':
			opt_quiet = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case O_VERSION:
			puts(schism_banner(0));
			exit(0);
		case 'h':
			usage(stdout, argv[0]);
			exit(0);
		default:
			usage(stderr, argv[0]);
			exit(2);
		}
	}

	/* the pool stress test needs something to play */
	if (opt_sfx_pool_stress > 0 && !sfx_nbank_files)
		opt_sfx_bank_song = 1;

	if (opt_sfx_only && optind == argc)
		return NULL;

	if (optind != argc - 1) {
		usage(stderr, argv[0]);
		exit(2);
	}
	if (opt_arena && opt_mem_limit) {
		fprintf(stderr, "schismplay: --mem-limit only works with the libc allocator\n");
		exit(2);
	}

	return argv[optind];
}

/* ------------------------------------------------------------------------ */
/* memory
 *
 * --arena runs the engine on the reference arena allocator, over one block
 * that's the only memory schismplay itself takes from malloc() for it.
 * Otherwise, with --mem-stats or --mem-limit, the libc allocator is wrapped
 * to count what's in use. */

static struct mem_arena arena;
static void *arena_block = NULL;

/* in front of each counted block; ENGINE_ALIGN_DEFAULT keeps the data after
 * it aligned */
struct counted_hdr {
	size_t size;
	size_t cls;
};
#define COUNTED_HDR ENGINE_ALIGN_DEFAULT

static struct {
	size_t used[ENGINE_MEM_CLASSES], peak[ENGINE_MEM_CLASSES], blocks[ENGINE_MEM_CLASSES];
	size_t total, fails;
} counted;

static void counted_add(enum engine_mem_class cls, size_t size)
{
	counted.used[cls] += size;
	counted.total += size;
	if (counted.used[cls] > counted.peak[cls])
		counted.peak[cls] = counted.used[cls];
}

static int counted_over_limit(size_t more)
{
	if (opt_mem_limit && (more > opt_mem_limit || counted.total > opt_mem_limit - more)) {
		counted.fails++;
		return 1;
	}
	return 0;
}

static void *counted_alloc(SCHISM_UNUSED void *ctx, size_t size, size_t align, enum engine_mem_class cls)
{
	unsigned char *p;
	struct counted_hdr *h;

	if (size > SIZE_MAX - COUNTED_HDR || counted_over_limit(size))
		return NULL;

	p = engine_allocator_libc.alloc(NULL, size + COUNTED_HDR, align, cls);
	if (!p)
		return NULL;

	h = (struct counted_hdr *)p;
	h->size = size;
	h->cls = cls;
	counted_add(cls, size);
	counted.blocks[cls]++;

	return p + COUNTED_HDR;
}

static void *counted_realloc(SCHISM_UNUSED void *ctx, void *ptr, size_t size, size_t align, enum engine_mem_class cls)
{
	unsigned char *p = (unsigned char *)ptr - COUNTED_HDR;
	struct counted_hdr *h = (struct counted_hdr *)p;
	size_t old = h->size;

	if (size > SIZE_MAX - COUNTED_HDR || (size > old && counted_over_limit(size - old)))
		return NULL;

	p = engine_allocator_libc.realloc(NULL, p, size + COUNTED_HDR, align, cls);
	if (!p)
		return NULL;

	h = (struct counted_hdr *)p;
	h->size = size;
	counted.used[cls] -= old;
	counted.total -= old;
	counted_add(cls, size);

	return p + COUNTED_HDR;
}

static void counted_free(SCHISM_UNUSED void *ctx, void *ptr, enum engine_mem_class cls)
{
	unsigned char *p = (unsigned char *)ptr - COUNTED_HDR;
	struct counted_hdr *h = (struct counted_hdr *)p;

	if (h->cls != (size_t)cls)
		fprintf(stderr, "schismplay: block of class %d freed as class %d\n", (int)h->cls, (int)cls);

	counted.used[h->cls] -= h->size;
	counted.total -= h->size;
	counted.blocks[h->cls]--;
	engine_allocator_libc.free(NULL, p, cls);
}

/* SIZE[,SONG,SCRATCH,ENGINE] */
static void setup_arena(void)
{
	size_t sizes[4] = {0};
	struct engine_allocator a;
	const char *p = opt_arena;
	int n;

	for (n = 0; n < 4; n++) {
		char tok[32];
		size_t len = strcspn(p, ",");

		if (len >= sizeof(tok))
			goto bad;
		memcpy(tok, p, len);
		tok[len] = '\0';
		/* an empty field is 0, a share of what's left */
		if (len && !parse_size(tok, &sizes[n]))
			goto bad;
		p += len;
		if (!*p)
			break;
		p++;
	}
	if (n == 4)
		goto bad;

	arena_block = malloc(sizes[0] ? sizes[0] : 1);
	if (!sizes[0] || !arena_block
		|| mem_arena_init(&arena, arena_block, sizes[0], sizes[1], sizes[2], sizes[3]) < 0) {
		fprintf(stderr, "schismplay: can't set up a %s arena\n", opt_arena);
		exit(2);
	}

	mem_arena_get_allocator(&arena, &a);
	engine_allocator_register(&a);
	return;

bad:
	fprintf(stderr, "schismplay: bad arena size '%s'\n", opt_arena);
	exit(2);
}

static void setup_allocator(void)
{
	struct engine_allocator a;

	if (opt_arena) {
		setup_arena();
	} else if (opt_mem_stats || opt_mem_limit) {
		memset(&a, 0, sizeof(a));
		a.alloc = counted_alloc;
		a.realloc = counted_realloc;
		a.free = counted_free;
		engine_allocator_register(&a);
	}
}

/* After shutdown_all(), which frees the song, so the "exit" numbers from
 * --mem-stats show whatever the engine never gave back. */
static void release_memory(void)
{
	if (arena_block) {
		free(arena_block);
		arena_block = NULL;
	}
}

static void print_mem_stats(const char *stage)
{
	static const char *names[ENGINE_MEM_CLASSES] = {"engine", "song", "scratch"};
	int c;

	if (!opt_mem_stats)
		return;

	for (c = 0; c < ENGINE_MEM_CLASSES; c++) {
		if (opt_arena) {
			const struct mem_arena_region *r = &arena.r[c];
			fprintf(stderr, "mem-stats %s %s peak=%" PRIuSZ " used=%" PRIuSZ " blocks=%" PRIuSZ
				" size=%" PRIuSZ " fails=%" PRIuSZ "\n",
				stage, names[c], r->peak, r->top, r->live, r->size, r->fails);
		} else {
			fprintf(stderr, "mem-stats %s %s peak=%" PRIuSZ " used=%" PRIuSZ " blocks=%" PRIuSZ "\n",
				stage, names[c], counted.peak[c], counted.used[c], counted.blocks[c]);
		}
	}
	if (opt_arena)
		fprintf(stderr, "mem-stats %s unfreed song=%" PRIuSZ " scratch=%" PRIuSZ "\n",
			stage, arena.song_leaks, arena.scratch_leaks);
}

/* ------------------------------------------------------------------------ */
/* sound effects */

static int load_sfx_sources(void)
{
	int i;

	for (i = 0; i < sfx_ncues; i++) {
		struct sfx_cue *c = sfx_cues + i;

		c->src = c->file ? sfx_source_load(c->file) : sfx_source_from_song_sample(c->sample);
		if (!c->src) {
			if (c->file)
				fprintf(stderr, "schismplay: couldn't load effect %s\n", c->file);
			else
				fprintf(stderr, "schismplay: sample %d has no data\n", c->sample);
			return 0;
		}
	}

	return 1;
}

static void free_sfx_sources(void)
{
	int i;

	for (i = 0; i < sfx_ncues; i++) {
		sfx_source_free(sfx_cues[i].src);
		sfx_cues[i].src = NULL;
	}
	free(sfx_cues);
	sfx_cues = NULL;
	sfx_ncues = 0;
}

/* Every cue is started now with a delay, so where it lands only depends on
 * how much audio has been mixed since, which in a render is exact. */
static void schedule_sfx(void)
{
	int i;

	for (i = 0; i < sfx_ncues; i++) {
		struct sfx_cue *c = sfx_cues + i;
		struct sfx_play_args args = {
			.volume = c->volume,
			.pan = c->pan,
			.frequency = c->rate,
			.loop = c->length > 0,
			.delay = c->at,
			.duration = c->length,
		};

		if (sfx_play_ex(c->src, &args) == SFX_VOICE_NONE)
			fprintf(stderr, "schismplay: no free voice for effect %d\n", i + 1);
	}
}

/* --sfx-stress: hammer the bus from this thread while the audio thread
 * mixes. Uses its own generator, so a run does the same calls in the same
 * order every time (the timing against the audio thread still varies). */
static uint32_t stress_state = 0x12345678;

static uint32_t stress_rand(void)
{
	stress_state ^= stress_state << 13;
	stress_state ^= stress_state >> 17;
	stress_state ^= stress_state << 5;
	return stress_state;
}

static float stress_unit(void)
{
	return (stress_rand() & 0xffff) / 65535.0f;
}

static sfx_source_t *stress_tone(void)
{
	int16_t buf[4410];
	int i;

	for (i = 0; i < 4410; i++)
		buf[i] = (int16_t)(4000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / 44100.0));

	return sfx_source_from_pcm(buf, 4410, SFX_PCM_S16, 1, 44100);
}

static void stress_step(sfx_voice_t *handles, int nhandles, sfx_source_t **tones, int ntones,
	unsigned long *ops)
{
	sfx_voice_t *h = handles + stress_rand() % nhandles;
	sfx_source_t *src;
	int k = stress_rand() % 100;

	if (k < 40) {
		struct sfx_play_args args = {
			.volume = stress_unit() * 0.2f,
			.pan = stress_unit() * 2.0f - 1.0f,
			.pitch = 0.25f + stress_unit() * 3.0f,
			.loop = (stress_rand() & 3) == 0,
			.delay = (stress_rand() & 7) ? 0.0 : stress_unit() * 0.1,
			.duration = (stress_rand() & 3) ? 0.0 : stress_unit() * 0.5,
		};
		if (sfx_ncues && (stress_rand() & 1))
			src = sfx_cues[stress_rand() % sfx_ncues].src;
		else
			src = tones[stress_rand() % ntones];
		*h = sfx_play_ex(src, &args);
	} else if (k < 65) {
		sfx_stop(*h);
	} else if (k < 75) {
		sfx_set_volume(*h, stress_unit() * 0.2f);
	} else if (k < 83) {
		sfx_set_pan(*h, stress_unit() * 2.0f - 1.0f);
	} else if (k < 90) {
		sfx_set_pitch(*h, 0.25f + stress_unit() * 3.0f);
	} else if (k < 95) {
		sfx_set_frequency(*h, 4000 + stress_rand() % 90000);
	} else if (k < 97) {
		(void)sfx_is_playing(*h);
		(void)sfx_bus_voices_playing();
	} else if (k < 98) {
		sfx_bus_set_volume(0.5f + stress_unit());
	} else if (k < 99) {
		/* free a tone that may be playing and make a new one */
		int t = stress_rand() % ntones;
		sfx_source_free(tones[t]);
		tones[t] = stress_tone();
	} else {
		sfx_stop_all();
	}

	(*ops)++;
}

/* ------------------------------------------------------------------------ */
/* the effects voice pool */

static int pool_wanted(void)
{
	return opt_sfx_pool || sfx_nbank_files || opt_sfx_bank_song || sfx_nnotes || opt_sfx_pool_stress > 0;
}

static int setup_pool(void)
{
	if (!pool_wanted())
		return 1;
	if (sfx_pool_init(opt_sfx_pool ? (uint32_t)opt_sfx_pool : 64) < 0) {
		fprintf(stderr, "schismplay: couldn't set up the effects voice pool\n");
		return 0;
	}
	sfx_pool_set_steal(opt_sfx_steal);
	sfx_pool_set_tick_rate(opt_sfx_tick_rate);
	sfx_pool_set_volume(opt_sfx_pool_volume);
	sfx_pool_set_interpolation(opt_sfx_pool_interp);
	if (opt_seeded)
		sfx_pool_set_seed(opt_seed);
	return 1;
}

/* Bank files are loaded before the song, so that on the arena the space
 * their loads borrow from the SONG region comes back when the song loads.
 * The song bank, if any, goes in slot 0 afterwards. */
static int load_bank_files(void)
{
	int i, base = opt_sfx_bank_song ? 1 : 0;

	sfx_nbanks = base + sfx_nbank_files;
	if (!sfx_nbanks)
		return 1;
	sfx_banks = mem_calloc(sfx_nbanks, sizeof(*sfx_banks));
	for (i = 0; i < sfx_nbank_files; i++) {
		sfx_banks[base + i] = sfx_bank_load(sfx_bank_files[i]);
		if (!sfx_banks[base + i]) {
			fprintf(stderr, "schismplay: couldn't load bank %s\n", sfx_bank_files[i]);
			return 0;
		}
		if (verbose)
			fprintf(stderr, "bank %d: %d instruments, %d samples\n", base + i,
				sfx_bank_num_instruments(sfx_banks[base + i]),
				sfx_bank_num_samples(sfx_banks[base + i]));
	}
	return 1;
}

static int load_song_bank(void)
{
	if (!opt_sfx_bank_song)
		return 1;
	sfx_banks[0] = sfx_bank_from_song();
	if (!sfx_banks[0]) {
		fprintf(stderr, "schismplay: the song has no samples to make a bank from\n");
		return 0;
	}
	return 1;
}

static void free_banks(void)
{
	int i;

	for (i = 0; i < sfx_nbanks; i++)
		sfx_bank_free(sfx_banks[i]);
	free(sfx_banks);
	sfx_banks = NULL;
	sfx_nbanks = 0;
	free(sfx_bank_files);
	sfx_bank_files = NULL;
	free(sfx_notes);
	sfx_notes = NULL;
}

static uint32_t note_frame(const struct sfx_note_cue *c)
{
	return (uint32_t)floor(c->at * opt_rate + 0.5);
}

/* Start the notes due before frame `until` (counted from the start of the
 * render), each with a delay in frames from `now`, so a render puts it on
 * the exact sample. Notes go in time order. Starting them only shortly
 * before they're due keeps notes that are still waiting from filling the
 * pool, so a full pool and its steal policy behave the way they would for
 * a host starting notes as things happen. */
static int next_note = 0;

static int cmp_note_time(const void *a, const void *b)
{
	const struct sfx_note_cue *x = a, *y = b;
	uint32_t fx = note_frame(x), fy = note_frame(y);

	if (fx != fy)
		return fx < fy ? -1 : 1;
	return (x->index_in_args > y->index_in_args) - (x->index_in_args < y->index_in_args);
}

static void schedule_notes_until(size_t now, size_t until)
{
	for (; next_note < sfx_nnotes && note_frame(sfx_notes + next_note) < until; next_note++) {
		struct sfx_note_cue *c = sfx_notes + next_note;
		struct sfx_note_args args;
		sfx_bank_t *bank;
		int i = c->index_in_args;
		uint32_t at = note_frame(c);

		if (c->bank >= sfx_nbanks) {
			fprintf(stderr, "schismplay: note %d: no bank %d\n", i + 1, c->bank);
			continue;
		}
		bank = sfx_banks[c->bank];
		sfx_note_args_init(&args);
		if (c->is_sample || !sfx_bank_num_instruments(bank))
			args.sample = c->index;
		else
			args.instrument = c->index;
		args.note = c->note;
		args.volume = c->volume;
		args.pan = c->pan;
		args.delay = (at > now) ? (uint32_t)(at - now) : 0;
		args.duration = (uint32_t)floor(c->duration * opt_rate + 0.5);
		if (c->duration > 0 && !args.duration)
			args.duration = 1;
		if (sfx_note_on_ex(bank, &args) == SFX_NOTE_NONE)
			fprintf(stderr, "schismplay: note %d didn't start\n", i + 1);
	}
}

static void sort_notes(void)
{
	int i;

	for (i = 0; i < sfx_nnotes; i++)
		sfx_notes[i].index_in_args = i;
	qsort(sfx_notes, sfx_nnotes, sizeof(*sfx_notes), cmp_note_time);
	next_note = 0;
}

/* --sfx-pool-stress: random calls on the pool from this thread while the
 * audio thread mixes, with stress_rand() so the sequence of calls is the
 * same every run. Uses the --sfx-bank banks, or the song's if none. */
static void pool_stress_step(sfx_note_t *handles, int nhandles, unsigned long *counts)
{
	sfx_note_t *h = handles + stress_rand() % nhandles;
	int k = stress_rand() % 100;

	if (k < 35 && sfx_nbanks) {
		sfx_bank_t *bank = sfx_banks[stress_rand() % sfx_nbanks];
		struct sfx_note_args args;
		int nins = sfx_bank_num_instruments(bank), nsmp = sfx_bank_num_samples(bank);

		sfx_note_args_init(&args);
		if (nins && (stress_rand() & 3))
			args.instrument = 1 + stress_rand() % nins;
		else
			args.sample = 1 + stress_rand() % MAX(nsmp, 1);
		args.note = 37 + stress_rand() % 48;
		args.volume = stress_rand() % 40;
		args.pan = stress_rand() % 65;
		if (!(stress_rand() & 3)) {
			args.cutoff = stress_rand() % 128;
			args.resonance = stress_rand() % 128;
		}
		args.delay = (stress_rand() & 7) ? 0 : stress_rand() % 4410;
		args.duration = (stress_rand() & 1) ? 0 : stress_rand() % 44100;
		*h = sfx_note_on_ex(bank, &args);
		counts[0]++;
	} else if (k < 55) {
		sfx_note_off(*h);
		counts[1]++;
	} else if (k < 65) {
		sfx_note_cut(*h);
		counts[2]++;
	} else if (k < 90) {
		switch (stress_rand() % 5) {
		case 0: sfx_note_set_volume(*h, stress_rand() % 65); break;
		case 1: sfx_note_set_pan(*h, stress_rand() % 65); break;
		case 2: sfx_note_set_pitch(*h, 0.25f + stress_unit() * 3.0f); break;
		case 3: sfx_note_set_cutoff(*h, stress_rand() % 128); break;
		case 4: sfx_note_set_resonance(*h, stress_rand() % 128); break;
		}
		counts[3]++;
	} else if (k < 97) {
		struct sfx_note_info info;
		(void)sfx_note_get_info(*h, &info);
		(void)sfx_pool_voices_playing();
		counts[4]++;
	} else if (k < 99) {
		sfx_pool_set_volume(0.5f + stress_unit());
		sfx_pool_set_steal(stress_rand() % 3);
		counts[4]++;
	} else {
		sfx_pool_stop_all();
		counts[2]++;
	}
}

/* ------------------------------------------------------------------------ */

/* The engine reads its settings out of a config file. Build one in memory
 * from the command line rather than reading the user's, so the output only
 * depends on the arguments. */
static void load_settings(void)
{
	cfg_file_t cfg;

	memset(&cfg, 0, sizeof(cfg));

	cfg_set_number(&cfg, "Audio", "sample_rate", opt_rate);
	cfg_set_number(&cfg, "Audio", "bits", opt_bits);
	cfg_set_number(&cfg, "Audio", "channels", opt_channels);
	cfg_set_number(&cfg, "Mixer Settings", "interpolation_mode", opt_interp);
	cfg_set_number(&cfg, "Diskwriter", "rate", opt_rate);
	cfg_set_number(&cfg, "Diskwriter", "bits", opt_bits);
	cfg_set_number(&cfg, "Diskwriter", "channels", opt_channels);

	cfg_load_audio(&cfg);
	cfg_load_midi(&cfg);
	cfg_load_disko(&cfg);

	cfg_free(&cfg);
}

static int render(const char *song)
{
	uint32_t pool_peak = 0;
	int q;

	disko_set_export_limit(opt_length > 0 ? (size_t)(opt_length * opt_rate + 0.5) : 0);

	if (song_export(opt_output, "WAV") != SAVE_SUCCESS) {
		fprintf(stderr, "schismplay: couldn't start writing %s\n", opt_output);
		return 1;
	}

	/* after starting the export, so the effects' delays count from its
	 * first frame */
	schedule_sfx();
	sort_notes();
	render_frames = 0;

	do {
		/* one disk writer buffer ahead is enough: it's never more than
		 * 16384 frames */
		schedule_notes_until(render_frames, render_frames + 16384);
		/* an idle pool would let the render finish before a later note */
		if (next_note < sfx_nnotes && !sfx_pool_voices_playing())
			schedule_notes_until(render_frames, (size_t)note_frame(sfx_notes + next_note) + 1);
		q = disko_sync();
		if (verbose && sfx_pool_running())
			pool_peak = MAX(pool_peak, sfx_pool_voices_playing());
	} while (q == DW_SYNC_MORE && !interrupted);

	if (verbose && sfx_pool_running())
		fprintf(stderr, "effects pool: at most %u notes at once\n", pool_peak);

	if (interrupted) {
		fprintf(stderr, "schismplay: interrupted\n");
		return 1;
	}

	if (q != DW_SYNC_DONE) {
		fprintf(stderr, "schismplay: error while rendering %s\n", song ? song : "effects");
		return 1;
	}

	return 0;
}

static int play(void)
{
	int last_order = -1, last_row = -1;
	unsigned int last_time = (unsigned int)-1;
	int ended = 0;
	timer_ticks_t start = timer_ticks();
	sfx_voice_t stress_handles[256] = {0};
	sfx_source_t *stress_tones[4] = {0};
	unsigned long stress_ops = 0;
	sfx_note_t pool_handles[512] = {0};
	unsigned long pool_counts[5] = {0};
	uint32_t pool_peak = 0;
	int i;

	/* with --sfx-only the song stays stopped and the effects play over
	 * silence */
	if (!opt_sfx_only) {
		if (opt_loop)
			song_start();
		else
			song_start_once();
	}

	schedule_sfx();
	sort_notes();
	schedule_notes_until(0, SIZE_MAX);

	if (opt_sfx_stress > 0)
		for (i = 0; i < (int)ARRAY_SIZE(stress_tones); i++)
			stress_tones[i] = stress_tone();

	for (;;) {
		unsigned int t;
		int order, row;
		double elapsed = (timer_ticks() - start) / 1000.0;

		audio_worker();

		if (opt_sfx_stress > 0) {
			if (elapsed >= opt_sfx_stress)
				break;
			for (i = 0; i < 50; i++)
				stress_step(stress_handles, ARRAY_SIZE(stress_handles),
					stress_tones, ARRAY_SIZE(stress_tones), &stress_ops);
			timer_msleep(1);
			continue;
		}
		if (opt_sfx_pool_stress > 0) {
			uint32_t n;
			if (elapsed >= opt_sfx_pool_stress)
				break;
			for (i = 0; i < 50; i++)
				pool_stress_step(pool_handles, ARRAY_SIZE(pool_handles), pool_counts);
			n = sfx_pool_voices_playing();
			pool_peak = MAX(pool_peak, n);
			timer_msleep(1);
			continue;
		}

		song_lock_audio();
		ended = !!(current_song->flags & SONG_ENDREACHED);
		order = current_song->current_order;
		row = current_song->row;
		song_unlock_audio();

		t = song_get_current_time();

		/* one line a second, or on every new order */
		if (!opt_quiet && (t != last_time || order != last_order)) {
			printf("time %u:%02u  order %3d  row %3d\n", t / 60, t % 60, order, row);
			fflush(stdout);
			last_time = t;
			last_order = order;
			last_row = row;
		}

		/* once the song is over, wait for the effects to finish */
		if (interrupted || (ended && !sfx_bus_voices_playing() && !sfx_pool_voices_playing()))
			break;
		if (opt_length > 0 && (ended ? elapsed : t) >= opt_length)
			break;

		timer_msleep(10);
	}

	if (opt_sfx_stress > 0) {
		sfx_stop_all();
		for (i = 0; i < (int)ARRAY_SIZE(stress_tones); i++)
			sfx_source_free(stress_tones[i]);
		printf("sfx stress: %lu calls in %.1f s, song time %u s, %u voices still playing\n",
			stress_ops, (timer_ticks() - start) / 1000.0, song_get_current_time(),
			sfx_bus_voices_playing());
		return interrupted ? 1 : 0;
	}
	if (opt_sfx_pool_stress > 0) {
		printf("pool stress: %lu note-on, %lu note-off, %lu cut, %lu setters, %lu queries in %.1f s;"
			" song time %u s, peak %u voices, %u still playing\n",
			pool_counts[0], pool_counts[1], pool_counts[2], pool_counts[3], pool_counts[4],
			(timer_ticks() - start) / 1000.0, song_get_current_time(), pool_peak,
			sfx_pool_voices_playing());
		sfx_pool_stop_all();
		return interrupted ? 1 : 0;
	}

	if (!opt_quiet)
		printf("%s at order %d row %d\n", ended ? "finished" : "stopped", last_order, last_row);

	return interrupted ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *song;
	int r;

	engine_host_register(&host);

	os_sysinit(&argc, &argv);

	song = parse_options(argc, argv);

	/* before anything in the engine allocates */
	setup_allocator();
	if (opt_driver_spec)
		audio_parse_driver_spec(opt_driver_spec, (char **)&opt_driver, (char **)&opt_device);

	float_init();
	cpu_init();
	ver_init();

	/* Same seeding as schismtracker, so without --seed the mixer's random
	 * numbers come out the same as there for a given time(). */
	srand(time(NULL));
	if (opt_seeded)
		csf_seed_random(opt_seed);

	dmoz_init();

	mt_init();
	SCHISM_RUNTIME_ASSERT(!util_initumask(), "Failed to initialize umask mutex");
	SCHISM_RUNTIME_ASSERT(!atm_init(), "Failed to initialize atomics!");
	SCHISM_RUNTIME_ASSERT(timer_init(), "Failed to initialize a timers backend!");
	SCHISM_RUNTIME_ASSERT(localtime_r_init(), "Failed to initialize localtime_r replacement!");

	song_initialise();
	load_settings();

	if (!opt_output) {
		opening_audio = 1;
		audio_init(opt_driver, opt_device);
		opening_audio = 0;
		audio_opened = 1;
		if (!opt_quiet)
			printf("audio driver %s, device %s\n", song_audio_driver(), song_audio_device());
	}

	song_init_modplug();

#ifdef USE_FLAC
	flac_init();
#endif
#ifdef USE_AVFORMAT
	avformat_init();
#endif
#ifdef USE_ZLIB
	gzip_init();
#endif
#ifdef USE_BZIP2
	bzip2_init();
#endif
#ifdef USE_LZMA
	xz_init();
#endif
#ifdef USE_ZSTD
	zstd_init();
#endif
#ifdef USE_GSTREAMER
	gstreamer_init();
#endif

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (sfx_bus_init(opt_sfx_voices) < 0) {
		fprintf(stderr, "schismplay: couldn't set up the effects bus\n");
		shutdown_all();
		release_memory();
		return 1;
	}
	sfx_bus_set_volume(opt_sfx_volume);

	if (!setup_pool() || !load_bank_files()) {
		free_banks();
		sfx_pool_quit();
		sfx_bus_quit();
		shutdown_all();
		release_memory();
		return 1;
	}

	if (song && !song_load_unchecked(song)) {
		fprintf(stderr, "schismplay: couldn't load %s\n", song);
		print_mem_stats("load");
		free_banks();
		sfx_pool_quit();
		sfx_bus_quit();
		shutdown_all();
		print_mem_stats("exit");
		release_memory();
		return 1;
	}
	print_mem_stats("load");

	if (!load_sfx_sources() || !load_song_bank()) {
		free_banks();
		sfx_pool_quit();
		sfx_bus_quit();
		free_sfx_sources();
		shutdown_all();
		print_mem_stats("exit");
		release_memory();
		return 1;
	}

	/* the song's samples have been copied by now, so it can go */
	if (opt_sfx_only && song)
		song_new(0);

	r = opt_output ? render(song) : play();
	print_mem_stats(opt_output ? "render" : "play");

	sfx_pool_quit();
	free_banks();
	sfx_bus_quit();
	free_sfx_sources();

	shutdown_all();
	print_mem_stats("exit");
	release_memory();
	return r;
}
