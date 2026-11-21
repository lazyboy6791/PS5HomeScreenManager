/* ATRAC9 encoder, see atrac9.h. Ported from Prospero Themes' Shared/Atrac9Codec.cs (GPL-3.0). */
#include "atrac9.h"

#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atrac9_tables.h"

#define A9_MONO 0
#define A9_STEREO 1
#define A9_LFE 2
#define MAX_CHANNELS 8
#define MAX_BLOCKS 5
#define MAX_FPS 4

/* ------------------------------------------------------------------------------------------------------------------ */
/* errors: a failure anywhere below jumps back to the entry point                                                       */
/* ------------------------------------------------------------------------------------------------------------------ */

static __thread jmp_buf *t_jb;
static __thread char t_err[200];

static void a9_fail(const char *msg) {
  snprintf(t_err, sizeof(t_err), "%s", msg);
  if (t_jb) longjmp(*t_jb, 1);
  abort();
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* codebooks                                                                                                            */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  int valid, count, vbits, maxbits, mask, nsym;
  uint8_t *bits;
  uint16_t *codes;
  int16_t *lookup;
  int *near_;
} a9_book_t;

static a9_book_t g_sfu[7], g_sfs[6], g_spec[2][8][4];
static uint8_t g_curves[48][48];
static double g_sin[9][256], g_cos[9][256];
static int g_shuffle[9][256];
static double g_mdct[9][256], g_imdct[9][256];
typedef struct {
  int start, value, boundary;
  int g[32];
} a9_cand_t;
#define NCAND (6 * 12 * 6)
static a9_cand_t g_cand[NCAND];
static int g_silent_grad[32];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static const int k_grad_starts[6] = {12, 14, 16, 18, 20, 22};
static const int k_boundaries[6] = {0, 1, 2, 4, 8, 15};
#define GRAD_VALUES 12
#define SILENT_START 16
#define SILENT_VALUE 5
#define SILENT_BOUNDARY 0
#define MAX_PRECISION_LIMIT 7
#define FRAME_CAP 1.25

static int sign_extend(int v, int bits) { return (int)((uint32_t)v << (32 - bits)) >> (32 - bits); }

static int digit(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

static int hex3(const char *s) {
  int v = 0;
  for (int i = 0; i < 3; i++) {
    char c = s[i];
    v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return v;
}

static void book_build(a9_book_t *b, const at9_book_spec_t *sp) {
  memset(b, 0, sizeof(*b));
  if (!sp->bits) return;
  b->valid = 1;
  b->count = sp->count;
  b->vbits = sp->vbits;
  b->maxbits = sp->maxbits;
  b->mask = (1 << sp->vbits) - 1;
  b->nsym = (int)strlen(sp->bits);
  b->bits = calloc((size_t)b->nsym, 1);
  b->codes = calloc((size_t)b->nsym, sizeof(uint16_t));
  b->lookup = malloc(sizeof(int16_t) << b->maxbits);
  for (int i = 0; i < (1 << b->maxbits); i++) b->lookup[i] = -1;
  for (int s = 0; s < b->nsym; s++) {
    b->bits[s] = (uint8_t)digit(sp->bits[s]);
    b->codes[s] = (uint16_t)hex3(sp->codes + s * 3);
    if (!b->bits[s]) continue;
    int shift = b->maxbits - b->bits[s];
    for (int k = b->codes[s] << shift; k < ((b->codes[s] + 1) << shift); k++) b->lookup[k] = (int16_t)s;
  }
}

/* Symbols without a code map to the closest symbol that has one (ties go to the shorter code). */
static const int *book_near(a9_book_t *b) {
  if (b->near_) return b->near_;
  int n = b->nsym;
  int *values = malloc(sizeof(int) * (size_t)n * (size_t)b->count);
  b->near_ = malloc(sizeof(int) * (size_t)n);
  for (int s = 0; s < n; s++)
    for (int j = 0, x = s; j < b->count; j++, x >>= b->vbits) values[s * b->count + j] = sign_extend(x & b->mask, b->vbits);
  for (int s = 0; s < n; s++) {
    if (b->bits[s]) {
      b->near_[s] = s;
      continue;
    }
    int best = -1;
    double best_err = INFINITY;
    for (int t = 0; t < n; t++) {
      if (!b->bits[t]) continue;
      double err = 0;
      for (int j = 0; j < b->count; j++) {
        double d = values[s * b->count + j] - values[t * b->count + j];
        err += d * d;
      }
      err = err * 64 + b->bits[t];
      if (err < best_err) {
        best_err = err;
        best = t;
      }
    }
    b->near_[s] = best;
  }
  free(values);
  return b->near_;
}

static void fill_gradient(int *g, int start, int end, int start_value, int end_value, int units) {
  int values = end_value - start_value;
  for (int i = 0; i < end; i++) g[i] = start_value;
  for (int i = end; i <= units; i++) g[i] = end_value;
  if (values == 0) return;
  const uint8_t *curve = g_curves[end - start - 1];
  double scale = (abs(values) - 1) / 31.0;
  for (int i = start; i < end; i++) {
    int step = (int)(curve[i - start] * scale);
    g[i] = values > 0 ? start_value + 1 + step : start_value - 1 - step;
  }
}

static void tables_init(void) {
  for (int i = 0; i < 7; i++) book_build(&g_sfu[i], &k_at9_sfu[i]);
  for (int i = 0; i < 6; i++) book_build(&g_sfs[i], &k_at9_sfs[i]);
  for (int a = 0; a < 2; a++)
    for (int b = 0; b < 8; b++)
      for (int c = 0; c < 4; c++) book_build(&g_spec[a][b][c], &k_at9_spec[a][b][c]);
  for (int n = 0; n < 48; n++)
    for (int i = 0; i <= n; i++) g_curves[n][i] = (uint8_t)k_at9_basecurve[i * 48 / (n + 1)];
  for (int b = 0; b <= 8; b++) {
    int n = 1 << b;
    for (int i = 0; i < n; i++) {
      double v = M_PI * (4 * i + 1) / (4.0 * n);
      g_sin[b][i] = sin(v);
      g_cos[b][i] = cos(v);
      int x = i ^ (i >> 1), r = 0;
      for (int k = 0; k < b; k++, x >>= 1) r = (r << 1) | (x & 1);
      g_shuffle[b][i] = r;
    }
  }
  for (int bits = 6; bits <= 8; bits++) {
    int n = 1 << bits;
    for (int i = 0; i < n; i++) g_mdct[bits][i] = (sin(((i + 0.5) / n - 0.5) * M_PI) + 1) * 0.5;
    for (int i = 0; i < n; i++)
      g_imdct[bits][i] = g_mdct[bits][i] / (g_mdct[bits][n - 1 - i] * g_mdct[bits][n - 1 - i] + g_mdct[bits][i] * g_mdct[bits][i]);
  }
  int ci = 0;
  for (int si = 0; si < 6; si++)
    for (int value = 0; value < GRAD_VALUES; value++)
      for (int bi = 0; bi < 6; bi++) {
        a9_cand_t *c = &g_cand[ci++];
        c->start = k_grad_starts[si];
        c->value = value;
        c->boundary = k_boundaries[bi];
        memset(c->g, 0, sizeof(c->g));
        fill_gradient(c->g, c->start, 31, value, 31, 31);
      }
  memset(g_silent_grad, 0, sizeof(g_silent_grad));
  fill_gradient(g_silent_grad, SILENT_START, 31, SILENT_VALUE, 31, 31);
}

/* DCT-IV of 2^bits values; tmp is a work buffer of the same size. */
static void dct4(int bits, const double *input, double *output, double *tmp) {
  int size = 1 << bits, last = size - 1, half = size >> 1;
  const double *sn = g_sin[bits], *cs = g_cos[bits];
  for (int i = 0; i < half; i++) {
    int i2 = i * 2;
    double a = input[i2], b = input[last - i2];
    tmp[i2] = a * cs[i] + b * sn[i];
    tmp[i2 + 1] = a * sn[i] - b * cs[i];
  }
  int stages = bits - 1;
  for (int stage = 0; stage < stages; stage++) {
    int blocks = 1 << stage, size_bits = stages - stage;
    int bsize = 1 << size_bits, bhalf = bsize >> 1;
    sn = g_sin[size_bits - 1];
    cs = g_cos[size_bits - 1];
    for (int blk = 0; blk < blocks; blk++) {
      for (int i = 0; i < bhalf; i++) {
        int f = (blk * bsize + i) * 2, k = f + bsize;
        double a = tmp[f] - tmp[k], b = tmp[f + 1] - tmp[k + 1];
        tmp[f] += tmp[k];
        tmp[f + 1] += tmp[k + 1];
        tmp[k] = a * cs[i] + b * sn[i];
        tmp[k + 1] = a * sn[i] - b * cs[i];
      }
    }
  }
  const int *sh = g_shuffle[bits];
  for (int i = 0; i < size; i++) output[i] = tmp[sh[i]];
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the file and its config                                                                                              */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  int sri, cci, rate, frame_bytes, fps, sf_bytes, power, frame_samples, sf_samples, nch, high, nblocks;
  int blocks[MAX_BLOCKS];
} a9_cfg_t;

static const int k_chan_cfg[6][5] = {{A9_MONO}, {A9_MONO, A9_MONO}, {A9_STEREO}, {A9_STEREO, A9_MONO, A9_LFE, A9_STEREO},
                                     {A9_STEREO, A9_MONO, A9_LFE, A9_STEREO, A9_STEREO}, {A9_STEREO, A9_STEREO}};
static const int k_chan_cfg_n[6] = {1, 2, 1, 4, 5, 2};

static void cfg_read(const uint8_t *b, a9_cfg_t *c) {
  if (b[0] != 0xfe) a9_fail("ATRAC9 config does not start with 0xFE");
  int sri = b[1] >> 4, cci = (b[1] >> 1) & 7;
  if (b[1] & 1) a9_fail("ATRAC9 config validation bit is set");
  if (cci > 5) a9_fail("unknown ATRAC9 channel layout");
  int frame_bytes = ((b[2] << 3) | (b[3] >> 5)) + 1;
  int sfi = (b[3] >> 3) & 3;
  if (sfi & 1) a9_fail("unsupported ATRAC9 superframe size");
  memset(c, 0, sizeof(*c));
  c->sri = sri;
  c->cci = cci;
  c->rate = k_at9_rates[sri];
  c->high = sri > 7;
  c->nblocks = k_chan_cfg_n[cci];
  for (int i = 0; i < c->nblocks; i++) {
    c->blocks[i] = k_chan_cfg[cci][i];
    c->nch += c->blocks[i] == A9_STEREO ? 2 : 1;
  }
  c->frame_bytes = frame_bytes;
  c->fps = 1 << sfi;
  c->sf_bytes = frame_bytes << sfi;
  c->power = k_at9_power[sri];
  c->frame_samples = 1 << c->power;
  c->sf_samples = c->frame_samples << sfi;
}

typedef struct {
  char id[5];
  size_t off, len;
} a9_chunk_t;

typedef struct {
  const uint8_t *b;
  size_t n;
  a9_cfg_t cfg;
  a9_chunk_t chunks[16];
  int nchunks;
  int has_fact, has_loop;
  uint32_t fact[3], loop[2];
  size_t data_off, data_len;
  long superframes;
} a9_file_t;

static uint32_t u32(const uint8_t *b, size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }
static void w32(uint8_t *b, size_t o, uint32_t v) {
  b[o] = (uint8_t)v;
  b[o + 1] = (uint8_t)(v >> 8);
  b[o + 2] = (uint8_t)(v >> 16);
  b[o + 3] = (uint8_t)(v >> 24);
}

static const uint8_t k_guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d, 0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

static void file_parse(const uint8_t *b, size_t n, a9_file_t *f) {
  memset(f, 0, sizeof(*f));
  f->b = b;
  f->n = n;
  if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) a9_fail("not a RIFF WAVE file");
  long fmt_at = -1;
  size_t fmt_len = 0;
  long data_at = -1;
  size_t data_len = 0;
  for (size_t p = 12; p + 8 <= n;) {
    uint64_t size = u32(b, p + 4);
    if (p + 8 + size > n) a9_fail("a chunk runs past the end of the file");
    size_t body = p + 8;
    if (f->nchunks < 16) {
      a9_chunk_t *c = &f->chunks[f->nchunks++];
      memcpy(c->id, b + p, 4);
      c->id[4] = 0;
      c->off = body;
      c->len = (size_t)size;
    }
    if (!memcmp(b + p, "fmt ", 4)) {
      fmt_at = (long)body;
      fmt_len = (size_t)size;
    } else if (!memcmp(b + p, "fact", 4) && size >= 12) {
      f->has_fact = 1;
      f->fact[0] = u32(b, body);
      f->fact[1] = u32(b, body + 4);
      f->fact[2] = u32(b, body + 8);
    } else if (!memcmp(b + p, "smpl", 4) && size >= 60 && u32(b, body + 28) >= 1) {
      f->has_loop = 1;
      f->loop[0] = u32(b, body + 44);
      f->loop[1] = u32(b, body + 48);
    } else if (!memcmp(b + p, "data", 4)) {
      data_at = (long)body;
      data_len = (size_t)size;
    }
    p += 8 + (size_t)size + (size & 1);
  }
  if (fmt_at < 0 || fmt_len < 52 || (b[fmt_at] | b[fmt_at + 1] << 8) != 0xfffe || memcmp(b + fmt_at + 24, k_guid, 16))
    a9_fail("not an ATRAC9 file");
  if (data_at < 0) a9_fail("no audio data");
  cfg_read(b + fmt_at + 44, &f->cfg);
  if ((b[fmt_at + 2] | b[fmt_at + 3] << 8) != f->cfg.nch) a9_fail("channel count does not match the ATRAC9 config");
  if (u32(b, (size_t)fmt_at + 4) != (uint32_t)f->cfg.rate) a9_fail("sample rate does not match the ATRAC9 config");
  if ((b[fmt_at + 12] | b[fmt_at + 13] << 8) != f->cfg.sf_bytes) a9_fail("block size does not match the ATRAC9 config");
  if (data_len % (size_t)f->cfg.sf_bytes) a9_fail("audio data is not a whole number of superframes");
  f->data_off = (size_t)data_at;
  f->data_len = data_len;
  f->superframes = (long)(data_len / (size_t)f->cfg.sf_bytes);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the profile: which coding features a file uses                                                                       */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  int modes, max_bands, max_prec, lfe_reuse, max_frame, fine, bex;
  uint8_t padding[256];
  int npad;
  uint8_t seen[256];
} a9_profile_t;

static void prof_add_padding(a9_profile_t *p, uint8_t v) {
  if (p->seen[v]) return;
  p->seen[v] = 1;
  p->padding[p->npad++] = v;
}

static void prof_default(a9_profile_t *p) {
  memset(p, 0, sizeof(*p));
  p->modes = 8;
  p->max_bands = 13;
  p->max_prec = 7;
  p->lfe_reuse = 2;
  prof_add_padding(p, 1);
}

static uint8_t prof_padding_byte(const a9_profile_t *p) { return p->npad > 0 && !p->seen[1] ? p->padding[0] : 1; }

/* ------------------------------------------------------------------------------------------------------------------ */
/* channels and blocks                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  int plan_bits, kind, weights, base, len;
} a9_sfplan_t;

typedef struct {
  int index;
  int sf[48], sf_prev[48], prec[48], fine[48];
  uint8_t set[48];
  int q[256], qf[256];
  double spectra[256], prev[256], pcm[256];
  int bex_values[4];
  int mask[49];
  int coded, bex_mode;
  /* encoder only */
  double x[256];
  int scratch_q[256], memo_bits[32 * 16], base_prec[32];
  double memo_err[32 * 16];
  const float *src;
  a9_sfplan_t sfplan;
} a9_chan_t;

