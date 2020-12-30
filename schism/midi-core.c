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

#define NEED_TIME
#include "headers.h"

#include "event.h"

#include "util.h"

#include "midi.h"
#include "song.h"

#include "sdlmain.h"

#include "it.h"

#include "dmoz.h"

#include <ctype.h>
#include <assert.h>

static int _connected = 0;
/* midi_mutex is locked by the main thread,
midi_port_mutex is for the port thread(s),
and midi_record_mutex is by the event/sound thread
*/
static SDL_mutex *midi_mutex = NULL;
static SDL_mutex *midi_port_mutex = NULL;
static SDL_mutex *midi_record_mutex = NULL;
static SDL_mutex *midi_play_mutex = NULL;
static SDL_cond *midi_play_cond = NULL;

static struct midi_provider *port_providers = NULL;


/* all this just for usleep?! (maybe we should have sys/win32/usleep.c) */
#ifdef WIN32
#include <windows.h>

static void (*__win32_usleep)(unsigned int usec) = NULL;
static void __win32_old_usleep(unsigned int u)
{
	/* bah, only Win95 and "earlier" actually needs this... */
	SleepEx(u/1000,FALSE);
}

static FARPROC __ihatewindows_f1 = NULL;
static FARPROC __ihatewindows_f2 = NULL;
static FARPROC __ihatewindows_f3 = NULL;
static HANDLE __midi_timer = NULL;

static void __win32_new_usleep(unsigned int u)
{
	LARGE_INTEGER due;
	due.QuadPart = -(10 * (__int64)u);
	__ihatewindows_f2(__midi_timer, &due, 0, NULL, NULL, 0);
	__ihatewindows_f3(__midi_timer, INFINITE);
}

static void __win32_pick_usleep(void)
{
	HINSTANCE k32;

	k32 = GetModuleHandle("KERNEL32.DLL");
	if (!k32) k32 = LoadLibrary("KERNEL32.DLL");
	if (!k32) k32 = GetModuleHandle("KERNEL32.DLL");
	if (!k32) goto FAIL;
	__ihatewindows_f1 = (FARPROC)GetProcAddress(k32,"CreateWaitableTimer");
	__ihatewindows_f2 = (FARPROC)GetProcAddress(k32,"SetWaitableTimer");
	__ihatewindows_f3 = (FARPROC)GetProcAddress(k32,"WaitForSingleObject");
	if (!__ihatewindows_f1 || !__ihatewindows_f2 || !__ihatewindows_f3)
		goto FAIL;
	__midi_timer = (HANDLE)__ihatewindows_f1(NULL,TRUE,NULL);
	if (!__midi_timer) goto FAIL;

	/* grumble */
	__win32_usleep = __win32_new_usleep;
	return;
FAIL:
	__win32_usleep = __win32_old_usleep;
}

#define SLEEP_FUNC(x)   __win32_usleep(x)

#else

#define SLEEP_FUNC(x)   usleep(x)

#endif

/* configurable midi stuff */
int midi_flags = MIDI_TICK_QUANTIZE | MIDI_RECORD_NOTEOFF
		| MIDI_RECORD_VELOCITY | MIDI_RECORD_AFTERTOUCH
		| MIDI_PITCHBEND;

int midi_pitch_depth = 12;
int midi_amplification = 100;
int midi_c5note = 60;

#define CFG_GET_MI(v,d) midi_ ## v = cfg_get_number(cfg, "MIDI", #v, d)

static void _cfg_load_midi_part_locked(struct midi_port *q)
{
	struct cfg_section *c;
	/*struct midi_provider *p;*/
	cfg_file_t cfg;
	const char *sn;
	char *ptr, *ss, *sp;
	char buf[256];
	int j;

	ss = q->name;
	if (!ss) return;
	while (isspace(*ss)) ss++;
	if (!*ss) return;

	sp = q->provider ? q->provider->name : NULL;
	if (sp) {
		while (isspace(*sp)) sp++;
		if (!*sp) sp = NULL;
	}

	ptr = dmoz_path_concat(cfg_dir_dotschism, "config");
	cfg_init(&cfg, ptr);

	/* look for MIDI port sections */
	for (c = cfg.sections; c; c = c->next) {
		j = -1;
		sscanf(c->name, "MIDI Port %d", &j);
		if (j < 1) continue;
		sn = cfg_get_string(&cfg, c->name, "name", buf, 255, NULL);
		if (!sn) continue;
		if (strcasecmp(ss, sn) != 0) continue;
		sn = cfg_get_string(&cfg, c->name, "provider", buf, 255, NULL);
		if (sn && sp && strcasecmp(sp, sn) != 0) continue;
		/* okay found port */
		if ((q->iocap & MIDI_INPUT) && cfg_get_number(&cfg, c->name, "input", 0)) {
			q->io |= MIDI_INPUT;
		}
		if ((q->iocap & MIDI_OUTPUT) && cfg_get_number(&cfg, c->name, "output", 0)) {
			q->io |= MIDI_OUTPUT;
		}
		if (q->io && q->enable) q->enable(q);
	}

	cfg_free(&cfg);
	free(ptr);
}


void cfg_load_midi(cfg_file_t *cfg)
{
	midi_config_t *md, *mc;
	char buf[17], buf2[33];
	int i;

	CFG_GET_MI(flags, MIDI_TICK_QUANTIZE | MIDI_RECORD_NOTEOFF
		| MIDI_RECORD_VELOCITY | MIDI_RECORD_AFTERTOUCH
		| MIDI_PITCHBEND);
	CFG_GET_MI(pitch_depth, 12);
	CFG_GET_MI(amplification, 100);
	CFG_GET_MI(c5note, 60);

	song_lock_audio();
	md = &default_midi_config;
	cfg_get_string(cfg,"MIDI","start", md->start, 31, "FF");
	cfg_get_string(cfg,"MIDI","stop", md->stop, 31, "FC");
	cfg_get_string(cfg,"MIDI","tick", md->tick, 31, "");
	cfg_get_string(cfg,"MIDI","note_on", md->note_on, 31, "9c n v");
	cfg_get_string(cfg,"MIDI","note_off", md->note_off, 31, "9c n 0");
	cfg_get_string(cfg,"MIDI","set_volume", md->set_volume, 31, "");
	cfg_get_string(cfg,"MIDI","set_panning", md->set_panning, 31, "");
	cfg_get_string(cfg,"MIDI","set_bank", md->set_bank, 31, "");
	cfg_get_string(cfg,"MIDI","set_program", md->set_program, 31, "Cc p");
	for (i = 0; i < 16; i++) {
		snprintf(buf, 16, "SF%X", i);
		cfg_get_string(cfg, "MIDI", buf, md->sfx[i], 31,
				i == 0 ? "F0F000z" : "");
	}

	for (i = 0; i < 128; i++) {
		snprintf(buf, 16, "Z%02X", i + 0x80);
		if (i < 16)
			snprintf(buf2, 32, "F0F001%02x", i * 8);
		else
			buf2[0] = '\0';
		cfg_get_string(cfg, "MIDI", buf, md->zxx[i], 31, buf2);
	}

	mc = &current_song->midi_config;
	memcpy(mc, md, sizeof(midi_config_t));


	song_unlock_audio();
}

