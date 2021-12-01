#ifndef MODPLUG_MIXER_H
#define MODPLUG_MIXER_H

#include "sndfile.h"

// Stuff moved from sndfile.h
#define MIXING_ATTENUATION 5
#define MIXING_CLIPMIN (-0x04000000)
#define MIXING_CLIPMAX (0x03FFFFFF)
#define VOLUMERAMPPRECISION 12
#define FILTERPRECISION 13

void init_mix_buffer(int32_t *, uint32_t);
void stereo_fill(int32_t *, uint32_t, int32_t *, int32_t *);
void end_channel_ofs(song_voice_t *, int32_t *, uint32_t);
// void interleave_front_rear(int32_t *, int32_t *, uint32_t);
void mono_from_stereo(int32_t *, uint32_t);

void stereo_mix_to_float(const int32_t *, float *, float *, uint32_t);
void float_to_stereo_mix(const float *, const float *, int32_t *, uint32_t);
void mono_mix_to_float(const int32_t *, float *, uint32_t);
void float_to_mono_mix(const float *, int32_t *, uint32_t);

uint32_t csf_create_stereo_mix(song_t *csf, int count);

void setup_channel_filter(song_voice_t *pChn, int reset, int flt_modifier, int freq);

// typedef uint32_t (*convert_clip_t)(void *, int *, uint32_t, int*, int*) __attribute__((cdecl))

uint32_t clip_32_to_8(void *, int32_t *, uint32_t, int32_t *, int32_t *);
uint32_t clip_32_to_16(void *, int32_t *, uint32_t, int32_t *, int32_t *);
uint32_t clip_32_to_24(void *, int32_t *, uint32_t, int32_t *, int32_t *);
uint32_t clip_32_to_32(void *, int32_t *, uint32_t, int32_t *, int32_t *);

void eq_mono(song_t *, int32_t *, uint32_t);
void eq_stereo(song_t *, int32_t *, uint32_t);
void initialize_eq(int, float);
void set_eq_gains(const uint32_t *, uint32_t, const uint32_t *, int, int);

// sndmix.c
extern int32_t g_dry_rofs_vol;
extern int32_t g_dry_lofs_vol;

// mixer.c
void ResampleMono8BitFirFilter(signed char *oldbuf, signed char *newbuf, unsigned long oldlen, unsigned long newlen);
void ResampleMono16BitFirFilter(signed short *oldbuf, signed short *newbuf, unsigned long oldlen, unsigned long newlen);
void ResampleStereo8BitFirFilter(signed char *oldbuf, signed char *newbuf, unsigned long oldlen, unsigned long newlen);
void ResampleStereo16BitFirFilter(signed short *oldbuf, signed short *newbuf, unsigned long oldlen,
                                  unsigned long newlen);

#endif