typedef struct {
  int type, nch;
  a9_chan_t ch[2];
  int have_bands, band_count, units, stereo_units, bex_enabled, ext_unit, units_prev, grad_mode, boundary, primary, has_ext, lfe_reuse;
  int gradient[49], signs[32];
  int reuse; /* encoder only */
} a9_block_t;

static void block_init(a9_block_t *b, int type) {
  memset(b, 0, sizeof(*b));
  b->type = type;
  b->nch = type == A9_STEREO ? 2 : 1;
  b->ch[0].index = 0;
  b->ch[1].index = 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* bits                                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  const uint8_t *b;
  size_t len;
  size_t end; /* bit position */
  size_t pos;
} a9_br_t;

static uint32_t br_peek(const a9_br_t *r, int n) {
  size_t i = r->pos >> 3;
  uint32_t w = 0;
  for (int k = 0; k < 4; k++) w = (w << 8) | (i + (size_t)k < r->len ? r->b[i + (size_t)k] : 0u);
  return (w << (r->pos & 7)) >> (32 - n);
}

static int br_read(a9_br_t *r, int n) {
  if (n == 0) return 0;
  uint32_t v = br_peek(r, n);
  r->pos += (size_t)n;
  if (r->pos > r->end) a9_fail("frame runs past the end of its superframe");
  return (int)v;
}

static int br_signed(a9_br_t *r, int n) { return sign_extend(br_read(r, n), n); }

static int br_huff(a9_br_t *r, const a9_book_t *cb) {
  int s = cb->lookup[br_peek(r, cb->maxbits)];
  if (s < 0) a9_fail("invalid Huffman code");
  r->pos += cb->bits[s];
  if (r->pos > r->end) a9_fail("frame runs past the end of its superframe");
  return s;
}

typedef struct {
  uint8_t *b;
  size_t pos;
} a9_bw_t;

