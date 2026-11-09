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
#include "sfx-bus.h"
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
	O_SFX,
	O_SFX_SAMPLE,
	O_SFX_ONLY,
	O_SFX_VOICES,
	O_SFX_VOLUME,
	O_SFX_STRESS,
};

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
		{"sfx", 1, NULL, O_SFX},
		{"sfx-sample", 1, NULL, O_SFX_SAMPLE},
		{"sfx-only", 0, NULL, O_SFX_ONLY},
		{"sfx-voices", 1, NULL, O_SFX_VOICES},
		{"sfx-volume", 1, NULL, O_SFX_VOLUME},
		{"sfx-stress", 1, NULL, O_SFX_STRESS},
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

	if (opt_sfx_only && optind == argc)
		return NULL;

	if (optind != argc - 1) {
		usage(stderr, argv[0]);
		exit(2);
	}

	return argv[optind];
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

	/* after starting the export, so the effects' delays count from its
	 * first frame */
	schedule_sfx();

	do {
		q = disko_sync();
	} while (q == DW_SYNC_MORE && !interrupted);

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
		if (interrupted || (ended && !sfx_bus_voices_playing()))
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

	if (sfx_bus_init(opt_sfx_voices) < 0) {
		fprintf(stderr, "schismplay: couldn't set up the effects bus\n");
		shutdown_all();
		return 1;
	}
	sfx_bus_set_volume(opt_sfx_volume);

	if (song && !song_load_unchecked(song)) {
		fprintf(stderr, "schismplay: couldn't load %s\n", song);
		shutdown_all();
		return 1;
	}

	if (!load_sfx_sources()) {
		shutdown_all();
		return 1;
	}

	/* the song's samples have been copied by now, so it can go */
	if (opt_sfx_only && song)
		song_new(0);

	r = opt_output ? render(song) : play();

	sfx_bus_quit();
	free_sfx_sources();

	shutdown_all();
	return r;
}
