# The engine and its hosts

Schism Tracker's playback engine is built as a static library,
`libschismengine.a`, and two programs link it:

* `schismtracker`, the tracker, which adds the UI;
* `schismplay` (`./configure --enable-schismplay`), a command-line player
  and renderer with no UI at all.

The engine reaches the program it's linked into only through a table of
callbacks, `struct engine_host` in `include/engine-host.h`. It doesn't call
UI functions or read UI variables directly, and `make check-engine-boundary`
checks that (see "Boundary check" below).

## What's in the library

All of `player/` and `fmt/`, and from `schism/`: `atomic.c`,
`audio_loadsave.c`, `audio_playback.c`, `charset*.c`, `config-parser.c`,
`cpu.c`, `disko.c`, `dmoz.c`, `engine-host.c`, `ieee-float.c`, `loadso.c`,
`mem.c`, `midi-core.c`, `midi-ip.c`, `mplink.c`, `mt.c`, `sample-edit.c`,
`slurp.c`, `str.c`, `timer.c`, `util.c`, `version.c`. From `sys/`: the audio,
init, timer, thread and dmoz files of whichever SDL is configured, the
platform's native audio, MIDI, osdefs and dmoz files, `sys/posix`, and the
`sys/stdlib` replacements.

In `Makefile.am` each platform block has an engine list (`files_sdl3`,
`files_macosx`, ...) that goes into the library and a UI list (`ui_sdl3`,
`ui_macosx`, ...) that goes into `schismtracker_SOURCES`. The UI side is
the pages, widgets, dialogs, menus, video, fonts, keyboard and event
handling, clipboard, help text, `config.c`, `fakemem.c`, `main.c`, and the
tracker's end of the host interface, `schism/tracker-host.c`.

## The host interface

A program fills in a `struct engine_host` and calls
`engine_host_register()` before it initialises anything else. Members left
NULL get defaults that do nothing, except `exit` (calls `exit()`) and
`assert_fail` (prints and calls `abort()`). The engine calls them through
the global `engine_hooks`, whose members are never NULL.

Unless the table says otherwise, a callback runs on whatever thread called
into the engine, which for both programs is the main thread.

| Callback | When the engine calls it | schismtracker | schismplay |
|---|---|---|---|
| `log(color, flags, text)` | every `log_appendf()`, `log_append_timestamp()`, `log_nl()`, `log_perror()` line; `flags` can have `ENGINE_LOG_TIMESTAMP` | `log_host_append()` in page_log.c: the message log, or stdout with `--headless` | prints to stderr with `-v` |
| `log_underline()` | `log_underline()` | `log_host_underline()` in page_log.c | default (nothing) |
| `status_text(text)` | `status_text_flash()` | `status_text_host()` in status.c | prints to stderr with `-v` |
| `redraw()` | dmoz removed a file from the list being shown; MIDI input echoed on the MIDI page | sets `NEED_UPDATE` | default |
| `song_changed()` | `song_new()`, a song was loaded | `main_song_changed_cb()` in page.c | default |
| `song_mode_changed()` | playback started, stopped or paused | `main_song_mode_changed_cb()` in page.c, which calls every page's `song_mode_changed_cb` | default |
| `message_reset()` | a loaded song brought its own message | `message_reset_selection()` | default |
| `memused_changed()` | sample-edit.c changed sample data | `memused_songchanged()` | default |
| `audio_driver_changed()` | `audio_init()` opened a device | `preferences_audio_driver_changed()` | default |
| `playback_reset()` | play state reset on start, stop or loop; audio lock held | `pattern_editor_playback_reset()`: clears the pitch bend memory | default |
| `playback_stopped()` | `song_stop_unlocked()`, including when the song ends in the audio thread | `pattern_editor_playback_stopped()`: puts back the tracing setting MIDI recording changed | default |
| `post_event(ev)` | play position moved (`SCHISM_EVENT_PLAYBACK`, audio thread), MIDI input (`SCHISM_EVENT_MIDI_*`), network MIDI ports changed | `events_push_event()` | default (drops it; schismplay polls the play position) |
| `vis_resize(frames)` | the mixing buffer changed size (audio thread) | `vis_set_size()` | default |
| `vis_feed(buf, frames, bits, channels)` | every mixed buffer, `buf` NULL when nothing played (audio thread) | `tracker_vis_feed()`: hands it to the waterfall/FFT code if one of those views is showing | default |
| `midi_key(k)` | `midi_engine_handle_event()` turned a MIDI event into a key event | `handle_key()` | default |
| `midi_activity(port, data, len, outgoing)` | MIDI data in, or out while the monitor is showing; MIDI record lock held, may be a MIDI or timer thread | `tracker_midi_activity()`: fills in the MIDI page's last-event display | default |
| `midi_monitor_active()` | before reporting outgoing data, and to decide whether to echo input to the outputs | true on the MIDI page | default (0) |
| `config_dir()` | loading per-port MIDI settings | `cfg_dir_dotschism` | default (NULL: no per-port settings) |
| `diskwrite_begin(est_frames, rate)` | `disko_export_song()` started | opens the export progress dialog, whose cancel button calls `disko_export_cancel()` | takes one number from `rand()` (see below) |
| `diskwrite_progress(frames, est_frames, rate)` | after each `disko_sync()` block | updates the dialog | default |
| `diskwrite_end(canceled)` | the export finished, failed or was canceled | closes the dialog unless it closed itself | default |
| `exit(status)` | `audio_init()` couldn't open any device; Wii and Wii U power/home events | `schism_exit()` | shuts down and exits, status 1 if this happened while opening audio |
| `assert_fail(msg, exp, file, line)` | a `SCHISM_RUNTIME_ASSERT` failed | `tracker_assert_fail()`: writes the crash log and shows the message box | prints and aborts |

