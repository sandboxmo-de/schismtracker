# The sound effects bus

Games want music and sound effects out of the same device. The effects bus
lets a host play plain PCM sounds on top of whatever song the engine is
playing. They're mixed into the song's mix buffer before the final clip and
conversion, so they come out of live playback and the disk writer alike,
and they share the output format, clipping and VU meter with the song.

This is a simple mixer of its own, separate from the song's channels. Effects
don't go through the song's interpolation modes, filters, envelopes or
equalizer.

The API is in `include/sfx-bus.h` and the code in `player/sfx-bus.c`.

## Using it

    sfx_bus_init(32);                      /* room for 32 voices at once */

    sfx_source_t *boom = sfx_source_load("boom.wav");
    sfx_source_t *beep = sfx_source_from_pcm(pcm, frames, SFX_PCM_S16, 1, 22050);
    sfx_source_t *bell = sfx_source_from_song_sample(5);

    sfx_voice_t v = sfx_play(boom, 0.8f, -0.3f, 1.0f, 0);
    sfx_set_pan(v, 0.3f);
    sfx_stop(v);

    sfx_bus_set_volume(0.5f);              /* all effects, not the song */

    sfx_source_free(boom);                 /* stops its voices first */
    sfx_bus_quit();

`sfx_bus_init()` sets up the voice pool. The host picks its size, anywhere
from 1 to `SFX_MAX_VOICES` (65535). When every voice is busy, `sfx_play()`
returns `SFX_VOICE_NONE` and nothing is cut off to make room.

Sources:

* `sfx_source_from_pcm(data, frames, format, channels, rate)`: unsigned
  8-bit, signed 8-bit or native-endian signed 16-bit, mono or interleaved
  stereo, at any rate.
* `sfx_source_load(filename)`: goes through Schism's sample loaders, so it
  takes WAV, AIFF, AU, ITS, FLAC and whatever else the build can load as a
  sample. Ogg Vorbis, MP3 and the like need one of the decoder libraries
  (libavformat, GStreamer, Media Foundation or Core Audio). The raw loader
  is left out because it accepts any file at all.
* `sfx_source_from_song_sample(n)`: copies sample `n` of the current song,
  with its C-5 rate as the source rate and its loop points if it has any.
  The copy means loading another song doesn't affect effects already
  playing.

A source always holds its own copy of the data, converted to 16-bit. Loop
points can be set with `sfx_source_set_loop()`. A voice started with `loop`
set plays from the start, then repeats the loop (or the whole source if it
has no loop points) until it's stopped. Ping-pong and sustain loops on song
samples are played as plain forward loops.

`sfx_play_ex()` takes a `struct sfx_play_args`, which adds two things
`sfx_play()` doesn't have:

* `delay`: seconds of output to wait before the voice starts. The wait is
  counted in mixed frames, so in a render it's exact to the frame.
* `duration`: stop the voice after this many seconds of playing. This is
  mostly for looped sounds.

Voice handles carry a generation count, so a handle to a voice that has
finished stays invalid when its slot is reused. Calls on an old handle do
nothing.

## Volume and pan

Volume is a linear gain: 1.0 plays the source at its own level, 0.5 is about
-6 dB. Values above 1 are allowed and can clip. Hosts with DirectSound-style
volumes in hundredths of a dB can convert with `powf(10, mB / 2000.0f)`.

Pan runs from -1 (left) through 0 (centre) to +1 (right) with a balance law:

    left gain  = volume * (pan > 0 ? 1 - pan : 1)
    right gain = volume * (pan < 0 ? 1 + pan : 1)

So a centred sound plays at full volume on both sides, the same as an
unpanned DirectSound buffer, and panning fades the far side linearly. A
stereo source's left channel only goes left and its right only goes right.
For mono output the two sides are averaged.

Both gains are then multiplied by the bus volume. A 16-bit sample at gain
1.0 lands in the mix buffer at the same level as a full-scale 16-bit render,
so an effect played at 1.0, centred, at the output rate comes out of a
16-bit render bit for bit.