static void bw_write(a9_bw_t *w, int v, int n) {
  for (int i = n - 1; i >= 0; i--, w->pos++)
    if (i < 32 && ((v >> i) & 1)) w->b[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
}
static void bw_align(a9_bw_t *w) { w->pos = (w->pos + 7) & ~(size_t)7; }

/* ------------------------------------------------------------------------------------------------------------------ */
/* shared bit allocation helpers                                                                                        */
/* ------------------------------------------------------------------------------------------------------------------ */

/* Which units use the second set of spectrum codebooks, from the scale factors (as the decoder derives it). */
static void codebook_sets(const a9_cfg_t *cfg, a9_chan_t *ch, int units) {
  int *sf = ch->sf;
  memset(ch->set, 0, sizeof(ch->set));
  if (units <= 1 || cfg->high) return;
  int keep = sf[units];
  sf[units] = sf[units - 1];
  int avg = 0;
  if (units > 12) {
    for (int i = 0; i < 12; i++) avg += sf[i];
    avg = (avg + 6) / 12;
  }
  for (int i = 8; i < units; i++) {
    int lo = sf[i - 1] < sf[i + 1] ? sf[i - 1] : sf[i + 1];
    if (sf[i] - lo >= 3 || sf[i] - sf[i - 1] + sf[i] - sf[i + 1] >= 3) ch->set[i] = 1;
  }
  for (int i = 12; i < units; i++) {
    if (ch->set[i]) continue;
    int lo = sf[i - 1] < sf[i + 1] ? sf[i - 1] : sf[i + 1];
    if (sf[i] - lo >= 2 && sf[i] >= avg - (k_at9_unitstart[i + 1] - k_at9_unitstart[i] == 16 ? 1 : 0)) ch->set[i] = 1;
  }
  sf[units] = keep;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* decoder (parsing, and synthesis without band extension)                                                              */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  const a9_file_t *file;
  const a9_cfg_t *cfg;
  a9_block_t blocks[MAX_BLOCKS];
  double out_buf[256], work[256];
  a9_br_t br;
  size_t superframe_end;
  a9_profile_t profile;
} a9_dec_t;

static void dec_init(a9_dec_t *d, const a9_file_t *f) {
  memset(d, 0, sizeof(*d));
  d->file = f;
  d->cfg = &f->cfg;
  for (int i = 0; i < f->cfg.nblocks; i++) block_init(&d->blocks[i], f->cfg.blocks[i]);
  d->br.b = f->b;
  d->br.len = f->n;
}

static void dec_begin_superframe(a9_dec_t *d, long s) {
  size_t start = d->file->data_off + (size_t)s * (size_t)d->cfg->sf_bytes;
  d->superframe_end = start + (size_t)d->cfg->sf_bytes;
  d->br.pos = start * 8;
  d->br.end = d->superframe_end * 8;
}

static void dec_end_superframe(a9_dec_t *d) {
  for (size_t p = d->br.pos >> 3; p < d->superframe_end; p++) prof_add_padding(&d->profile, d->file->b[p]);
}

static void make_gradient(a9_block_t *blk, int start, int end, int sv, int ev) {
  if (start >= end || end > 31) a9_fail("invalid bit allocation range");
  if (blk->boundary > blk->units) a9_fail("invalid bit allocation boundary");
  fill_gradient(blk->gradient, start, end, sv, ev, blk->units);
}

static void precisions(a9_block_t *blk, a9_chan_t *ch) {
  int *sf = ch->sf, *mask = ch->mask;
  int n = blk->units;
  memset(mask, 0, sizeof(int) * (size_t)(n + 1));
  if (blk->grad_mode != 0) {
    for (int i = 1; i < n; i++) {
      int d = sf[i] - sf[i - 1];
      if (d > 1) mask[i] += d - 1 < 5 ? d - 1 : 5;
      else if (d < -1) mask[i - 1] += -d - 1 < 5 ? -d - 1 : 5;
    }
  }
  for (int i = 0; i < n; i++) {
    int p;
    if (blk->grad_mode != 0) {
      p = sf[i] + mask[i] - blk->gradient[i];
      if (p > 0) p = blk->grad_mode == 1 ? p >> 1 : blk->grad_mode == 2 ? (3 * p) >> 3 : p >> 2;
    } else {
      p = sf[i] - blk->gradient[i];
    }
    p = (p > 1 ? p : 1) + (i < blk->boundary ? 1 : 0);
    if (p > 30) a9_fail("precision out of range");
    ch->fine[i] = p > 15 ? p - 15 : 0;
    ch->prec[i] = p < 15 ? p : 15;
  }
}

static void delta_offset(a9_br_t *br, int *sf, const int *weights, int base, const a9_book_t *cb, int count) {
  sf[0] = br_read(br, cb->vbits);
  for (int i = 1; i < count; i++) sf[i] = (sf[i - 1] + br_huff(br, cb)) & cb->mask;
  for (int i = 0; i < count; i++) sf[i] += base - (weights ? weights[i] : 0);
}

static void distance(a9_br_t *br, int *sf, const int *baseline, int base_len, int n) {
  const a9_book_t *cb = &g_sfs[br_read(br, 2) + 2];
  int count = n < base_len ? n : base_len;
  for (int i = 0; i < count; i++) sf[i] = baseline[i] + sign_extend(br_huff(br, cb), cb->vbits);
  for (int i = count; i < n; i++) sf[i] = br_read(br, 5);
}

static void offset_baseline(a9_br_t *br, int *sf, const int *baseline, int base_len, int n) {
  int base = br_read(br, 5) - 16;
  const a9_book_t *cb = &g_sfu[br_read(br, 2) + 1];
  int count = n < base_len ? n : base_len;
  delta_offset(br, sf, NULL, 0, cb, count);
  for (int i = 0; i < count; i++) sf[i] += base + baseline[i];
  for (int i = count; i < n; i++) sf[i] = br_read(br, 5);
}

static void read_scale_factors(a9_dec_t *d, a9_block_t *blk, a9_chan_t *ch, int first) {
  a9_br_t *br = &d->br;
  int *sf = ch->sf;
  int n = blk->ext_unit, mode = br_read(br, 2);
  memset(sf, 0, sizeof(ch->sf));
  if (mode == 0) {
    const int *weights = k_at9_sfweights[br_read(br, 3)];
    int base = br_read(br, 5);
    const a9_book_t *cb = &g_sfu[br_read(br, 2) + 3];
    delta_offset(br, sf, weights, base, cb, n);
  } else if (ch->index == 0) {
    if (mode == 1) {
      int len = br_read(br, 2) + 2, base = len < 5 ? br_read(br, 5) : 0;
      for (int i = 0; i < n; i++) sf[i] = br_read(br, len) + base;
    } else {
      if (first) a9_fail("scale factors refer to a previous frame in the first frame");
      if (mode == 2) distance(br, sf, ch->sf_prev, blk->units_prev, n);
      else offset_baseline(br, sf, ch->sf_prev, blk->units_prev, n);
    }
  } else if (mode == 1) {
    distance(br, sf, blk->ch[0].sf, n, n);
  } else if (mode == 2) {
    offset_baseline(br, sf, blk->ch[0].sf, n, n);
  } else {
    if (first) a9_fail("scale factors refer to a previous frame in the first frame");
    distance(br, sf, ch->sf_prev, blk->units_prev, n);
  }
  for (int i = 0; i < n; i++)
    if (sf[i] < 0 || sf[i] > 31) a9_fail("scale factor out of range");
  memcpy(ch->sf_prev, sf, sizeof(ch->sf));
}

static void read_spectra(a9_dec_t *d, a9_chan_t *ch) {
  a9_br_t *br = &d->br;
  int max_huff = d->cfg->high ? 1 : 7;
  memset(ch->q, 0, sizeof(ch->q));
  memset(ch->qf, 0, sizeof(ch->qf));
  for (int i = 0; i < ch->coded; i++) {
    int start = k_at9_unitstart[i], end = k_at9_unitstart[i + 1], bits = ch->prec[i] + 1;
    if (bits <= max_huff) {
      const a9_book_t *cb = &g_spec[ch->set[i]][bits][k_at9_unitbook[i]];
      if (!cb->valid) a9_fail("invalid spectrum codebook");
      for (int k = start; k < end; k += cb->count) {
        int v = br_huff(br, cb);
        for (int j = 0; j < cb->count; j++, v >>= cb->vbits) ch->q[k + j] = sign_extend(v & cb->mask, cb->vbits);
      }
    } else {
      for (int k = start; k < end; k++) ch->q[k] = br_signed(br, bits);
    }
  }
  for (int i = 0; i < ch->coded; i++) {
    if (ch->fine[i] == 0) continue;
    for (int k = k_at9_unitstart[i]; k < k_at9_unitstart[i + 1]; k++) ch->qf[k] = br_signed(br, ch->fine[i] + 1);
  }
}

static void read_bex(a9_dec_t *d, a9_block_t *blk) {
  a9_br_t *br = &d->br;
  int band = 0;
  if (blk->bex_enabled) {
    if (blk->units < 13 || blk->units > 20) a9_fail("band extension with an invalid band count");
    band = k_at9_bexgroups[blk->units - 13][2];
    if (blk->type == A9_STEREO) {
      int m = br_read(br, 2);
      blk->ch[1].bex_mode = band > 2 ? m : 4;
    } else {
      br_read(br, 1);
    }
  }
  blk->has_ext = br_read(br, 1);
  if (!blk->has_ext) return;
  if (!blk->bex_enabled) {
    br_read(br, 2);
    int skip = br_read(br, 5);
    for (int i = 0; i < skip; i++) br_read(br, 1);
    return;
  }
  int mode = br_read(br, 2);
  blk->ch[0].bex_mode = band > 2 ? mode : 4;
  int len = br_read(br, 5);
  if (len == 0) return;
  size_t end = br->pos + (size_t)len;
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    for (int i = 0; i < k_at9_bexcounts[ch->bex_mode][band]; i++) ch->bex_values[i] = br_read(br, k_at9_bexlens[ch->bex_mode][band][i]);
  }
  if (br->pos > end) a9_fail("band extension data is longer than declared");
}