#define CFG_SET_MI(v) cfg_set_number(cfg, "MIDI", #v, midi_ ## v)
void cfg_save_midi(cfg_file_t *cfg)
{
	struct cfg_section *c;
	struct midi_provider *p;
	struct midi_port *q;
	midi_config_t *md, *mc;
	char buf[33];
	char *ss;
	int i, j;

	CFG_SET_MI(flags);
	CFG_SET_MI(pitch_depth);
	CFG_SET_MI(amplification);
	CFG_SET_MI(c5note);

	song_lock_audio();
	md = &default_midi_config;

	/* overwrite default */
	mc = &current_song->midi_config;
	memcpy(md, mc, sizeof(midi_config_t));

	cfg_set_string(cfg,"MIDI","start", md->start);
	cfg_set_string(cfg,"MIDI","stop", md->stop);
	cfg_set_string(cfg,"MIDI","tick", md->tick);
	cfg_set_string(cfg,"MIDI","note_on", md->note_on);
	cfg_set_string(cfg,"MIDI","note_off", md->note_off);
	cfg_set_string(cfg,"MIDI","set_volume", md->set_volume);
	cfg_set_string(cfg,"MIDI","set_panning", md->set_panning);
	cfg_set_string(cfg,"MIDI","set_bank", md->set_bank);
	cfg_set_string(cfg,"MIDI","set_program", md->set_program);
	for (i = 0; i < 16; i++) {
		snprintf(buf, 32, "SF%X", i);
		cfg_set_string(cfg, "MIDI", buf, md->sfx[i]);
	}
	for (i = 0; i < 128; i++) {
		snprintf(buf, 32, "Z%02X", i + 0x80);
		cfg_set_string(cfg, "MIDI", buf, md->zxx[i]);
	}
	song_unlock_audio();

	/* write out only enabled midi ports */
	i = 1;
	SDL_mutexP(midi_mutex);
	q = NULL;
	for (p = port_providers; p; p = p->next) {
		while (midi_port_foreach(p, &q)) {
			ss = q->name;
			if (!ss) continue;
			while (isspace(*ss)) ss++;
			if (!*ss) continue;
			if (!q->io) continue;

			snprintf(buf, 32, "MIDI Port %d", i); i++;
			cfg_set_string(cfg, buf, "name", ss);
			ss = p->name;
			if (ss) {
				while (isspace(*ss)) ss++;
				if (*ss) {
					cfg_set_string(cfg, buf, "provider", ss);
				}
			}
			cfg_set_number(cfg, buf, "input", q->io & MIDI_INPUT ? 1 : 0);
			cfg_set_number(cfg, buf, "output", q->io & MIDI_OUTPUT ? 1 : 0);
		}
	}
	//TODO: Save number of MIDI-IP ports
	SDL_mutexV(midi_mutex);

	/* delete other MIDI port sections */
	for (c = cfg->sections; c; c = c->next) {
		j = -1;
		sscanf(c->name, "MIDI Port %d", &j);
		if (j < i) continue;
		c->omit = 1;
	}

}


static void _midi_engine_connect(void)
{
#ifdef USE_NETWORK
	ip_midi_setup();
#endif
#ifdef USE_OSS
	oss_midi_setup();
#endif
#ifdef USE_ALSA
	alsa_midi_setup();
#endif
#ifdef WIN32
	win32mm_midi_setup();
#endif
#ifdef MACOSX
	macosx_midi_setup();
#endif
}

void midi_engine_poll_ports(void)
{
	struct midi_provider *n;

	if (!midi_mutex) return;

	SDL_mutexP(midi_mutex);
	for (n = port_providers; n; n = n->next) {
		if (n->poll) n->poll(n);
	}
	SDL_mutexV(midi_mutex);
}

int midi_engine_start(void)
{
	if (_connected)
		return 1;

	midi_mutex        = SDL_CreateMutex();
	midi_record_mutex = SDL_CreateMutex();
	midi_play_mutex   = SDL_CreateMutex();
	midi_port_mutex   = SDL_CreateMutex();
	midi_play_cond    = SDL_CreateCond();

	if (!(midi_mutex && midi_record_mutex && midi_play_mutex && midi_port_mutex && midi_play_cond)) {
		if (midi_mutex)        SDL_DestroyMutex(midi_mutex);
		if (midi_record_mutex) SDL_DestroyMutex(midi_record_mutex);
		if (midi_play_mutex)   SDL_DestroyMutex(midi_play_mutex);
		if (midi_port_mutex)   SDL_DestroyMutex(midi_port_mutex);
		if (midi_play_cond)    SDL_DestroyCond(midi_play_cond);
		midi_mutex = midi_record_mutex = midi_play_mutex = midi_port_mutex = NULL;
		midi_play_cond = NULL;
		return 0;
	}

	_midi_engine_connect();
	_connected = 1;
	return 1;
}

void midi_engine_reset(void)
{
	if (!_connected) return;
	midi_engine_stop();
	_midi_engine_connect();
}

void midi_engine_stop(void)
{
	struct midi_provider *n, *p;
	struct midi_port *q;

	if (!_connected) return;
	if (!midi_mutex) return;

	SDL_mutexP(midi_mutex);
	for (n = port_providers; n;) {
		p = n->next;

		q = NULL;
		while (midi_port_foreach(p, &q)) {
			midi_port_unregister(q->num);
		}

		if (n->thread) {
			SDL_KillThread(n->thread);
		}
		free(n->name);
		free(n);
		n = p;
	}
	_connected = 0;
	SDL_mutexV(midi_mutex);
}


/* PORT system */
static struct midi_port **port_top = NULL;
static int port_count = 0;
static int port_alloc = 0;

struct midi_port *midi_engine_port(int n, const char **name)
{
	struct midi_port *pv = NULL;

