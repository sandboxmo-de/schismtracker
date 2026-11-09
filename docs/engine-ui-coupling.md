# Engine to UI coupling, and how schismplay deals with it

`schismplay` is a second program built from this tree (`./configure
--enable-schismplay`). It contains the player (`player/`), the loaders
(`fmt/`), the song, playback and disk writer code from `schism/`, and the
audio, timer, thread and filesystem backends from `sys/`. It doesn't contain
pages, widgets, video, fonts, keyboard or mouse handling, dialogs, menus, the
clipboard or help text.

The engine files still refer to a number of UI symbols. This document lists
them, says which ones `schismplay` satisfies with a stub
(`schism/schismplay-stubs.c`), and which ones needed a hook in the engine
instead.

## Building and running

    ./configure --enable-schismplay [--with-sdl3 | --with-sdl2 | --with-sdl12]
    make

    schismplay song.it                         # play to the end
    schismplay -d disk -l 10 song.it           # play 10 s through SDL's disk driver
    schismplay -o out.wav song.it              # render, 44100 Hz 16-bit linear
    schismplay -o out.wav -r 48000 -b 24 -i polyphase -l 60 --seed 1 song.it

    scripts/render-compare.py --corpus DIR --ext it,s3m --seed 1 \
        tracker=./schismtracker play=./schismplay

## How the list was made

Build the full program, split the object files into a UI set and an engine
set, link the engine set with an empty `main()`, and look at what's left
undefined. With the SDL 3 build on macOS that's 44 symbols; the SDL 1.2 and
SDL 2 builds give the same list apart from the backend names.

UI set (left out of schismplay): `schism/page*.c`, `widget*.c`, `dialog.c`,
`menu.c`, `keyboard.c`, `video.c`, `vgamem.c`, `fonts.c`, `itf.c`, `status.c`,
`palettes.c`, `pattern-view.c`, `xpmdata.c`, `clippy.c`, `main.c`, `events.c`,
`config.c`, `fakemem.c`, `auto/helptext.c`, `auto/default-font.c`,
`schism/auto/schismico*.c`, `sys/*/video.c`, `sys/*/events.c`,
`sys/*/clippy.c`, `sys/x11/*`, `sys/macosx/{macosx-sdlmain.m,ibook-support.c,clippy.m}`.

Engine set (linked): all of `player/` and `fmt/`, plus `schism/atomic.c`,
`audio_loadsave.c`, `audio_playback.c`, `charset*.c`, `config-parser.c`,
`cpu.c`, `disko.c`, `dmoz.c`, `ieee-float.c`, `loadso.c`, `mem.c`,
`midi-core.c`, `midi-ip.c`, `mplink.c`, `mt.c`, `sample-edit.c`, `slurp.c`,
`str.c`, `timer.c`, `util.c`, `version.c`, and from `sys/` the audio, init,
timer, mt and dmoz files of whichever SDL is configured, the native audio,
MIDI, osdefs and dmoz files of the platform, and `sys/posix`.

`config.c`, `events.c` and `fakemem.c` look like engine files but are mostly
UI glue, so they're left out and the few symbols the engine wants from them
are stubbed. `midi-core.c` is linked even though schismplay never starts the
MIDI engine, because `cfg_load_midi()` is what fills in `default_midi_config`
(the SFx/Zxx macro defaults). Without it, songs that use Zxx filter macros
render differently.

## 1. Inside player/

| Where | Symbol | What schismplay does |
|---|---|---|
| player/effects.c:894, player/snd_gm.c:121-172 | `status.flags & MIDI_LIKE_TRACKER` | stub: `status` is defined in the stub file, all zero, which matches a fresh config (`midi_like_tracker = 0`) |
| player/effects.c, player/sndmix.c | `rand()` | hook, see section 8 |

## 2. Logging

| Where | Symbol (page_log.c) | What schismplay does |
|---|---|---|
| 22 loaders, csndfile.c, audio_loadsave.c, audio_playback.c, disko.c | `log_appendf`, `log_appendf_timestamp`, `log_append_timestamp` | stub: printed to stderr with `-v`, dropped otherwise |
| audio_loadsave.c, audio_playback.c | `log_nl`, `log_underline` | stub: no-op |
| audio_loadsave.c, disko.c, dmoz.c | `log_perror` | stub: always printed to stderr |

