/* ATRAC9 encoder for the console's home music (bgm_home.at9).
 *
 * A C port of the encoder in Prospero Themes' Shared/Atrac9Codec.cs (GPL-3.0), which is a port of Blaze414's ps5-theme
 * encoder (GPL-3.0-or-later), which builds on LibAtrac9 by Alex Barney (MIT). The output is laid out like the console's own
 * file (same ATRAC9 config, same loop chunks) and stays inside what that file uses. Every superframe is decoded again while
 * it is written and compared with the input; a result that doesn't match is refused. Only mono and stereo input. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Called now and then with 0..1; return non-zero to cancel. */
typedef int (*a9_progress_fn)(void *ctx, double fraction);

/* Encodes 16-bit samples (at the stock file's sample rate) into an .at9 laid out like `stock`. `right` may equal `left`
 * (mono). `intro` is how many samples play once before the part that loops (0: all of it loops). On success *out is a
 * malloc'd file; on failure err says why (a cancelled run has err "cancelled"). */
int a9_encode(const uint8_t *stock, size_t stock_len, const int16_t *left, const int16_t *right, size_t n, int intro,
              a9_progress_fn progress, void *progress_ctx, uint8_t **out, size_t *out_len, char *err, size_t errsz);

/* The stock file's sample rate and channel count (what the input has to be converted to). 0 on success. */
int a9_stock_info(const uint8_t *stock, size_t stock_len, int *rate, int *channels, char *err, size_t errsz);

/* Decodes up to max_samples of the front (stereo) pair of an .at9 file into malloc'd 16-bit buffers. 0 on success. */
int a9_decode_front(const uint8_t *stock, size_t stock_len, long max_samples, int16_t **left, int16_t **right, long *n, int *rate,
                    char *err, size_t errsz);
