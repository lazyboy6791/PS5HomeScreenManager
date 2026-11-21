#include "sysbg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "icons.h"
#include "log.h"
#include "texture.h"

#define DDS_FILE_BYTES 8294548u /* 148 byte header + 3840 x 2160 x 1 byte (BC7) */
#define ZSTD_BLOCK     131072u

static const struct {
  const char *id, *name;
} kTiles[] = {
    {"NPXS40037", "PlayStation Plus"}, {"NPXS40047", "PlayStation Store"}, {"NPXS40053", "TV & Video"},
    {"NPXS40054", "Music"},            {"NPXS40056", "All Apps"},          {"NPXS40063", "Explore"},
    {"NPXS40071", "Game Library"},     {"NPXS40075", "Media Gallery"},     {"NPXS40139", "App Library"},
    {"NPXS40080", "Share Play"},       {"NPXS40110", "Disc Player"},       {"NPXS40144", "Unsupported Disc"},
    {"NPXS40154", "Remote Play"},
};
#define NTILES ((int)(sizeof(kTiles) / sizeof(kTiles[0])))

static char g_dir[300] = "/data/homescreenmanager/sysbg";
static char g_assets[300] = "/system_ex/vsh_asset";

void sysbg_init(const char *data_dir, const char *asset_dir) {
  snprintf(g_dir, sizeof(g_dir), "%s/sysbg", data_dir);
  if (asset_dir && *asset_dir) snprintf(g_assets, sizeof(g_assets), "%s", asset_dir);
}

int sysbg_is(const char *tid) { return sysbg_name(tid) != NULL; }

const char *sysbg_name(const char *tid) {
  if (!tid) return NULL;
  for (int i = 0; i < NTILES; i++)
    if (!strcmp(tid, kTiles[i].id)) return kTiles[i].name;
  return NULL;
}

static void asset_path(const char *tid, char *out, size_t n) { snprintf(out, n, "%s/bg_%s.dds.zst", g_assets, tid); }
static void orig_path(const char *tid, char *out, size_t n) { snprintf(out, n, "%s/%s.orig", g_dir, tid); }
static void jpg_path(const char *tid, char *out, size_t n) { snprintf(out, n, "%s/%s.jpg", g_dir, tid); }

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

int sysbg_available(const char *tid) {
  char p[400];
  if (!sysbg_is(tid)) return 0;
  asset_path(tid, p, sizeof(p));
  return exists(p);
}

int sysbg_custom(const char *tid) {
  char p[400];
  if (!sysbg_is(tid)) return 0;
  orig_path(tid, p, sizeof(p));
  return exists(p);
}

int sysbg_ids(char (*ids)[16], int max) {
  int n = 0;
  for (int i = 0; i < NTILES && n < max; i++) snprintf(ids[n++], 16, "%s", kTiles[i].id);
  return n;
}

int sysbg_custom_ids(char (*ids)[16], int max) {
  int n = 0;
  for (int i = 0; i < NTILES && n < max; i++)
    if (sysbg_custom(kTiles[i].id)) snprintf(ids[n++], 16, "%s", kTiles[i].id);
  return n;
}

static void mkdir_p(const char *dir) {
  char tmp[400];
  snprintf(tmp, sizeof(tmp), "%s", dir);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0777);
      *p = '/';
    }
  mkdir(tmp, 0777);
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

/* writes next to the target and renames over it, so a half-written file never stands in for the real one */
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

/* The picture as a zstd file the console reads: raw (stored) blocks, so no compressor is needed. The frame has
 * the content size in its header, like the console's own files. */
static unsigned char *zstd_stored(const unsigned char *src, size_t n, size_t *out_n) {
  size_t blocks = (n + ZSTD_BLOCK - 1) / ZSTD_BLOCK;
  size_t total = 4 + 1 + 1 + 4 + blocks * 3 + n;
  unsigned char *o = malloc(total), *p = o;
  if (!o) return NULL;
  *p++ = 0x28; *p++ = 0xB5; *p++ = 0x2F; *p++ = 0xFD; /* magic */
  *p++ = 0x80;                                         /* content size in 4 bytes, no checksum, no dictionary */
  *p++ = 0x60;                                         /* window 4 MiB, like the console's own frames */
  *p++ = (unsigned char)(n & 0xFF); *p++ = (unsigned char)((n >> 8) & 0xFF);
  *p++ = (unsigned char)((n >> 16) & 0xFF); *p++ = (unsigned char)((n >> 24) & 0xFF);
  for (size_t off = 0; off < n; off += ZSTD_BLOCK) {
    size_t take = n - off < ZSTD_BLOCK ? n - off : ZSTD_BLOCK;
    uint32_t hdr = (uint32_t)(take << 3) | (off + take >= n ? 1u : 0u); /* raw block */
    *p++ = (unsigned char)(hdr & 0xFF); *p++ = (unsigned char)((hdr >> 8) & 0xFF); *p++ = (unsigned char)((hdr >> 16) & 0xFF);
    memcpy(p, src + off, take);
    p += take;
  }
  *out_n = (size_t)(p - o);
  return o;
}