static void unpack_block(a9_dec_t *d, a9_block_t *blk, int frame_index) {
  a9_br_t *br = &d->br;
  const a9_cfg_t *cfg = d->cfg;
  int first = br_read(br, 1) == 0;
  int reuse = br_read(br, 1);
  if (first != (frame_index == 0)) a9_fail("superframe flag does not match the frame position");
  if (blk->type == A9_LFE) {
    a9_chan_t *lfe = &blk->ch[0];
    blk->units = 2;
    lfe->coded = 2;
    memset(lfe->sf, 0, sizeof(lfe->sf));
    lfe->sf[0] = br_read(br, 5);
    lfe->sf[1] = br_read(br, 5);
    int p = reuse ? 8 : 4;
    blk->lfe_reuse = reuse;
    lfe->prec[0] = lfe->prec[1] = p;
    lfe->fine[0] = lfe->fine[1] = 0;
    memset(lfe->q, 0, sizeof(lfe->q));
    memset(lfe->qf, 0, sizeof(lfe->qf));
    for (int k = 0; k < 4; k++) lfe->q[k] = br_signed(br, p + 1);
    br->pos = (br->pos + 7) & ~(size_t)7;
    return;
  }
  if (first && reuse) a9_fail("first frame of a superframe reuses band parameters");
  int min_bands = cfg->high ? 1 : 3;
  if (!reuse) {
    blk->band_count = br_read(br, 4) + min_bands;
    if (blk->band_count > k_at9_maxbands[cfg->sri]) a9_fail("band count out of range");
    blk->units = k_at9_bandunits[blk->band_count];
    int stereo_band = blk->type == A9_STEREO ? br_read(br, 4) + min_bands : blk->band_count;
    if (stereo_band > blk->band_count) a9_fail("stereo band out of range");
    blk->stereo_units = k_at9_bandunits[stereo_band];
    blk->bex_enabled = br_read(br, 1);
    if (blk->bex_enabled) {
      int ext = br_read(br, 4) + min_bands;
      if (ext < blk->band_count || ext > (cfg->high ? 16 : 18)) a9_fail("extension band out of range");
      blk->ext_unit = k_at9_bandunits[ext];
    } else {
      blk->ext_unit = blk->units;
    }
    blk->have_bands = 1;
  } else if (!blk->have_bands) {
    a9_fail("block reuses band parameters it never had");
  }

  blk->grad_mode = br_read(br, 2);
  int start, end, sv, ev;
  if (blk->grad_mode != 0) {
    start = br_read(br, 5);
    end = 31;
    sv = br_read(br, 5);
    ev = 31;
  } else {
    start = br_read(br, 6);
    end = br_read(br, 6) + 1;
    sv = br_read(br, 5);
    ev = br_read(br, 5);
  }
  blk->boundary = br_read(br, 4);
  make_gradient(blk, start, end, sv, ev);

  if (blk->type == A9_STEREO) {
    blk->primary = br_read(br, 1);
    int signs = br_read(br, 1);
    memset(blk->signs, 0, sizeof(blk->signs));
    if (signs)
      for (int i = blk->stereo_units; i < blk->units; i++) blk->signs[i] = br_read(br, 1);
  } else {
    blk->primary = 0;
  }
  read_bex(d, blk);

  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    ch->coded = ch->index == blk->primary ? blk->units : blk->stereo_units;
    read_scale_factors(d, blk, ch, first);
    precisions(blk, ch);
    codebook_sets(cfg, ch, ch->coded);
    read_spectra(d, ch);
  }
  blk->units_prev = blk->bex_enabled ? blk->ext_unit : blk->units;
  br->pos = (br->pos + 7) & ~(size_t)7;
}

static void imdct_chan(a9_dec_t *d, a9_chan_t *ch) {
  int size = d->cfg->frame_samples, half = size >> 1;
  const double *w = g_imdct[d->cfg->power];
  double *o = d->out_buf, *prev = ch->prev, *pcm = ch->pcm;
  dct4(d->cfg->power, ch->spectra, o, d->work);
  for (int i = 0; i < half; i++) {
    pcm[i] = w[i] * o[i + half] + prev[i];
    pcm[i + half] = w[i + half] * -o[size - 1 - i] - prev[i + half];
    prev[i] = w[size - 1 - i] * -o[half - i - 1];
    prev[i + half] = w[half - i - 1] * o[i];
  }
}

static void synthesize(a9_dec_t *d, a9_block_t *blk) {
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    double *sp = ch->spectra;
    memset(sp, 0, sizeof(ch->spectra));
    for (int i = 0; i < ch->coded; i++) {
      double step = k_at9_step[ch->prec[i]], fine = k_at9_stepfine[ch->fine[i]];
      for (int k = k_at9_unitstart[i]; k < k_at9_unitstart[i + 1]; k++) sp[k] = ch->q[k] * step + ch->qf[k] * fine;
    }
  }
  if (blk->type == A9_STEREO && blk->stereo_units < blk->units) {
    a9_chan_t *src = &blk->ch[blk->primary], *dst = &blk->ch[blk->primary ? 0 : 1];
    for (int i = blk->stereo_units; i < blk->units; i++)
      for (int k = k_at9_unitstart[i]; k < k_at9_unitstart[i + 1]; k++) dst->spectra[k] = blk->signs[i] ? -src->spectra[k] : src->spectra[k];
  }
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    for (int i = 0; i < blk->units; i++) {
      double scale = ldexp(1.0, ch->sf[i] - 15);
      for (int k = k_at9_unitstart[i]; k < k_at9_unitstart[i + 1]; k++) ch->spectra[k] *= scale;
    }
    if (blk->type != A9_LFE && blk->bex_enabled && blk->has_ext) a9_fail("the music uses band extension, which this decoder does not handle");
    imdct_chan(d, ch);
  }
}

static void profile_add_block(a9_profile_t *p, const a9_block_t *blk) {
  if (blk->type == A9_LFE) {
    p->lfe_reuse |= 1 << blk->lfe_reuse;
    return;
  }
  p->modes |= 1 << blk->grad_mode;
  if (blk->band_count > p->max_bands) p->max_bands = blk->band_count;
  p->bex = p->bex || blk->bex_enabled;
  for (int c = 0; c < blk->nch; c++) {
    const a9_chan_t *ch = &blk->ch[c];
    for (int i = 0; i < ch->coded; i++) {
      if (ch->prec[i] > p->max_prec) p->max_prec = ch->prec[i];
      if (ch->fine[i]) p->fine = 1;
    }
  }
}

/* Decodes the next frame of the superframe. dst (one pointer per channel in file order, NULL for a channel not wanted) gets the
 * frame's samples (-1..1, before clamping) at offset; with dst NULL the frame is only parsed. */
static void dec_frame(a9_dec_t *d, int frame_in_sf, float *const *dst, int offset) {
  size_t before = d->br.pos;
  for (int b = 0; b < d->cfg->nblocks; b++) {
    unpack_block(d, &d->blocks[b], frame_in_sf);
    profile_add_block(&d->profile, &d->blocks[b]);
  }
  int mf = (int)((d->br.pos - before) >> 3);
  if (mf > d->profile.max_frame) d->profile.max_frame = mf;
  if (!dst) return;
  int c = 0;
  for (int b = 0; b < d->cfg->nblocks; b++) {
    a9_block_t *blk = &d->blocks[b];
    int need = 0;
    for (int i = 0; i < blk->nch; i++) need |= dst[c + i] != NULL;
    if (need) synthesize(d, blk);
    for (int i = 0; i < blk->nch; i++) {
      float *o = dst[c++];
      if (!o) continue;
      for (int k = 0; k < d->cfg->frame_samples; k++) o[offset + k] = (float)(blk->ch[i].pcm[k] / 32768);
    }
  }
}

