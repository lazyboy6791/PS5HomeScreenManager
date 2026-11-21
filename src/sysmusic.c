#include "sysmusic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "atrac9.h"
#include "audioin.h"
#include "log.h"
#include "sha256.h"

#define VOLUME_MIN (-12.0)
#define VOLUME_MAX 12.0
#define REF_SECONDS 90        /* how much of the console's own music is measured */
#define CONSOLE_GAIN_DB 5.0   /* the console's music is heard louder than its level measures (found on a console: +6 dB matched) */
#define REF_FALLBACK 0.08     /* RMS to aim for when the console's file can't be measured */
#define FADE_IN 0.5
#define FADE_OUT 1.5
#define STALL_SECONDS 60      /* no progress for this long: the console probably slept */

static char g_dir[300] = "/data/homescreenmanager/music";
static char g_assets[300] = "/system_ex/vsh_asset";

void sysmusic_init(const char *data_dir, const char *asset_dir) {
  snprintf(g_dir, sizeof(g_dir), "%s/music", data_dir);
  if (asset_dir && *asset_dir) snprintf(g_assets, sizeof(g_assets), "%s", asset_dir);
  char tmp[400];
  snprintf(tmp, sizeof(tmp), "%s/bgm_home.at9.hsmnew", g_assets); /* left by a job that was cut short */
  unlink(tmp);
}

static void target_path(char *o, size_t n) { snprintf(o, n, "%s/bgm_home.at9", g_assets); }
static void orig_path(char *o, size_t n) { snprintf(o, n, "%s/original.at9", g_dir); }
static void state_path(char *o, size_t n) { snprintf(o, n, "%s/state.txt", g_dir); }

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void mkdir_p(const char *dir) {
  char t[400];
  snprintf(t, sizeof(t), "%s", dir);
  for (char *p = t + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(t, 0777);
      *p = '/';
    }
  mkdir(t, 0777);
}

static int read_all(const char *path, unsigned char **buf, size_t *len, size_t max) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || (size_t)sz > max) {
    fclose(f);
    return -1;
  }
  *buf = malloc((size_t)sz);
  if (!*buf || fread(*buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(*buf);
    *buf = NULL;
    fclose(f);
    return -1;
  }
  fclose(f);
  *len = (size_t)sz;
  return 0;
}

static int write_replace(const char *path, const void *data, size_t len, mode_t mode) {
  char tmp[420];
  snprintf(tmp, sizeof(tmp), "%s.hsmnew", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fflush(f) == 0) && ok;
  ok = (fclose(f) == 0) && ok;
  struct stat st;
  ok = ok && stat(tmp, &st) == 0 && (size_t)st.st_size == len;
  if (!ok || rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  chmod(path, mode);
  return 0;
}

static void set_err(char *err, size_t n, const char *msg) {
  if (err && n) snprintf(err, n, "%s", msg);
}

/* ---- the state file: key=value lines ---- */
typedef struct {
  char sha[65], name[200];
  double seconds, level_db, volume_db;
  int cut, ref_console;
} state_t;

static int state_load(state_t *s) {
  memset(s, 0, sizeof(*s));
  char p[400];
  state_path(p, sizeof(p));
  FILE *f = fopen(p, "r");
  if (!f) return 0;
  char line[400];
  int got = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = 0;
    if (!strcmp(line, "sha")) snprintf(s->sha, sizeof(s->sha), "%s", eq), got = 1;
    else if (!strcmp(line, "name")) snprintf(s->name, sizeof(s->name), "%s", eq);
    else if (!strcmp(line, "seconds")) s->seconds = atof(eq);
    else if (!strcmp(line, "level_db")) s->level_db = atof(eq);
    else if (!strcmp(line, "volume_db")) s->volume_db = atof(eq);
    else if (!strcmp(line, "cut")) s->cut = atoi(eq);
    else if (!strcmp(line, "ref")) s->ref_console = atoi(eq);
  }
  fclose(f);
  return got && strlen(s->sha) == 64;
}

static int state_save(const state_t *s) {
  char p[400], buf[600];
  state_path(p, sizeof(p));
  char nm[200];
  snprintf(nm, sizeof(nm), "%s", s->name);
  for (char *c = nm; *c; c++)
    if (*c == '\n' || *c == '\r') *c = ' ';
  int n = snprintf(buf, sizeof(buf), "sha=%s\nname=%s\nseconds=%.2f\nlevel_db=%.1f\nvolume_db=%.1f\ncut=%d\nref=%d\n", s->sha, nm, s->seconds, s->level_db,
                   s->volume_db, s->cut, s->ref_console);
  return write_replace(p, buf, (size_t)n, 0666);
}

static int file_sha(const char *path, char out[65]) {
  unsigned char *b = NULL;
  size_t n = 0;
  if (read_all(path, &b, &n, 64u << 20)) return -1;
  sha256_hex(b, n, out);
  free(b);
  return 0;
}