	if (!midi_port_mutex) return NULL;
	SDL_mutexP(midi_port_mutex);
	if (n >= 0 && n < port_count) {
		pv = port_top[n];
		if (name) *name = pv->name;
	}
	SDL_mutexV(midi_port_mutex);
	return pv;
}

int midi_engine_port_count(void)
{
	int pc;
	if (!midi_port_mutex) return 0;
	SDL_mutexP(midi_port_mutex);
	pc = port_count;
	SDL_mutexV(midi_port_mutex);
	return pc;
}

/* midi engines register a provider (one each!) */
struct midi_provider *midi_provider_register(const char *name,
		struct midi_driver *driver)
{
	struct midi_provider *n;

	if (!midi_mutex) return NULL;

	n = mem_calloc(1, sizeof(struct midi_provider));
	n->name = str_dup(name);
	n->poll = driver->poll;
	n->enable = driver->enable;
	n->disable = driver->disable;
	if (driver->flags & MIDI_PORT_CAN_SCHEDULE) {
		n->send_later = driver->send;
		n->drain = driver->drain;
	} else {
		n->send_now = driver->send;
	}

	SDL_mutexP(midi_mutex);
	n->next = port_providers;
	port_providers = n;

	if (driver->thread) {
		// FIXME this cast is stupid
		n->thread = SDL_CreateThread((int (*)(void*))driver->thread, n);
	}

	SDL_mutexV(midi_mutex);

	return n;
}

/* midi engines list ports this way */
int midi_port_register(struct midi_provider *pv, int inout, const char *name,
void *userdata, int free_userdata)
{
	struct midi_port *p, **pt;
	int i;

	if (!midi_port_mutex) return -1;

	p = mem_alloc(sizeof(struct midi_port));
	p->io = 0;
	p->iocap = inout;
	p->name = str_dup(name);
	p->enable = pv->enable;
	p->disable = pv->disable;
	p->send_later = pv->send_later;
	p->send_now = pv->send_now;
	p->drain = pv->drain;

	p->free_userdata = free_userdata;
	p->userdata = userdata;
	p->provider = pv;

	for (i = 0; i < port_alloc; i++) {
		if (port_top[i] == NULL) {
			port_top[i] = p;
			p->num = i;
			port_count++;
			_cfg_load_midi_part_locked(p);
			return i;
		}
	}

	SDL_mutexP(midi_port_mutex);
	port_alloc += 4;
	pt = realloc(port_top, sizeof(struct midi_port *) * port_alloc);
	if (!pt) {
		free(p->name);
		free(p);
		SDL_mutexV(midi_port_mutex);
		return -1;
	}
	pt[port_count] = p;
	for (i = port_count+1; i < port_alloc; i++) pt[i] = NULL;
	port_top = pt;
	p->num = port_count;
	port_count++;

	/* finally, and just before unlocking, load any configuration for it... */
	_cfg_load_midi_part_locked(p);

	SDL_mutexV(midi_port_mutex);
	return p->num;
}

int midi_port_foreach(struct midi_provider *p, struct midi_port **cursor)
{
	int i;
	if (!midi_port_mutex) return 0;

	SDL_mutexP(midi_port_mutex);
	do {
		if (!*cursor) {
			i = 0;
		} else {
			i = ((*cursor)->num) + 1;
			while (i < port_alloc && !port_top[i]) i++;
		}
		if (i >= port_alloc) {
			*cursor = NULL;
			SDL_mutexV(midi_port_mutex);
			return 0;
		}
		*cursor = port_top[i];
	} while (p && (*cursor)->provider != p);
	SDL_mutexV(midi_port_mutex);
	return 1;
}

static int _midi_send_unlocked(const unsigned char *data, unsigned int len, unsigned int delay,
			int from)
{
	struct midi_port *ptr = NULL;
	int need_timer = 0;
#if 0
	unsigned int i;
printf("MIDI: ");
	for (i = 0; i < len; i++) {
		printf("%02x ", data[i]);
	}
puts("");
fflush(stdout);
#endif
	if (from == 0) {
		/* from == 0 means from immediate; everyone plays */
		while (midi_port_foreach(NULL, &ptr)) {
			if ((ptr->io & MIDI_OUTPUT)) {
				if (ptr->send_now)
					ptr->send_now(ptr, data, len, 0);
				else if (ptr->send_later)
					ptr->send_later(ptr, data, len, 0);
			}
		}
	} else if (from == 1) {
		/* from == 1 means from buffer-flush; only "now" plays */
		while (midi_port_foreach(NULL, &ptr)) {
			if ((ptr->io & MIDI_OUTPUT)) {
				if (ptr->send_now)
					ptr->send_now(ptr, data, len, 0);
			}
		}
	} else {
		/* from == 2 means from buffer-write; only "later" plays */
		while (midi_port_foreach(NULL, &ptr)) {
			if ((ptr->io & MIDI_OUTPUT)) {
				if (ptr->send_later)
					ptr->send_later(ptr, data, len, delay);
				else if (ptr->send_now)
					need_timer = 1;
			}
		}
	}
	return need_timer;
}

void midi_send_now(const unsigned char *seq, unsigned int len)
{
	if (!midi_record_mutex) return;

	SDL_mutexP(midi_record_mutex);
	_midi_send_unlocked(seq, len, 0, 0);
	SDL_mutexV(midi_record_mutex);
}

/*----------------------------------------------------------------------------------*/

/* okay, our local queue is a little confusing,
 * but it works on the premise that the people who need it (oss)
 * also have systems with high-speed context switches.
 *
 * this sucks for freebsd/386 users as they've got a high-latency
 * audio system (oss) and slow context switches (x86). someone should
 * port alsa to freebsd. maybe someone else will solve this problem,
 * or maybe the computers that are slow enough to matter simply won't
 * be bothered.
 *
 * midi, that is, real midi, is 31250bps. that's 3125 bits per msec,
 * or 391 bytes per msec. i use a fixed buffer here because access needs
 * to be fast, and attempting to handle more will simply help people
 * using software/only setups.
 *
 * really, software-only midi, without kernel assistance sucks.
 */
struct qent {
	int used;
	unsigned char b[391];
};
static struct qent *qq = NULL;
static int midims, ms10s, qlen;

