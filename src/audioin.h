/* Reading a song (WAV or MP3) and making it ready for the home music: 48 kHz (or the console file's rate), stereo, leveled to
 * the console's own music, with a fade at both ends so the loop doesn't click. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MUSIC_MAX_SECONDS 300 /* longer songs are cut here (one number, change it for longer loops) */

typedef struct {
  int16_t *l, *r; /* r == l never: a mono song gets a copy */
  long n;
  int rate;          /* sample rate of l/r */
  double src_seconds; /* length of the song as read (up to the cut) */
  int src_rate, src_channels, was_cut;
} audio_t;

/* Decodes WAV (PCM 8/16/24/32, float; any channel count, mixed down to stereo) or MP3, at most max_seconds of it. */
int audio_decode(const uint8_t *data, size_t len, double max_seconds, audio_t *out, char *err, size_t errsz);
void audio_free(audio_t *a);
/* Converts to another sample rate (a new audio_t; the old one is freed). */
int audio_resample(audio_t *a, int rate, char *err, size_t errsz);
/* Loudness of the song: RMS (0..1 full scale) over the parts that aren't near-silent. 0 if all silent. */
double audio_level(const int16_t *l, const int16_t *r, long n, int rate);
/* Multiplies by gain (with a soft limit so that nothing clips), then fades in and out (seconds). */
void audio_apply(audio_t *a, double gain, double fade_in, double fade_out);
