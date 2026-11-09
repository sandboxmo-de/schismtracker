# Engine memory

The playback engine (`libschismengine.a`, see [engine.md](engine.md)) gets
its memory from an allocator that the host registers. On the desktop that's
the libc heap, and nothing changes. A host without a general-purpose heap
can hand the engine a few fixed regions instead. This page covers what the
engine allocates and when, which parts of the library are held to that rule,
the allocator interface, the arena allocator that ships with it, and the
checks that keep it that way.

## Lifetime classes

Every request carries one of three classes (`include/engine-alloc.h`). The
class says how long the block lives, so an allocator can put each class in
its own region and empty a region in one go.

**ENGINE** lives until the program exits, or until the host explicitly frees
it. It's small and mostly allocated once at startup:

- the `song_t` structure itself (`csf_allocate`), which is reused from one
  song to the next;
- the mixing buffer and the audio device buffer, sized when the device opens
  (`audio_playback.c`);
- the audio driver and device lists and names, and the results of
  `audio_parse_driver_spec`;
- the OPL emulator chip (`fmopl2.c`, `fmopl3.c`);
- the effects bus voice pool and every effect source (`sfx-bus.c`);
- the strings a format loader's `read_info` puts in a `dmoz_file_t` (title,
  artist, `smp_filename`). These only come up when the tracker browses
  files.

**SONG** is the loaded module: patterns, sample data, instruments and the
edit history. `csf_allocate` and `csf_reinit` call `engine_song_begin()`
before the first SONG allocation, and `csf_free` and `csf_reinit` call
`engine_song_end()` once everything in SONG has been freed. It's by far the
biggest class, and sample data makes up most of it.

**SCRATCH** is for temporaries that are gone before the operation that made
them returns:

- file buffers from `slurp` while a module or sample loads, and the
  decompression buffers for compressed files and MO3;
- memory streams (`disko_memopen`) during export and charset conversion;
- converted and case-folded strings from the charset code;
- formatted log and status lines (`engine-host.c`, `util.c`).

Loading a song or sample and exporting are bracketed with
`engine_scratch_begin()` and `engine_scratch_end()`, and the brackets nest.
An allocator may assume every SCRATCH block made inside a bracket is free at
the matching end. SCRATCH allocations outside a bracket are allowed (log
lines, for instance) and are freed one by one.

A load fails if any allocation failed while the loaders ran, even one a
loader didn't check: `load_song_slurp` compares `engine_alloc_failures()`
before and after.

## Which objects are core

`libschismengine.a` is split in two. The core engine allocates only through
`engine-alloc.h`. The rest of the library may still use `malloc()` and
`free()`, because it's code a host without a general heap leaves out or
replaces anyway. `engine_heap_sources` in `Makefile.am` lists that rest;
everything else in the library is core.

Excluded, and why:

| Source | Why it may use the libc heap |
|---|---|
| `schism/mem-libc.c` | It's the default allocator, so it's the one place that's meant to call `malloc()`. |
| `schism/libc-owned.c` | Helpers that hand `malloc()` memory to the tracker and platform code (see below). |
| `schism/mem.c` | The aborting `mem_alloc` family. The tracker and platform code use it; core code doesn't. |
| `schism/config-parser.c` | Config files. A host without a filesystem has none. |
| `schism/dmoz.c` | Directory listing for the file browser. |
| `schism/loadso.c` | Dynamic loading of optional libraries. |
| `schism/midi-core.c`, `schism/midi-ip.c` | MIDI port management and network MIDI. |
| `schism/mt.c`, `schism/timer.c` | Thread and timer backends, whose objects the platform allocates. |
| `sys/posix/osdefs.c` and the `files_*` groups | Platform backends (SDL, macOS, Win32, OS/2, Wii, Xbox and so on), mmap file reading, and the third-party decoders and decompressors (FLAC, libavformat, GStreamer, Media Foundation, zlib, bzip2, xz, zstd). They pass memory to libraries that call `malloc()` themselves, or belong to a platform that has a heap. |

Everything else is core: the module and sample loaders in `fmt/` apart from
the third-party ones above, all of `player/` including `sfx-bus.c`, and in
`schism/` the load/save and playback glue (`audio_loadsave.c`,
`audio_playback.c`), `charset*.c`, `disko.c`, `slurp.c`, `str.c`, `util.c`,
`sample-edit.c`, `mplink.c`, `engine-host.c`, `engine-alloc.c` and
`mem-arena.c`. With the default configure options on macOS that's 76
objects.

`make check-engine-boundary` checks this. Besides the symbol and link checks
described in engine.md, it runs `nm` on each core object and fails if one
refers to `malloc`, `calloc`, `realloc`, `free`, `strdup`, `asprintf` and the
like, to the `mem.c` wrappers, or to any of the libc-owned helpers below.

### Helpers whose results the caller frees with free()

