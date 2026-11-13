#include "backup.h"

#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arrange.h"
#include "custom.h"
#include "folders.h"
#include "json.h"

#define BK_FORMAT "HomeScreenManager-backup"
#define BK_MAX_ENTRIES 4000
#define BK_MAX_FILE (64u * 1024 * 1024)
#define BK_MAX_FOLDERS 100
#define BK_MAX_ITEMS 600
#define BK_MAX_MISSING 300

static char g_data[256], g_ver[32];

void backup_init(const backup_cfg_t *c) {
  snprintf(g_data, sizeof(g_data), "%s", c && c->data_dir ? c->data_dir : "");
  snprintf(g_ver, sizeof(g_ver), "%s", c && c->app_version ? c->app_version : "");
}

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t crc32_of(const void *p, size_t n) {
  const unsigned char *b = p;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= b[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static unsigned char *read_file(const char *path, size_t max, size_t *len) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  unsigned char *b = malloc((size_t)st.st_size + 1);
  if (!b) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(b, 1, (size_t)st.st_size, f);
  fclose(f);
  if (n != (size_t)st.st_size) {
    free(b);
    return NULL;
  }
  *len = n;
  return b;
}

static sqlite3_stmt *prep(hh_t *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
  return st;
}

static int count1(hh_t *h, const char *sql) {
  sqlite3_stmt *st = prep(h, sql);
  int n = 0;
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  return n;
}

static const char *col(sqlite3_stmt *st, int i) {
  const char *s = (const char *)sqlite3_column_text(st, i);
  return s ? s : "";
}

static int present(hh_t *h, const char *tid) {
  hh_state_t st;
  return hh_valid_tid(tid) && hh_get_state(h, tid, &st) == 0 && st.present;
}

/* ------------------------------------------------------------------------- */
/* ZIP writer (entries are stored, not compressed)                           */
/* ------------------------------------------------------------------------- */

typedef struct {
  char name[100];
  uint32_t crc, size, off;
} zent_t;

typedef struct {
  strbuf_t *out;
  zent_t *e;
  int n, cap, bad;
} zipw_t;

#define DOS_DATE ((46 << 9) | (1 << 5) | 1) /* 2026-01-01: the entries carry no real time */

static void le16(strbuf_t *sb, unsigned v) {
  char b[2] = {(char)(v & 255), (char)((v >> 8) & 255)};
  sb_putn(sb, b, 2);
}
static void le32(strbuf_t *sb, uint32_t v) {
  char b[4] = {(char)(v & 255), (char)((v >> 8) & 255), (char)((v >> 16) & 255), (char)((v >> 24) & 255)};
  sb_putn(sb, b, 4);
}

static void zip_add(zipw_t *z, const char *name, const void *data, size_t len) {
  if (z->bad || len > 0x7FFFFFFFu || strlen(name) >= sizeof(((zent_t *)0)->name)) {
    z->bad = 1;
    return;
  }
  if (z->n == z->cap) {
    int nc = z->cap ? z->cap * 2 : 64;
    zent_t *ne = realloc(z->e, (size_t)nc * sizeof(zent_t));
    if (!ne) {
      z->bad = 1;
      return;
    }
    z->e = ne;
    z->cap = nc;
  }
  zent_t *e = &z->e[z->n++];
  snprintf(e->name, sizeof(e->name), "%s", name);
  e->crc = crc32_of(data, len);
  e->size = (uint32_t)len;
  e->off = (uint32_t)z->out->len;
  le32(z->out, 0x04034b50u);
  le16(z->out, 20);
  le16(z->out, 0x0800); /* names are UTF-8 */
  le16(z->out, 0);      /* stored */
  le16(z->out, 0);
  le16(z->out, DOS_DATE);
  le32(z->out, e->crc);
  le32(z->out, e->size);
  le32(z->out, e->size);
  le16(z->out, (unsigned)strlen(name));
  le16(z->out, 0);
  sb_putn(z->out, name, strlen(name));
  sb_putn(z->out, (const char *)data, len);
}

static void zip_finish(zipw_t *z) {
  uint32_t cd = (uint32_t)z->out->len;
  for (int i = 0; i < z->n; i++) {
    const zent_t *e = &z->e[i];
    le32(z->out, 0x02014b50u);
    le16(z->out, 20);
    le16(z->out, 20);
    le16(z->out, 0x0800);
    le16(z->out, 0);
    le16(z->out, 0);
    le16(z->out, DOS_DATE);
    le32(z->out, e->crc);
    le32(z->out, e->size);
    le32(z->out, e->size);
    le16(z->out, (unsigned)strlen(e->name));
    le16(z->out, 0);
    le16(z->out, 0);
    le16(z->out, 0);
    le16(z->out, 0);
    le32(z->out, 0);
    le32(z->out, e->off);
    sb_putn(z->out, e->name, strlen(e->name));
  }
  uint32_t cdlen = (uint32_t)z->out->len - cd;
  le32(z->out, 0x06054b50u);
  le16(z->out, 0);
  le16(z->out, 0);
  le16(z->out, (unsigned)z->n);
  le16(z->out, (unsigned)z->n);
  le32(z->out, cdlen);
  le32(z->out, cd);
  le16(z->out, 0);
}

/* ------------------------------------------------------------------------- */
/* export                                                                    */
/* ------------------------------------------------------------------------- */

static void add_file_if(zipw_t *z, const char *zip_name, const char *path) {
  size_t len;
  unsigned char *b = read_file(path, BK_MAX_FILE, &len);
  if (b) {
    zip_add(z, zip_name, b, len);
    free(b);
  }
}

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int backup_export(hh_t *h, const char *settings_path, strbuf_t *out) {
  zipw_t z = {out, NULL, 0, 0, 0};
  strbuf_t sb;
  char path[400], zname[100];
  int n_fold = 0, n_hid = 0, n_cus = 0, n_ord = 0, has_settings = 0;

  /* folders, with their games in order and their own picture and background */
  sb_init(&sb);
  sb_puts(&sb, "[");
  sqlite3_stmt *st = prep(h, "SELECT title_id, name, tab, color, custom_icon FROM hh.folders ORDER BY title_id");
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    const char *id = col(st, 0);
    int icon = 0, bg = 0;
    if (sqlite3_column_int(st, 4)) {
      snprintf(path, sizeof(path), "%s/folders/%s.png", g_data, id);
      snprintf(zname, sizeof(zname), "folders/%s.png", id);
      if (file_exists(path)) {
        add_file_if(&z, zname, path);
        icon = 1;
      }
    }
    snprintf(path, sizeof(path), "%s/folders/%s_bg.jpg", g_data, id);
    snprintf(zname, sizeof(zname), "folders/%s_bg.jpg", id);
    if (file_exists(path)) {
      add_file_if(&z, zname, path);
      bg = 1;
    }
    sb_printf(&sb, "%s{\"id\":", n_fold ? "," : "");
    sb_json_str(&sb, id);
    sb_puts(&sb, ",\"name\":");
    sb_json_str(&sb, col(st, 1));
    sb_puts(&sb, ",\"tab\":");
    sb_json_str(&sb, col(st, 2));
    sb_puts(&sb, ",\"color\":");
    sb_json_str(&sb, col(st, 3));
    sb_printf(&sb, ",\"icon\":%s,\"bg\":%s,\"items\":[", icon ? "true" : "false", bg ? "true" : "false");
    sqlite3_stmt *it = prep(h, "SELECT title_id, was_hidden FROM hh.folder_items WHERE folder=?1 ORDER BY pos");
    if (it) {
      sqlite3_bind_text(it, 1, id, -1, SQLITE_TRANSIENT);
      int first = 1;
      while (sqlite3_step(it) == SQLITE_ROW) {
        sb_printf(&sb, "%s{\"id\":", first ? "" : ",");
        sb_json_str(&sb, col(it, 0));
        sb_printf(&sb, ",\"hidden\":%d}", sqlite3_column_int(it, 1) ? 1 : 0);
        first = 0;
      }
      sqlite3_finalize(it);
    }
    sb_puts(&sb, "]}");
    n_fold++;
  }
  if (st) sqlite3_finalize(st);
  sb_puts(&sb, "]");
  zip_add(&z, "folders.json", sb.data, sb.len);
  sb_free(&sb);

  /* tiles hidden on their own (folder members are hidden by their folder) */
  sb_init(&sb);
  sb_puts(&sb, "[");
  st = prep(h, "SELECT title_id FROM hh.wanted WHERE title_id NOT LIKE 'HHFD%' AND title_id NOT IN "
               "(SELECT title_id FROM hh.folder_items) ORDER BY title_id");
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    if (n_hid) sb_puts(&sb, ",");
    sb_json_str(&sb, col(st, 0));
    n_hid++;
  }
  if (st) sqlite3_finalize(st);
  sb_puts(&sb, "]");
  zip_add(&z, "hidden.json", sb.data, sb.len);
  sb_free(&sb);

  /* custom names, pictures and backgrounds of ordinary tiles */
  sb_init(&sb);
  sb_puts(&sb, "[");
  st = prep(h, "SELECT title_id, name, icon_file IS NOT NULL, bg_file IS NOT NULL FROM hh.custom "
               "WHERE title_id NOT LIKE 'HHFD%' ORDER BY title_id");
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    const char *id = col(st, 0);
    int has_name = sqlite3_column_type(st, 1) != SQLITE_NULL && col(st, 1)[0];
    int icon = 0, bg = 0;
    if (sqlite3_column_int(st, 2)) {
      snprintf(path, sizeof(path), "%s/icons/%s.png", g_data, id);
      snprintf(zname, sizeof(zname), "icons/%s.png", id);
      if (file_exists(path)) {
        add_file_if(&z, zname, path);
        icon = 1;
      }
    }
    if (sqlite3_column_int(st, 3)) {
      snprintf(path, sizeof(path), "%s/backgrounds/%s.jpg", g_data, id);
      snprintf(zname, sizeof(zname), "backgrounds/%s.jpg", id);
      if (file_exists(path)) {
        add_file_if(&z, zname, path);
        bg = 1;
      }
    }
    if (!has_name && !icon && !bg) continue;
    sb_printf(&sb, "%s{\"id\":", n_cus ? "," : "");
    sb_json_str(&sb, id);
    sb_puts(&sb, ",\"name\":");
    sb_json_str(&sb, has_name ? col(st, 1) : NULL);
    sb_printf(&sb, ",\"icon\":%s,\"bg\":%s}", icon ? "true" : "false", bg ? "true" : "false");
    n_cus++;
  }
  if (st) sqlite3_finalize(st);
  sb_puts(&sb, "]");
  zip_add(&z, "custom.json", sb.data, sb.len);
  sb_free(&sb);

  /* the saved home screen order, front to back */
  sb_init(&sb);
  sb_puts(&sb, "[");
  st = prep(h, "SELECT title_id, pinned FROM hh.arranged ORDER BY pos");
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    sb_printf(&sb, "%s{\"id\":", n_ord ? "," : "");
    sb_json_str(&sb, col(st, 0));
    sb_printf(&sb, ",\"pinned\":%d}", sqlite3_column_int(st, 1) ? 1 : 0);
    n_ord++;
  }
  if (st) sqlite3_finalize(st);
  sb_puts(&sb, "]");
  zip_add(&z, "order.json", sb.data, sb.len);
  sb_free(&sb);

  /* the settings */
  size_t slen;
  unsigned char *sd = settings_path ? read_file(settings_path, 64 * 1024, &slen) : NULL;
  if (sd) {
    zip_add(&z, "settings.txt", sd, slen);
    free(sd);
    has_settings = 1;
  }

  /* the description of the file */
  char when[32] = "";
  time_t now = time(NULL);
  struct tm tm;
  if (localtime_r(&now, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
  sb_init(&sb);
  sb_printf(&sb, "{\"format\":\"%s\",\"version\":1,\"app_version\":", BK_FORMAT);
  sb_json_str(&sb, g_ver);
  sb_puts(&sb, ",\"created\":");
  sb_json_str(&sb, when);
  sb_printf(&sb, ",\"folders\":%d,\"hidden\":%d,\"custom\":%d,\"order\":%d,\"settings\":%s}", n_fold, n_hid, n_cus, n_ord,
            has_settings ? "true" : "false");
  zip_add(&z, "manifest.json", sb.data, sb.len);
  sb_free(&sb);

  if (!z.bad) zip_finish(&z);
  free(z.e);
  return (z.bad || out->oom) ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* import: reading the file                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
  char name[100];
  size_t off, size;
  uint32_t crc;
} bk_ent_t;

struct backup {
  unsigned char *zip;
  size_t len;
  bk_ent_t *e;
  int n;
};

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_err(char *err, size_t n, const char *msg) {
  if (err && n) snprintf(err, n, "%s", msg);
}

/* 0 = found and its checksum is right, -1 = not in the file, -2 = damaged */
static int bk_find(const backup_t *b, const char *name, const unsigned char **p, size_t *len) {
  for (int i = 0; i < b->n; i++) {
    if (strcmp(b->e[i].name, name)) continue;
    const bk_ent_t *e = &b->e[i];
    if (crc32_of(b->zip + e->off, e->size) != e->crc) return -2;
    *p = b->zip + e->off;
    *len = e->size;
    return 0;
  }
  return -1;
}

typedef struct {
  const char *js;
  json_tok_t *t;
  int n;
} jdoc_t;

static int jdoc_open(const backup_t *b, const char *name, json_type_t want, jdoc_t *d) {
  const unsigned char *p;
  size_t l;
  memset(d, 0, sizeof(*d));
  if (bk_find(b, name, &p, &l) != 0) return -1;
  int max = (int)(l / 2) + 16;
  if (max > 400000) return -1;
  d->t = malloc(sizeof(json_tok_t) * (size_t)max);
  if (!d->t) return -1;
  d->js = (const char *)p;
  d->n = json_parse(d->js, l, d->t, max);
  if (d->n < 1 || d->t[0].type != want) {
    free(d->t);
    d->t = NULL;
    return -1;
  }
  return 0;
}
static void jdoc_close(jdoc_t *d) {
  free(d->t);
  d->t = NULL;
}
static int jstr(const jdoc_t *d, int obj, const char *key, char *out, size_t sz) {
  int v = json_get(d->js, d->t, d->n, obj, key);
  out[0] = '\0';
  if (v < 0) return -1;
  return json_str(d->js, d->t, v, out, sz) < 0 ? -1 : 0;
}
static int jbool(const jdoc_t *d, int obj, const char *key) {
  int v = json_get(d->js, d->t, d->n, obj, key);
  return v >= 0 && d->t[v].type == JSON_PRIM && d->js[d->t[v].start] == 't';
}
static int jint(const jdoc_t *d, int obj, const char *key) {
  int v = json_get(d->js, d->t, d->n, obj, key);
  return (v >= 0 && d->t[v].type == JSON_PRIM) ? atoi(d->js + d->t[v].start) : 0;
}

backup_t *backup_open(unsigned char *zip, size_t len, char *err, size_t err_size) {
  if (len < 22 || zip[0] != 'P' || zip[1] != 'K') {
    set_err(err, err_size, "That isn't a Home Screen Manager backup file (it isn't a ZIP file).");
    return NULL;
  }
  long eocd = -1;
  for (long i = (long)len - 22; i >= 0 && i >= (long)len - 22 - 65535; i--)
    if (rd32(zip + i) == 0x06054b50u) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    set_err(err, err_size, "That ZIP file is damaged or incomplete.");
    return NULL;
  }
  unsigned n = rd16(zip + eocd + 10);
  uint32_t cd_size = rd32(zip + eocd + 12), cd_off = rd32(zip + eocd + 16);
  if (n == 0xFFFF || cd_off == 0xFFFFFFFFu || n > BK_MAX_ENTRIES || (size_t)cd_off + cd_size > len) {
    set_err(err, err_size, "That ZIP file is damaged or too large to read.");
    return NULL;
  }
  backup_t *b = calloc(1, sizeof(*b));
  if (!b) return NULL;
  b->e = calloc(n ? n : 1, sizeof(bk_ent_t));
  if (!b->e) {
    free(b);
    return NULL;
  }
  size_t pos = cd_off;
  for (unsigned i = 0; i < n; i++) {
    if (pos + 46 > cd_off + (size_t)cd_size || rd32(zip + pos) != 0x02014b50u) goto damaged;
    unsigned flags = rd16(zip + pos + 8), method = rd16(zip + pos + 10);
    uint32_t crc = rd32(zip + pos + 16), csize = rd32(zip + pos + 20), usize = rd32(zip + pos + 24);
    unsigned nl = rd16(zip + pos + 28), xl = rd16(zip + pos + 30), cl = rd16(zip + pos + 32);
    uint32_t lo = rd32(zip + pos + 42);
    if (pos + 46 + nl + xl + cl > cd_off + (size_t)cd_size || nl == 0 || nl >= sizeof(b->e[0].name)) goto damaged;
    char name[100];
    memcpy(name, zip + pos + 46, nl);
    name[nl] = '\0';
    pos += 46 + nl + xl + cl;
    if (name[nl - 1] == '/') continue; /* a folder entry */
    if ((flags & 1) || method != 0 || csize != usize) {
      free(b->e);
      free(b);
      set_err(err, err_size, "This backup is compressed or protected in a way Home Screen Manager can't read. Use the file exactly as it was exported.");
      return NULL;
    }
    if (strstr(name, "..") || name[0] == '/' || strchr(name, '\\')) goto damaged;
    if ((size_t)lo + 30 > len || rd32(zip + lo) != 0x04034b50u) goto damaged;
    size_t data = (size_t)lo + 30 + rd16(zip + lo + 26) + rd16(zip + lo + 28);
    if (data > len || usize > len - data) goto damaged;
    bk_ent_t *e = &b->e[b->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->off = data;
    e->size = usize;
    e->crc = crc;
  }
  b->zip = zip;
  b->len = len;

  /* it must be one of ours */
  jdoc_t m;
  if (jdoc_open(b, "manifest.json", JSON_OBJ, &m) != 0) {
    b->zip = NULL;
    backup_close(b);
    set_err(err, err_size, "That ZIP file isn't a Home Screen Manager backup.");
    return NULL;
  }
  char fmt[48];
  jstr(&m, 0, "format", fmt, sizeof(fmt));
  int ver = jint(&m, 0, "version");
  jdoc_close(&m);
  if (strcmp(fmt, BK_FORMAT)) {
    b->zip = NULL;
    backup_close(b);
    set_err(err, err_size, "That ZIP file isn't a Home Screen Manager backup.");
    return NULL;
  }
  if (ver > 1) {
    b->zip = NULL;
    backup_close(b);
    set_err(err, err_size, "This backup was made by a newer version of Home Screen Manager. Update first.");
    return NULL;
  }
  return b;

damaged:
  free(b->e);
  free(b);
  set_err(err, err_size, "That ZIP file is damaged.");
  return NULL;
}

void backup_close(backup_t *b) {
  if (!b) return;
  free(b->zip);
  free(b->e);
  free(b);
}

/* ------------------------------------------------------------------------- */
/* import: what the file holds                                               */
/* ------------------------------------------------------------------------- */

static void note_missing(char (*arr)[16], int *n, const char *tid) {
  for (int i = 0; i < *n; i++)
    if (!strcmp(arr[i], tid)) return;
  if (*n < BK_MAX_MISSING) snprintf(arr[(*n)++], 16, "%.15s", tid);
}

void backup_summary_json(hh_t *h, const backup_t *b, strbuf_t *sb) {
  char (*missing)[16] = calloc(BK_MAX_MISSING, 16);
  int nm = 0;
  jdoc_t d;
  char s[160];
  sb_puts(sb, "{\"ok\":true");

  if (jdoc_open(b, "manifest.json", JSON_OBJ, &d) == 0) {
    jstr(&d, 0, "created", s, sizeof(s));
    sb_puts(sb, ",\"created\":");
    sb_json_str(sb, s);
    jstr(&d, 0, "app_version", s, sizeof(s));
    sb_puts(sb, ",\"app_version\":");
    sb_json_str(sb, s);
    jdoc_close(&d);
  }

  sb_puts(sb, ",\"folders\":[");
  int shown = 0;
  if (missing && jdoc_open(b, "folders.json", JSON_ARR, &d) == 0) {
    for (int i = 0, fo; (fo = json_at(d.t, d.n, 0, i)) >= 0 && i < BK_MAX_FOLDERS; i++) {
      char name[100];
      jstr(&d, fo, "name", name, sizeof(name));
      int games = 0, gone = 0, items = json_get(d.js, d.t, d.n, fo, "items");
      for (int j = 0, io; items >= 0 && (io = json_at(d.t, d.n, items, j)) >= 0; j++) {
        char tid[32];
        jstr(&d, io, "id", tid, sizeof(tid));
        games++;
        if (!present(h, tid)) {
          gone++;
          note_missing(missing, &nm, tid);
        }
      }
      sb_printf(sb, "%s{\"name\":", shown ? "," : "");
      sb_json_str(sb, name);
      sb_printf(sb, ",\"games\":%d,\"missing\":%d,\"icon\":%s,\"bg\":%s}", games, gone, jbool(&d, fo, "icon") ? "true" : "false",
                jbool(&d, fo, "bg") ? "true" : "false");
      shown++;
    }
    jdoc_close(&d);
  }
  sb_puts(sb, "]");

  int hidden = 0, hidden_gone = 0;
  if (missing && jdoc_open(b, "hidden.json", JSON_ARR, &d) == 0) {
    for (int i = 0, io; (io = json_at(d.t, d.n, 0, i)) >= 0; i++) {
      char tid[32];
      if (json_str(d.js, d.t, io, tid, sizeof(tid)) < 0) continue;
      hidden++;
      if (!present(h, tid)) {
        hidden_gone++;
        note_missing(missing, &nm, tid);
      }
    }
    jdoc_close(&d);
  }
  int custom = 0, custom_gone = 0;
  if (missing && jdoc_open(b, "custom.json", JSON_ARR, &d) == 0) {
    for (int i = 0, co; (co = json_at(d.t, d.n, 0, i)) >= 0; i++) {
      char tid[32];
      jstr(&d, co, "id", tid, sizeof(tid));
      custom++;
      if (!present(h, tid)) {
        custom_gone++;
        note_missing(missing, &nm, tid);
      }
    }
    jdoc_close(&d);
  }
  int order = 0;
  if (jdoc_open(b, "order.json", JSON_ARR, &d) == 0) {
    for (int i = 0; json_at(d.t, d.n, 0, i) >= 0; i++) order++;
    jdoc_close(&d);
  }
  const unsigned char *p;
  size_t l;
  sb_printf(sb, ",\"hidden\":%d,\"hidden_missing\":%d,\"custom\":%d,\"custom_missing\":%d,\"order\":%d,\"settings\":%s", hidden,
            hidden_gone, custom, custom_gone, order, bk_find(b, "settings.txt", &p, &l) == 0 ? "true" : "false");
  sb_printf(sb, ",\"existing_folders\":%d,\"missing\":[", count1(h, "SELECT count(*) FROM hh.folders"));
  for (int i = 0; i < nm; i++) {
    if (i) sb_puts(sb, ",");
    sb_json_str(sb, missing[i]);
  }
  sb_puts(sb, "]}");
  free(missing);
}

/* ------------------------------------------------------------------------- */
/* import: applying it                                                       */
/* ------------------------------------------------------------------------- */

static void problem(backup_progress_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void problem(backup_progress_t *p, const char *fmt, ...) {
  char one[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(one, sizeof(one), fmt, ap);
  va_end(ap);
  p->problems++;
  size_t used = strlen(p->notes);
  if (used + strlen(one) + 3 < sizeof(p->notes)) snprintf(p->notes + used, sizeof(p->notes) - used, "%s%s", used ? "; " : "", one);
}

int backup_apply(hh_t *h, backup_t *b, backup_progress_t *p, int (*apply_settings)(const char *, size_t), char *err,
                 size_t err_size) {
  const unsigned char *data;
  size_t len;
  jdoc_t d;
  p->folder_i = p->folder_n = 0;
  p->folders_made = p->folders_skipped = p->hidden_done = p->custom_done = p->order_done = p->settings_done = p->problems = 0;
  p->notes[0] = '\0';
  if (h->nusers == 0) {
    set_err(err, err_size, "The home screen database layout isn't supported.");
    return -1;
  }
  char (*old_ids)[16] = calloc(BK_MAX_FOLDERS, 16);
  char (*new_ids)[16] = calloc(BK_MAX_FOLDERS, 16);
  if (!old_ids || !new_ids) {
    free(old_ids);
    free(new_ids);
    return -1;
  }
  int nmap = 0;

  /* 1. settings */
  if (apply_settings && bk_find(b, "settings.txt", &data, &len) == 0) {
    snprintf(p->phase, sizeof(p->phase), "Restoring the settings…");
    if (apply_settings((const char *)data, len) == 0) p->settings_done = 1;
    else problem(p, "the settings could not be restored");
  }

  /* 2. folders: each one is created like a new folder (this is the slow part) */
  if (jdoc_open(b, "folders.json", JSON_ARR, &d) == 0) {
    int total = 0;
    while (json_at(d.t, d.n, 0, total) >= 0 && total < BK_MAX_FOLDERS) total++;
    p->folder_n = total;
    for (int i = 0, fo; i < total && (fo = json_at(d.t, d.n, 0, i)) >= 0; i++) {
      char name[100], tab[16], color[16], oldid[32];
      jstr(&d, fo, "name", name, sizeof(name));
      jstr(&d, fo, "tab", tab, sizeof(tab));
      jstr(&d, fo, "color", color, sizeof(color));
      jstr(&d, fo, "id", oldid, sizeof(oldid));
      p->folder_i = i + 1;
      strbuf_t csv;
      sb_init(&csv);
      int kept = 0, items = json_get(d.js, d.t, d.n, fo, "items");
      for (int j = 0, io; items >= 0 && (io = json_at(d.t, d.n, items, j)) >= 0 && j < BK_MAX_ITEMS; j++) {
        char tid[32];
        jstr(&d, io, "id", tid, sizeof(tid));
        if (!present(h, tid)) continue; /* not installed here */
        sb_printf(&csv, "%s%s", kept ? "," : "", tid);
        kept++;
      }
      if (!kept || csv.oom) {
        p->folders_skipped++;
        problem(p, "folder \"%s\" skipped: none of its games are installed", name);
        sb_free(&csv);
        continue;
      }
      snprintf(p->phase, sizeof(p->phase), "Creating folder \"%s\" (%d of %d)…", name, i + 1, total);
      char out_id[16] = "", note[200] = "";
      int changed = 1;
      if (folders_save(h, "", name, tab, color, csv.data, out_id, note, sizeof(note), &changed) != 0) {
        problem(p, "folder \"%s\": %s", name, h->err);
        sb_free(&csv);
        continue;
      }
      sb_free(&csv);
      p->folders_made++;
      if (nmap < BK_MAX_FOLDERS) {
        snprintf(old_ids[nmap], 16, "%.15s", oldid);
        snprintf(new_ids[nmap], 16, "%.15s", out_id);
        nmap++;
      }
      char zn[100];
      if (jbool(&d, fo, "icon")) {
        snprintf(zn, sizeof(zn), "folders/%s.png", oldid);
        if (bk_find(b, zn, &data, &len) == 0) {
          snprintf(p->phase, sizeof(p->phase), "Folder \"%s\": adding its picture (%d of %d)…", name, i + 1, total);
          if (folders_set_icon(h, out_id, data, len) != 0) problem(p, "folder \"%s\": its picture: %s", name, h->err);
        }
      }
      if (jbool(&d, fo, "bg")) {
        snprintf(zn, sizeof(zn), "folders/%s_bg.jpg", oldid);
        if (bk_find(b, zn, &data, &len) == 0) {
          snprintf(p->phase, sizeof(p->phase), "Folder \"%s\": adding its background (%d of %d)…", name, i + 1, total);
          if (folders_set_bg(h, out_id, data, len) != 0) problem(p, "folder \"%s\": its background: %s", name, h->err);
        }
      }
    }
    jdoc_close(&d);
  }

  /* 3. tiles you hid */
  if (jdoc_open(b, "hidden.json", JSON_ARR, &d) == 0) {
    snprintf(p->phase, sizeof(p->phase), "Hiding your hidden tiles…");
    for (int i = 0, io; (io = json_at(d.t, d.n, 0, i)) >= 0; i++) {
      char tid[32];
      hh_state_t st;
      if (json_str(d.js, d.t, io, tid, sizeof(tid)) < 0 || !present(h, tid)) continue;
      if (hh_get_state(h, tid, &st) == 0 && st.wanted) continue;
      if (hh_hide(h, tid) == 0) p->hidden_done++;
      else problem(p, "couldn't hide %s: %s", tid, h->err);
    }
    jdoc_close(&d);
  }

  /* 4. custom names, pictures and backgrounds */
  if (jdoc_open(b, "custom.json", JSON_ARR, &d) == 0) {
    snprintf(p->phase, sizeof(p->phase), "Restoring custom names, pictures and backgrounds…");
    for (int i = 0, co; (co = json_at(d.t, d.n, 0, i)) >= 0; i++) {
      char tid[32], name[100], zn[100];
      jstr(&d, co, "id", tid, sizeof(tid));
      if (!present(h, tid)) continue;
      int did = 0;
      if (jstr(&d, co, "name", name, sizeof(name)) == 0 && name[0]) {
        if (custom_set_name(h, tid, name) == 0) did = 1;
        else problem(p, "%s: its name: %s", tid, h->err);
      }
      if (jbool(&d, co, "icon")) {
        snprintf(zn, sizeof(zn), "icons/%s.png", tid);
        if (bk_find(b, zn, &data, &len) == 0) {
          if (custom_set_icon(h, tid, data, len, 1) == 0) did = 1;
          else problem(p, "%s: its picture: %s", tid, h->err);
        }
      }
      if (jbool(&d, co, "bg")) {
        snprintf(zn, sizeof(zn), "backgrounds/%s.jpg", tid);
        if (bk_find(b, zn, &data, &len) == 0) {
          if (custom_set_bg(h, tid, data, len) == 0) did = 1;
          else problem(p, "%s: its background: %s", tid, h->err);
        }
      }
      p->custom_done += did;
    }
    jdoc_close(&d);
  }

  /* 5. the saved order (folders under their new ids). An empty list leaves the current order alone. */
  if (jdoc_open(b, "order.json", JSON_ARR, &d) == 0) {
    snprintf(p->phase, sizeof(p->phase), "Restoring the home screen order…");
    strbuf_t items, pinned;
    sb_init(&items);
    sb_init(&pinned);
    int n_items = 0, n_pinned = 0;
    for (int i = 0, oo; (oo = json_at(d.t, d.n, 0, i)) >= 0; i++) {
      char tid[32];
      jstr(&d, oo, "id", tid, sizeof(tid));
      for (int k = 0; k < nmap; k++)
        if (!strcmp(old_ids[k], tid)) {
          snprintf(tid, sizeof(tid), "%s", new_ids[k]);
          break;
        }
      if (!present(h, tid)) continue;
      sb_printf(&items, "%s%s", n_items++ ? "," : "", tid);
      if (jint(&d, oo, "pinned")) sb_printf(&pinned, "%s%s", n_pinned++ ? "," : "", tid);
    }
    if (n_items && !items.oom && !pinned.oom) {
      if (arrange_set(h, items.data, n_pinned ? pinned.data : "") == 0) p->order_done = n_items;
      else problem(p, "the order: %s", h->err);
    }
    sb_free(&items);
    sb_free(&pinned);
    jdoc_close(&d);
  }

  snprintf(p->phase, sizeof(p->phase), "Done.");
  free(old_ids);
  free(new_ids);
  return 0;
}