void midi_queue_alloc(int my_audio_buffer_samples, int sample_size, int samples_per_second)
{
	int buffer_size = (my_audio_buffer_samples * sample_size);

	if (qq) {
		free(qq);
		qq = NULL;
	}

	/* how long is the audio buffer in 10 msec?
	 * well, (sample_size*samples_per_second)/80 is the number of bytes per msec
	 */
	midims = sample_size * samples_per_second;
	if ((midims % 80) != 0) midims += (80 - (midims % 80));
	ms10s = midims / 80;
	midims /= 8;

	if (ms10s > buffer_size) {
		/* okay, there's not even 10msec of audio data; midi queueing will be impossible */
		qlen = 0;
		return;
	}

	if ((buffer_size % ms10s) != 0) {
		buffer_size += (ms10s - (buffer_size % ms10s));
	}
	qlen = buffer_size / ms10s;
	/* now qlen is the number of msec in digital output buffer */

	qq = mem_calloc(qlen, sizeof(struct qent));
}

static SDL_Thread *midi_queue_thread = NULL;

static int _midi_queue_run(UNUSED void *xtop)
{
	int i;

#ifdef WIN32
	__win32_pick_usleep();
	SetPriorityClass(GetCurrentProcess(),HIGH_PRIORITY_CLASS);
	SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
	/*SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);*/
#endif

	SDL_mutexP(midi_play_mutex);
	for (;;) {
		SDL_CondWait(midi_play_cond, midi_play_mutex);

		for (i = 0; i < qlen; i++) {
			SDL_mutexP(midi_record_mutex);
			_midi_send_unlocked(qq[i].b, qq[i].used, 0, 1);
			SDL_mutexV(midi_record_mutex);
			SLEEP_FUNC(10000); /* 10msec */
			qq[i].used = 0;
		}
	}

	return 0; /* never happens */
}

int midi_need_flush(void)
{
	struct midi_port *ptr;
	int need_explicit_flush = 0;
	int i;

	if (!midi_record_mutex || !midi_play_mutex) return 0;

	ptr = NULL;

	while (midi_port_foreach(NULL, &ptr)) {
		if ((ptr->io & MIDI_OUTPUT)) {
			if (!ptr->drain && ptr->send_now)
				need_explicit_flush = 1;
		}
	}
	if (!need_explicit_flush) return 0;

	for (i = 0; i < qlen; i++) {
		if (qq[i].used) return 1;
	}

	return 0;
}

void midi_send_flush(void)
{
	struct midi_port *ptr = NULL;
	int need_explicit_flush = 0;

	if (!midi_record_mutex || !midi_play_mutex) return;

	while (midi_port_foreach(NULL, &ptr)) {
		if ((ptr->io & MIDI_OUTPUT)) {
			if (ptr->drain) ptr->drain(ptr);
			else if (ptr->send_now) need_explicit_flush=1;
		}
	}

	/* no need for midi sync huzzah; driver does it for us... */
	if (!need_explicit_flush) return;

	if (!midi_queue_thread) {
		midi_queue_thread = SDL_CreateThread(_midi_queue_run, NULL);
		if (midi_queue_thread) {
			log_appendf(3, "Started MIDI queue thread");
		} else {
			log_appendf(2, "ACK: Couldn't start MIDI thread; things are likely going to go boom!");
		}
	}

	SDL_mutexP(midi_play_mutex);
	SDL_CondSignal(midi_play_cond);
	SDL_mutexV(midi_play_mutex);
}

void midi_send_buffer(const unsigned char *data, unsigned int len, unsigned int pos)
{
	if (!midi_record_mutex) return;

	SDL_mutexP(midi_record_mutex);

	/* just for fun... */
//	if (status.current_page == PAGE_MIDI) {
//		status.last_midi_real_len = len;
//		if (len > sizeof(status.last_midi_event)) {
//			status.last_midi_len = sizeof(status.last_midi_event);
//		} else {
//			status.last_midi_len = len;
//		}
//		memcpy(status.last_midi_event, data, status.last_midi_len);
//		status.flags |= MIDI_EVENT_CHANGED;
//		status.last_midi_port = NULL;
//		time(&status.last_midi_time);
//		status.flags |= NEED_UPDATE;
//	}

	/* pos is still in miliseconds */
	if (midims != 0 && _midi_send_unlocked(data, len, pos/midims, 2)) {
		/* grr, we need a timer */

		/* calculate pos in buffer */
		pos /= ms10s;
		assert(((unsigned)pos) < ((unsigned)qlen));

		if ((len + qq[pos].used) > sizeof(qq[pos].b)) {
			len = sizeof(qq[pos].b) - qq[pos].used;
			/* okay, we're going to lose data here */
		}
		memcpy(qq[pos].b+qq[pos].used, data, len);
		qq[pos].used += len;
	}

	SDL_mutexV(midi_record_mutex);
}

/*----------------------------------------------------------------------------------*/

void midi_port_unregister(int num)
{
	struct midi_port *q;
	int i;

	if (!midi_port_mutex) return;

	SDL_mutexP(midi_port_mutex);
	for (i = 0; i < port_alloc; i++) {
		if (port_top[i] && port_top[i]->num == num) {
			q = port_top[i];
			if (q->disable) q->disable(q);
			if (q->free_userdata) free(q->userdata);
			free(q);

			port_top[i] = NULL;
			port_count--;
			break;
		}
	}
	SDL_mutexV(midi_port_mutex);
}

void midi_received_cb(struct midi_port *src, unsigned char *data, unsigned int len)
{
	unsigned char d4[4];
	int cmd;

	if (!len) return;
	if (len < 4) {
		memset(d4, 0, sizeof(d4));
		memcpy(d4, data, len);
		data = d4;
	}

	/* just for fun... */
	SDL_mutexP(midi_record_mutex);
	status.last_midi_real_len = len;
	if (len > sizeof(status.last_midi_event)) {
		status.last_midi_len = sizeof(status.last_midi_event);
	} else {
		status.last_midi_len = len;
	}
	memcpy(status.last_midi_event, data, status.last_midi_len);
	status.flags |= MIDI_EVENT_CHANGED;
	status.last_midi_port = src;
	time(&status.last_midi_time);
	SDL_mutexV(midi_record_mutex);

	/* pass through midi events when on midi page */
//	if (status.current_page == PAGE_MIDI) {
//		midi_send_now(data,len);
//		status.flags |= NEED_UPDATE;
//	}

	cmd = ((*data) & 0xF0) >> 4;
	if (cmd == 0x8 || (cmd == 0x9 && data[2] == 0)) {
		midi_event_note(MIDI_NOTEOFF, data[0] & 15, data[1], 0);
	} else if (cmd == 0x9) {
		midi_event_note(MIDI_NOTEON, data[0] & 15, data[1], data[2]);
	} else if (cmd == 0xA) {
		midi_event_note(MIDI_KEYPRESS, data[0] & 15, data[1], data[2]);
	} else if (cmd == 0xB) {
		midi_event_controller(data[0] & 15, data[1], data[2]);
	} else if (cmd == 0xC) {
		midi_event_program(data[0] & 15, data[1]);
	} else if (cmd == 0xD) {
		midi_event_aftertouch(data[0] & 15, data[1]);
	} else if (cmd == 0xE) {
		midi_event_pitchbend(data[0] & 15, data[1]);
	} else if (cmd == 0xF) {
		switch ((*data & 15)) {
		case 0: /* sysex */
			if (len <= 2) return;
			midi_event_sysex(data+1, len-2);
			break;
		case 6: /* tick */
			midi_event_tick();
			break;
		default:
			/* something else */
			midi_event_system((*data & 15), (data[1])
					| (data[2] << 8)
					| (data[3] << 16));
			break;
		}
	}
}