int sysmusic_available(void) {
  char p[400];
  target_path(p, sizeof(p));
  return exists(p);
}

int sysmusic_custom(void) {
  state_t s;
  char o[400];
  orig_path(o, sizeof(o));
  return state_load(&s) && exists(o);
}

void sysmusic_json(strbuf_t *sb) {
  state_t s;
  int custom = sysmusic_custom();
  state_load(&s);
  int changed = 0;
  if (custom) {
    char p[400], h[65];
    target_path(p, sizeof(p));
    changed = file_sha(p, h) != 0 || strcmp(h, s.sha) != 0;
  }
  sb_printf(sb, "{\"available\":%s,\"custom\":%s,\"changed\":%s,\"name\":", sysmusic_available() ? "true" : "false", custom ? "true" : "false",
            changed ? "true" : "false");
  sb_json_str(sb, custom ? s.name : "");
  sb_printf(sb, ",\"seconds\":%.1f,\"cut\":%s,\"cut_at\":%d,\"ref_console\":%s,\"level_db\":%.1f,\"volume_db\":%.1f,\"vol_min\":%.0f,\"vol_max\":%.0f}", custom ? s.seconds : 0.0,
            custom && s.cut ? "true" : "false", MUSIC_MAX_SECONDS, custom && s.ref_console ? "true" : "false", custom ? s.level_db : 0.0, custom ? s.volume_db : 0.0, VOLUME_MIN, VOLUME_MAX);
}

/* ---- the job ---- */
typedef struct {
  sysmusic_progress_fn fn;
  void *ctx;
  double from, to;
  time_t last;
  int stalled;
} prog_t;

static int prog_cb(void *p, double f) {
  prog_t *pr = p;
  time_t now = time(NULL);
  if (now - pr->last > STALL_SECONDS) {
    pr->stalled = 1; /* the console slept (or the clock jumped): don't carry on with half-known state */
    return 1;
  }
  pr->last = now;
  return pr->fn ? pr->fn(pr->ctx, pr->from + (pr->to - pr->from) * f, "Converting the song…") : 0;
}

static int is_at9_riff(const unsigned char *b, size_t n) { return n > 64 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4); }

