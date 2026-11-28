#include "avatar.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "icons.h"
#include "log.h"
#include "platform.h"
#include "texture.h"

static char g_root[300] = "/system_data/priv/cache/profile";
static char g_dir[300] = "/data/homescreenmanager/avatar";

static const int kSizes[4] = {64, 128, 260, 440};
#define AV_DXGI_BC3 77
#define ONLINE "online.json"

void avatar_init(const char *data_dir, const char *profile_root) {
  snprintf(g_dir, sizeof(g_dir), "%s/avatar", data_dir);
  if (profile_root && *profile_root) snprintf(g_root, sizeof(g_root), "%s", profile_root);
}

static void set_err(char *err, size_t n, const char *msg) {
  if (err && n) snprintf(err, n, "%s", msg);
}

int avatar_valid_id(const char *id) {
  if (!id || id[0] != '0' || id[1] != 'x' || strlen(id) != 10) return 0;
  for (int i = 2; i < 10; i++)
    if (!isxdigit((unsigned char)id[i])) return 0;
  return 1;
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p) {
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

/* The file is written beside its place under a .tmp name, checked, and renamed over it. */
static int write_replace(const char *path, const void *data, size_t len, mode_t mode) {
  char tmp[460];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
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

static void pfile(char *out, size_t n, const char *id, const char *name) {
  snprintf(out, n, "%s/%s/%s", g_root, id, name);
}
static void bdir(char *out, size_t n, const char *id) { snprintf(out, n, "%s/%s", g_dir, id); }
static void bfile(char *out, size_t n, const char *id, const char *name) {
  snprintf(out, n, "%s/%s/%s", g_dir, id, name);
}
/* Everything a console profile folder holds for the picture (as Prospero Manager writes it): avatar and picture in four sizes, plus a
 * 440 px PNG of each. 0-3 avatar*.dds, 4-7 picture*.dds, 8 avatar.png, 9 picture.png. */
#define AV_FILES 10
/* what a picture change writes: only the four avatar*.dds (the other six are still backed up and put back by Use the original) */
#define AV_WRITE 4
static void fname(char *out, size_t n, int i) {
  if (i < 4) snprintf(out, n, "avatar%d.dds", kSizes[i]);
  else if (i < 8) snprintf(out, n, "picture%d.dds", kSizes[i - 4]);
  else snprintf(out, n, "%s.png", i == 8 ? "avatar" : "picture");
}

static mode_t mode_of(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 ? (st.st_mode & 0777) : 0644;
}

int avatar_available(void) { return is_dir(g_root); }

static int list_ids(char (*ids)[16], int max) {
  int n = 0;
  DIR *d = opendir(g_root);
  struct dirent *e;
  while (d && (e = readdir(d)) && n < max)
    if (avatar_valid_id(e->d_name)) {
      char p[600];
      snprintf(p, sizeof(p), "%s/%.15s", g_root, e->d_name);
      if (is_dir(p)) snprintf(ids[n++], 16, "%.15s", e->d_name);
    }
  if (d) closedir(d);
  for (int i = 1; i < n; i++) /* a stable order */
    for (int j = i; j > 0 && strcmp(ids[j - 1], ids[j]) > 0; j--) {
      char t[16];
      memcpy(t, ids[j], 16);
      memcpy(ids[j], ids[j - 1], 16);
      memcpy(ids[j - 1], t, 16);
    }
  return n;
}

/* the state file: "created_online=0|1" and one "had=<name>" per file that was there */
static int state_read(const char *id, int *created_online, int had[AV_FILES]) {
  char p[460];
  bfile(p, sizeof(p), id, "state.txt");
  unsigned char *b = NULL;
  size_t bl = 0;
  *created_online = 0;
  memset(had, 0, AV_FILES * sizeof(int));
  if (read_all(p, &b, &bl, 4096)) return -1;
  char *s = malloc(bl + 1);
  if (!s) {
    free(b);
    return -1;
  }
  memcpy(s, b, bl);
  s[bl] = 0;
  free(b);
  for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
    if (!strcmp(line, "created_online=1")) *created_online = 1;
    for (int i = 0; i < AV_FILES; i++) {
      char want[48], nm[32];
      fname(nm, sizeof(nm), i);
      snprintf(want, sizeof(want), "had=%s", nm);
      if (!strcmp(line, want)) had[i] = 1;
    }
  }
  free(s);
  return 0;
}

int avatar_custom(const char *id) {
  char p[460];
  if (!avatar_valid_id(id)) return 0;
  bfile(p, sizeof(p), id, "state.txt");
  return is_file(p);
}

int avatar_custom_ids(char (*ids)[16], int max) {
  char all[64][16];
  int n = list_ids(all, 64), k = 0;
  for (int i = 0; i < n && k < max; i++)
    if (avatar_custom(all[i])) snprintf(ids[k++], 16, "%s", all[i]);
  return k;
}

/* --- reading the picture the user has now (a small DXT5 decoder) --- */

static void rgb565(uint16_t c, int o[3]) {
  o[0] = ((c >> 11) & 31) * 255 / 31;
  o[1] = ((c >> 5) & 63) * 255 / 63;
  o[2] = (c & 31) * 255 / 31;
}

static int decode_dxt5(const unsigned char *d, size_t n, int size, uint8_t **rgba) {
  size_t blocks = (size_t)((size + 3) / 4) * ((size + 3) / 4) * 16;
  if (n != 128 + blocks || memcmp(d, "DDS ", 4) || memcmp(d + 84, "DXT5", 4)) return -1;
  uint8_t *px = calloc((size_t)size * size, 4);
  if (!px) return -1;
  const unsigned char *b = d + 128;
  int bw = (size + 3) / 4;
  for (int by = 0; by < bw; by++)
    for (int bx = 0; bx < bw; bx++, b += 16) {
      int a[8];
      a[0] = b[0];
      a[1] = b[1];
      if (a[0] > a[1])
        for (int i = 2; i < 8; i++) a[i] = ((8 - i) * a[0] + (i - 1) * a[1]) / 7;
      else {
        for (int i = 2; i < 6; i++) a[i] = ((6 - i) * a[0] + (i - 1) * a[1]) / 5;
        a[6] = 0;
        a[7] = 255;
      }
      uint64_t ab = 0;
      for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
      uint16_t c0 = (uint16_t)(b[8] | b[9] << 8), c1 = (uint16_t)(b[10] | b[11] << 8);
      int col[4][3];
      rgb565(c0, col[0]);
      rgb565(c1, col[1]);
      for (int k = 0; k < 3; k++) {
        col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
        col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
      }
      uint32_t cb = (uint32_t)(b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24);
      for (int i = 0; i < 16; i++) {
        int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= size || y >= size) continue;
        uint8_t *o = px + ((size_t)y * size + x) * 4;
        int ci = (cb >> (2 * i)) & 3;
        o[0] = (uint8_t)col[ci][0];
        o[1] = (uint8_t)col[ci][1];
        o[2] = (uint8_t)col[ci][2];
        o[3] = (uint8_t)a[(ab >> (3 * i)) & 7];
      }
    }
  *rgba = px;
  return 0;
}

