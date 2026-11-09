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
 * It has no UI; it registers its own engine callbacks below, and
 * schismplay-stubs.c covers what the engine still calls directly. */

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

#include <signal.h>

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

static const struct engine_host host = {
	.log = host_log,
	.status_text = host_status_text,
	.diskwrite_begin = host_diskwrite_begin,
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
		"  -q, --quiet               don't show the play position\n"
		"  -v, --verbose             show loader and engine messages\n"
		"      --version\n"
		"  -h, --help\n",
		argv0);
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
};

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
			audio_parse_driver_spec(optarg, (char **)&opt_driver, (char **)&opt_device);
			break;
		case O_LOOP:
			opt_loop = 1;
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

	if (optind != argc - 1) {
		usage(stderr, argv[0]);
		exit(2);
	}

	return argv[optind];
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
	int q;

	disko_set_export_limit(opt_length > 0 ? (size_t)(opt_length * opt_rate + 0.5) : 0);

	if (song_export(opt_output, "WAV") != SAVE_SUCCESS) {
		fprintf(stderr, "schismplay: couldn't start writing %s\n", opt_output);
		return 1;
	}

	do {
		q = disko_sync();
	} while (q == DW_SYNC_MORE && !interrupted);

	if (interrupted) {
		fprintf(stderr, "schismplay: interrupted\n");
		return 1;
	}

	if (q != DW_SYNC_DONE) {
		fprintf(stderr, "schismplay: error while rendering %s\n", song);
		return 1;
	}

	return 0;
}

static int play(void)
{
	int last_order = -1, last_row = -1;
	unsigned int last_time = (unsigned int)-1;
	int ended;

	if (opt_loop)
		song_start();
	else
		song_start_once();

	for (;;) {
		unsigned int t;
		int order, row;

		audio_worker();

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

		if (ended || interrupted)
			break;
		if (opt_length > 0 && t >= opt_length)
			break;

		timer_msleep(10);
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

	if (!song_load_unchecked(song)) {
		fprintf(stderr, "schismplay: couldn't load %s\n", song);
		shutdown_all();
		return 1;
	}

	r = opt_output ? render(song) : play();

	shutdown_all();
	return r;
}
