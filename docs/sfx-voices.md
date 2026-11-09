# The effects voice pool

The effects bus ([sfx-bus.md](sfx-bus.md)) plays raw PCM with a small mixer
of its own. The effects voice pool plays samples and instruments through
the same per-voice code the song uses: the engine's interpolation (nearest,
linear, cubic, polyphase FIR), resonant filters and filter envelopes,
volume, pan and pitch envelopes with their loops and sustain loops, sample
sustain loops released by a note-off, instrument fadeout, auto-vibrato,
volume and pan swing, pitch/pan separation, and the song's volume ramping.
A game can ship instruments made in the tracker and play them as sound
effects that sound the way they do in the tracker.

The API is in `include/sfx-voices.h` and the code in `player/sfx-voices.c`.

The two paths live side by side:

* **The bus** is the cheap path: one-shot or looped PCM, linear
  interpolation, gain and pan, a 2 ms ramp. It costs very little per voice
  and needs no banks.
* **The pool** is the full engine path: everything listed above, at about
  the cost of a song voice (numbers below). Effects come from banks, and a
  plain PCM effect is a bank sample with no instrument.

Neither uses the song's voices, and the song can't cut either of them off.

## Its own voices

The song mixes 64 pattern channels plus background (NNA) voices out of a
fixed array of 256 `song_voice_t`. When that fills up, the song culls its
quietest voices. The pool has its own array of `song_voice_t`, sized by the
host and allocated up front as ENGINE memory, so effects never compete with
the song for voices and the song's NNA handling never sees them.

Song-wide state doesn't reach pool voices either. The pool has its own:

| | song | pool |
|---|---|---|
| tick rate | the song's tempo and speed | `sfx_pool_set_tick_rate()`, default tempo 125 (50 ticks a second) |
| global volume | the song's, and Vxx/Wxx | always 128 |
| mixing volume | the song's | `sfx_pool_set_mixing_volume()`, default 48 |
| channel volume, pan, mutes | per channel | 64, from the note-on, none |
| random numbers | `csf_random()` | its own generator (`sfx_pool_set_seed()`) |
| interpolation | the engine setting | the engine setting, or `sfx_pool_set_interpolation()` |
| effects, MIDI macros, OPL | yes | no |

What the pool does share is the output: rate, channel count, ramp lengths,
and the reverse-stereo and no-surround settings.

## How it shares the song's code

The song's per-tick, per-voice update (envelopes, fadeout, vibrato, filter
setup, volume and ramp calculation) used to sit inside `csf_read_note()`
and read song-wide settings straight from `song_t`. It's now
`csf_voice_update()` in `player/sndmix.c`, which takes a
`struct csf_voice_env` holding the few settings it needs: global and
mixing volume, flags, mix rate and channels, interpolation, pan separation,
ramp lengths, tick length, and a random number source. The song fills one
in from itself every tick and the pool fills in its own. MIDI output, OPL
and Zxx macros only run when the env points at a song.

Likewise the per-voice part of `csf_create_stereo_mix()` is now
`csf_mix_voice()` in `player/mixer.c`, and the voice side of `fx_key_off()`
is `csf_voice_key_off()`. The song calls all three the way it ran the code
before, and renders of the test corpus are byte-identical.

A note-on sets up the voice the way `csf_instrument_change()` and
`csf_note_change()` set up an idle pattern channel for a note with an
instrument number and no effects, with Impulse Tracker's compatibility
behaviour throughout.

Each voice keeps its own tick clock, which starts on the note's first
sample. Its first tick runs at once, like a pattern note on its row, and
the following ones come a tick length apart after that. So a note's
envelopes line up with its start to the sample wherever the host's buffers
happen to split, and a note-off scheduled a whole number of ticks after the
start lands on a tick the way a pattern note-off does.

When a voice stops it leaves a small DC offset behind, the same as a song
voice. The song adds those to one accumulator that decays over the next
buffers. The pool ramps each one out on the voice itself, because the
decay rounds, and a shared accumulator would make the output depend on
where the host splits its buffers. The voice frees itself when the offset
reaches zero (a few thousand frames at most) and counts as playing until
then.

## Using it

    sfx_pool_init(256);                      /* voices, all allocated now */
    sfx_pool_set_steal(SFX_STEAL_QUIETEST);

    sfx_bank_t *bank = sfx_bank_load("effects.it");   /* or .iti, .xi, .wav, ... */
    sfx_note_t n = sfx_note_on(bank, 3, NOTE_MIDC, 48, 32);  /* instrument 3, C-5 */
    ...
    sfx_note_off(n);        /* key off: sustain loops let go, the fadeout runs */
    ...
    sfx_bank_free(bank);    /* cuts any of its notes first */
    sfx_pool_quit();