Because page_log.c isn't linked, schismplay can't hit the `log_underline()`
out-of-bounds write that makes `schismtracker --headless` crash (see "Upstream
issues" below).

## 3. Status bar and page state

| Where | Symbol | What schismplay does |
|---|---|---|
| audio_loadsave.c, audio_playback.c, disko.c | `status_text_flash` | stub: stderr with `-v` |
| audio_loadsave.c | `main_song_changed_cb`, `message_reset_selection` | stub: no-op |
| audio_playback.c:437 | `pages[n].song_mode_changed_cb` | stub: `pages[PAGE_MAX]` defined, all zero, so every callback is NULL |
| audio_loadsave.c, disko.c | `set_page`, `instrument_list_subpage` | stub: no-op / plain int |
| audio_loadsave.c, mplink.c, disko.c | `instrument_get_current`, `sample_get_current` | stub: return 1; only reached from editor commands |
| mplink.c | `song_vars_sync_stereo` | stub: no-op |
| audio_playback.c:1637 | `preferences_audio_driver_changed` | stub: no-op |
| audio_playback.c:1642 | `schism_exit` | defined in schismplay.c: shuts down and exits, with an error message and status 1 if it's called while opening the audio device |
| audio_playback.c | `playback_tracing`, `midi_playback_tracing`, `midi_last_bend_hit` | stub: variables defined, zero |
| audio_loadsave.c, sample-edit.c | `memused_songchanged` (fakemem.c) | stub: no-op |
| everywhere | `status` (page.c) | stub: zeroed `struct tracker_status` |

## 4. Visualiser feed in the audio callback

`vis_set_size` and `vis_work_{8,16,32}{m,s}` (page_waterfall.c) are stubbed as
no-ops. The `vis_work_*` calls only happen when `status.current_page` is the
waterfall page or `status.vis_style` is FFT, and neither is ever true in
schismplay.

## 5. Disk writer dialog

| Where | Symbol | What schismplay does |
|---|---|---|
| disko.c | `dialog_create_custom` | stub: returns a static `struct dialog`, because disko.c stores its cancel callbacks in it |
| disko.c, audio_loadsave.c | `dialog_create`, `dialog_destroy`, `dialog_destroy_all` | stub |
| disko.c | `draw_text`, `draw_fill_chars`, `draw_vu_meter`, `draw_box` | stub: no-op |

disko.c itself is linked unchanged apart from the export length limit below,
so schismplay renders go through exactly the same code as `--diskwrite`,
including the `rand()` call that picks the progress bar colour (which matters
when the mixer still uses `rand()`, see section 8).

## 6. Config, events, MIDI input, macOS

| Where | Symbol | What schismplay does |
|---|---|---|
| midi-core.c | `cfg_dir_dotschism` (config.c) | stub: NULL; only used for per-port MIDI config, which is never loaded |
| midi-core.c | `handle_key` (page.c) | stub: no-op; MIDI input isn't started |
| audio_playback.c, midi-core.c | `events_push_event` (events.c) | stub: drops the event; schismplay polls the play position instead |
| sys/macosx/osdefs.m | `macosx_ibook_fnswitch` (ibook-support.c) | stub: returns -1, which tells osdefs.m there's nothing to restore |
| sys/macosx/osdefs.m | `status`, `pages` via `ACTIVE_PAGE` | the stubs above; the key handler that uses them is never called |
| everywhere via `SCHISM_RUNTIME_ASSERT` | `schism_assert_fail` (main.c) | defined in schismplay.c: prints and aborts |

Settings: schismplay doesn't read the user's config file. It builds a
`cfg_file_t` in memory from its command line and passes it to
`cfg_load_audio()`, `cfg_load_midi()` and `cfg_load_disko()`, so everything it
doesn't set gets the same defaults as a fresh schismtracker config.

## 7. Engine hooks added

Two small hooks in engine files. Neither changes schismtracker's output unless
it's used.

* **Mixer random numbers** (`csf_seed_random()`, `csf_random()` in
  player/csndfile.c, declared in player/sndfile.h). The six `rand()` calls in
  player/effects.c and player/sndmix.c now call `csf_random()`. Until
  `csf_seed_random()` is called it just returns `rand()`, so the default
  behaviour (and the libc sequence) is the same as before. After seeding, the
  mixer uses its own xorshift32 generator scaled to 0..RAND_MAX, and the rest
  of the program's `rand()` calls no longer affect it. Both programs take
  `--seed=N` to turn it on.
* **Export length limit** (`disko_set_export_limit()` in schism/disko.c).
  Stops the next song export after a given number of frames. schismplay uses
  it for `--length`. The default of 0 leaves exports alone. Multi-file
  (per-channel) exports stop at the end of the buffer that crosses the limit
  rather than on the exact frame.

## 8. Shared libc PRNG

Before the hook above, the mixer drew from the process-wide `rand()`:

| Where | Used for |
|---|---|
| player/effects.c (instrument change) | IT instrument random volume / pan swing on note start |
| player/effects.c (panbrello, tremolo), player/sndmix.c (vibrato, auto-vibrato) | random LFO waveform |

`main.c` seeds it with `srand(time(NULL))`, and the UI draws from the same
stream (disko.c progress bar colour, page_about.c, page_patedit.c, util.c's
mkstemp replacement). So any song with non-zero RV/RP on an instrument, or a
random waveform command, rendered differently from one second to the next.

In a 29-module regression set (24 IT, 5 S3M), 17 modules are affected: their
output changes between two unseeded runs a few seconds apart, and between
`--seed=1` and `--seed=2`, and the two sets of 17 are the same. The other 12
don't change with either.

## Verification (macOS arm64, sdl12-compat / sdl2-compat / SDL 3)

`scripts/render-compare.py` renders every module with each program given,
hashes the PCM data and reports how many come out identical. All runs at
44100 Hz, 16-bit stereo, linear interpolation, to song end.

* schismplay and schismtracker build with `--with-sdl12`, `--with-sdl2` and
  `--with-sdl3` (one SDL each). On this machine the SDL 2 and SDL 1.2
  libraries are sdl2-compat and sdl12-compat running on SDL 3, so the
  separate code paths in `sys/` are exercised but native SDL 2 isn't.
* With `time()` pinned by an out-of-tree interposer and no `--seed`: all six
  programs (schismtracker and schismplay, three backends each) give identical
  PCM on 29/29 modules, and all six match hashes rendered by unmodified
  upstream schismtracker on 29/29.
* With `--seed=12345`, nothing pinned: all six identical on 29/29, and two
  runs give the same hashes. 12/29 match the unmodified-upstream reference;
  the 17 that don't are exactly the RNG-sensitive ones above, which is
  expected since the seeded generator isn't libc's.
* `nm -g -U schismplay` against the global symbols defined by the UI objects
  of the same build: the only names in common are the stubs listed here plus
  `main`, `schism_exit` and `schism_assert_fail`. No UI object is in
  `schismplay_OBJECTS`.
* Real-time play through SDL's `disk` audio driver (`-d disk -l 3`) runs for
  3 seconds of wall time on each backend, prints the order and row moving
  forward, and the driver's output file holds 134144 non-silent frames.

## Upstream issues seen along the way (not fixed here)

* `log_underline()` (page_log.c) reads `lines[-1]` in headless mode, and
  `log_underline_impl` writes below its buffer when the length is 0.
  `schismtracker --headless --diskwrite=...` segfaults in `song_load_unchecked`
  because of it. The non-headless `--diskwrite` path works.
* Shutdown race: `schism_exit()` calls `midi_engine_stop()` before
  `audio_quit()`. The audio thread can still be in `audio_callback()` calling
  `midi_need_flush()`, which locks a mutex that was just destroyed. With the
  dummy SDL drivers, most `--diskwrite` runs segfault after "Diskwrite
  complete"; the WAV is already finished by then. schismplay closes audio
  first and never starts the MIDI engine, and hasn't crashed on exit.
* In non-headless `--diskwrite` mode a file that fails to load drops into
  `event_loop()` and never exits. schismplay exits with status 1.

## Not covered

* The schismplay file lists for Windows, classic Mac OS, Wii, Wii U, OS/2 and
  Xbox are written by analogy with schismtracker's and haven't been built.
* On macOS, `os_sysinit()` still sets up the HID keyboard callback in
  osdefs.m, because that file also provides things the engine needs. It's
  harmless but it is keyboard code.
* schismplay links the same frameworks as schismtracker on macOS (Cocoa
  included) because osdefs.m and dmoz.m use them.