int avatar_current_png(const char *id, unsigned char **png, size_t *len) {
  char p[460];
  unsigned char *b = NULL;
  size_t bl = 0;
  if (!avatar_valid_id(id)) return -1;
  pfile(p, sizeof(p), id, "avatar128.dds");
  if (read_all(p, &b, &bl, 1u << 20) == 0) {
    uint8_t *px = NULL;
    int rc = decode_dxt5(b, bl, 128, &px);
    free(b);
    if (rc == 0) {
      rc = img_encode_png(px, 128, 128, 4, png, len);
      free(px);
      return rc;
    }
  }
  pfile(p, sizeof(p), id, "avatar.png");
  if (read_all(p, &b, &bl, 2u << 20) == 0 && bl > 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
    *png = b;
    *len = bl;
    return 0;
  }
  free(b);
  return -1;
}

/* --- several pictures per user, one of them picked at random now and then ---
 * <data>/avatar/pool/<ID>/<n>.png are the user's pictures (square, 512 px), "auto" says that a different one may be
 * picked by itself, "cur" is the number that was written last. Nothing here touches the console's folder except
 * through avatar_apply, so the first original is kept the same way. */
static void pdir(char *out, size_t n, const char *id) { snprintf(out, n, "%s/pool/%.15s", g_dir, id); }
static void pfileb(char *out, size_t n, const char *id, const char *name) { snprintf(out, n, "%s/pool/%.15s/%.40s", g_dir, id, name); }