static void midi_push_event(Uint8 code, void *data1, size_t data1_len, int alloc)
{
	SDL_Event e = { .user = { .type = SCHISM_EVENT_MIDI, .code = code, .data1 = data1 } };

	if (data1 && alloc) {
		e.user.data1 = mem_alloc(data1_len);
		memcpy(e.user.data1, data1, data1_len);
	}

	SDL_PushEvent(&e);
}

void midi_event_note(enum midi_note mnstatus, int channel, int note, int velocity)
{
	int st[4] = { mnstatus, channel, note, velocity };

	midi_push_event(SCHISM_EVENT_MIDI_NOTE, st, sizeof(st), 1);
}

void midi_event_controller(int channel, int param, int value)
{
	int st[4] = { value, channel, param };

	midi_push_event(SCHISM_EVENT_MIDI_CONTROLLER, st, sizeof(st), 1);
}

void midi_event_program(int channel, int value)
{
	int st[4] = { value, channel };

	midi_push_event(SCHISM_EVENT_MIDI_PROGRAM, st, sizeof(st), 1);
}

void midi_event_aftertouch(int channel, int value)
{
	int st[4] = { value, channel };

	midi_push_event(SCHISM_EVENT_MIDI_AFTERTOUCH, st, sizeof(st), 1);
}

void midi_event_pitchbend(int channel, int value)
{
	int st[4] = { value, channel };

	midi_push_event(SCHISM_EVENT_MIDI_PITCHBEND, st, sizeof(st), 1);
}

void midi_event_system(int argv, int param)
{
	int st[4] = { argv, param };

	midi_push_event(SCHISM_EVENT_MIDI_SYSTEM, st, sizeof(st), 1);
}

void midi_event_tick(void)
{
	midi_push_event(SCHISM_EVENT_MIDI_TICK, NULL, 0, 0);
}

void midi_event_sysex(const unsigned char *data, unsigned int len)
{
	size_t packet_len = len + sizeof(len);
	void *packet = mem_alloc(packet_len);

	memcpy(packet, &len, sizeof(len));
	memcpy(packet + sizeof(len), data, len);

	midi_push_event(SCHISM_EVENT_MIDI_SYSEX, packet, packet_len, 0);
}



/* --------------------------------------------------------------------------------------------------------- */