int sysmusic_apply(const unsigned char *data, size_t len, const char *name, double volume_db, sysmusic_progress_fn progress, void *ctx,
                   char *err, size_t errsz) {
  char target[400], orig[400];
  target_path(target, sizeof(target));
  orig_path(orig, sizeof(orig));
  if (!(volume_db >= VOLUME_MIN && volume_db <= VOLUME_MAX)) volume_db = 0;
  if (!exists(target)) {
    set_err(err, errsz, "This console has no home screen music file.");
    return -1;
  }
  if (progress && progress(ctx, 0.01, "Reading the console's music…")) {
    set_err(err, errsz, "cancelled");
    return -1;
  }
  /* the console's own file is the template and the loudness reference */
  unsigned char *stock = NULL;
  size_t slen = 0;
  /* if the file on the console is ours, the saved original is the template; if something else replaced it since (a firmware
   * update), the file there now is the new "original" */
  int ours = 0;
  if (sysmusic_custom()) {
    state_t st0;
    char h0[65];
    ours = state_load(&st0) && file_sha(target, h0) == 0 && !strcmp(h0, st0.sha);
  }
  const char *sp = ours ? orig : target;
  if (read_all(sp, &stock, &slen, 64u << 20) || !is_at9_riff(stock, slen)) {
    free(stock);
    set_err(err, errsz, "The console's music file isn't what was expected, so it was left alone.");
    return -1;
  }
  int rc = -1, rate = 0, channels = 0;
  audio_t au = {0};
  int16_t *rl = NULL, *rr = NULL;
  long rn = 0;
  uint8_t *out = NULL;
  size_t outlen = 0;
  char e[300] = "";
  if (a9_stock_info(stock, slen, &rate, &channels, e, sizeof(e))) {
    set_err(err, errsz, e);
    goto done;
  }
  if (audio_decode(data, len, MUSIC_MAX_SECONDS, &au, e, sizeof(e))) {
    set_err(err, errsz, e);
    goto done;
  }
  if (progress && progress(ctx, 0.12, "Matching the sample rate…")) {
    set_err(err, errsz, "cancelled");
    goto done;
  }
  if (au.rate != rate && audio_resample(&au, rate, e, sizeof(e))) {
    set_err(err, errsz, e);
    goto done;
  }
  /* level: the console's own music is the reference */
  double ref = 0;
  int rrate = 0;
  if (a9_decode_front(stock, slen, (long)REF_SECONDS * rate, &rl, &rr, &rn, &rrate, e, sizeof(e)) == 0) ref = audio_level(rl, rr, rn, rrate);
  if (!(ref > 0.003 && ref < 0.7)) ref = REF_FALLBACK;
  else ref *= pow(10.0, CONSOLE_GAIN_DB / 20.0);
  double lvl = audio_level(au.l, au.r, au.n, au.rate);
  if (!(lvl > 0)) {
    set_err(err, errsz, "That song is silent.");
    goto done;
  }
  double gain = ref / lvl * pow(10.0, volume_db / 20.0);
  if (gain > 30) gain = 30;
  if (gain < 0.02) gain = 0.02;
  double level_db = 20 * log10(lvl * gain);
  audio_apply(&au, gain, FADE_IN, FADE_OUT);
  if (progress && progress(ctx, 0.2, "Converting the song…")) {
    set_err(err, errsz, "cancelled");
    goto done;
  }
  prog_t pr = {progress, ctx, 0.2, 0.9, time(NULL), 0};
  if (a9_encode(stock, slen, au.l, au.r, (size_t)au.n, 0, prog_cb, &pr, &out, &outlen, e, sizeof(e))) {
    set_err(err, errsz, pr.stalled ? "The console seems to have gone to sleep while converting. Nothing was changed; please try again." : e);
    goto done;
  }
  int orate = 0, och = 0;
  if (a9_stock_info(out, outlen, &orate, &och, e, sizeof(e)) || orate != rate || och != channels) {
    set_err(err, errsz, "internal error: the converted music doesn't look right, so nothing was changed.");
    goto done;
  }
  if (progress && progress(ctx, 0.92, "Saving…")) {
    set_err(err, errsz, "cancelled");
    goto done;
  }
  struct statvfs vs;
  if (statvfs(g_assets, &vs) == 0 && (unsigned long long)vs.f_bavail * vs.f_frsize < (unsigned long long)outlen + (32ull << 20)) {
    set_err(err, errsz, "The console's system drive is almost full.");
    goto done;
  }
  mkdir_p(g_dir);
  if (!ours) { /* keep the console's own file the first time (now known to be a valid one) */
    if (write_replace(orig, stock, slen, 0666) != 0) {
      set_err(err, errsz, "Couldn't keep a copy of the original music.");
      goto done;
    }
  }
  struct stat st;
  mode_t mode = stat(target, &st) == 0 ? (st.st_mode & 0777) : 0666;
  if (write_replace(target, out, outlen, mode) != 0) {
    set_err(err, errsz, "Couldn't write the music on the console. The old music is still in place.");
    goto done;
  }
  state_t s;
  memset(&s, 0, sizeof(s));
  sha256_hex(out, outlen, s.sha);
  snprintf(s.name, sizeof(s.name), "%s", name && *name ? name : "song");
  s.seconds = au.src_seconds;
  s.cut = au.was_cut;
  s.level_db = level_db;
  s.volume_db = volume_db;
  s.ref_console = ref != REF_FALLBACK;
  if (state_save(&s) != 0) hh_log("home music: couldn't save its state file");
  hh_log("home music: written (%zu bytes, %.0f s%s, level %.1f dB, volume %+.1f dB, reference %s)", outlen, au.src_seconds, au.was_cut ? ", cut" : "",
         level_db, volume_db, ref == REF_FALLBACK ? "fallback" : "console music");
  if (progress) progress(ctx, 1.0, "Done");
  rc = 0;
done:
  free(stock);
  free(out);
  free(rl);
  free(rr);
  audio_free(&au);
  return rc;
}

int sysmusic_reset(char *note, size_t notesz, char *err, size_t errsz) {
  if (note && notesz) note[0] = 0;
  char target[400], orig[400], sp[400];
  target_path(target, sizeof(target));
  orig_path(orig, sizeof(orig));
  state_path(sp, sizeof(sp));
  state_t s;
  if (!state_load(&s)) { /* nothing of ours */
    unlink(orig);
    return 0;
  }
  char h[65];
  int same = file_sha(target, h) == 0 && !strcmp(h, s.sha);
  if (same) {
    unsigned char *o = NULL;
    size_t ol = 0;
    struct stat st;
    mode_t mode = stat(target, &st) == 0 ? (st.st_mode & 0777) : 0666;
    if (read_all(orig, &o, &ol, 64u << 20) || !is_at9_riff(o, ol) || write_replace(target, o, ol, mode) != 0) {
      free(o);
      set_err(err, errsz, "Couldn't put the original music back.");
      return -1;
    }
    free(o);
    hh_log("home music: original put back");
  } else {
    if (note) snprintf(note, notesz, "The music file on the console was changed after Home Screen Manager wrote it (a firmware update or another tool), so it was left as it is.");
    hh_log("home music: file changed since, left alone");
  }
  unlink(orig);
  unlink(sp);
  return 0;
}