A handful of helpers return memory that the caller releases with `free()`,
and most of those callers are tracker pages and platform files, some of
which can't be built on any one machine. So these keep handing out
`malloc()` memory whichever allocator is registered, and live in
`libc-owned.c`:

- `charset_iconv` (and the inline `charset_iconv_easy`),
  `charset_compose_to_set`, `charset_case_fold_to_set`;
- `disko_memclose(ds, 1)`, which leaves the buffer in `ds->data`;
- `str_break`, `str_escape`, `str_unescape`, `str_concat`, `str_realloc`.

Core code uses `charset_iconv_engine`, `charset_iconv_engine_len`,
`charset_compose_to_set_engine`, `charset_case_fold_to_set_engine` and
`disko_memclose_engine`. Their results are SCRATCH memory, freed with
`engine_free(p, ENGINE_MEM_SCRATCH)`. The libc-owned versions call these and
copy the result into a `malloc()` block.

Two structures cross the line, and their headers (`dmoz.h`, `song.h`) say
who owns what:

- In a `dmoz_file_t`, `path` and `base` are `malloc()` blocks that dmoz frees
  with `free()`. `title`, `artist` and `smp_filename` are set by the format
  loaders, so they're ENGINE memory and dmoz frees them with
  `engine_free()`. Anything else that sets them has to use
  `engine_strdup(..., ENGINE_MEM_ENGINE)` or similar. `smp_filename` may
  point at `base` or `title`, and isn't freed then.
- `audio_parse_driver_spec` returns ENGINE memory.

## The allocator interface

```c
struct engine_allocator {
	void *ctx;
	unsigned int flags;   /* ENGINE_ALLOC_ONE_SONG */
	void *(*alloc)(void *ctx, size_t size, size_t align, enum engine_mem_class cls);
	void *(*realloc)(void *ctx, void *ptr, size_t size, size_t align, enum engine_mem_class cls);
	void (*free)(void *ctx, void *ptr, enum engine_mem_class cls);
	void (*song_begin)(void *ctx), (*song_end)(void *ctx);       /* optional */
	void (*scratch_begin)(void *ctx), (*scratch_end)(void *ctx); /* optional */
};
```

The host calls `engine_allocator_register()` before anything in the engine
allocates, and doesn't switch while blocks are live, since a block is always
freed through whichever allocator is registered at the time. `free` gets the
class the block was allocated with. `realloc` never sees a NULL pointer or a
zero size, and leaves the old block alone when it fails.

Engine code calls `engine_alloc`, `engine_calloc`, `engine_realloc`,
`engine_alloc_aligned`, `engine_free`, `engine_strdup`, `engine_strndup` and
`engine_asprintf`. None of them abort. They return NULL and the caller copes.

`ENGINE_ALLOC_ONE_SONG` tells the engine the allocator can't hold two songs
at once. The tracker normally loads a new song next to the old one and swaps
them when the load succeeds. With the flag set, the load path
(`song_load_in_place`) empties the current song first and loads into it, so
`song_end` always comes before the next `song_begin`. A failed load then
leaves an empty song rather than the old one.

## The arena allocator

`schism/mem-arena.c` is the reference allocator for hosts without a heap.
`mem_arena_init()` takes one block and splits it into SONG, SCRATCH and
ENGINE regions. Sizes given as 0 share whatever's left: ENGINE gets
`MEM_ARENA_DEFAULT_ENGINE` (256 KB), or half of what's left if that's less,
and SONG and SCRATCH split the remainder 4:3. ENGINE gets a fixed size
because what the engine keeps there hardly changes from song to song.
`mem_arena_get_allocator()` fills in a `struct engine_allocator` with
`ENGINE_ALLOC_ONE_SONG` set.

Each region is a bump allocator. A block has a small header with its start
offset, size, a link to the previous block and a freed flag. Freeing the
newest block moves the top back down, past any blocks under it that were
already freed. Freeing an older block only marks it, and its space comes
back once everything above it is gone. `realloc` of the newest block grows
or shrinks it in place, which suits the memory streams and sample loaders
that build a buffer up a piece at a time.

`song_end` empties the SONG region once no song is open. `scratch_begin`
records the SCRATCH top, and `scratch_end` cuts the region back to it. Both
count blocks that were still live at that point (`song_leaks`,
`scratch_leaks`), which `schismplay --mem-stats` prints. The ENGINE region is
never emptied.

The arena never calls the libc heap. `test/cases/mem-arena.c` covers it.

## Setting up a host without a general-purpose heap

1. Reserve one block of memory for the engine and pick the region sizes
   (next section).
2. Call `mem_arena_init()` and `mem_arena_get_allocator()`, then
   `engine_allocator_register()`, before any other engine call. Or write your
   own allocator against `engine-alloc.h`, for instance to put SONG in a
   large, slower memory and the rest in fast internal memory.
3. Call `song_initialise()` and check its result. It returns -1 if the song
   structure doesn't fit in ENGINE, and nothing else in the engine works
   after that, so report it and stop.