/* returns 1 if the key was handled */
static int handle_key_global(struct key_event * k)
{
	int i, ins_mode;
	int _mp_active = 0;	// Can be nuked, methinks
	int currently_grabbed = SDL_GRAB_OFF;

	if (_mp_active == 2 && (k->mouse == MOUSE_CLICK && k->state == KEY_RELEASE)) {
		status.flags |= NEED_UPDATE;
//		dialog_destroy_all();
		_mp_active = 0;
		// eat it...
		return 1;
	}
	if ((!_mp_active) && k->state == KEY_PRESS && k->mouse == MOUSE_CLICK) {
		if (k->x >= 63 && k->x <= 77 && k->y >= 6 && k->y <= 7) {
			status.vis_style++;
			status.vis_style %= VIS_SENTINEL;
			status.flags |= NEED_UPDATE;
			return 1;
		} else if (k->y == 5 && k->x == 50) {
//			minipop_slide(kbd_get_current_octave(), "Octave", 0, 8,
//				kbd_set_current_octave, NULL, 50, 5);
			return 1;
		} else if (k->y == 4 && k->x >= 50 && k->x <= 52) {
//			minipop_slide(song_get_current_speed(), "Speed", 1, 255,
//				song_set_current_speed, song_set_initial_speed, 51, 4);
			return 1;
		} else if (k->y == 4 && k->x >= 54 && k->x <= 56) {
//			minipop_slide(song_get_current_tempo(), "Tempo", 32, 255,
//				song_set_current_tempo, song_set_initial_tempo, 55, 4);
			return 1;
		} else if (k->y == 3 && k->x >= 50 && k-> x <= 77) {
//			if (page_is_instrument_list(status.current_page)
//			    || status.current_page == PAGE_SAMPLE_LIST
//			    || (!(status.flags & CLASSIC_MODE)
//				&& (status.current_page == PAGE_ORDERLIST_PANNING
//				    || status.current_page == PAGE_ORDERLIST_VOLUMES)))
//				ins_mode = 0;
//			else
				ins_mode = song_is_instrument_mode();
//			if (ins_mode) {
//				minipop_slide(instrument_get_current(), "Instrument",
//					status.current_page == PAGE_INSTRUMENT_LIST ? 1 : 0,
//					99 /* FIXME */, instrument_set, NULL, 58, 3);
//			} else {
//				minipop_slide(sample_get_current(), "Sample",
//					status.current_page == PAGE_SAMPLE_LIST ? 1 : 0,
//					99 /* FIXME */, sample_set, NULL, 58, 3);
//			}

		} else if (k->x >= 12 && k->x <= 18) {
			if (k->y == 7 || k->y == 6 || k->y == 5) {
				return 1;
			}
//			if (k->y == 7) {
//				minipop_slide(get_current_row(), "Row",
//					0, song_get_rows_in_pattern(get_current_pattern()),
//					set_current_row, NULL, 14, 7);
//				return 1;
//			} else if (k->y == 6) {
//				minipop_slide(get_current_pattern(), "Pattern",
//					0, csf_get_num_patterns(current_song),
//					set_current_pattern, NULL, 14, 6);
//				return 1;
//			} else if (k->y == 5) {
//				minipop_slide(get_current_order(), "Order",
//					0, csf_get_num_orders(current_song),
//					set_current_order, NULL, 14, 5);
//				return 1;
//			}
		}
	} else if ((!_mp_active) && k->mouse == MOUSE_DBLCLICK) {
		if (k->y == 4 && k->x >= 11 && k->x <= 28) {
//			set_page(PAGE_SAVE_MODULE);
			return 1;
		} else if (k->y == 3 && k->x >= 11 && k->x <= 35) {
//			set_page(PAGE_SONG_VARIABLES);
			return 1;
		}
	}

	/* shortcut */
	if (k->mouse != MOUSE_NONE) {
		return 0;
	}

	/* first, check the truly global keys (the ones that still work if
	 * a dialog's open) */
	switch (k->sym) {
	case SDLK_RETURN:
		if ((k->mod & KMOD_CTRL) && k->mod & KMOD_ALT) {
			if (k->state == KEY_PRESS)
				return 1;
//			toggle_display_fullscreen();
			return 1;
		}
		break;
	case SDLK_m:
		if (k->mod & KMOD_CTRL) {
			if (k->state == KEY_RELEASE)
				return 1;
//			video_mousecursor(MOUSE_CYCLE_STATE);
			return 1;
		}
		break;

	case SDLK_d:
		if (k->mod & KMOD_CTRL) {
			if (k->state == KEY_RELEASE)
				return 1; /* argh */
			i = SDL_WM_GrabInput(SDL_GRAB_QUERY);
			if (i == SDL_GRAB_QUERY)
				i = currently_grabbed;
			currently_grabbed = i = (i != SDL_GRAB_ON ? SDL_GRAB_ON : SDL_GRAB_OFF);
			SDL_WM_GrabInput(i);
//			status_text_flash(i
//				? "Mouse and keyboard grabbed, press Ctrl+D to release"
//				: "Mouse and keyboard released");
			return 1;
		}
		break;

	case SDLK_i:
		/* reset audio stuff? */
		if (k->mod & KMOD_CTRL) {
			if (k->state == KEY_RELEASE)
				return 1;
			audio_reinit();
			return 1;
		}
		break;
	case SDLK_e:
		/* This should reset everything display-related. */
		if (k->mod & KMOD_CTRL) {
			if (k->state == KEY_RELEASE)
				return 1;
//			font_init();
			status.flags |= NEED_UPDATE;
			return 1;
		}
		break;
	case SDLK_HOME:
		if (!(k->mod & KMOD_ALT)) break;
		if (status.flags & DISKWRITER_ACTIVE) break;
		if (k->state == KEY_RELEASE)
			return 0;
		kbd_set_current_octave(kbd_get_current_octave() - 1);
		return 1;
	case SDLK_END:
		if (!(k->mod & KMOD_ALT)) break;
		if (status.flags & DISKWRITER_ACTIVE) break;
		if (k->state == KEY_RELEASE)
			return 0;
		kbd_set_current_octave(kbd_get_current_octave() + 1);
		return 1;
	default:
		break;
	}

	/* next, if there's no dialog, check the rest of the keys */
	if (status.flags & DISKWRITER_ACTIVE) return 0;

//	switch (k->sym) {
//	case SDLK_q:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS) {
//				if (k->mod & KMOD_SHIFT)
//					exit(0);
//				show_exit_prompt();
//			}
//			return 1;
//		}
//		break;
//	case SDLK_n:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
////			if (k->state == KEY_PRESS)
////				new_song_dialog();
//			return 1;
//		}
//		break;
//	case SDLK_g:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
////			if (k->state == KEY_PRESS)
////				show_song_timejump();
//			return 1;
//		}
//		break;
//	case SDLK_p:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				show_song_length();
//			return 1;
//		}
//		break;
//	case SDLK_F1:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_CONFIG);
//		} else if (k->mod & KMOD_SHIFT) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(status.current_page == PAGE_MIDI ? PAGE_MIDI_OUTPUT : PAGE_MIDI);
//		} else if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_HELP);
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F2:
//		if (k->mod & KMOD_CTRL) {
//			if (status.current_page == PAGE_PATTERN_EDITOR) {
//				_mp_finish(NULL);
//				if (k->state == KEY_PRESS && status.dialog_type == DIALOG_NONE) {
//					pattern_editor_length_edit();
//				}
//				return 1;
//			}
//			if (status.dialog_type != DIALOG_NONE)
//				return 0;
//		} else if (NO_MODIFIER(k->mod)) {
//			if (status.current_page == PAGE_PATTERN_EDITOR) {
//				if (k->state == KEY_PRESS) {
//					if (status.dialog_type & DIALOG_MENU) {
//						return 0;
//					} else if (status.dialog_type != DIALOG_NONE) {
////						dialog_yes_NULL();
//						status.flags |= NEED_UPDATE;
//					} else {
//						_mp_finish(NULL);
//						pattern_editor_display_options();
//					}
//				}
//			} else {
//				if (status.dialog_type != DIALOG_NONE)
//					return 0;
//				_mp_finish(NULL);
//				if (k->state == KEY_PRESS)
//					set_page(PAGE_PATTERN_EDITOR);
//			}
//			return 1;
//		}
//		break;
//	case SDLK_F3:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_SAMPLE_LIST);
//		} else {
//			_mp_finish(NULL);
//			if (k->mod & KMOD_CTRL) set_page(PAGE_LIBRARY_SAMPLE);
//			break;
//		}
//		return 1;
//	case SDLK_F4:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (NO_MODIFIER(k->mod)) {
//			if (status.current_page == PAGE_INSTRUMENT_LIST) return 0;
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_INSTRUMENT_LIST);
//		} else {
//			if (k->mod & KMOD_SHIFT) return 0;
//			_mp_finish(NULL);
//			if (k->mod & KMOD_CTRL) set_page(PAGE_LIBRARY_INSTRUMENT);
//			break;
//		}
//		return 1;
//	case SDLK_F5:
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				song_start();
//		} else if (k->mod & KMOD_SHIFT) {
//			if (status.dialog_type != DIALOG_NONE)
//				return 0;
//			_mp_finish(NULL);
//			if (k->state == KEY_RELEASE)
//				set_page(PAGE_PREFERENCES);
//		} else if (NO_MODIFIER(k->mod)) {
//			if (song_get_mode() == MODE_STOPPED
//			|| (song_get_mode() == MODE_SINGLE_STEP && status.current_page == PAGE_INFO)) {
//				_mp_finish(NULL);
//				if (k->state == KEY_PRESS)
//					song_start();
//			}
//			if (k->state == KEY_PRESS) {
//				if (status.dialog_type != DIALOG_NONE)
//					return 0;
//				_mp_finish(NULL);
//				set_page(PAGE_INFO);
//			}
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F6:
//		if (k->mod & KMOD_SHIFT) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				song_start_at_order(get_current_order(), 0);
//		} else if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				song_loop_pattern(get_current_pattern(), 0);
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F7:
//		if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				play_song_from_mark();
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F8:
//		if (k->mod & KMOD_SHIFT) {
//			if (k->state == KEY_PRESS)
//				song_pause();
//		} else if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				song_stop();
//			status.flags |= NEED_UPDATE;
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F9:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_SHIFT) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_MESSAGE);
//		} else if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_LOAD_MODULE);
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_l:
//	case SDLK_r:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_RELEASE)
//				set_page(PAGE_LOAD_MODULE);
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_s:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_RELEASE)
//				save_song_or_save_as();
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_w:
//		/* Ctrl-W _IS_ in IT, and hands don't leave home row :) */
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_RELEASE)
//				set_page(PAGE_SAVE_MODULE);
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F10:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (k->mod & KMOD_ALT) break;
//		if (k->mod & KMOD_CTRL) break;
//
//		_mp_finish(NULL);
//		if (k->mod & KMOD_SHIFT) {
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_EXPORT_MODULE);
//		} else {
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_SAVE_MODULE);
//		}
//		return 1;
//	case SDLK_F11:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (status.current_page == PAGE_ORDERLIST_PANNING) {
//				if (k->state == KEY_PRESS)
//					set_page(PAGE_ORDERLIST_VOLUMES);
//			} else {
//				if (k->state == KEY_PRESS)
//					set_page(PAGE_ORDERLIST_PANNING);
//			}
//		} else if (k->mod & KMOD_CTRL) {
//			if (k->state == KEY_PRESS) {
//				_mp_finish(NULL);
//				if (status.current_page == PAGE_LOG) {
//					show_about();
//				} else {
//					set_page(PAGE_LOG);
//				}
//			}
//		} else if (k->state == KEY_PRESS && (k->mod & KMOD_ALT)) {
//			_mp_finish(NULL);
//			if (song_toggle_orderlist_locked())
//				status_text_flash("Order list locked");
//			else
//				status_text_flash("Order list unlocked");
//		} else {
//			break;
//		}
//		return 1;
//	case SDLK_F12:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		if ((k->mod & KMOD_ALT) && status.current_page == PAGE_INFO) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_WATERFALL);
//		} else if (k->mod & KMOD_CTRL) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_PALETTE_EDITOR);
//		} else if (k->mod & KMOD_SHIFT) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS) {
//				fontedit_return_page = status.current_page;
//				set_page(PAGE_FONT_EDIT);
//			}
//
//		} else if (NO_MODIFIER(k->mod)) {
//			_mp_finish(NULL);
//			if (k->state == KEY_PRESS)
//				set_page(PAGE_SONG_VARIABLES);
//		} else {
//			break;
//		}
//		return 1;
//	/* hack alert */
//	case SDLK_f:
//		if (!(k->mod & KMOD_CTRL))
//			return 0;
//		/* fall through */
//	case SDLK_SCROLLOCK:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		_mp_finish(NULL);
//		if (k->mod & KMOD_ALT) {
//			if (k->state == KEY_PRESS) {
//				midi_flags ^= (MIDI_DISABLE_RECORD);
//				status_text_flash("MIDI Input %s",
//					(midi_flags & MIDI_DISABLE_RECORD)
//					? "Disabled" : "Enabled");
//			}
//			return 1;
//		} else {
//			/* os x steals plain scroll lock for brightness,
//			 * so catch ctrl+scroll lock here as well */
//			if (k->state == KEY_PRESS) {
//				midi_playback_tracing = (playback_tracing = !playback_tracing);
//				status_text_flash("Playback tracing %s",
//						  (playback_tracing ? "enabled" : "disabled"));
//			}
//			return 1;
//		}
//	default:
//		if (status.dialog_type != DIALOG_NONE)
//			return 0;
//		break;
//	}

	/* got a bit ugly here, sorry */
	i = k->sym;
	if (k->mod & KMOD_ALT) {
		switch (i) {
		case SDLK_F1: i = 0; break;
		case SDLK_F2: i = 1; break;
		case SDLK_F3: i = 2; break;
		case SDLK_F4: i = 3; break;
		case SDLK_F5: i = 4; break;
		case SDLK_F6: i = 5; break;
		case SDLK_F7: i = 6; break;
		case SDLK_F8: i = 7; break;
		default:
			return 0;
		};
		if (k->state == KEY_RELEASE)
			return 1;

		song_toggle_channel_mute(i);
		status.flags |= NEED_UPDATE;
		return 1;
	}

	/* oh well */
	return 0;
}

