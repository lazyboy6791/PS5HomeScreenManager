#include "sysicon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "icons.h"
#include "log.h"

/* tile id -> the file below <root>/ (Prospero Themes' catalog lists the same ten for 13.60 and for older firmware) */
static const struct {
  const char *id, *rel;
} kTiles[] = {
    {"NPXS40047", "NPXS40047/appdb/default/icon0.png"},       {"NPXS40037", "NPXS40037/appdb/default/icon0.png"},
    {"NPXS40071", "NPXS40071/appdb/default/icon0.png"},       {"NPXS40139", "NPXS40071/appdb/NPXS40139/icon0.png"},
    {"NPXS40053", "NPXS40016/appdb/NPXS40053/icon0.png"},     {"NPXS40054", "NPXS40016/appdb/NPXS40054/icon0.png"},
    {"NPXS40056", "NPXS40016/appdb/NPXS40056/icon0.png"},     {"NPXS40075", "NPXS40075/appdb/default/icon0.png"},
    {"NPXS40063", "NPXS40063/appdb/default/icon0.png"},
};
#define NTILES ((int)(sizeof(kTiles) / sizeof(kTiles[0])))

static char g_dir[300] = "/data/homescreenmanager/sysicon";
static char g_live[300] = "/mnt/rnps/apps";
static char g_ship[300] = "/system_ex/rnps/apps";

void sysicon_init(const char *data_dir, const char *live_root, const char *shipped_root) {
  snprintf(g_dir, sizeof(g_dir), "%s/sysicon", data_dir);
  if (live_root && *live_root) snprintf(g_live, sizeof(g_live), "%s", live_root);
  if (shipped_root && *shipped_root) snprintf(g_ship, sizeof(g_ship), "%s", shipped_root);
}

static const char *rel_of(const char *tid) {
  if (!tid) return NULL;
  for (int i = 0; i < NTILES; i++)
    if (!strcmp(kTiles[i].id, tid)) return kTiles[i].rel;
  return NULL;
}

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void orig_path(const char *tid, char *o, size_t n) { snprintf(o, n, "%s/%s.orig", g_dir, tid); }
static void where_path(const char *tid, char *o, size_t n) { snprintf(o, n, "%s/%s.where", g_dir, tid); }

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
  char tmp[460];
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

int sysicon_is(const char *tid) { return rel_of(tid) != NULL; }

int sysicon_path(const char *tid, char *out, size_t outsz) {
  const char *rel = rel_of(tid);
  if (!rel || !out || !outsz) return -1;
  snprintf(out, outsz, "%s/%s", g_live, rel); /* firmware with live copies reads these and ignores the shipped ones */
  if (exists(out)) return 0;
  snprintf(out, outsz, "%s/%s", g_ship, rel);
  return exists(out) ? 0 : -1;
}

int sysicon_available(const char *tid) {
  char p[420];
  return sysicon_path(tid, p, sizeof(p)) == 0;
}

int sysicon_custom(const char *tid) {
  char o[400];
  if (!rel_of(tid)) return 0;
  orig_path(tid, o, sizeof(o));
  return exists(o);
}

int sysicon_custom_ids(char (*ids)[16], int max) {
  int n = 0;
  for (int i = 0; i < NTILES && n < max; i++)
    if (sysicon_custom(kTiles[i].id)) snprintf(ids[n++], 16, "%s", kTiles[i].id);
  return n;
}

static int is_png(const unsigned char *b, size_t n) {
  return n > 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8);
}

int sysicon_write(const char *tid, const unsigned char *img, size_t len, int clear, char *err, size_t errsz) {
  char path[420], orig[400], wh[400];
  if (!rel_of(tid)) {
    set_err(err, errsz, "That tile's picture can't be changed this way.");
    return -1;
  }
  if (sysicon_path(tid, path, sizeof(path)) != 0) {
    set_err(err, errsz, "This console has no picture file for that tile.");
    return -1;
  }
  unsigned char *png = NULL;
  size_t pl = 0;
  if (icon_from_image(img, len, clear ? "none" : "dark", &png, &pl)) {
    set_err(err, errsz, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).");
    return -1;
  }
  orig_path(tid, orig, sizeof(orig));
  where_path(tid, wh, sizeof(wh));
  char dir[420];
  snprintf(dir, sizeof(dir), "%s", path);
  char *sl = strrchr(dir, '/');
  if (sl) *sl = 0;
  struct statvfs vs;
  if (statvfs(dir, &vs) == 0 && (unsigned long long)vs.f_bavail * vs.f_frsize < (unsigned long long)pl + (4ull << 20)) {
    free(png);
    set_err(err, errsz, "The console's system drive is almost full.");
    return -1;
  }
  mkdir_p(g_dir);
  if (!exists(orig)) { /* keep the console's own picture the first time, once it has been checked to be a PNG */
    unsigned char *o = NULL;
    size_t ol = 0;
    if (read_all(path, &o, &ol, 16u << 20) != 0 || !is_png(o, ol)) {
      free(o);
      free(png);
      set_err(err, errsz, "The tile's current picture file isn't what was expected, so it was left alone.");
      return -1;
    }
    int rc = write_replace(orig, o, ol, 0666) || write_replace(wh, path, strlen(path), 0666);
    free(o);
    if (rc) {
      unlink(orig);
      free(png);
      set_err(err, errsz, "Couldn't keep a copy of the original picture.");
      return -1;
    }
  }
  struct stat st;
  mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 0777) : 0644;
  int rc = write_replace(path, png, pl, mode);
  free(png);
  if (rc) {
    set_err(err, errsz, "Couldn't write the picture on the console.");
    return -1;
  }
  hh_log("system tile picture %s: written to %s, original kept", tid, path);
  return 0;
}

int sysicon_reset(const char *tid, char *err, size_t errsz) {
  char orig[400], wh[400], path[420] = "";
  if (!rel_of(tid)) {
    set_err(err, errsz, "That tile's picture can't be changed this way.");
    return -1;
  }
  orig_path(tid, orig, sizeof(orig));
  where_path(tid, wh, sizeof(wh));
  if (!exists(orig)) return 0; /* never changed */
  unsigned char *w = NULL;
  size_t wl = 0;
  if (read_all(wh, &w, &wl, sizeof(path) - 1) == 0) {
    memcpy(path, w, wl);
    path[wl] = 0;
  }
  free(w);
  if (!path[0] && sysicon_path(tid, path, sizeof(path)) != 0) {
    set_err(err, errsz, "This console has no picture file for that tile.");
    return -1;
  }
  unsigned char *o = NULL;
  size_t ol = 0;
  struct stat st;
  mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 0777) : 0644;
  if (read_all(orig, &o, &ol, 16u << 20) != 0 || write_replace(path, o, ol, mode) != 0) {
    free(o);
    set_err(err, errsz, "Couldn't put the original picture back.");
    return -1;
  }
  free(o);
  unlink(orig);
  unlink(wh);
  hh_log("system tile picture %s: original put back in %s", tid, path);
  return 0;
}