static void set_err(char *err, size_t n, const char *msg) {
  if (err && n) snprintf(err, n, "%s", msg);
}

int sysbg_prepare(const unsigned char *img, size_t len, sysbg_blob_t *out, char *err, size_t errsz) {
  memset(out, 0, sizeof(*out));
  uint8_t *rgba = icon_load_cover(img, len, TEX_BG_W, TEX_BG_H);
  if (!rgba || img_encode_jpg(rgba, TEX_BG_W, TEX_BG_H, 90, &out->jpg, &out->jpg_len) != 0) {
    free(rgba);
    sysbg_blob_free(out);
    set_err(err, errsz, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).");
    return -1;
  }
  free(rgba);
  tex_fmt_t f;
  memset(&f, 0, sizeof(f));
  f.kind = TEX_DDS;
  f.w = TEX_BG_W;
  f.h = TEX_BG_H;
  f.mips = 1;
  f.dxgi = 98; /* BC7 */
  unsigned char *dds = NULL;
  size_t dl = 0;
  if (tex_encode(out->jpg, out->jpg_len, &f, &dds, &dl) != 0) {
    sysbg_blob_free(out);
    set_err(err, errsz, "The picture couldn't be converted.");
    return -1;
  }
  if (dl != DDS_FILE_BYTES) { /* the console's own files are exactly this size: anything else is not the same texture */
    hh_log("system background: the converted texture is %zu bytes, not %u: not written", dl, DDS_FILE_BYTES);
    free(dds);
    sysbg_blob_free(out);
    set_err(err, errsz, "The converted picture isn't the size the console uses, so nothing was written.");
    return -1;
  }
  out->file = zstd_stored(dds, dl, &out->file_len);
  free(dds);
  if (!out->file) {
    sysbg_blob_free(out);
    set_err(err, errsz, "Out of memory.");
    return -1;
  }
  return 0;
}

void sysbg_blob_free(sysbg_blob_t *b) {
  free(b->file);
  free(b->jpg);
  memset(b, 0, sizeof(*b));
}

int sysbg_write(const char *tid, const sysbg_blob_t *b, char *err, size_t errsz) {
  char path[400], orig[400], jpg[400];
  if (!sysbg_is(tid)) {
    set_err(err, errsz, "That tile can't take a background.");
    return -1;
  }
  asset_path(tid, path, sizeof(path));
  orig_path(tid, orig, sizeof(orig));
  jpg_path(tid, jpg, sizeof(jpg));
  struct stat st;
  if (stat(path, &st) != 0) {
    set_err(err, errsz, "This console has no background file for that tile.");
    return -1;
  }
  struct statvfs vs;
  if (statvfs(g_assets, &vs) == 0 && (unsigned long long)vs.f_bavail * vs.f_frsize < (unsigned long long)b->file_len + (32ull << 20)) {
    set_err(err, errsz, "The console's system drive is almost full.");
    return -1;
  }
  mkdir_p(g_dir);
  if (!exists(orig)) { /* keep the console's own file the first time, once it has been checked to be one */
    unsigned char *o = NULL;
    size_t ol = 0;
    if (read_all(path, &o, &ol, 64u << 20) != 0 || ol < 16 || o[0] != 0x28 || o[1] != 0xB5 || o[2] != 0x2F || o[3] != 0xFD) {
      free(o);
      set_err(err, errsz, "The tile's current background file isn't what was expected, so it was left alone.");
      return -1;
    }
    int rc = write_replace(orig, o, ol, 0666);
    free(o);
    if (rc) {
      set_err(err, errsz, "Couldn't keep a copy of the original background.");
      return -1;
    }
  }
  if (write_replace(path, b->file, b->file_len, st.st_mode & 0777) != 0) {
    set_err(err, errsz, "Couldn't write the background on the console.");
    return -1;
  }
  write_replace(jpg, b->jpg, b->jpg_len, 0666);
  hh_log("system background %s: written (%zu bytes), original kept", tid, b->file_len);
  return 0;
}

int sysbg_reset(const char *tid, char *err, size_t errsz) {
  char path[400], orig[400], jpg[400];
  if (!sysbg_is(tid)) {
    set_err(err, errsz, "That tile can't take a background.");
    return -1;
  }
  asset_path(tid, path, sizeof(path));
  orig_path(tid, orig, sizeof(orig));
  jpg_path(tid, jpg, sizeof(jpg));
  if (!exists(orig)) return 0; /* never changed */
  unsigned char *o = NULL;
  size_t ol = 0;
  struct stat st;
  mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 0777) : 0666;
  if (read_all(orig, &o, &ol, 64u << 20) != 0 || write_replace(path, o, ol, mode) != 0) {
    free(o);
    set_err(err, errsz, "Couldn't put the original background back.");
    return -1;
  }
  free(o);
  unlink(orig);
  unlink(jpg);
  hh_log("system background %s: original put back", tid);
  return 0;
}

int sysbg_preview_path(const char *tid, char *out, size_t outsz) {
  if (!sysbg_is(tid)) return -1;
  jpg_path(tid, out, outsz);
  return exists(out) ? 0 : -1;
}