static int pool_nums(const char *id, int *nums, int max) {
  char d[400];
  int n = 0;
  pdir(d, sizeof(d), id);
  DIR *dp = opendir(d);
  struct dirent *e;
  while (dp && (e = readdir(dp)) && n < max) {
    int v = 0;
    char *end = NULL;
    v = (int)strtol(e->d_name, &end, 10);
    if (v > 0 && end && !strcmp(end, ".png")) nums[n++] = v;
  }
  if (dp) closedir(dp);
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && nums[j - 1] > nums[j]; j--) {
      int t = nums[j];
      nums[j] = nums[j - 1];
      nums[j - 1] = t;
    }
  return n;
}

static int pool_auto(const char *id) {
  char p[460];
  pfileb(p, sizeof(p), id, "auto");
  return is_file(p);
}

static int pool_cur(const char *id) {
  char p[460];
  unsigned char *b = NULL;
  size_t bl = 0;
  pfileb(p, sizeof(p), id, "cur");
  if (read_all(p, &b, &bl, 32)) return 0;
  char t[40] = "";
  memcpy(t, b, bl < 31 ? bl : 31);
  free(b);
  return atoi(t);
}

static void pool_set_cur(const char *id, int num) {
  char p[460], t[32];
  pfileb(p, sizeof(p), id, "cur");
  int n = snprintf(t, sizeof(t), "%d\n", num);
  write_replace(p, t, (size_t)n, 0644);
}

int avatar_pool_add(const char *id, const unsigned char *img, size_t len, char *err, size_t errsz) {
  char dir[400];
  if (!avatar_valid_id(id)) {
    set_err(err, errsz, "That isn't a profile folder.");
    return -1;
  }
  snprintf(dir, sizeof(dir), "%s/%s", g_root, id);
  if (!is_dir(dir)) {
    set_err(err, errsz, "This user has no profile folder on the console.");
    return -1;
  }
  int nums[AV_POOL_MAX + 1];
  int n = pool_nums(id, nums, AV_POOL_MAX + 1);
  if (n >= AV_POOL_MAX) {
    set_err(err, errsz, "The list is full. Take a picture out of it first.");
    return -1;
  }
  uint8_t *px = icon_load_cover(img, len, 512, 512);
  unsigned char *png = NULL;
  size_t pl = 0;
  if (!px || img_encode_png(px, 512, 512, 4, &png, &pl)) {
    free(px);
    set_err(err, errsz, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).");
    return -1;
  }
  free(px);
  pdir(dir, sizeof(dir), id);
  mkdir_p(dir);
  int next = n ? nums[n - 1] + 1 : 1;
  char p[460], name[32];
  snprintf(name, sizeof(name), "%d.png", next);
  pfileb(p, sizeof(p), id, name);
  int rc = write_replace(p, png, pl, 0644);
  free(png);
  if (rc) {
    set_err(err, errsz, "Couldn't save the picture.");
    return -1;
  }
  hh_log("profile picture %s: picture %d added to the list (%d in all)", id, next, n + 1);
  return 0;
}

int avatar_pool_remove(const char *id, int num) {
  char p[460], name[32];
  if (!avatar_valid_id(id) || num <= 0) return -1;
  snprintf(name, sizeof(name), "%d.png", num);
  pfileb(p, sizeof(p), id, name);
  if (unlink(p)) return -1;
  if (pool_cur(id) == num) {
    pfileb(p, sizeof(p), id, "cur");
    unlink(p);
  }
  return 0;
}

int avatar_pool_get(const char *id, int num, unsigned char **png, size_t *len) {
  char p[460], name[32];
  if (!avatar_valid_id(id) || num <= 0) return -1;
  snprintf(name, sizeof(name), "%d.png", num);
  pfileb(p, sizeof(p), id, name);
  return read_all(p, png, len, 4u << 20);
}

int avatar_pool_auto_set(const char *id, int on) {
  char p[460], d[400];
  if (!avatar_valid_id(id)) return -1;
  pfileb(p, sizeof(p), id, "auto");
  if (!on) {
    unlink(p);
    return 0;
  }
  pdir(d, sizeof(d), id);
  mkdir_p(d);
  return write_replace(p, "1\n", 2, 0644);
}

static void pool_used_add(const char *id, int num);

int avatar_pool_use(const char *id, int num, char *err, size_t errsz) {
  unsigned char *png = NULL;
  size_t pl = 0;
  if (avatar_pool_get(id, num, &png, &pl)) {
    set_err(err, errsz, "That picture isn't in the list any more.");
    return -1;
  }
  int rc = avatar_apply(id, png, pl, err, errsz);
  free(png);
  if (rc == 0) pool_set_cur(id, num);
  if (rc == 0) pool_used_add(id, num);
  return rc;
}