`tracker-host.c` builds the tracker's table; `schismplay.c` builds its own.

### Engine state

Settings the engine reads, which used to sit in the UI's `status.flags`, are
in a separate flags word owned by the engine, with the same names:
`CLASSIC_MODE`, `MAKE_BACKUPS`, `NUMBERED_BACKUPS`, `SONG_NEEDS_SAVE`,
`DISKWRITER_ACTIVE`, `DISKWRITER_ACTIVE_PATTERN`, `NO_NETWORK`,
`MIDI_LIKE_TRACKER` and `PLAY_AFTER_LOAD`. Use `engine_flag_test(mask)`
(returns the masked bits), `engine_flag_set()`, `engine_flag_clear()` and
`engine_flag_toggle()`. The tracker sets them from its config file and
command line. schismplay doesn't set any, which matches a fresh config;
the engine still sets `DISKWRITER_ACTIVE` and `SONG_NEEDS_SAVE` itself. `status.flags` keeps the flags only the UI uses.

### Editor commands live in the UI

Commands that need UI state (the current sample or instrument, a dialog, a
page switch) are in UI files and call engine functions for the actual work:
`song_load_instrument_with_prompt()` (page_loadinst.c),
the sample-to-instrument assignment after loading a sample
(page_loadsample.c), `song_pattern_to_sample()` (page_orderpan.c),
`song_get_current_instrument()` (page_patedit.c), and updating the stereo
toggle on the song variables page after `song_set_stereo()` and friends
(the callers do it). On macOS, the keyboard event filter and the iBook fn-key
handling are in `sys/macosx/ibook-support.c`; the tracker calls
`macosx_ibook_init()` and `macosx_ibook_quit()` around `os_sysinit()` and
`os_sysexit()`.

## Boundary check

    make check-engine-boundary        # also run by `make check`

or by hand:

    scripts/check-engine-boundary.sh libschismengine.a [UI objects...]

It does two things:

1. Lists the symbols `libschismengine.a` leaves undefined (with `nm -g -P`)
   and compares them with the global symbols the UI objects define
   (`schismtracker_OBJECTS`). Any symbol in both is reported and fails the
   check.
2. Links every object in the library (`-force_load` on macOS,
   `--whole-archive` elsewhere) with a `main()` that registers an empty
   `struct engine_host`, using schismplay's flags and libraries. Anything
   left undefined fails the link and the check.

On macOS arm64 with each of SDL 3, sdl2-compat and sdl12-compat:

    engine library: 806 symbols defined, 196 needed from outside
    ok: none of them is defined by the 51 UI objects
    ok: the whole library links on its own with the default callbacks

(197 and 50 UI objects with SDL 1.2, which has no clipboard backend.)

## Running schismplay

    schismplay song.it                         # play to the end
    schismplay -d disk -l 10 song.it           # play 10 s through SDL's disk driver
    schismplay -o out.wav song.it              # render, 44100 Hz 16-bit linear
    schismplay -o out.wav -r 48000 -b 24 -i polyphase -l 60 --seed 1 song.it

schismplay doesn't read the user's config file. It builds a `cfg_file_t` in
memory from its command line and passes it to `cfg_load_audio()`,
`cfg_load_midi()` and `cfg_load_disko()`, so everything it doesn't set gets
the same defaults as a fresh schismtracker config. `cfg_load_midi()` matters
for output: it fills in the default SFx/Zxx macros.