/* this is the important one */
void handle_key(struct key_event *k)
{
//	if (_handle_ime(k))
//		return;

	/* okay... */
//	if (!(status.flags & DISKWRITER_ACTIVE) && ACTIVE_PAGE.pre_handle_key) {
//		if (ACTIVE_PAGE.pre_handle_key(k)) return;
//	}

	if (handle_key_global(k)) return;
//	if (!(status.flags & DISKWRITER_ACTIVE) && menu_handle_key(k)) return;
//	if (widget_handle_key(k)) return;

	/* now check a couple other keys. */
	switch (k->sym) {
	case SDLK_LEFT:
		if (k->state == KEY_RELEASE) return;
//		if (status.flags & DISKWRITER_ACTIVE) return;
//		if ((k->mod & KMOD_CTRL) && status.current_page != PAGE_PATTERN_EDITOR) {
//			_mp_finish(NULL);
//			if (song_get_mode() == MODE_PLAYING)
//				song_set_current_order(song_get_current_order() - 1);
//			return;
//		}
		break;
	case SDLK_RIGHT:
		if (k->state == KEY_RELEASE) return;
//		if (status.flags & DISKWRITER_ACTIVE) return;
//		if ((k->mod & KMOD_CTRL) && status.current_page != PAGE_PATTERN_EDITOR) {
//			_mp_finish(NULL);
//			if (song_get_mode() == MODE_PLAYING)
//				song_set_current_order(song_get_current_order() + 1);
//			return;
//		}
		break;
	case SDLK_ESCAPE:
		/* TODO | Page key handlers should return true/false depending on if the key was handled
		   TODO | (same as with other handlers), and the escape key check should go *after* the
		   TODO | page gets a chance to grab it. This way, the load sample page can switch back
		   TODO | to the sample list on escape like it's supposed to. (The status.current_page
		   TODO | checks above won't be necessary, either.) */
//		if (NO_MODIFIER(k->mod) && status.dialog_type == DIALOG_NONE
//		    && status.current_page != PAGE_LOAD_SAMPLE
//		    && status.current_page != PAGE_LOAD_INSTRUMENT) {
//			if (k->state == KEY_RELEASE) return;
////			if (_mp_active) {
////				_mp_finish(NULL);
////				return;
////			}
////			menu_show();
//			return;
//		}
		break;
	case SDLK_SLASH:
		if (k->state == KEY_RELEASE) return;
//		if (status.flags & DISKWRITER_ACTIVE) return;
		if (k->orig_sym == SDLK_KP_DIVIDE) {
			kbd_set_current_octave(kbd_get_current_octave() - 1);
		}
		return;
	case SDLK_ASTERISK:
		if (k->state == KEY_RELEASE) return;
//		if (status.flags & DISKWRITER_ACTIVE) return;
		if (k->orig_sym == SDLK_KP_MULTIPLY) {
			kbd_set_current_octave(kbd_get_current_octave() + 1);
		}
		return;
	case SDLK_LEFTBRACKET:
		if (k->state == KEY_RELEASE) break;
//		if (status.flags & DISKWRITER_ACTIVE) return;
		if (k->mod & KMOD_SHIFT) {
			song_set_current_speed(song_get_current_speed() - 1);
//			status_text_flash("Speed set to %d frames per row", song_get_current_speed());
			if (!(song_get_mode() & (MODE_PLAYING | MODE_PATTERN_LOOP))) {
				song_set_initial_speed(song_get_current_speed());
			}
//		} else if ((k->mod & KMOD_CTRL) && !(status.flags & CLASSIC_MODE)) {
		} else if ((k->mod & KMOD_CTRL)) {
			song_set_current_tempo(song_get_current_tempo() - 1);
//			status_text_flash("Tempo set to %d frames per row", song_get_current_tempo());
			if (!(song_get_mode() & (MODE_PLAYING | MODE_PATTERN_LOOP))) {
				song_set_initial_tempo(song_get_current_tempo());
			}
		} else if (NO_MODIFIER(k->mod)) {
			song_set_current_global_volume(song_get_current_global_volume() - 1);
//			status_text_flash("Global volume set to %d", song_get_current_global_volume());
			if (!(song_get_mode() & (MODE_PLAYING | MODE_PATTERN_LOOP))) {
				song_set_initial_global_volume(song_get_current_global_volume());
			}
		}
		return;
	case SDLK_RIGHTBRACKET:
		if (k->state == KEY_RELEASE) break;
//		if (status.flags & DISKWRITER_ACTIVE) return;
		if (k->mod & KMOD_SHIFT) {
			song_set_current_speed(song_get_current_speed() + 1);
//			status_text_flash("Speed set to %d frames per row", song_get_current_speed());
			if (!(song_get_mode() & (MODE_PLAYING | MODE_PATTERN_LOOP))) {
				song_set_initial_speed(song_get_current_speed());
			}
//		} else if ((k->mod & KMOD_CTRL) && !(status.flags & CLASSIC_MODE)) {
		} else if ((k->mod & KMOD_CTRL)) {
			song_set_current_tempo(song_get_current_tempo() + 1);
//			status_text_flash("Tempo set to %d frames per row", song_get_current_tempo());
			if (!(song_get_mode() & (MODE_PLAYING | MODE_PATTERN_LOOP))) {
				song_set_initial_tempo(song_get_current_tempo());
			}
		} else if (NO_MODIFIER(k->mod)) {
			song_set_current_global_volume(song_get_current_global_volume() + 1);
//			status_text_flash("Global volume set to %d", song_get_current_global_volume());
			if (!(song_get_mode() & (MODE_PLAYING | MODE_PATTERN_LOOP))) {
				song_set_initial_global_volume(song_get_current_global_volume());
			}
		}
		return;

	default:
		break;
	}

	/* and if we STILL didn't handle the key, pass it to the page.
	 * (or dialog, if one's active) */
//	if (status.dialog_type & DIALOG_BOX) {
//		dialog_handle_key(k);
//	} else {
//		if (status.flags & DISKWRITER_ACTIVE) return;
//		if (ACTIVE_PAGE.handle_key) ACTIVE_PAGE.handle_key(k);
//	}
}