static unsigned rnd_next(void) {
  static unsigned s;
  if (!s) s = (unsigned)time(NULL) * 2654435761u ^ (unsigned)getpid();
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

/* The pictures already shown in the current round of the list: every picture is shown once before any comes
 * again. Kept in the file "used" of the user's list folder (numbers, one per line). */
static int pool_used_read(const char *id, int *out, int max) {
  char p[460];
  unsigned char *b = NULL;
  size_t bl = 0;
  int n = 0;
  pfileb(p, sizeof(p), id, "used");
  if (read_all(p, &b, &bl, 512)) return 0;
  char *t = (char *)malloc(bl + 1);
  if (!t) { free(b); return 0; }
  memcpy(t, b, bl);
  t[bl] = 0;
  free(b);
  for (char *q = t; *q && n < max;) {
    while (*q == '\n' || *q == ' ') q++;
    if (!*q) break;
    out[n++] = atoi(q);
    while (*q && *q != '\n') q++;
  }
  free(t);
  return n;
}

static void pool_used_write(const char *id, const int *v, int n) {
  char p[460], t[400];
  int len = 0;
  pfileb(p, sizeof(p), id, "used");
  for (int i = 0; i < n && len < (int)sizeof(t) - 12; i++) len += snprintf(t + len, sizeof(t) - (size_t)len, "%d\n", v[i]);
  write_replace(p, t, (size_t)len, 0644);
}

static void pool_used_add(const char *id, int num) {
  int u[AV_POOL_MAX + 1], n = pool_used_read(id, u, AV_POOL_MAX);
  for (int i = 0; i < n; i++)
    if (u[i] == num) return;
  if (n < AV_POOL_MAX) u[n++] = num;
  pool_used_write(id, u, n);
}

/* A random picture of the user's list that has not been shown in this round (when the list has two or more).
 * When all have been shown the round starts again, and the first pick is never the one in use. */
int avatar_pool_random(const char *id, char *err, size_t errsz) {
  int nums[AV_POOL_MAX];
  int pn = pool_nums(id, nums, AV_POOL_MAX);
  if (pn < 2) {
    set_err(err, errsz, "The list needs two pictures.");
    return -1;
  }
  int cur = pool_cur(id), used[AV_POOL_MAX + 1], un = pool_used_read(id, used, AV_POOL_MAX);
  int cand[AV_POOL_MAX], c = 0;
  for (int pass = 0; pass < 2 && !c; pass++) {
    for (int k = 0; k < pn; k++) {
      int seen = nums[k] == cur;
      for (int j = 0; j < un && !seen; j++)
        if (used[j] == nums[k]) seen = 1;
      if (!seen) cand[c++] = nums[k];
    }
    if (!c) {
      /* everything has been shown: a new round, starting with all but the picture in use */
      un = 0;
      pool_used_write(id, used, 0);
    }
  }
  if (!c) cand[c++] = nums[0];
  int pick = cand[rnd_next() % (unsigned)c];
  int rc = avatar_pool_use(id, pick, err, errsz);
  if (rc == 0) hh_log("profile picture %s: picture no. %d picked by itself (%d in the list, %d not shown yet in this round)", id, pick, pn, c - 1);
  return rc;
}

int avatar_rotate_all(void) {
  char ids[64][16];
  int n = list_ids(ids, 64), changed = 0;
  for (int i = 0; i < n; i++) {
    if (!pool_auto(ids[i])) continue;
    char err[300] = "";
    if (avatar_pool_random(ids[i], err, sizeof(err)) == 0) changed++;
    else if (strcmp(err, "The list needs two pictures.")) hh_log("profile picture %s: picking another picture failed: %s", ids[i], err);
  }
  return changed;
}

static void rm_pool_dir(const char *id) {
  char d[400], p[460];
  pdir(d, sizeof(d), id);
  DIR *dp = opendir(d);
  struct dirent *e;
  while (dp && (e = readdir(dp)))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      snprintf(p, sizeof(p), "%s/%.40s", d, e->d_name);
      unlink(p);
    }
  if (dp) closedir(dp);
  rmdir(d);
}

void avatar_pool_clear_all(void) {
  char d[400];
  snprintf(d, sizeof(d), "%s/pool", g_dir);
  DIR *dp = opendir(d);
  struct dirent *e;
  while (dp && (e = readdir(dp)))
    if (avatar_valid_id(e->d_name)) rm_pool_dir(e->d_name);
  if (dp) closedir(dp);
  rmdir(d);
}

