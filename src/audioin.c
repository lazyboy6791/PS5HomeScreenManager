#include "audioin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3/minimp3.h"

static void set_err(char *err, size_t n, const char *msg) {
  if (err && n) snprintf(err, n, "%s", msg);
}

void audio_free(audio_t *a) {
  free(a->l);
  free(a->r);
  memset(a, 0, sizeof(*a));
}

static int16_t clip16(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }

static uint32_t rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

/* ---- WAV ---------------------------------------------------------------------------------------------------------- */

static int decode_wav(const uint8_t *d, size_t len, double max_seconds, audio_t *o, char *err, size_t errsz) {
  size_t fmt = 0, fmt_len = 0, data = 0, data_len = 0;
  for (size_t p = 12; p + 8 <= len;) {
    uint32_t size = rd32(d + p + 4);
    size_t body = p + 8;
    if (!memcmp(d + p, "fmt ", 4)) {
      fmt = body;
      fmt_len = size;
    } else if (!memcmp(d + p, "data", 4)) {
      data = body;
      data_len = size;
      if (data + data_len > len) data_len = len - data; /* streamed files: the size can be wrong */
      break;
    }
    p = body + size + (size & 1);
  }
  if (!fmt || fmt_len < 16 || !data) {
    set_err(err, errsz, "That WAV file couldn't be read.");
    return -1;
  }
  int tag = rd16(d + fmt), ch = rd16(d + fmt + 2), rate = (int)rd32(d + fmt + 4), bits = rd16(d + fmt + 14);
  if (tag == 0xFFFE && fmt_len >= 26) tag = rd16(d + fmt + 24); /* extensible: the first two bytes of the sub-format */
  if ((tag != 1 && tag != 3) || ch < 1 || ch > 8 || rate < 8000 || rate > 192000 || (bits != 8 && bits != 16 && bits != 24 && bits != 32) || (tag == 3 && bits != 32)) {
    set_err(err, errsz, "That WAV file isn't a kind this can read (use 16 or 24 bit PCM, or float).");
    return -1;
  }
  int bps = bits / 8;
  long frames = (long)(data_len / ((size_t)bps * (size_t)ch));
  long want = (long)(max_seconds * rate);
  if (frames > want) {
    frames = want;
    o->was_cut = 1;
  }
  if (frames <= 0) {
    set_err(err, errsz, "That WAV file has no sound.");
    return -1;
  }
  o->l = malloc(sizeof(int16_t) * (size_t)frames);
  o->r = malloc(sizeof(int16_t) * (size_t)frames);
  if (!o->l || !o->r) {
    set_err(err, errsz, "Out of memory.");
    return -1;
  }
  for (long i = 0; i < frames; i++) {
    double v[8];
    for (int c = 0; c < ch; c++) {
      const uint8_t *s = d + data + ((size_t)i * (size_t)ch + (size_t)c) * (size_t)bps;
      if (tag == 3) {
        float f;
        memcpy(&f, s, 4);
        v[c] = f * 32768.0;
      } else if (bits == 8) {
        v[c] = (s[0] - 128) * 256.0;
      } else if (bits == 16) {
        v[c] = (int16_t)rd16(s);
      } else if (bits == 24) {
        v[c] = (double)(((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24)) >> 8) / 256.0;
      } else {
        v[c] = (double)(int32_t)rd32(s) / 65536.0;
      }
    }
    double l, r;
    if (ch == 1) l = r = v[0];
    else if (ch == 2) {
      l = v[0];
      r = v[1];
    } else { /* FL FR FC LFE BL BR SL SR: front pair, centre at -3 dB, the rest at -6 dB, LFE left out */
      l = v[0] + (ch > 2 ? v[2] * 0.707 : 0) + (ch > 4 ? v[4] * 0.5 : 0) + (ch > 6 ? v[6] * 0.5 : 0);
      r = v[1] + (ch > 2 ? v[2] * 0.707 : 0) + (ch > 5 ? v[5] * 0.5 : 0) + (ch > 7 ? v[7] * 0.5 : 0);
      l *= 0.6;
      r *= 0.6;
    }
    o->l[i] = clip16(l);
    o->r[i] = clip16(r);
  }
  o->n = frames;
  o->rate = o->src_rate = rate;
  o->src_channels = ch;
  o->src_seconds = (double)frames / rate;
  return 0;
}

/* ---- MP3 ---------------------------------------------------------------------------------------------------------- */