/* Why `got` uses something `stock` never does, or NULL. */
static const char *fits_profile(const a9_profile_t *got, const a9_profile_t *stock, int frame_bytes) {
  static char buf[160];
  if (got->modes & ~stock->modes) return "uses a bit allocation mode the system sound never does";
  if (got->max_bands > stock->max_bands) {
    snprintf(buf, sizeof(buf), "uses %d bands; the system sound uses at most %d", got->max_bands, stock->max_bands);
    return buf;
  }
  if (got->max_prec > stock->max_prec) {
    snprintf(buf, sizeof(buf), "uses precision %d; the system sound uses at most %d", got->max_prec, stock->max_prec);
    return buf;
  }
  if (got->fine && !stock->fine) return "uses fine precision, which the system sound never does";
  if (got->bex && !stock->bex) return "uses band extension, which the system sound never does";
  if (got->lfe_reuse & ~stock->lfe_reuse) return "codes the LFE channel differently from the system sound";
  int cap = (int)floor(frame_bytes * FRAME_CAP);
  int limit = stock->max_frame > cap ? stock->max_frame : cap;
  if (got->max_frame > limit) {
    snprintf(buf, sizeof(buf), "has a %d-byte frame; the limit is %d", got->max_frame, limit);
    return buf;
  }
  uint8_t pad = prof_padding_byte(stock);
  for (int i = 0; i < got->npad; i++) {
    uint8_t v = got->padding[i];
    if (v != pad && !stock->seen[v]) {
      snprintf(buf, sizeof(buf), "fills unused space with 0x%02x where the system sound uses 0x%02x", v, pad);
      return buf;
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* encoder                                                                                                              */
/* ------------------------------------------------------------------------------------------------------------------ */

typedef struct {
  const a9_cfg_t *cfg;
  int max_bands, max_precision, lfe_reuse;
  uint8_t padding;
  a9_block_t states[MAX_FPS][MAX_BLOCKS];
  int audible[MAX_FPS][MAX_BLOCKS];
  const double *win;
  double z[512], u[256], work[256];
  int budget, cap, fps, frame_samples;
  unsigned char cost_valid[MAX_FPS][NCAND];
  int cost_bits[MAX_FPS][NCAND];
  double cost_err[MAX_FPS][NCAND];
  int choice[MAX_FPS], smallest[MAX_FPS], trial[MAX_FPS];
} a9_enc_t;

static double js_round(double v) {
  double f = floor(v);
  return v - f >= 0.5 ? f + 1 : f;
}

static void enc_init(a9_enc_t *e, const a9_cfg_t *cfg, const a9_profile_t *profile, const float *const *windows) {
  memset(e, 0, sizeof(*e));
  e->cfg = cfg;
  int mb = k_at9_maxbands[cfg->sri];
  e->max_bands = profile->max_bands < mb ? profile->max_bands : mb;
  e->max_precision = profile->max_prec < MAX_PRECISION_LIMIT ? profile->max_prec : MAX_PRECISION_LIMIT;
  e->lfe_reuse = (profile->lfe_reuse & 2) != 0 ? 1 : 0;
  e->padding = prof_padding_byte(profile);
  e->fps = cfg->fps;
  e->frame_samples = cfg->frame_samples;
  e->budget = cfg->sf_bytes * 8;
  e->cap = (int)floor(cfg->frame_bytes * FRAME_CAP) * 8;
  e->win = g_mdct[cfg->power];
  for (int fi = 0; fi < e->fps; fi++) {
    int c = 0;
    for (int bi = 0; bi < cfg->nblocks; bi++) {
      a9_block_t *blk = &e->states[fi][bi];
      block_init(blk, cfg->blocks[bi]);
      for (int k = 0; k < blk->nch; k++) {
        blk->ch[k].src = windows[c++];
        if (blk->type != A9_LFE && blk->ch[k].src) e->audible[fi][bi] = 1;
      }
    }
  }
}

static void enc_forward(a9_enc_t *e, a9_chan_t *ch, int from) {
  const float *src = ch->src;
  if (!src) {
    memset(ch->x, 0, sizeof(ch->x));
    return;
  }
  int n = e->frame_samples, h = n >> 1;
  for (int i = 0; i < 2 * n; i++) {
    double v = src[from + i];
    v = v > 1 ? 1 : v < -1 ? -1 : v;
    e->z[i] = v * 32768 * (i < n ? e->win[i] : e->win[2 * n - 1 - i]);
  }
  for (int k = 0; k < h; k++) e->u[k] = -e->z[3 * h - 1 - k] - e->z[3 * h + k];
  for (int k = h; k < n; k++) e->u[k] = e->z[k - h] - e->z[3 * h - 1 - k];
  dct4(e->cfg->power, e->u, ch->x, e->work);
  double scale = 2.0 / n;
  for (int k = 0; k < n; k++) ch->x[k] *= scale;
}

static int enc_bands_for(const a9_enc_t *e, const a9_block_t *blk, int limit) {
  int last = -1;
  for (int c = 0; c < blk->nch; c++) {
    for (int k = 255; k > last; k--) {
      if (fabs(blk->ch[c].x[k]) >= 3.0517578125e-5) {
        last = k;
        break;
      }
    }
  }
  int b = e->cfg->high ? 1 : 3;
  while (b < limit && k_at9_unitstart[k_at9_bandunits[b]] <= last) b++;
  return b;
}

static int scale_factor_for(double peak) {
  if (!(peak > 0)) return 0;
  double v = ceil(log2(peak) + 15 - 1e-9);
  return (int)(v < 0 ? 0 : v > 31 ? 31 : v);
}

static a9_sfplan_t plan_scale_factors(const a9_block_t *blk, const a9_chan_t *ch, const a9_chan_t *other) {
  const int *sf = ch->sf;
  int n = blk->units;
  a9_sfplan_t best;
  memset(&best, 0, sizeof(best));
  best.plan_bits = 0x7fffffff;
  int have = 0;
  for (int w = 0; w < 8; w++) {
    const int *weights = k_at9_sfweights[w];
    int lo = 99, hi = -99;
    for (int i = 0; i < n; i++) {
      int v = sf[i] + weights[i];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    int base = lo < 31 ? lo : 31;
    if (base < 0) continue;
    for (int len = 3; len <= 6; len++) {
      if (hi - base >= 1 << len) continue;
      const a9_book_t *cb = &g_sfu[len];
      int bits = 2 + 3 + 5 + 2 + len, ok = 1;
      for (int i = 1; i < n && ok; i++) {
        int d = (sf[i] + weights[i] - (sf[i - 1] + weights[i - 1])) & cb->mask;
        if (cb->bits[d] == 0) ok = 0;
        else bits += cb->bits[d];
      }
      if (!ok) continue;
      if (!have || bits < best.plan_bits) {
        best = (a9_sfplan_t){bits, 0, w, base, len};
        have = 1;
      }
    }
  }
  if (ch->index == 0) {
    int lo = 31, hi = 0;
    for (int i = 0; i < n; i++) {
      if (sf[i] < lo) lo = sf[i];
      if (sf[i] > hi) hi = sf[i];
    }
    for (int len = 2; len <= 5; len++) {
      int base = len < 5 ? lo : 0;
      if (hi - base >= 1 << len) continue;
      int bits = 2 + 2 + (len < 5 ? 5 : 0) + n * len;
      if (!have || bits < best.plan_bits) {
        best = (a9_sfplan_t){bits, 1, 0, base, len};
        have = 1;
      }
    }
  } else {
    for (int len = 2; len <= 5; len++) {
      const a9_book_t *cb = &g_sfs[len];
      int half = 1 << (len - 1);
      int bits = 2 + 2, ok = 1;
      for (int i = 0; i < n && ok; i++) {
        int d = sf[i] - other->sf[i];
        if (d < -half || d >= half || cb->bits[d & cb->mask] == 0) ok = 0;
        else bits += cb->bits[d & cb->mask];
      }
      if (!ok) continue;
      if (!have || bits < best.plan_bits) {
        best = (a9_sfplan_t){bits, 2, 0, 0, len};
        have = 1;
      }
    }
  }
  if (!have) a9_fail("internal error: no scale factor coding fits");
  return best;
}

static void enc_setup_block(a9_enc_t *e, a9_block_t *blk, int band_count, int reuse) {
  blk->band_count = band_count;
  blk->units = k_at9_bandunits[band_count];
  blk->stereo_units = blk->units;
  blk->ext_unit = blk->units;
  blk->reuse = reuse;
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    ch->coded = blk->units;
    memset(ch->sf, 0, sizeof(ch->sf));
    for (int i = 0; i < blk->units; i++) {
      double peak = 0;
      for (int k = k_at9_unitstart[i]; k < k_at9_unitstart[i + 1]; k++) {
        double a = fabs(ch->x[k]);
        if (a > peak) peak = a;
      }
      ch->sf[i] = scale_factor_for(peak);
    }
    codebook_sets(e->cfg, ch, blk->units);
    memset(ch->mask, 0, sizeof(ch->mask));
    for (int i = 1; i < blk->units; i++) {
      int d = ch->sf[i] - ch->sf[i - 1];
      if (d > 1) ch->mask[i] += d - 1 < 5 ? d - 1 : 5;
      else if (d < -1) ch->mask[i - 1] += -d - 1 < 5 ? -d - 1 : 5;
    }
    for (int i = 0; i < 32 * 16; i++) ch->memo_bits[i] = -1;
  }
  for (int c = 0; c < blk->nch; c++) blk->ch[c].sfplan = plan_scale_factors(blk, &blk->ch[c], &blk->ch[0]);
}

static void write_scale_factors(a9_bw_t *wr, const a9_block_t *blk, const a9_chan_t *ch) {
  a9_sfplan_t plan = ch->sfplan;
  const int *sf = ch->sf;
  int n = blk->units;
  switch (plan.kind) {
  case 0: {
    const int *weights = k_at9_sfweights[plan.weights];
    const a9_book_t *cb = &g_sfu[plan.len];
    bw_write(wr, 0, 2);
    bw_write(wr, plan.weights, 3);
    bw_write(wr, plan.base, 5);
    bw_write(wr, plan.len - 3, 2);
    bw_write(wr, sf[0] + weights[0] - plan.base, plan.len);
    for (int i = 1; i < n; i++) {
      int d = (sf[i] + weights[i] - (sf[i - 1] + weights[i - 1])) & cb->mask;
      bw_write(wr, cb->codes[d], cb->bits[d]);
    }
    break;
  }
  case 1:
    bw_write(wr, 1, 2);
    bw_write(wr, plan.len - 2, 2);
    if (plan.len < 5) bw_write(wr, plan.base, 5);
    for (int i = 0; i < n; i++) bw_write(wr, sf[i] - plan.base, plan.len);
    break;
  default: {
    const a9_chan_t *other = &blk->ch[0];
    const a9_book_t *cb = &g_sfs[plan.len];
    bw_write(wr, 1, 2);
    bw_write(wr, plan.len - 2, 2);
    for (int i = 0; i < n; i++) {
      int d = (sf[i] - other->sf[i]) & cb->mask;
      bw_write(wr, cb->codes[d], cb->bits[d]);
    }
    break;
  }
  }
}

/* Quantizes one unit at precision p (1..15). Returns the coded bits and the squared error; with store the values are kept for writing. */
static int quantize_unit(const a9_enc_t *e, a9_chan_t *ch, int unit, int p, int store, double *err) {
  int start = k_at9_unitstart[unit], end = k_at9_unitstart[unit + 1];
  double scale = ldexp(1.0, ch->sf[unit] - 15), step = k_at9_step[p];
  int lim = (1 << p) - 1;
  int *q = store ? ch->q : ch->scratch_q;
  const double *x = ch->x;
  for (int k = start; k < end; k++) {
    double c = js_round(x[k] / scale / step);
    q[k] = c > lim ? lim : c < -lim ? -lim : (int)c;
  }
  int bits = 0;
  if (p + 1 <= (e->cfg->high ? 1 : 7)) {
    a9_book_t *cb = &g_spec[ch->set[unit]][p + 1][k_at9_unitbook[unit]];
    const int *nr = book_near(cb);
    for (int k = start; k < end; k += cb->count) {
      int sym = 0;
      for (int j = 0; j < cb->count; j++) sym |= (q[k + j] & cb->mask) << (j * cb->vbits);
      if (cb->bits[sym] == 0) {
        sym = nr[sym];
        for (int j = 0, v = sym; j < cb->count; j++, v >>= cb->vbits) q[k + j] = sign_extend(v & cb->mask, cb->vbits);
      }
      bits += cb->bits[sym];
    }
  } else {
    bits = (end - start) * (p + 1);
  }
  double sum = 0;
  for (int k = start; k < end; k++) {
    double d = x[k] - q[k] * step * scale;
    sum += d * d;
  }
  *err = sum;
  return bits;
}

static int unit_precision(const a9_chan_t *ch, int unit, const int *g, int boundary) {
  int p = ch->sf[unit] + ch->mask[unit] - g[unit];
  if (p > 0) p >>= 2;
  return (p > 1 ? p : 1) + (unit < boundary ? 1 : 0);
}

static int header_bits(const a9_enc_t *e, const a9_block_t *blk) {
  if (blk->type == A9_LFE) return 2 + 10 + 4 * ((e->lfe_reuse ? 8 : 4) + 1);
  int bits = 2;
  if (!blk->reuse) bits += 4 + (blk->type == A9_STEREO ? 4 : 0) + 1;
  bits += 2 + 5 + 5 + 4;
  if (blk->type == A9_STEREO) bits += 2;
  return bits + 1;
}

static void prepare_gradient(a9_block_t *blk, const int *g) {
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    for (int i = 0; i < blk->units; i++) {
      int p = ch->sf[i] + ch->mask[i] - g[i];
      if (p > 0) p >>= 2;
      ch->base_prec[i] = p > 1 ? p : 1;
    }
  }
}

static int block_cost(const a9_enc_t *e, a9_block_t *blk, int boundary, int *bytes_in_bits, double *err) {
  int bits = header_bits(e, blk);
  double sum = 0;
  int units = blk->units;
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    bits += ch->sfplan.plan_bits;
    for (int i = 0; i < units; i++) {
      int p = ch->base_prec[i] + (i < boundary ? 1 : 0);
      if (p > e->max_precision) {
        *bytes_in_bits = 0;
        *err = 0;
        return 0;
      }
      int key = i * 16 + p;
      int b = ch->memo_bits[key];
      if (b < 0) {
        double ee;
        b = ch->memo_bits[key] = quantize_unit(e, ch, i, p, 0, &ee);
        ch->memo_err[key] = ee;
      }
      bits += b;
      sum += ch->memo_err[key];
    }
  }
  *bytes_in_bits = (bits + 7) / 8 * 8;
  *err = sum;
  return 1;
}

static int silent_block_bits(const a9_enc_t *e, a9_block_t *blk) {
  if (blk->type == A9_LFE) return (header_bits(e, blk) + 7) / 8 * 8;
  prepare_gradient(blk, g_silent_grad);
  int bits;
  double err;
  if (!block_cost(e, blk, 0, &bits, &err)) a9_fail("internal error: silent block over the precision limit");
  return bits;
}

static void write_block(a9_enc_t *e, a9_bw_t *wr, a9_block_t *blk, int frame_index, int plan_start, int plan_value, int plan_boundary, const int *g) {
  bw_write(wr, frame_index != 0 ? 1 : 0, 1);
  if (blk->type == A9_LFE) {
    bw_write(wr, e->lfe_reuse, 1);
    bw_write(wr, 0, 10);
    bw_write(wr, 0, 4 * ((e->lfe_reuse ? 8 : 4) + 1));
    bw_align(wr);
    return;
  }
  bw_write(wr, blk->reuse ? 1 : 0, 1);
  int min_bands = e->cfg->high ? 1 : 3;
  if (!blk->reuse) {
    bw_write(wr, blk->band_count - min_bands, 4);
    if (blk->type == A9_STEREO) bw_write(wr, blk->band_count - min_bands, 4);
    bw_write(wr, 0, 1);
  }
  int boundary = plan_boundary < blk->units ? plan_boundary : blk->units;
  bw_write(wr, 3, 2);
  bw_write(wr, plan_start, 5);
  bw_write(wr, plan_value, 5);
  bw_write(wr, boundary, 4);
  if (blk->type == A9_STEREO) {
    bw_write(wr, 0, 1);
    bw_write(wr, 0, 1);
  }
  bw_write(wr, 0, 1);
  for (int c = 0; c < blk->nch; c++) {
    a9_chan_t *ch = &blk->ch[c];
    write_scale_factors(wr, blk, ch);
    for (int i = 0; i < blk->units; i++) {
      int p = unit_precision(ch, i, g, boundary), start = k_at9_unitstart[i], end = k_at9_unitstart[i + 1];
      double ee;
      quantize_unit(e, ch, i, p, 1, &ee);
      if (p + 1 <= (e->cfg->high ? 1 : 7)) {
        const a9_book_t *cb = &g_spec[ch->set[i]][p + 1][k_at9_unitbook[i]];
        for (int k = start; k < end; k += cb->count) {
          int sym = 0;
          for (int j = 0; j < cb->count; j++) sym |= (ch->q[k + j] & cb->mask) << (j * cb->vbits);
          bw_write(wr, cb->codes[sym], cb->bits[sym]);
        }
      } else {
        for (int k = start; k < end; k++) bw_write(wr, ch->q[k] & ((1 << (p + 1)) - 1), p + 1);
      }
    }
  }
  bw_align(wr);
}

static long enc_total(const a9_enc_t *e, const int *pick) {
  long sum = 0;
  for (int fi = 0; fi < e->fps; fi++) {
    if (pick[fi] < 0) return 0x7fffffffffffffffL;
    sum += e->cost_bits[fi][pick[fi]];
  }
  return sum;
}

static void enc_pick(a9_enc_t *e, double lambda, int *into) {
  for (int fi = 0; fi < e->fps; fi++) {
    int best = -1;
    double score = INFINITY;
    for (int ci = 0; ci < NCAND; ci++) {
      if (!e->cost_valid[fi][ci]) continue;
      double v = e->cost_err[fi][ci] + lambda * e->cost_bits[fi][ci];
      if (v < score || (v == score && best >= 0 && e->cost_bits[fi][ci] < e->cost_bits[fi][best])) {
        score = v;
        best = ci;
      }
    }
    into[fi] = best;
  }
}

static int enc_allocate(a9_enc_t *e) {
  int nb = e->cfg->nblocks;
  for (int fi = 0; fi < e->fps; fi++) {
    a9_block_t *blocks = e->states[fi];
    int fixed_bits = 0;
    for (int bi = 0; bi < nb; bi++)
      if (!e->audible[fi][bi]) fixed_bits += silent_block_bits(e, &blocks[bi]);
    for (int ci = 0; ci < NCAND; ci++) {
      const a9_cand_t *c = &g_cand[ci];
      if (ci % 6 == 0)
        for (int bi = 0; bi < nb; bi++)
          if (e->audible[fi][bi]) prepare_gradient(&blocks[bi], c->g);
      int bits = fixed_bits, ok = 1;
      double err = 0;
      for (int bi = 0; bi < nb; bi++) {
        if (!e->audible[fi][bi]) continue;
        int b;
        double ee;
        if (!block_cost(e, &blocks[bi], c->boundary, &b, &ee)) {
          ok = 0;
          break;
        }
        bits += b;
        err += ee;
      }
      e->cost_valid[fi][ci] = (unsigned char)(ok && bits <= e->cap);
      e->cost_bits[fi][ci] = bits;
      e->cost_err[fi][ci] = err;
    }
  }
  enc_pick(e, 0, e->choice);
  if (enc_total(e, e->choice) <= e->budget) return 1;
  for (int fi = 0; fi < e->fps; fi++) {
    int best = -1;
    for (int ci = 0; ci < NCAND; ci++) {
      if (!e->cost_valid[fi][ci]) continue;
      if (best < 0 || e->cost_bits[fi][ci] < e->cost_bits[fi][best] ||
          (e->cost_bits[fi][ci] == e->cost_bits[fi][best] && e->cost_err[fi][ci] < e->cost_err[fi][best]))
        best = ci;
    }
    e->smallest[fi] = best;
  }
  if (enc_total(e, e->smallest) > e->budget) return 0;
  double lo = -12, hi = 12;
  for (int k = 0; k < 48; k++) {
    double mid = (lo + hi) / 2;
    enc_pick(e, pow(10, mid), e->trial);
    if (enc_total(e, e->trial) <= e->budget) hi = mid;
    else lo = mid;
  }
  enc_pick(e, pow(10, hi), e->choice);
  if (enc_total(e, e->choice) > e->budget) memcpy(e->choice, e->smallest, sizeof(int) * (size_t)e->fps);
  return 1;
}

static void enc_superframe(a9_enc_t *e, uint8_t *dst) {
  const a9_cfg_t *cfg = e->cfg;
  int min_bands = cfg->high ? 1 : 3;
  for (int fi = 0; fi < e->fps; fi++)
    for (int bi = 0; bi < cfg->nblocks; bi++)
      for (int c = 0; c < e->states[fi][bi].nch; c++) enc_forward(e, &e->states[fi][bi].ch[c], fi * e->frame_samples);
  int found = 0;
  for (int band_limit = e->max_bands; !found; band_limit--) {
    if (band_limit < min_bands) a9_fail("the console's ATRAC9 bitrate is too low for this encoder");
    for (int fi = 0; fi < e->fps; fi++) {
      for (int bi = 0; bi < cfg->nblocks; bi++) {
        a9_block_t *blk = &e->states[fi][bi];
        if (blk->type == A9_LFE) continue;
        int bands = e->audible[fi][bi] ? enc_bands_for(e, blk, band_limit) : min_bands;
        enc_setup_block(e, blk, bands, fi > 0 && e->states[fi - 1][bi].band_count == bands);
      }
    }
    found = enc_allocate(e);
  }
  memset(dst, 0, (size_t)cfg->sf_bytes);
  a9_bw_t wr = {dst, 0};
  for (int fi = 0; fi < e->fps; fi++) {
    size_t start = wr.pos;
    const a9_cand_t *c = &g_cand[e->choice[fi]];
    for (int bi = 0; bi < cfg->nblocks; bi++) {
      if (e->audible[fi][bi]) write_block(e, &wr, &e->states[fi][bi], fi, c->start, c->value, c->boundary, c->g);
      else write_block(e, &wr, &e->states[fi][bi], fi, SILENT_START, SILENT_VALUE, SILENT_BOUNDARY, g_silent_grad);
    }
    if ((int)(wr.pos - start) > e->cap) a9_fail("internal error: frame over its size limit");
  }
  if ((int)wr.pos > e->budget) a9_fail("internal error: superframe over budget");
  memset(dst + (wr.pos >> 3), e->padding, (size_t)cfg->sf_bytes - (wr.pos >> 3));
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* input windows, the check of the output, and the entry points                                                         */
/* ------------------------------------------------------------------------------------------------------------------ */

#define LOOP_START_SECONDS 2 /* seconds into the song where the loop starts */
#define LOOP_AFTER_SECONDS 1 /* seconds of audio after the loop end */
#define SEGMENT 1024
#define MIN_SNR 1.0
#define MIN_SEGMENT_SNR (-3.0)
#define QUIET (1.0 / (1 << 15))
#define SILENCE (1.0 / (1 << 20))

typedef struct {
  const a9_cfg_t *cfg;
  const int16_t *left, *right;
  long song, n, delay, loop_from;
  int src_of[MAX_CHANNELS]; /* per channel: 0 left, 1 right, 2 mid, -1 none */
  float *win[MAX_CHANNELS];
  long window_len;
} a9_feed_t;

static float feed_sample(const a9_feed_t *f, int which, long t) {
  if (t < 0 || t >= f->n) return 0.f;
  long i = t < f->song ? t : f->loop_from + (t - f->song);
  if (which == 0) return f->left[i] / 32768.f;
  if (which == 1) return f->right[i] / 32768.f;
  return (float)(((double)f->left[i] + f->right[i]) / 2 / 32768.0);
}

static void feed_fill(a9_feed_t *f, long s) {
  long from = s * f->cfg->sf_samples - f->delay;
  for (int c = 0; c < f->cfg->nch; c++) {
    if (f->src_of[c] < 0) continue;
    for (long i = 0; i < f->window_len; i++) f->win[c][i] = feed_sample(f, f->src_of[c], from + i);
  }
}

typedef struct {
  a9_dec_t dec;
  const a9_cfg_t *cfg;
  float *const *windows;
  float *out[MAX_CHANNELS];
  long n, delay;
  int used, segs;
  double *sig, *err;
} a9_verify_t;

static double around(const double *arr, int len, int k, int r) {
  double sum = 0;
  for (int j = k - r > 0 ? k - r : 0; j <= (k + r < len - 1 ? k + r : len - 1); j++) sum += arr[j];
  return sum;
}

static double v_samples(const a9_verify_t *v, int k, int r) {
  long lo = (long)(k - r) * SEGMENT;
  if (lo < 0) lo = 0;
  long hi = (long)(k + r + 1) * SEGMENT;
  if (hi > v->n) hi = v->n;
  return (double)(hi - lo) * v->used;
}

static void verify_check(a9_verify_t *v, long s) {
  int fps = v->cfg->fps, size = v->cfg->frame_samples;
  dec_begin_superframe(&v->dec, s);
  for (int f = 0; f < fps; f++) dec_frame(&v->dec, f, (float *const *)v->out, f * size);
  dec_end_superframe(&v->dec);
  long from = s * fps * size;
  long len = (long)fps * size;
  for (int c = 0; c < v->cfg->nch; c++) {
    const float *o = v->out[c];
    const float *src = v->windows[c];
    if (!src) {
      for (long i = 0; i < len; i++)
        if (fabs((double)o[i]) > SILENCE) a9_fail("a silent channel decoded to sound");
      continue;
    }
    for (long i = 0; i < len; i++) {
      long t = from + i - v->delay;
      if (t < 0 || t >= v->n) continue;
      double a = src[i] > 1 ? 1.0 : src[i] < -1 ? -1.0 : (double)src[i], e = o[i] - a;
      int seg = (int)(t / SEGMENT);
      v->sig[seg] += a * a;
      v->err[seg] += e * e;
    }
  }
}

static void verify_finish(a9_verify_t *v, const a9_profile_t *limits) {
  const char *why = fits_profile(&v->dec.profile, limits, v->cfg->frame_bytes);
  if (why) {
    char m[200];
    snprintf(m, sizeof(m), "internal error: the encoded music %s", why);
    a9_fail(m);
  }
  double signal = 0, noise = 0, worst = 99;
  for (int k = 0; k < v->segs; k++) {
    signal += v->sig[k];
    noise += v->err[k];
    if (around(v->sig, v->segs, k, 1) / v_samples(v, k, 1) <= QUIET * QUIET) {
      if (v->err[k] / v_samples(v, k, 0) > QUIET * QUIET) a9_fail("a quiet passage decoded with audible noise");
    }
    double s5 = around(v->sig, v->segs, k, 2);
    if (s5 / v_samples(v, k, 2) > QUIET * QUIET) {
      double e3 = around(v->err, v->segs, k, 1);
      double w = 10 * log10(s5 / (e3 > 1e-30 ? e3 : 1e-30));
      if (w < worst) worst = w;
    }
  }
  double snr = noise != 0 && signal / ((double)v->n * v->used) > QUIET * QUIET ? 10 * log10(signal / noise) : 99;
  if (!(snr >= MIN_SNR) || !(worst >= MIN_SEGMENT_SNR)) {
    char m[200];
    snprintf(m, sizeof(m), "the encoded music does not match the input closely enough (%.1f dB overall, %.1f dB worst passage)", snr, worst);
    a9_fail(m);
  }
}

/* The file with `superframes` empty superframes: fmt as it was, fact and smpl with the new sample counts and loop, no other chunks. */
static uint8_t *build_file(const a9_file_t *stock, long superframes, long samples, long delay, int has_loop, long loop_start, long loop_end, size_t *out_len) {
  size_t data_len = (size_t)superframes * (size_t)stock->cfg.sf_bytes;
  typedef struct {
    char id[5];
    uint8_t *body;
    size_t len;
  } part_t;
  part_t parts[17];
  int np = 0, have_fact = 0;
  for (int i = 0; i < stock->nchunks; i++) {
    const a9_chunk_t *ch = &stock->chunks[i];
    part_t *p = &parts[np];
    if (!strcmp(ch->id, "fmt ")) {
      p->len = ch->len;
      p->body = malloc(p->len);
      memcpy(p->body, stock->b + ch->off, p->len);
    } else if (!strcmp(ch->id, "fact")) {
      p->len = ch->len;
      p->body = malloc(p->len);
      memcpy(p->body, stock->b + ch->off, p->len);
      w32(p->body, 0, (uint32_t)samples);
      if (p->len >= 12) {
        w32(p->body, 4, (uint32_t)delay);
        w32(p->body, 8, (uint32_t)delay);
      }
      have_fact = 1;
    } else if (!strcmp(ch->id, "smpl") && ch->len >= 60) {
      p->len = ch->len;
      p->body = malloc(p->len);
      memcpy(p->body, stock->b + ch->off, p->len);
      w32(p->body, 44, (uint32_t)(has_loop ? loop_start : 0));
      w32(p->body, 48, (uint32_t)(has_loop ? loop_end : samples - 1));
    } else if (!strcmp(ch->id, "data")) {
      p->len = data_len;
      p->body = calloc(data_len ? data_len : 1, 1);
    } else {
      continue;
    }
    memcpy(p->id, ch->id, 5);
    np++;
  }
  if (!have_fact) {
    part_t fact;
    memcpy(fact.id, "fact", 5);
    fact.len = 12;
    fact.body = calloc(12, 1);
    w32(fact.body, 0, (uint32_t)samples);
    w32(fact.body, 4, (uint32_t)delay);
    w32(fact.body, 8, (uint32_t)delay);
    for (int i = np; i > 1; i--) parts[i] = parts[i - 1];
    parts[1] = fact;
    np++;
  }
  size_t size = 12;
  for (int i = 0; i < np; i++) size += 8 + parts[i].len + (parts[i].len & 1);
  uint8_t *r = calloc(size, 1);
  memcpy(r, "RIFF", 4);
  w32(r, 4, (uint32_t)(size - 8));
  memcpy(r + 8, "WAVE", 4);
  size_t pos = 12;
  for (int i = 0; i < np; i++) {
    memcpy(r + pos, parts[i].id, 4);
    w32(r, pos + 4, (uint32_t)parts[i].len);
    memcpy(r + pos + 8, parts[i].body, parts[i].len);
    pos += 8 + parts[i].len + (parts[i].len & 1);
    free(parts[i].body);
  }
  *out_len = size;
  return r;
}

typedef struct {
  a9_enc_t *enc;
  a9_dec_t *vdec;
  a9_verify_t *ver;
  a9_feed_t *feed;
  uint8_t *output;
  a9_file_t *outfile;
  float *vout[MAX_CHANNELS];
  double *sig, *err;
} a9_run_t;

static void run_free(a9_run_t *r) {
  free(r->enc);
  free(r->ver);
  free(r->vdec);
  if (r->feed)
    for (int c = 0; c < MAX_CHANNELS; c++) free(r->feed->win[c]);
  free(r->feed);
  for (int c = 0; c < MAX_CHANNELS; c++) free(r->vout[c]);
  free(r->sig);
  free(r->err);
  free(r->outfile);
}

int a9_stock_info(const uint8_t *stock, size_t stock_len, int *rate, int *channels, char *err, size_t errsz) {
  pthread_once(&g_once, tables_init);
  jmp_buf jb;
  t_jb = &jb;
  t_err[0] = 0;
  if (setjmp(jb)) {
    t_jb = NULL;
    if (err && errsz) snprintf(err, errsz, "%s", t_err);
    return -1;
  }
  a9_file_t f;
  file_parse(stock, stock_len, &f);
  if (rate) *rate = f.cfg.rate;
  if (channels) *channels = f.cfg.nch;
  t_jb = NULL;
  return 0;
}

int a9_encode(const uint8_t *stock, size_t stock_len, const int16_t *left, const int16_t *right, size_t n_in, int intro,
              a9_progress_fn progress, void *progress_ctx, uint8_t **out, size_t *out_len, char *err, size_t errsz) {
  pthread_once(&g_once, tables_init);
  *out = NULL;
  *out_len = 0;
  static __thread a9_run_t *run_ptr;
  a9_run_t *run = calloc(1, sizeof(*run));
  run_ptr = run;
  uint8_t *volatile result = NULL;
  jmp_buf jb;
  t_jb = &jb;
  t_err[0] = 0;
  if (setjmp(jb)) {
    t_jb = NULL;
    free(result);
    run_free(run_ptr);
    free(run_ptr);
    if (err && errsz) snprintf(err, errsz, "%s", t_err);
    return -1;
  }
  a9_file_t file;
  file_parse(stock, stock_len, &file);
  const a9_cfg_t *cfg = &file.cfg;
  if (cfg->high) a9_fail("this console's music uses a high sample rate the encoder does not support");
  if (cfg->power != 8) a9_fail("unsupported ATRAC9 frame size");
  if (cfg->fps > MAX_FPS) a9_fail("unsupported ATRAC9 superframe size");
  long song = (long)n_in;
  if (song <= 0) a9_fail("the music is empty");

  /* looping files get the layout of the console's own music: the loop starts inside the audio and more audio follows its end */
  if (!(intro >= 0 && intro <= song - 1)) intro = 0;
  long loop_start = 0, loop_end = 0, loop_from = 0, tail = 0;
  int loops = file.has_loop;
  if (loops) {
    if (intro > 0) {
      long after = LOOP_AFTER_SECONDS * (long)cfg->rate;
      if ((song - intro) / 4 < after) after = (song - intro) / 4;
      loop_start = intro;
      loop_end = song - 1;
      loop_from = intro;
      tail = after;
    } else {
      long start = LOOP_START_SECONDS * (long)cfg->rate, after = LOOP_AFTER_SECONDS * (long)cfg->rate;
      if (song / 4 < start) start = song / 4;
      if (song / 4 < after) after = song / 4;
      loop_start = start;
      loop_end = song + start - 1;
      loop_from = 0;
      tail = start + after;
    }
  }
  long n = song + (loops ? tail : 0);

  /* what the system file uses bounds what the encoder writes */
  a9_profile_t profile;
  if (file.superframes > 0) {
    a9_dec_t *dec = calloc(1, sizeof(*dec));
    run->vdec = dec;
    dec_init(dec, &file);
    for (long s = 0; s < file.superframes; s++) {
      dec_begin_superframe(dec, s);
      for (int f = 0; f < cfg->fps; f++) dec_frame(dec, f, NULL, 0);
      dec_end_superframe(dec);
    }
    profile = dec->profile;
    free(dec);
    run->vdec = NULL;
  } else {
    prof_default(&profile);
  }
  if (!(profile.modes & 8)) a9_fail("the system sound is coded in a way this encoder does not support");

  long size = cfg->frame_samples;
  long delay = file.has_fact && (long)file.fact[2] > size ? (long)file.fact[2] : size;
  long frames = (delay + n + size - 1) / size;
  long superframes = (frames + cfg->fps - 1) / cfg->fps;

  a9_feed_t *feed = calloc(1, sizeof(*feed));
  run->feed = feed;
  feed->cfg = cfg;
  feed->left = left;
  feed->right = right;
  feed->song = song;
  feed->n = n;
  feed->delay = delay;
  feed->loop_from = loop_from;
  feed->window_len = (long)(cfg->fps + 1) * cfg->frame_samples;
  for (int c = 0; c < MAX_CHANNELS; c++) feed->src_of[c] = -1;
  if (cfg->blocks[0] == A9_STEREO) {
    feed->src_of[0] = 0;
    feed->src_of[1] = 1;
  } else if (cfg->nblocks > 1 && cfg->blocks[1] == A9_MONO) {
    feed->src_of[0] = 0;
    if (cfg->nch > 2) feed->src_of[2] = 1;
  } else {
    feed->src_of[0] = 2;
  }
  for (int c = 0; c < cfg->nch; c++)
    if (feed->src_of[c] >= 0) feed->win[c] = malloc(sizeof(float) * (size_t)feed->window_len);

  size_t olen = 0;
  uint8_t *output = build_file(&file, superframes, n, delay, loops, loop_start, loop_end, &olen);
  result = output;
  a9_file_t *outfile = calloc(1, sizeof(*outfile));
  run->outfile = outfile;
  file_parse(output, olen, outfile);

  a9_enc_t *enc = calloc(1, sizeof(*enc));
  run->enc = enc;
  enc_init(enc, cfg, &profile, (const float *const *)feed->win);
  a9_verify_t *ver = calloc(1, sizeof(*ver));
  run->ver = ver;
  dec_init(&ver->dec, outfile);
  ver->cfg = cfg;
  ver->windows = (float *const *)feed->win;
  ver->n = n;
  ver->delay = delay;
  for (int c = 0; c < cfg->nch; c++) {
    run->vout[c] = calloc((size_t)cfg->fps * (size_t)cfg->frame_samples, sizeof(float));
    ver->out[c] = run->vout[c];
    if (feed->win[c]) ver->used++;
  }
  ver->segs = (int)((n + SEGMENT - 1) / SEGMENT);
  run->sig = calloc((size_t)ver->segs, sizeof(double));
  run->err = calloc((size_t)ver->segs, sizeof(double));
  ver->sig = run->sig;
  ver->err = run->err;

  for (long s = 0; s < superframes; s++) {
    feed_fill(feed, s);
    enc_superframe(enc, output + outfile->data_off + (size_t)s * (size_t)cfg->sf_bytes);
    verify_check(ver, s);
    if (progress && s % 64 == 0 && progress(progress_ctx, (double)s / (double)superframes)) a9_fail("cancelled");
  }
  verify_finish(ver, &profile);
  if (progress) progress(progress_ctx, 1.0);

  run_free(run);
  free(run);
  t_jb = NULL;
  *out = output;
  *out_len = olen;
  return 0;
}

/* The first seconds of an .at9 file as a stereo mix, as 16-bit samples: what the console's own music sounds like on a
 * stereo output, so a new song can be leveled to it. The front pair counts fully; the centre and the surround pairs are
 * added at -3 dB (the usual downmix); the LFE channel is left out. */
int a9_decode_front(const uint8_t *stock, size_t stock_len, long max_samples, int16_t **left, int16_t **right, long *n, int *rate,
                    char *err, size_t errsz) {
  pthread_once(&g_once, tables_init);
  *left = *right = NULL;
  *n = 0;
  a9_dec_t *dec = calloc(1, sizeof(*dec));
  float *buf[MAX_CHANNELS] = {0};
  int16_t *outl = NULL, *outr = NULL;
  jmp_buf jb;
  t_jb = &jb;
  t_err[0] = 0;
  volatile int failed = 0;
  if (setjmp(jb)) failed = 1;
  if (!failed) {
    a9_file_t file;
    file_parse(stock, stock_len, &file);
    const a9_cfg_t *cfg = &file.cfg;
    if (!dec) a9_fail("out of memory");
    if (file.superframes <= 0) a9_fail("the file has no audio");
    if (cfg->nch < 2 || cfg->blocks[0] != A9_STEREO) a9_fail("the file does not start with a stereo pair");
    dec_init(dec, &file);
    long per_sf = (long)cfg->fps * cfg->frame_samples;
    long sfs = (max_samples + per_sf - 1) / per_sf;
    if (sfs > file.superframes) sfs = file.superframes;
    long total = sfs * per_sf;
    /* which channels carry sound, and how they are mixed: kind 0 = front pair, 1 = centre, 2 = surround pair, -1 = LFE */
    int kind[MAX_CHANNELS], side[MAX_CHANNELS], ch = 0; /* side: 0 left, 1 right, 2 both */
    for (int b = 0; b < cfg->nblocks; b++) {
      int type = cfg->blocks[b], nc = type == A9_STEREO ? 2 : 1;
      for (int c = 0; c < nc && ch < MAX_CHANNELS; c++) {
        kind[ch] = type == A9_LFE ? -1 : b == 0 ? 0 : type == A9_MONO ? 1 : 2;
        side[ch++] = type == A9_STEREO ? c : 2;
      }
    }
    float *dst[MAX_CHANNELS] = {0};
    for (int c = 0; c < ch; c++)
      if (kind[c] >= 0) {
        buf[c] = malloc(sizeof(float) * (size_t)per_sf);
        if (!buf[c]) a9_fail("out of memory");
        dst[c] = buf[c];
      }
    outl = malloc(sizeof(int16_t) * (size_t)total);
    outr = malloc(sizeof(int16_t) * (size_t)total);
    if (!outl || !outr) a9_fail("out of memory");
    for (long sf = 0; sf < sfs; sf++) {
      dec_begin_superframe(dec, sf);
      for (int f = 0; f < cfg->fps; f++) dec_frame(dec, f, dst, f * cfg->frame_samples);
      dec_end_superframe(dec);
      for (long k = 0; k < per_sf; k++) {
        double l = 0, r = 0;
        for (int c = 0; c < ch; c++) {
          if (kind[c] < 0) continue;
          double v = buf[c][k] * (kind[c] == 0 ? 1.0 : 0.7071);
          if (side[c] != 1) l += v;
          if (side[c] != 0) r += v;
        }
        l *= 32768.0;
        r *= 32768.0;
        outl[sf * per_sf + k] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        outr[sf * per_sf + k] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
      }
    }
    *left = outl;
    *right = outr;
    *n = total;
    if (rate) *rate = cfg->rate;
    outl = outr = NULL;
  }
  t_jb = NULL;
  free(dec);
  for (int c = 0; c < MAX_CHANNELS; c++) free(buf[c]);
  free(outl);
  free(outr);
  if (failed) {
    if (err && errsz) snprintf(err, errsz, "%s", t_err);
    return -1;
  }
  return 0;
}