void avatar_json(strbuf_t *sb) {
  char ids[64][16];
  int n = list_ids(ids, 64);
  sb_printf(sb, "{\"ok\":true,\"available\":%s,\"profiles\":[", avatar_available() ? "true" : "false");
  for (int i = 0; i < n; i++) {
    char p[460];
    pfile(p, sizeof(p), ids[i], "avatar128.dds");
    int has = is_file(p);
    pfile(p, sizeof(p), ids[i], "avatar.png");
    has = has || is_file(p);
    char uname[64] = "";
    plat_user_name((unsigned)strtoul(ids[i] + 2, NULL, 16), uname, sizeof(uname));
    sb_printf(sb, "%s{\"id\":\"%s\",\"custom\":%s,\"has_picture\":%s,\"name\":", i ? "," : "", ids[i],
              avatar_custom(ids[i]) ? "true" : "false", has ? "true" : "false");
    sb_json_str(sb, uname);
    int nums[AV_POOL_MAX];
    int pn = pool_nums(ids[i], nums, AV_POOL_MAX);
    sb_printf(sb, ",\"auto\":%s,\"pool\":[", pool_auto(ids[i]) ? "true" : "false");
    for (int k = 0; k < pn; k++) sb_printf(sb, "%s%d", k ? "," : "", nums[k]);
    sb_puts(sb, "]}");
  }
  sb_puts(sb, "]}");
}

int avatar_reset(const char *id, char *err, size_t errsz) {
  if (!avatar_valid_id(id)) {
    set_err(err, errsz, "That isn't a profile folder.");
    return -1;
  }
  {
    char ap[460];
    pfileb(ap, sizeof(ap), id, "auto"); /* the original stays: no more changing by itself */
    unlink(ap);
  }
  if (!avatar_custom(id)) return 0; /* never changed */
  int created = 0, had[AV_FILES];
  if (state_read(id, &created, had)) {
    set_err(err, errsz, "The saved original of the picture can't be read.");
    return -1;
  }
  char dir[400];
  snprintf(dir, sizeof(dir), "%s/%s", g_root, id);
  if (!is_dir(dir)) {
    set_err(err, errsz, "This user has no profile folder on the console any more.");
    return -1;
  }
  int failed = 0;
  for (int i = 0; i < AV_FILES; i++) {
    char name[40], pp[460], bp[460];
    fname(name, sizeof(name), i);
    pfile(pp, sizeof(pp), id, name);
    bfile(bp, sizeof(bp), id, name);
    if (had[i]) {
      unsigned char *b = NULL;
      size_t bl = 0;
      if (read_all(bp, &b, &bl, 4u << 20) || write_replace(pp, b, bl, mode_of(pp))) failed++;
      free(b);
    } else if (is_file(pp) && unlink(pp)) {
      failed++;
    }
  }
  char op[460];
  pfile(op, sizeof(op), id, ONLINE);
  if (created && is_file(op) && unlink(op)) failed++;
  if (failed) {
    set_err(err, errsz, "Couldn't put everything back. The saved original stays; try again.");
    return -1;
  }
  for (int i = 0; i < AV_FILES; i++) {
    char name[40], bp[460];
    fname(name, sizeof(name), i);
    bfile(bp, sizeof(bp), id, name);
    unlink(bp);
  }
  char sp[460], bd[400];
  bfile(sp, sizeof(sp), id, "state.txt");
  unlink(sp);
  bdir(bd, sizeof(bd), id);
  rmdir(bd);
  char ap[460], cp[460];
  pfileb(ap, sizeof(ap), id, "auto");
  unlink(ap);
  pfileb(cp, sizeof(cp), id, "cur");
  unlink(cp);
  hh_log("profile picture %s: the console's own picture is back", id);
  return 0;
}

static int online_is_ours(const char *id) {
  char sp[460];
  unsigned char *b = NULL;
  size_t bl = 0;
  bfile(sp, sizeof(sp), id, "state.txt");
  if (read_all(sp, &b, &bl, 4096)) return 0;
  char *t = malloc(bl + 1);
  int r = 0;
  if (t) {
    memcpy(t, b, bl);
    t[bl] = 0;
    r = strstr(t, "created_online=1") != NULL;
    free(t);
  }
  free(b);
  return r;
}