`sfx_note_on_ex()` takes a `struct sfx_note_args` (set it up with
`sfx_note_args_init()`) with the instrument or sample, the note, volume and
pan (0-64, or `SFX_DEFAULT` for the instrument's or sample's own), filter
cutoff and resonance (0-127, or `SFX_DEFAULT` for the instrument's), a
start delay in output frames, and a duration in frames after which the note
keys off. Delays and durations count frames the mixer has produced, so
they're exact in a render.

Notes are handles. A handle to a note that has ended stays invalid after
its voice is reused, and calls on it do nothing. `sfx_note_cut()` stops a
note with the song's short ramp, like `^^^` in a pattern.
`sfx_note_set_volume()`, `_pan()`, `_pitch()` (a multiplier on the note's
frequency), `_cutoff()` and `_resonance()` change a playing note from its
next tick. `sfx_note_get_info()` reports whether it's waiting, released or
fading, its volume after envelopes, its pan and its sample position.

### Banks

A bank is a copy of some samples and instruments in ENGINE memory. It
doesn't depend on the song: it survives song changes and can be loaded
before any song is.

* `sfx_bank_load()` takes a module (every sample, and every instrument if
  the module uses them), an instrument file such as `.iti` or `.xi` (as
  instrument 1, with its samples), or anything Schism loads as a sample
  (`.its`, `.wav`, `.flac` and so on, as sample 1 with no instruments).
* `sfx_bank_from_song()` copies the current song's samples and instruments.

Instrument and sample numbers are the source's. `sfx_note_on()` takes an
instrument number if the bank has instruments and a sample number if not;
`sfx_note_on_ex()` can play a bank's samples directly either way.

The instrument loaders only know how to load into the current song, so
`sfx_bank_load()` swaps a scratch song in for the length of an instrument
file's load, with the audio thread locked out. Load banks from the host
thread at a time when a short pause in the audio is acceptable, and not
with the audio lock held.

On the arena allocator a bank's own memory is ENGINE, but loading a module
or instrument file borrows space in the SONG region for the scratch song
until the next song load empties it. Load bank files before the song.

### Instruments in a pool

Each note-on takes its own voice, so an instrument's New Note Action
doesn't come into it: earlier notes always carry on, as with NNA continue.
Cut them through their handles if you need to. Duplicate check doesn't
apply for the same reason.

### Full pools

`sfx_pool_set_steal()` picks what a note-on does when every voice is busy:

* `SFX_STEAL_NONE` (the default): the note doesn't start.
* `SFX_STEAL_OLDEST`: the note that started first is cut.
* `SFX_STEAL_QUIETEST`: the note with the lowest volume after envelopes and
  fadeout is cut, among those that have started. One still waiting out its
  delay is only taken if nothing has started.

A stolen voice stops at once. Its last output ramps out through a DC
offset accumulator the pool keeps for that, so it doesn't click.

## Where it mixes

The pool mixes in the same place as the bus: in `csf_read()` after the
song's EQ, and in `csf_read_sfx()` over silence when the song is stopped or
has ended partway through a buffer. It mixes its voices into a scratch
buffer, applies the group volume (`sfx_pool_set_volume()`, 16.16 fixed
point, skipped at 1.0) and adds the result to the mix buffer before the
final clip and conversion. Live playback (`audio_playback.c`) and the disk
writer (`disko.c`) set `song_t.sfx_pool` and keep calling `csf_read_sfx()`
while either the bus or the pool is busy. Per-channel exports leave both
out. With no notes playing the pool isn't touched, so the output is
byte-identical to a build without it.

## Threads

The same rules as the bus. The host calls the pool's functions from one
thread. Each takes the audio lock while it touches voices, and the mixer
calls `sfx_pool_mix()` with the lock held. Voices are allocated in
`sfx_pool_init()`. Nothing allocates on the audio thread, and nothing
allocates with the lock held except the scratch song an instrument-file
bank is loaded into (the tracker's own instrument loading does the same).

## schismplay

    schismplay [-o out.wav] [--sfx-bank FILE]... [--sfx-bank-song] \
        [--sfx-note B:I:NOTE@SECONDS[:VOL[:PAN[:DURATION]]]]... \
        [--sfx-pool N] [--sfx-steal none|oldest|quietest] \
        [--sfx-tick-rate TEMPO] [--sfx-pool-volume V] [--sfx-pool-interp MODE] \
        [--sfx-only] [song]

* `--sfx-bank` loads a bank. Banks are numbered in the order given, from 0,
  or from 1 if `--sfx-bank-song` makes the loaded song bank 0.
* `--sfx-note` plays instrument `I` (or sample `N`, written `sN`) of bank
  `B` at the given time. `NOTE` is `C-5`, `C#5` or 1-120. `VOL` and `PAN`
  are 0-64 and can be left empty for the defaults. `DURATION` is seconds to
  the note-off.
* `--sfx-pool` sizes the pool (64 by default). The pool is only set up if
  one of these options is given.
* `--sfx-only` renders or plays the effects without the song.
* `--sfx-pool-stress SECONDS` makes random note-on, note-off, cut and setter
  calls from the main thread during playback, for thread testing.
* With `-v`, a render reports the most notes the pool had going at once.

In a render, notes are sorted by time and each one is started during the
disk writer buffer it falls in, with a delay in frames, so it lands on its
exact sample and notes that are still waiting don't fill the pool early.

## Checking it

`scripts/sfx-voices-equiv.py SCHISMPLAY` writes small IT modules (an
instrument with sustained volume, pan and pitch or filter envelopes,
fadeout, auto-vibrato, sample sustain and ping-pong sustain loops, resonant
filters; cubic, polyphase and linear interpolation; tempo 125 and 150),
renders each note as a song and through the pool with the pool's tick rate
set to the module's tempo, and compares them at 32 bits. The modules use
global volume 128, mixing volume 48, channel volume 64 and a centred
channel, which are the values the pool uses, and the outputs match
exactly.

`test/cases/sfx-voices.c` covers sample notes and pan, delays and
automatic note-offs, the tick rate, steal policies, handles and bank
freeing, and checks that splitting the output into different buffer sizes
gives the same samples.

The cost scales with the voice count. Rendering held notes with
`--sfx-only` on an M1 Pro (one core, `-O2`) takes about 0.1 ms of CPU per
voice per second of audio with linear interpolation and about 0.17 ms with
cubic or polyphase, so 1024 voices use roughly 10-18% of a core.