Renders go through the same disk writer as `schismtracker --diskwrite`.

    scripts/render-compare.py --corpus DIR --ext it,s3m --seed 1 \
        tracker=./schismtracker play=./schismplay

## Random numbers

The mixer's random numbers (IT instrument volume and pan swing, random LFO
waveforms) come from `csf_random()` in player/csndfile.c. Until
`csf_seed_random()` is called it returns `rand()`, so the default behaviour
and the libc sequence are what they always were. After seeding, the mixer
uses its own xorshift32 generator and nothing else in the program affects
it. Both programs take `--seed=N`.

Without `--seed`, both programs call `srand(time(NULL))`, and the mixer
shares libc's sequence with everything else that calls `rand()`. The
tracker's export dialog takes one number for the colour of its progress bar,
so schismplay's `diskwrite_begin` takes one too; that keeps unseeded renders
from the two programs identical for the same `time()`.

`disko_set_export_limit()` stops the next song export after a given number of
frames; schismplay's `--length` uses it. Multi-file (per-channel) exports
stop at the end of the buffer that crosses the limit.

## Verification (macOS arm64, SDL 3 / sdl2-compat / sdl12-compat)

`scripts/render-compare.py` renders every module with each program given,
hashes the PCM data and reports how many come out identical. Everything at
44100 Hz, 16-bit stereo, linear interpolation. "Before" is a build of the
branch point, before the split.

* schismtracker and schismplay build with each SDL; no new compiler
  warnings.
* 24 IT and 5 S3M modules, `--seed=12345`, to song end: before-schismplay,
  before-schismtracker, and the new schismplay and schismtracker from all
  three SDL builds are identical on 29/29.
* The same 29 without `--seed` and with `time()` pinned by an interposer:
  before and after, both programs, identical on 29/29.
* 45 MO3 files, schismplay `-l 30 --seed=12345`: before and the three new
  builds identical on 45/45. The 12 smallest, to song end with both
  programs: identical on 12/12.
* `make check-engine-boundary` passes with each SDL (output above).
* `schismtracker --diskwrite` writes the file, prints "Diskwrite complete,
  exiting..." and exits with status 0. Started with no song, the tracker
  exits with status 0 on SIGTERM. With a song loaded, SIGTERM opens the
  exit prompt, same as before the split, which can't be answered with the
  dummy video driver.

## Where later work would attach

These aren't implemented; this is where they'd go.

* **Host allocator with arena lifetimes.** Engine allocations go through
  `mem_alloc()`/`mem_calloc()`/`mem_realloc()` in schism/mem.c, plus direct
  `malloc`/`calloc` in the loaders and `csf_allocate()`. An `alloc`/`free`
  pair (taking an arena id: song, sample data, scratch) would be added to
  `struct engine_host`, mem.c would call it, and the direct calls would be
  moved over to mem.c. The default would be libc.
* **Sound-effect voices.** Keyjazz already plays notes outside the pattern
  data through `song_keydown()` in audio_playback.c, but on the song's own
  voices. A separate pool would sit next to `current_song->voices` and be
  mixed in `csf_read()`; the host interface needs nothing new except perhaps
  a callback when a voice ends.
* **Embedded host.** A host without SDL would provide its own audio backend
  (`schism_audio_backend_t`, include/backend/audio.h) and timer/thread
  backends, register a `struct engine_host`, and call `csf_read()` or let the
  backend's callback do it. Everything is still global (`current_song`, the
  flags word, `engine_hooks`); a `void *userdata` member would be the first
  step if a host ever needs more than one engine.

## Not covered

* Only macOS builds have been tested. The engine file lists for other
  platforms follow the old ones, but some of their platform files still call
  into the UI and would fail the boundary check there:
  `sys/win32/osdefs.c` (the menu bar, `cfg_video_want_menu_bar`,
  `schism_crash()` from the exception handler) and `sys/macos/osdefs.c`
  (`cfg_dir_dotschism`, `cfg_init_dir()`, `video_driver_name()`). Wii and
  Wii U use the host callbacks but haven't been built.
* Engine files still include some UI headers (`it.h` above all) for types and
  constants. That's a compile-time dependency only; the check above is about
  what gets linked.
* `os_sysinit()` on macOS still sets up the HID callback that reads the caps
  lock state, because osdefs.m also has things the engine needs. It's
  harmless in schismplay. Both programs link the same frameworks.
* With `--headless`, `log_append_timestamp()` in page_log.c sets the
  underline length of `lines[-1]`, since headless mode doesn't store lines,
  and the timestamped line itself is built as UCS-4 and passed to `puts()`,
  so only its first letter shows ("L" for "Loading ..."). Both predate this
  split and aren't fixed here.