4. Build the core objects plus whatever from the excluded list your platform
   has a use for. `make check-engine-boundary` tells you if a core object
   picked up a libc heap call.
5. Load sound effects once, after the engine is up, and keep them. They're
   ENGINE memory, and the arena only gets ENGINE space back when the newest
   blocks are freed.
6. A song that doesn't fit fails to load: the loader returns an error, the
   song is left empty, and SONG and SCRATCH are back where they started.

### Choosing region sizes

**ENGINE** barely depends on the song. `song_engine_mem_minimum()`
(`song.h`) returns what the engine itself keeps there: the song structure
and the OPL chip, plus `SONG_ENGINE_BLOCK_OVERHEAD` (64 bytes) per block for
the allocator's headers. On a 64-bit macOS build that's 192400 bytes, nearly
all of it `song_t` (178072) and the OPL3 chip (14200). It's computed from
`sizeof`, so it's right for whatever you build. On top of that, add:

- the effects pool, about 136 bytes per voice on a 64-bit build (8.7 KB for
  `sfx_bus_init(64)`), plus the effect sources you load;
- for live output, the mixing buffer and the device's own buffer, each
  samples × channels × bytes per sample, and a few hundred bytes of driver
  and device names;
- the `dmoz_file_t` strings, if the host browses files.

`schismplay` measured an ENGINE peak of about 201 KB rendering with the
default 64 effect voices, and about 224 KB playing live through the dummy
driver with 1024-sample buffers. The 256 KB default covers both. If ENGINE
is smaller than `song_engine_mem_minimum()`, `song_initialise()` fails.
Most of the other ENGINE allocations fail without stopping anything (no
OPL chip just means AdLib instruments are silent), so after a test run,
check the region's `fails` count as well.

**SONG** has to hold the biggest module you'll play, sample data and all.
Over the test corpus the median SONG peak was about 3.3 MB, but one outlier
needed 161 MB, so it depends entirely on what you play. **SCRATCH** has to
hold the biggest temporary a load makes: the file itself while it's read,
plus decompression buffers for compressed files and MO3. Its median peak
was about 0.7 MB, and it scales with file size more than with the song in
memory.

The simplest way to pick SONG and SCRATCH is to measure the files you'll
ship with `scripts/mem-stats.py` (below), take the largest peak of each
class and add some margin. The arena's peaks include block headers and
alignment, so they're the sizes a region needs.

## The audio thread

Nothing on the audio thread allocates or frees. The mixer (`csf_read`,
`csf_read_sfx`, `sfx_bus_mix`) only uses memory that was set up beforehand:

- the mix buffer and the device buffer are sized when the device opens, not
  on the first callback;
- a pattern that's missing or has no rows plays from the shared const
  `blank_pattern` instead of being allocated on the spot;
- the effects bus allocates and frees only on the host's thread, and never
  with the audio lock held.

Anything that changes song memory takes the audio lock first, as before.

## Generated tables

The lookup tables the mixer and the OPL emulators use are `const` arrays
generated ahead of time, so they take no RAM and no startup work:

- `include/player/precomp_lut.h` (interpolation) from `scripts/lutgen.c`;
- `include/player/fmopl2_tables.h` and `fmopl3_tables.h` (total level and
  sine tables) from `scripts/gen-opl-tables.c`, which uses the same
  arithmetic as the emulators' old `init_tables()` and gives bit-identical
  output.

`make check-tables` (also part of `make check`) builds the generators with
`CC_FOR_BUILD`, or `cc` if that's unset, runs them and compares the output
with the committed headers. If you change a generator, run it the same way
and commit the new header.

## Measuring

`schismplay` has three options for this:

    schismplay --arena SIZE[,SONG,SCRATCH,ENGINE] ...   # run on the arena
    schismplay --mem-stats ...                          # peak use per class
    schismplay --mem-limit SIZE ...                     # libc, fail past SIZE

With `--arena`, `schismplay` refuses to start if the ENGINE region is
smaller than `song_engine_mem_minimum()`, and it exits 1 after a run in
which any ENGINE allocation failed, since those failures don't stop
playback by themselves. Both cases print the region size.

`--mem-stats` prints the peak, current use and live block count of each
class after loading, after playing or rendering, and at exit. On the arena
it also prints the region sizes, failed requests, and blocks that were still
live at `song_end` or `scratch_end`. `--mem-limit` puts a counter around the
libc allocator, which is handy for out-of-memory testing under sanitizers.

`scripts/mem-stats.py` runs `schismplay --arena --mem-stats` on every module
in a directory and prints the peak of each class per file, then the max and
median:

    scripts/mem-stats.py --corpus DIR --ext it,s3m,mo3 --arena 1G --length 60 \
        path/to/schismplay

The peaks are what the arena handed out, block headers and alignment padding
included, so they're the region sizes a host would need for those files.