When the volume or pan of a playing voice changes, or it's stopped, the gains
move to the new values over about 2 ms instead of jumping, which avoids
clicks. A voice that hasn't produced any sound yet starts at its gains
directly.

## Resampling

Each voice steps through its source at `frequency / output rate` per output
frame, in 32.32 fixed point, and interpolates linearly between neighbouring
frames. The frequency is the source's rate times the pitch, or the
`frequency` passed to `sfx_play_ex()` or `sfx_set_frequency()`.

## Where it mixes

`csf_read()` (player/sndmix.c) adds the bus to each block of the mix buffer
after the song's channels, EQ and AGC, and before the clip and conversion to
the output format. Whoever calls `csf_read()` sets `song_t.sfx_bus` to the
bus or to NULL:

* The audio callback (`schism/audio_playback.c`) passes the bus, except while
  the disk writer is running. When the song is stopped, or ends partway
  through a buffer, it fills the rest with `csf_read_sfx()`, which mixes the
  effects over silence, so effects keep playing with no song.
* `disko_sync()` passes the bus for song exports. If the song ends while
  effects are still playing, the export carries on with effects only until
  they finish or the length limit is reached, rounded up to the end of the
  disk writer's buffer. Per-channel exports and pattern-to-sample renders
  leave the effects out.

With no voices playing, the bus adds nothing and output is byte-for-byte what
it was without it.

## Threads

* Call the API from one thread, the host's main thread. It isn't meant to
  be called from several threads at once, and it can't be called from the
  audio callback.
* Every call that touches voice state takes the audio lock
  (`song_lock_audio()`) for a short, fixed amount of work: a scan of the
  voice pool at worst. The mixer runs with the lock already held.
* Loading, copying and freeing source data happens on the calling thread
  outside the lock. The audio thread never allocates or frees anything.
* `sfx_source_free()` stops every voice playing the source while holding
  the lock, and only then frees it, so a source can be freed at any time.
* The disk writer takes the audio lock around each block it renders while
  the bus is set up, so effects can be started and stopped during an
  export too.

## schismplay

schismplay has options for trying the bus out and for testing it:

    --sfx=FILE@SECONDS[:VOL[:PAN[:RATE[:LENGTH]]]]
    --sfx-sample=N@SECONDS[:VOL[:PAN[:RATE[:LENGTH]]]]
    --sfx-only
    --sfx-voices=N
    --sfx-volume=V
    --sfx-stress=SECONDS

`--sfx` plays a file at a song time in seconds, and `--sfx-sample` plays
sample N of the loaded song. Both can be given as often as you like. VOL and
PAN are as above (defaults 1 and 0), RATE is the playback rate in Hz (0 or
missing for the source's own rate), and LENGTH loops the sound for that many
seconds. The file name is everything before the last `@`.

All the effects are started with delays when playback or the render starts.
In a render each one starts on frame `round(SECONDS * rate)`. In live
playback they're timed from when playback started, give or take one audio
buffer.

`--sfx-only` plays or renders just the effects over silence. A song file is
still loaded if you give one, so `--sfx-sample` works, but it isn't played.
Rendering a song with some effects, the song alone, and the effects alone
with `--sfx-only`, all at 32 bits, gives three files where the first is the
sum of the other two, as long as nothing clips.

    schismplay -o mix.wav -b 32 -l 20 --seed 1 \
        --sfx=boom.wav@1.5:0.3:-0.5 --sfx=wind.ogg@4:0.2:0:0:8 song.it

`--sfx-voices` sets the pool size (default 64), and `--sfx-volume` the bus
volume. `--sfx-stress=SECONDS` plays the song live and, for that long, makes
thousands of random calls a second from the main thread: starting voices
with random pitch, pan, loops, delays and durations, stopping them, changing
them, and freeing and reloading sources while they play. It's a test for
the locking, best run under ThreadSanitizer or AddressSanitizer with the
dummy audio driver:

    SDL_AUDIODRIVER=dummy schismplay -q --sfx-stress=30 song.it
