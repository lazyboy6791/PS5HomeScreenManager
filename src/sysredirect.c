#include "sysredirect.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#include "log.h"

#define URL_MAX 400

static char g_dir[320];
static char g_appinfo[320];

static const struct {
  const char *id, *name;
} TILES[] = {{"NPXS40047", "PlayStation Store"}, {"NPXS40037", "PlayStation Plus"}, {"NPXS40063", "Welcome"}};
#define NTILES ((int)(sizeof(TILES) / sizeof(TILES[0])))

/* Links offered in the menu's list (name, link). Anything else can be typed in. */
static const struct {
  const char *name, *url;
} PRESETS[] = {{"Trophies", "pstc:browse"},
              {"Saved Data (PS4)", "pssettings:play?function=savedata_ps4"},
              {"SSD Storage", "pssettings:play?mode=settings&function=storage&storage=internal&userid="},
              {"Network Settings", "pssettings:play?mode=settings&function=network"},
              {"Controller Settings", "pssettings:play?mode=settings&function=controller"},
              {"Debug Settings", "pssettings:play?function=debug_settings"},
              {"Remote Play", "psgm:play?id=NPXS40074"},
              {"Share Play", "psgm:play?id=NPXS40099"},
              {"Game Streaming", "psgm:play?id=NPXS40106"},
              {"Disc Player", "psgm:play?id=NPXS40140&arg=mode%3Dplay"},
              {"Media Gallery", "psmediagallery"},
              {NULL, NULL}};
#define NPRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])) - 1)

void sysredirect_init(const char *data_dir, const char *appinfo_db_path) {
  snprintf(g_dir, sizeof(g_dir), "%s/redirect", data_dir);
  snprintf(g_appinfo, sizeof(g_appinfo), "%s", appinfo_db_path && appinfo_db_path[0] ? appinfo_db_path : "/system_data/priv/mms/appinfo.db");
  mkdir(g_dir, 0777);
}

int sysredirect_is(const char *tid) {
  for (int i = 0; i < NTILES; i++)
    if (!strcmp(TILES[i].id, tid)) return 1;
  return 0;
}

/* ---- the saved state: four lines. The first three are what the tile held before ("=value" or "-" for nothing):
 * pprDeeplinkUri, the DEEPLINK_URI inside AppInfoJson, the appinfo.db row. The fourth is the link we set. ---- */
typedef struct {
  char orig_ppr[URL_MAX + 4], orig_json[URL_MAX + 4], orig_info[URL_MAX + 4], url[URL_MAX + 4];
} state_t;

static void state_path(const char *tid, char *out, size_t n) { snprintf(out, n, "%s/%s.txt", g_dir, tid); }

static int state_load(const char *tid, state_t *s) {
  char p[400], *f[4] = {s->orig_ppr, s->orig_json, s->orig_info, s->url};
  memset(s, 0, sizeof(*s));
  state_path(tid, p, sizeof(p));
  FILE *fp = fopen(p, "r");
  if (!fp) return -1;
  for (int i = 0; i < 4; i++) {
    if (!fgets(f[i], URL_MAX + 4, fp)) {
      fclose(fp);
      return -1;
    }
    f[i][strcspn(f[i], "\r\n")] = 0;
  }
  fclose(fp);
  return s->url[0] ? 0 : -1;
}

static int state_save(const char *tid, const state_t *s) {
  char p[400], t[420];
  state_path(tid, p, sizeof(p));
  snprintf(t, sizeof(t), "%s.tmp", p);
  mkdir(g_dir, 0777);
  FILE *fp = fopen(t, "w");
  if (!fp) return -1;
  fprintf(fp, "%s\n%s\n%s\n%s\n", s->orig_ppr, s->orig_json, s->orig_info, s->url);
  if (fclose(fp)) {
    unlink(t);
    return -1;
  }
  return rename(t, p);
}

int sysredirect_custom(const char *tid) {
  state_t s;
  return sysredirect_is(tid) && state_load(tid, &s) == 0;
}

int sysredirect_custom_ids(char (*ids)[16], int max) {
  int n = 0;
  for (int i = 0; i < NTILES && n < max; i++)
    if (sysredirect_custom(TILES[i].id)) snprintf(ids[n++], 16, "%s", TILES[i].id);
  return n;
}

/* ---- the databases ---- */
/* what a value looks like in the saved state */
static void enc(const char *v, char *out, size_t n) {
  if (v) snprintf(out, n, "=%s", v);
  else snprintf(out, n, "-");
}

/* reads the two values the tile holds in app.db; -1 if the console has no such columns */
static int read_app(hh_t *h, const char *tid, char *ppr, size_t pn, char *js, size_t jn, int *has_ppr, int *has_js) {
  sqlite3_stmt *st = NULL;
  *has_ppr = *has_js = 0;
  ppr[0] = js[0] = 0;
  if (sqlite3_prepare_v2(h->db,
                         "SELECT pprDeeplinkUri, CASE WHEN json_valid(AppInfoJson) THEN json_extract(AppInfoJson,'$.DEEPLINK_URI') END "
                         "FROM main.tbl_contentinfo WHERE titleId=?1",
                         -1, &st, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_STATIC);
  int rc = -1;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *a = sqlite3_column_text(st, 0), *b = sqlite3_column_text(st, 1);
    if (a) {
      snprintf(ppr, pn, "%s", a);
      *has_ppr = 1;
    }
    if (b) {
      snprintf(js, jn, "%s", b);
      *has_js = 1;
    }
    rc = 0;
  }
  sqlite3_finalize(st);
  return rc;
}