int avatar_apply(const char *id, const unsigned char *img, size_t len, char *err, size_t errsz) {
  if (!avatar_valid_id(id)) {
    set_err(err, errsz, "That isn't a profile folder.");
    return -1;
  }
  char dir[400];
  snprintf(dir, sizeof(dir), "%s/%s", g_root, id);
  if (!is_dir(dir)) {
    set_err(err, errsz, "This user has no profile folder on the console.");
    return -1;
  }
  /* the pictures are made first: a picture that can't be read changes nothing. Slots 0-3 are the four DDS sizes (avatar*.dds). */
  unsigned char *dds[AV_FILES] = {0};
  size_t dl[AV_FILES] = {0};
  int bad = 0;
  for (int i = 0; i < 4 && !bad; i++) {
    tex_fmt_t f = {TEX_DDS, kSizes[i], kSizes[i], 1, AV_DXGI_BC3, 1};
    if (tex_encode(img, len, &f, &dds[i], &dl[i])) bad = 1;
  }
  if (bad) {
    for (int i = 0; i < AV_FILES; i++) free(dds[i]);
    set_err(err, errsz, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).");
    return -1;
  }
  char op[460];
  pfile(op, sizeof(op), id, ONLINE);
  int had_online = is_file(op), fresh = !avatar_custom(id);
  if (fresh) { /* the first change keeps what is there now */
    char bd[400], sp[460], state[400];
    bdir(bd, sizeof(bd), id);
    mkdir_p(bd);
    int len_s = snprintf(state, sizeof(state), "created_online=%d\n", had_online ? 0 : 1);
    int ok = 1;
    for (int i = 0; i < AV_FILES && ok; i++) {
      char name[40], pp[460], bp[460];
      fname(name, sizeof(name), i);
      pfile(pp, sizeof(pp), id, name);
      if (!is_file(pp)) continue;
      bfile(bp, sizeof(bp), id, name);
      unsigned char *b = NULL;
      size_t bl = 0;
      if (read_all(pp, &b, &bl, 4u << 20) || write_replace(bp, b, bl, 0644)) ok = 0;
      free(b);
      if (ok) len_s += snprintf(state + len_s, sizeof(state) - (size_t)len_s, "had=%s\n", name);
    }
    bfile(sp, sizeof(sp), id, "state.txt");
    if (!ok || write_replace(sp, state, (size_t)len_s, 0644)) {
      for (int i = 0; i < AV_FILES; i++) free(dds[i]);
      char name[40], bp[460];
      for (int i = 0; i < AV_FILES; i++) {
        fname(name, sizeof(name), i);
        bfile(bp, sizeof(bp), id, name);
        unlink(bp);
      }
      rmdir(bd);
      set_err(err, errsz, "Couldn't keep a copy of the current picture, so nothing was changed.");
      return -1;
    }
  }
  int failed = 0;
  for (int i = 0; i < AV_WRITE && !failed; i++) {
    char name[40], pp[460];
    fname(name, sizeof(name), i);
    pfile(pp, sizeof(pp), id, name);
    if (write_replace(pp, dds[i], dl[i], mode_of(pp))) failed = 1;
  }
  /* online.json is written the way Prospero Manager writes it (user name + verified flag) when it is ours: created by us, never one
   * the console had */
  if (!failed && (!had_online || online_is_ours(id))) {
    char uname[64] = "", esc[160], js[400];
    plat_user_name((unsigned)strtoul(id + 2, NULL, 16), uname, sizeof(uname));
    size_t k = 0;
    for (size_t q = 0; uname[q] && k + 7 < sizeof(esc); q++) {
      unsigned char c = (unsigned char)uname[q];
      if (c == '"' || c == '\\') esc[k++] = '\\';
      if (c >= 0x20) esc[k++] = (char)c;
    }
    esc[k] = 0;
    int jl = snprintf(js, sizeof(js), "{\"firstName\":\"%s\",\"lastName\":\"\",\"isOfficiallyVerified\":\"true\"}", esc);
    if (write_replace(op, js, (size_t)jl, 0644)) failed = 1;
  }
  for (int i = 0; i < AV_FILES; i++) free(dds[i]);
  if (failed) {
    char e2[200] = "";
    int back = avatar_reset(id, e2, sizeof(e2)) == 0;
    set_err(err, errsz, back ? "Couldn't write the picture on the console. The old picture is back."
                             : "Couldn't write the picture on the console, and putting the old one back failed too. Use the original picture.");
    return -1;
  }
  hh_log("profile picture %s: new picture written (%d files), original kept", id, AV_WRITE);
  return 0;
}