static int decode_mp3(const uint8_t *d, size_t len, double max_seconds, audio_t *o, char *err, size_t errsz) {
  size_t pos = 0;
  if (len > 10 && !memcmp(d, "ID3", 3)) { /* skip the tag (it can hold a big cover picture) */
    size_t tag = ((size_t)(d[6] & 0x7f) << 21) | ((size_t)(d[7] & 0x7f) << 14) | ((size_t)(d[8] & 0x7f) << 7) | (d[9] & 0x7f);
    pos = 10 + tag;
    if (pos >= len) pos = 0;
  }
  mp3dec_t *dec = malloc(sizeof(*dec));
  mp3d_sample_t *pcm = malloc(sizeof(mp3d_sample_t) * MINIMP3_MAX_SAMPLES_PER_FRAME);
  if (!dec || !pcm) {
    free(dec);
    free(pcm);
    set_err(err, errsz, "Out of memory.");
    return -1;
  }
  mp3dec_init(dec);
  long cap = 0, n = 0, want = 0;
  int rate = 0, channels = 0, bad = 0;
  mp3dec_frame_info_t info;
  while (pos < len) {
    int samples = mp3dec_decode_frame(dec, d + pos, (int)(len - pos), pcm, &info);
    if (info.frame_bytes <= 0) break;
    pos += (size_t)info.frame_bytes;
    if (samples <= 0) {
      if (++bad > 4000) break; /* not an MP3 */
      continue;
    }
    if (!rate) {
      rate = info.hz;
      channels = info.channels;
      want = (long)(max_seconds * rate);
      cap = want + 1152;
      o->l = malloc(sizeof(int16_t) * (size_t)cap);
      o->r = malloc(sizeof(int16_t) * (size_t)cap);
      if (!o->l || !o->r) {
        free(dec);
        free(pcm);
        set_err(err, errsz, "Out of memory.");
        return -1;
      }
    }
    if (info.hz != rate) continue; /* a change of rate in the middle is not expected */
    for (int i = 0; i < samples && n < want; i++) {
      if (info.channels == 1) o->l[n] = o->r[n] = pcm[i];
      else {
        o->l[n] = pcm[i * 2];
        o->r[n] = pcm[i * 2 + 1];
      }
      n++;
    }
    if (n >= want) {
      o->was_cut = pos < len; /* more music follows */
      break;
    }
  }
  free(dec);
  free(pcm);
  if (n <= 0) {
    set_err(err, errsz, "That file couldn't be read as an MP3.");
    return -1;
  }
  o->n = n;
  o->rate = o->src_rate = rate;
  o->src_channels = channels;
  o->src_seconds = (double)n / rate;
  return 0;
}

int audio_decode(const uint8_t *data, size_t len, double max_seconds, audio_t *out, char *err, size_t errsz) {
  memset(out, 0, sizeof(*out));
  int rc;
  if (len > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) rc = decode_wav(data, len, max_seconds, out, err, errsz);
  else rc = decode_mp3(data, len, max_seconds, out, err, errsz);
  if (rc) audio_free(out);
  return rc;
}

/* ---- sample rate -------------------------------------------------------------------------------------------------- */

#define SINC_ZEROS 16

static double kernel(double x, double fc) { /* windowed sinc, x in input samples, cut-off fc (1 = Nyquist of the input) */
  double a = M_PI * x;
  double s = fabs(x) < 1e-9 ? 1.0 : sin(a * fc) / (a * fc);
  double half = SINC_ZEROS / fc;
  if (fabs(x) >= half) return 0;
  double w = 0.5 + 0.5 * cos(M_PI * x / half);
  return s * w * fc;
}

static int16_t *resample_one(const int16_t *in, long n, int from, int to, long *out_n) {
  double ratio = (double)to / from;
  long m = (long)floor((double)n * ratio);
  int16_t *o = malloc(sizeof(int16_t) * (size_t)(m > 0 ? m : 1));
  if (!o) return NULL;
  double fc = ratio < 1 ? ratio : 1; /* low-pass when going down */
  int taps = (int)ceil(SINC_ZEROS / fc);
  for (long j = 0; j < m; j++) {
    double t = (double)j / ratio;
    long c = (long)floor(t);
    double frac = t - (double)c;
    double sum = 0;
    for (int k = -taps + 1; k <= taps; k++) {
      long i = c + k;
      if (i < 0 || i >= n) continue;
      sum += in[i] * kernel((double)k - frac, fc);
    }
    o[j] = clip16(sum);
  }
  *out_n = m;
  return o;
}

int audio_resample(audio_t *a, int rate, char *err, size_t errsz) {
  if (a->rate == rate) return 0;
  long nl, nr;
  int16_t *l = resample_one(a->l, a->n, a->rate, rate, &nl);
  int16_t *r = l ? resample_one(a->r, a->n, a->rate, rate, &nr) : NULL;
  if (!l || !r) {
    free(l);
    free(r);
    set_err(err, errsz, "Out of memory.");
    return -1;
  }
  free(a->l);
  free(a->r);
  a->l = l;
  a->r = r;
  a->n = nl;
  a->rate = rate;
  return 0;
}

/* ---- level, gain, fades ------------------------------------------------------------------------------------------- */

double audio_level(const int16_t *l, const int16_t *r, long n, int rate) {
  long block = rate * 4 / 10; /* 400 ms */
  if (block < 1) block = 1;
  double sum = 0;
  long counted = 0;
  const double gate = 0.0031622776601683794; /* -50 dBFS */
  for (long s = 0; s + block <= n; s += block) {
    double e = 0;
    for (long i = s; i < s + block; i++) {
      double a = l[i] / 32768.0, b = r[i] / 32768.0;
      e += (a * a + b * b) * 0.5;
    }
    e /= (double)block;
    if (sqrt(e) >= gate) {
      sum += e;
      counted++;
    }
  }
  return counted ? sqrt(sum / (double)counted) : 0;
}

static double soft(double x) { /* gentle limit: linear below 0.8, then bends towards 1 */
  double a = fabs(x);
  if (a <= 0.8) return x;
  double y = 0.8 + 0.2 * tanh((a - 0.8) / 0.2);
  return x < 0 ? -y : y;
}

void audio_apply(audio_t *a, double gain, double fade_in, double fade_out) {
  long fi = (long)(fade_in * a->rate), fo = (long)(fade_out * a->rate);
  if (fi > a->n / 4) fi = a->n / 4;
  if (fo > a->n / 4) fo = a->n / 4;
  for (long i = 0; i < a->n; i++) {
    double f = 1;
    if (i < fi) f = 0.5 - 0.5 * cos(M_PI * (double)i / (double)fi);
    long from_end = a->n - 1 - i;
    if (from_end < fo) f *= 0.5 - 0.5 * cos(M_PI * (double)from_end / (double)fo);
    a->l[i] = clip16(soft(a->l[i] / 32768.0 * gain * f) * 32768.0);
    a->r[i] = clip16(soft(a->r[i] / 32768.0 * gain * f) * 32768.0);
  }
}