static int exec1(sqlite3 *db, const char *sql, const char *a, const char *b, const char *c) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
  const char *v[3] = {a, b, c};
  for (int i = 0; i < 3; i++)
    if (v[i]) sqlite3_bind_text(st, i + 1, v[i], -1, SQLITE_STATIC);
  int rc = sqlite3_step(st) == SQLITE_DONE ? sqlite3_changes(db) : -1;
  sqlite3_finalize(st);
  return rc;
}

/* appinfo.db: the row's value, or NULL when there is no row (or no file). Returns -1 if the database can't be used. */
static sqlite3 *open_info(void) {
  sqlite3 *db = NULL;
  if (access(g_appinfo, F_OK) != 0) return NULL;
  if (sqlite3_open_v2(g_appinfo, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db, 5000);
  return db;
}

static int read_info(const char *tid, char *out, size_t n, int *has) {
  *has = 0;
  out[0] = 0;
  sqlite3 *db = open_info();
  if (!db) return -1;
  sqlite3_stmt *st = NULL;
  int rc = -1;
  if (sqlite3_prepare_v2(db, "SELECT val FROM tbl_appinfo WHERE titleId=?1 AND key='DEEPLINK_URI'", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_STATIC);
    int s = sqlite3_step(st);
    if (s == SQLITE_ROW) {
      const unsigned char *v = sqlite3_column_text(st, 0);
      if (v) {
        snprintf(out, n, "%s", v);
        *has = 1;
      }
    }
    rc = (s == SQLITE_ROW || s == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return rc;
}

/* puts the three places to the given values (NULL = remove). Returns 0, or -1 with err. */
static int write_all(hh_t *h, const char *tid, const char *ppr, const char *js, const char *info, char *err, size_t errsz) {
  if (sqlite3_exec(h->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(err, errsz, "The console's database is busy. Try again in a moment.");
    return -1;
  }
  int a = exec1(h->db, "UPDATE main.tbl_contentinfo SET pprDeeplinkUri=?1 WHERE titleId=?2", ppr, tid, NULL);
  int b;
  if (js)
    b = exec1(h->db,
              "UPDATE main.tbl_contentinfo SET AppInfoJson=CASE WHEN json_valid(AppInfoJson) THEN json_set(AppInfoJson,'$.DEEPLINK_URI',?1) "
              "ELSE AppInfoJson END WHERE titleId=?2",
              js, tid, NULL);
  else
    b = exec1(h->db,
              "UPDATE main.tbl_contentinfo SET AppInfoJson=CASE WHEN json_valid(AppInfoJson) THEN json_remove(AppInfoJson,'$.DEEPLINK_URI') "
              "ELSE AppInfoJson END WHERE titleId=?1",
              tid, NULL, NULL);
  if (a < 0 || b < 0) {
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(err, errsz, "The console's database refused the change.");
    return -1;
  }
  sqlite3_exec(h->db, "COMMIT", NULL, NULL, NULL);
  sqlite3 *db = open_info();
  if (db) {
    if (info) {
      int had = 0;
      char cur[URL_MAX + 4];
      read_info(tid, cur, sizeof(cur), &had);
      if (had) {
        exec1(db, "UPDATE tbl_appinfo SET val=?1 WHERE titleId=?2 AND key='DEEPLINK_URI'", info, tid, NULL);
      } else {
        char meta[64] = "prior";
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT metaDataId FROM tbl_appinfo WHERE titleId=?1 LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
          sqlite3_bind_text(st, 1, tid, -1, SQLITE_STATIC);
          if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) snprintf(meta, sizeof(meta), "%s", sqlite3_column_text(st, 0));
          sqlite3_finalize(st);
        }
        exec1(db, "INSERT INTO tbl_appinfo (titleId, metaDataId, key, val) VALUES (?1, ?2, 'DEEPLINK_URI', ?3)", tid, meta, info);
      }
    } else {
      exec1(db, "DELETE FROM tbl_appinfo WHERE titleId=?1 AND key='DEEPLINK_URI'", tid, NULL, NULL);
    }
    sqlite3_close(db);
  }
  return 0;
}

/* ---- the calls ---- */
static int url_ok(const char *u) {
  size_t n = strlen(u);
  if (n < 3 || n > URL_MAX || !isalpha((unsigned char)u[0])) return 0;
  int colon = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)u[i];
    if (c < 0x20 || c == 0x7f || c == ' ') return 0;
    if (c == ':') colon = 1;
    if (!colon && !isalnum(c) && c != '+' && c != '-' && c != '.') return 0;
  }
  /* "scheme:...", or a bare name like the console's own "psmediagallery" */
  return u[strlen(u) - 1] != ':';
}