int midi_engine_handle_event(void *ev)
{
	struct key_event kk = {.is_synthetic = 0};
	int *st;
	SDL_Event *e = ev;

	if (e->type != SCHISM_EVENT_MIDI)
		return 0;

	st = e->user.data1;
	if (midi_flags & MIDI_DISABLE_RECORD) {
		free(e->user.data1);
		return 1;
	}

	switch (e->user.code) {
	case SCHISM_EVENT_MIDI_NOTE:
		if (st[0] == MIDI_NOTEON) {
			kk.state = KEY_PRESS;
		} else {
			if (!(midi_flags & MIDI_RECORD_NOTEOFF)) {
				/* don't record noteoff? okay... */
				break;
			}
			kk.state = KEY_RELEASE;
		}
		kk.midi_channel = st[1]+1;
		kk.midi_note = (st[2]+1 + midi_c5note) - 60;
		if (midi_flags & MIDI_RECORD_VELOCITY)
			kk.midi_volume = st[3];
		else
			kk.midi_volume = 128;
		kk.midi_volume = (kk.midi_volume * midi_amplification) / 100;
		handle_key(&kk);
		break;
	case SCHISM_EVENT_MIDI_PITCHBEND:
		/* wheel */
		kk.midi_channel = st[1]+1;
		kk.midi_volume = -1;
		kk.midi_note = -1;
		kk.midi_bend = st[0];
		handle_key(&kk);
		break;
	case SCHISM_EVENT_MIDI_CONTROLLER:
		/* controller events */
		break;
	case SCHISM_EVENT_MIDI_SYSTEM:
		switch (st[0]) {
		case 0x8: /* MIDI tick */
			break;
		case 0xA: /* MIDI start */
		case 0xB: /* MIDI continue */
			song_start();
			break;
		case 0xC: /* MIDI stop */
		case 0xF: /* MIDI reset */
			/* this is helpful when miditracking */
			song_stop();
			break;
		};
	case SCHISM_EVENT_MIDI_SYSEX:
		/* but missing the F0 and the stop byte (F7) */
		//len = *((unsigned int *)e->user.data1);
		//sysex = ((char *)e->user.data1)+sizeof(unsigned int);
		break;

	default:
		break;
	}
	free(e->user.data1);

	return 1;
}