int sysredirect_set(hh_t *h, const char *tid, const char *url, char *err, size_t errsz) {
  if (!sysredirect_is(tid)) {
    snprintf(err, errsz, "That tile's link can't be changed.");
    return -1;
  }
  if (!url_ok(url)) {
    snprintf(err, errsz, "Type a link like https://example.com or a console link (no spaces, up to %d characters).", URL_MAX);
    return -1;
  }
  char ppr[URL_MAX + 4], js[URL_MAX + 4], info[URL_MAX + 4];
  int hp, hj, hi;
  if (read_app(h, tid, ppr, sizeof(ppr), js, sizeof(js), &hp, &hj) != 0) {
    snprintf(err, errsz, "This console's database has no launch link for that tile, so it can't be changed.");
    return -1;
  }
  state_t s;
  if (state_load(tid, &s) != 0) { /* the first time: keep what the console has */
    read_info(tid, info, sizeof(info), &hi);
    enc(hp ? ppr : NULL, s.orig_ppr, sizeof(s.orig_ppr));
    enc(hj ? js : NULL, s.orig_json, sizeof(s.orig_json));
    enc(hi ? info : NULL, s.orig_info, sizeof(s.orig_info));
  }
  snprintf(s.url, sizeof(s.url), "%s", url);
  if (state_save(tid, &s)) {
    snprintf(err, errsz, "Couldn't save the original link: %s", strerror(errno));
    return -1;
  }
  if (write_all(h, tid, url, url, url, err, errsz)) return -1;
  hh_log("redirect %s: now opens %s", tid, url);
  return 0;
}

static const char *dec(const char *e) { return e[0] == '=' ? e + 1 : NULL; }

int sysredirect_reset(hh_t *h, const char *tid, char *err, size_t errsz) {
  state_t s;
  if (!sysredirect_is(tid) || state_load(tid, &s) != 0) return 0; /* nothing of ours */
  if (write_all(h, tid, dec(s.orig_ppr), dec(s.orig_json), dec(s.orig_info), err, errsz)) return -1;
  char p[400];
  state_path(tid, p, sizeof(p));
  unlink(p);
  hh_log("redirect %s: original link back", tid);
  return 0;
}

int sysredirect_reapply(hh_t *h) {
  int n = 0;
  for (int i = 0; i < NTILES; i++) {
    state_t s;
    char ppr[URL_MAX + 4], js[URL_MAX + 4], info[URL_MAX + 4], err[200];
    int hp, hj, hi;
    if (state_load(TILES[i].id, &s) != 0) continue;
    if (read_app(h, TILES[i].id, ppr, sizeof(ppr), js, sizeof(js), &hp, &hj) != 0) continue;
    read_info(TILES[i].id, info, sizeof(info), &hi);
    if (hp && hj && !strcmp(ppr, s.url) && !strcmp(js, s.url) && (hi || access(g_appinfo, F_OK) != 0) && !strcmp(info, hi ? s.url : info)) continue;
    if (write_all(h, TILES[i].id, s.url, s.url, s.url, err, sizeof(err)) == 0) {
      hh_log("redirect %s: the console had changed the link, ours is back", TILES[i].id);
      n++;
    }
  }
  return n;
}

void sysredirect_presets_json(strbuf_t *sb) {
  sb_puts(sb, "[");
  for (int i = 0; i < NPRESETS; i++) {
    if (i) sb_puts(sb, ",");
    sb_puts(sb, "{\"name\":");
    sb_json_str(sb, PRESETS[i].name);
    sb_puts(sb, ",\"url\":");
    sb_json_str(sb, PRESETS[i].url);
    sb_puts(sb, "}");
  }
  sb_puts(sb, "]");
}

void sysredirect_json(hh_t *h, strbuf_t *sb) {
  sb_puts(sb, "[");
  for (int i = 0; i < NTILES; i++) {
    char ppr[URL_MAX + 4], js[URL_MAX + 4];
    int hp, hj;
    int avail = read_app(h, TILES[i].id, ppr, sizeof(ppr), js, sizeof(js), &hp, &hj) == 0;
    state_t s;
    int custom = state_load(TILES[i].id, &s) == 0;
    if (i) sb_puts(sb, ",");
    sb_puts(sb, "{\"id\":");
    sb_json_str(sb, TILES[i].id);
    sb_puts(sb, ",\"name\":");
    sb_json_str(sb, TILES[i].name);
    sb_printf(sb, ",\"available\":%s,\"custom\":%s,\"url\":", avail ? "true" : "false", custom ? "true" : "false");
    sb_json_str(sb, custom ? s.url : (hp ? ppr : ""));
    sb_puts(sb, ",\"original\":");
    sb_json_str(sb, custom ? (dec(s.orig_ppr) ? dec(s.orig_ppr) : "") : "");
    sb_puts(sb, "}");
  }
  sb_puts(sb, "]");
}
