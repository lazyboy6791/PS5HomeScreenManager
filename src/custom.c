#include "custom.h"

#include <dirent.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "folders.h"
#include "icons.h"
#include "log.h"
#include "texture.h"

#define MAX_KEYS   8
#define MAX_CELLS  64
#define MAX_COLS   96

static struct {
  char data_dir[256];
  char appmeta_dir[256];
} C = {"/data/homescreenmanager", "/user/appmeta"};

void custom_init(const custom_cfg_t *cfg) {
  snprintf(C.data_dir, sizeof(C.data_dir), "%s", cfg->data_dir);
  snprintf(C.appmeta_dir, sizeof(C.appmeta_dir), "%s", cfg->appmeta_dir);
}

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void cerr(hh_t *h, const char *msg) {
  snprintf(h->err, sizeof(h->err), "%s", msg);
}

static sqlite3_stmt *prep(hh_t *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    snprintf(h->err, sizeof(h->err), "Database error: %s", sqlite3_errmsg(h->db));
    if (st) sqlite3_finalize(st);
    return NULL;
  }
  return st;
}

static void bind_texts(sqlite3_stmt *st, const char *a, const char *b, const char *c,
                       const char *d) {
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
  if (c) sqlite3_bind_text(st, 3, c, -1, SQLITE_TRANSIENT);
  if (d) sqlite3_bind_text(st, 4, d, -1, SQLITE_TRANSIENT);
}

/* Runs sql with up to four text parameters. Returns rows changed, -1 on error. */
static int run(hh_t *h, const char *sql, const char *a, const char *b, const char *c,
               const char *d) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return -1;
  bind_texts(st, a, b, c, d);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    snprintf(h->err, sizeof(h->err), "Database error: %s", sqlite3_errmsg(h->db));
    return -1;
  }
  return sqlite3_changes(h->db);
}

static int text1(hh_t *h, const char *sql, const char *a, char *out, size_t n) {
  out[0] = '\0';
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return -1;
  bind_texts(st, a, NULL, NULL, NULL);
  int found = 0;
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
    snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
    found = 1;
  }
  sqlite3_finalize(st);
  return found ? 0 : -1;
}

static int write_file(const char *path, const void *data, size_t len) {
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  ok = (fclose(f) == 0) && ok;
  if (!ok || rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  chmod(path, 0777);
  return 0;
}

static int copy_file(const char *from, const char *to) {
  FILE *f = fopen(from, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 16L * 1024 * 1024) {
    fclose(f);
    return -1;
  }
  unsigned char *b = malloc((size_t)sz);
  size_t got = b ? fread(b, 1, (size_t)sz, f) : 0;
  fclose(f);
  int rc = (b && got == (size_t)sz) ? write_file(to, b, got) : -1;
  free(b);
  return rc;
}

static void mkdirs(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  mkdir(tmp, 0777);
}

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* ------------------------------------------------------------------------- */
/* finding the cells that hold a title's name / picture                      */
/* ------------------------------------------------------------------------- */

typedef struct {
  char tbl[96];
  char col[64];
  char keycol[32];
  char keyval[96];
  char value[512];
} cell_t;

static int safe_ident(const char *s) {
  return s && s[0] && strlen(s) < 64 && !strchr(s, '"');
}

/* Concept ids of tid: rows keyed by concept id belong to it too. */
static int concept_ids(hh_t *h, const char *tid, char ids[][96], int max) {
  int n = 0;
  sqlite3_stmt *t = prep(h, "SELECT name FROM main.sqlite_master WHERE type='table' "
                            "AND (name LIKE 'tbl\\_iconinfo\\_%' ESCAPE '\\' OR name='tbl_contentinfo')");
  if (!t) return 0;
  while (sqlite3_step(t) == SQLITE_ROW && n < max) {
    const char *tbl = (const char *)sqlite3_column_text(t, 0);
    if (!safe_ident(tbl)) continue;
    static const char *const cols[] = {"localConceptId", "conceptId"};
    for (int c = 0; c < 2 && n < max; c++) {
      char sql[300];
      snprintf(sql, sizeof(sql), "SELECT DISTINCT \"%s\" FROM main.\"%s\" WHERE titleId=?1",
               cols[c], tbl);
      sqlite3_stmt *st = NULL;
      if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
        if (st) sqlite3_finalize(st);
        continue; /* no such column */
      }
      sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
      while (sqlite3_step(st) == SQLITE_ROW && n < max) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!v || !v[0] || strlen(v) >= 96 || !strcmp(v, "0") || !strcmp(v, "-1")) continue;
        int seen = 0;
        for (int i = 0; i < n; i++) seen |= !strcmp(ids[i], v);
        if (!seen) snprintf(ids[n++], 96, "%s", v);
      }
      sqlite3_finalize(st);
    }
  }
  sqlite3_finalize(t);

  /* a concept id shared by many tiles is a placeholder, not this game's
   * own: changing its rows would change those other tiles too */
  int kept = 0;
  for (int i = 0; i < n; i++) {
    long long most = 0;
    sqlite3_stmt *u = prep(h, "SELECT name FROM main.sqlite_master WHERE type='table' "
                              "AND name LIKE 'tbl\\_iconinfo\\_%' ESCAPE '\\'");
    while (u && sqlite3_step(u) == SQLITE_ROW) {
      const char *tbl = (const char *)sqlite3_column_text(u, 0);
      if (!safe_ident(tbl)) continue;
      char sql[300];
      snprintf(sql, sizeof(sql),
               "SELECT count(DISTINCT titleId) FROM main.\"%s\" WHERE localConceptId=?1", tbl);
      sqlite3_stmt *c = NULL;
      if (sqlite3_prepare_v2(h->db, sql, -1, &c, NULL) == SQLITE_OK) {
        sqlite3_bind_text(c, 1, ids[i], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(c) == SQLITE_ROW && sqlite3_column_int64(c, 0) > most)
          most = sqlite3_column_int64(c, 0);
      }
      if (c) sqlite3_finalize(c);
    }
    if (u) sqlite3_finalize(u);
    if (most <= 3) {  /* a game's PS4 and PS5 versions can share one */
      if (kept != i) memcpy(ids[kept], ids[i], 96);
      kept++;
    }
  }
  return kept;
}

static int is_name_col(const char *c) {
  size_t n = strlen(c);
  if (n >= 2 && !strcasecmp(c + n - 2, "id")) return 0;
  return strcasestr(c, "name") != NULL;
}

/* The columns that certainly hold a title's name. */
static int is_title_name_col(const char *c) {
  return !strcasecmp(c, "titleName") || !strcasecmp(c, "conceptName");
}

/* Every non-empty name (icon=0) or icon0Info (icon=1) value in rows that
 * belong to tid. */
#define K_NAME 0
#define K_ICON 1
#define K_BG   2

static int is_bg_col(const char *c) {
  return !strcasecmp(c, "pic0Info") || !strcasecmp(c, "pic1Info");
}

/* Where a tile's name, icon and background cells live. Looking through every table for them is
 * the slow part of finding a tile's cells, and it is the same for every tile, so it is done once
 * and kept until the database layout changes (its schema version). */
typedef struct {
  char tbl[96], col[64], keycol[24];
  int kind, has_tid;
} lay_t;
#define MAX_LAY 160
static lay_t g_lay[MAX_LAY];
static int g_lay_n = -1;
static long g_lay_schema = -1;
static pthread_mutex_t g_lay_mx = PTHREAD_MUTEX_INITIALIZER;

static long schema_version(hh_t *h) {
  sqlite3_stmt *st = prep(h, "PRAGMA main.schema_version");
  long v = -1;
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) v = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  return v;
}

static void build_layout(hh_t *h) { /* the caller holds g_lay_mx */
  g_lay_n = 0;
  sqlite3_stmt *t = prep(h, "SELECT name FROM main.sqlite_master WHERE type='table' "
                            "AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' "
                            "AND name NOT LIKE 'tbl\\_group\\_%' ESCAPE '\\' ORDER BY name");
  if (!t) return;
  while (sqlite3_step(t) == SQLITE_ROW) {
    const char *tn = (const char *)sqlite3_column_text(t, 0);
    if (!safe_ident(tn)) continue;
    char tbl[96];
    snprintf(tbl, sizeof(tbl), "%s", tn);
    char cols[MAX_COLS][64];
    int ncols = 0, has_tid = 0, has_lcid = 0, has_cid = 0;
    sqlite3_stmt *pi = NULL;
    if (sqlite3_prepare_v2(h->db, "SELECT name FROM pragma_table_info(?1, 'main')", -1, &pi, NULL) != SQLITE_OK) {
      if (pi) sqlite3_finalize(pi);
      continue;
    }
    sqlite3_bind_text(pi, 1, tbl, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(pi) == SQLITE_ROW && ncols < MAX_COLS) {
      const char *c = (const char *)sqlite3_column_text(pi, 0);
      if (!safe_ident(c)) continue;
      if (!strcmp(c, "titleId")) has_tid = 1;
      else if (!strcmp(c, "localConceptId")) has_lcid = 1;
      else if (!strcmp(c, "conceptId")) has_cid = 1;
      snprintf(cols[ncols++], 64, "%s", c);
    }
    sqlite3_finalize(pi);
    const char *keycol = has_tid ? "titleId" : has_lcid ? "localConceptId" : has_cid ? "conceptId" : NULL;
    if (!keycol) continue;
    for (int c = 0; c < ncols && g_lay_n < MAX_LAY; c++) {
      int kind = !strcasecmp(cols[c], "icon0Info") ? K_ICON : is_bg_col(cols[c]) ? K_BG : is_name_col(cols[c]) ? K_NAME : -1;
      if (kind < 0) continue;
      lay_t *L = &g_lay[g_lay_n++];
      snprintf(L->tbl, sizeof(L->tbl), "%s", tbl);
      snprintf(L->col, sizeof(L->col), "%s", cols[c]);
      snprintf(L->keycol, sizeof(L->keycol), "%s", keycol);
      L->kind = kind;
      L->has_tid = has_tid;
    }
  }
  sqlite3_finalize(t);
}

static int layout_for(hh_t *h, int kind, lay_t *out, int max) {
  pthread_mutex_lock(&g_lay_mx);
  long sv = schema_version(h);
  if (g_lay_n < 0 || sv < 0 || sv != g_lay_schema) {
    build_layout(h);
    g_lay_schema = sv;
  }
  int n = 0;
  for (int i = 0; i < g_lay_n && n < max; i++)
    if (g_lay[i].kind == kind) out[n++] = g_lay[i];
  pthread_mutex_unlock(&g_lay_mx);
  return n;
}

static int find_cells(hh_t *h, const char *tid, int kind, cell_t *out, int max) {
  char cids[MAX_KEYS][96];
  int ncid = concept_ids(h, tid, cids, MAX_KEYS);
  lay_t lay[MAX_LAY];
  int nl = layout_for(h, kind, lay, MAX_LAY);
  int n = 0;
  for (int li = 0; li < nl && n < max; li++) {
    const lay_t *L = &lay[li];
    const char *keys[MAX_KEYS];
    int nkeys = 0;
    if (L->has_tid) keys[nkeys++] = tid;
    else
      for (int i = 0; i < ncid; i++) keys[nkeys++] = cids[i];
    char sql[400];
    snprintf(sql, sizeof(sql), "SELECT DISTINCT \"%s\" FROM main.\"%s\" WHERE \"%s\"=?1", L->col, L->tbl, L->keycol);
    for (int k = 0; k < nkeys && n < max; k++) {
      sqlite3_stmt *st = prep(h, sql);
      if (!st) continue;
      sqlite3_bind_text(st, 1, keys[k], -1, SQLITE_TRANSIENT);
      while (sqlite3_step(st) == SQLITE_ROW && n < max) {
        if (sqlite3_column_type(st, 0) != SQLITE_TEXT) continue;
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!v || !v[0] || strlen(v) >= sizeof(out[n].value)) continue;
        cell_t *ce = &out[n++];
        snprintf(ce->tbl, sizeof(ce->tbl), "%s", L->tbl);
        snprintf(ce->col, sizeof(ce->col), "%s", L->col);
        snprintf(ce->keycol, sizeof(ce->keycol), "%s", L->keycol);
        snprintf(ce->keyval, sizeof(ce->keyval), "%s", keys[k]);
        snprintf(ce->value, sizeof(ce->value), "%s", v);
      }
      sqlite3_finalize(st);
    }
  }
  return n;
}

static int set_cell(hh_t *h, const cell_t *c, const char *from, const char *to) {
  char sql[400];
  snprintf(sql, sizeof(sql), "UPDATE main.\"%s\" SET \"%s\"=?1 WHERE \"%s\"=?2 AND \"%s\"=?3",
           c->tbl, c->col, c->keycol, c->col);
  return run(h, sql, to, c->keyval, from, NULL);
}

/* ------------------------------------------------------------------------- */
/* names                                                                     */
/* ------------------------------------------------------------------------- */

static int clean_name(const char *in, char *out, size_t n) {
  while (*in == ' ' || *in == '\t') in++;
  size_t len = strlen(in);
  while (len && (in[len - 1] == ' ' || in[len - 1] == '\t')) len--;
  if (len >= n || len > CUSTOM_NAME_MAX) return -1;
  for (size_t i = 0; i < len; i++)
    if ((unsigned char)in[i] < 0x20 || in[i] == 0x7f) return -1;
  memcpy(out, in, len);
  out[len] = '\0';
  return 0;
}

#define NAMESET_MAX 24

/* Remembers a cell's original value (replace=1: overwrite a saved one, used
 * when the system changed the value since). */
static int save_orig(hh_t *h, const char *tid, const char *kind, const cell_t *c,
                     const char *orig, int replace) {
  sqlite3_stmt *st = prep(h, replace
      ? "INSERT OR REPLACE INTO hh.custom_cells(title_id, kind, tbl, col, keycol, keyval, orig) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)"
      : "INSERT OR IGNORE INTO hh.custom_cells(title_id, kind, tbl, col, keycol, keyval, orig) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
  if (!st) return -1;
  const char *v[7] = {tid, kind, c->tbl, c->col, c->keycol, c->keyval, orig};
  for (int i = 0; i < 7; i++) sqlite3_bind_text(st, i + 1, v[i], -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

/* Changes every name cell of tid that holds one of its names (current,
 * original or an earlier custom one) to name. *changed = cells changed.
 * Returns -1 if no name cell was found. */
static int apply_name(hh_t *h, const char *tid, const char *name, int *changed) {
  *changed = 0;
  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  char(*set)[512] = calloc(NAMESET_MAX, 512);
  if (!cells || !set) {
    free(cells);
    free(set);
    cerr(h, "Out of memory.");
    return -1;
  }
  int n = find_cells(h, tid, K_NAME, cells, MAX_CELLS);

  /* this title's names: titleName/conceptName values and saved originals */
  int ns = 0;
  for (int i = 0; i < n && ns < NAMESET_MAX; i++)
    if (is_title_name_col(cells[i].col)) snprintf(set[ns++], 512, "%s", cells[i].value);
  char prev[256] = "";
  text1(h, "SELECT name FROM hh.custom WHERE title_id=?1", tid, prev, sizeof(prev));
  if (prev[0] && ns < NAMESET_MAX) snprintf(set[ns++], 512, "%s", prev);
  sqlite3_stmt *st = prep(h, "SELECT DISTINCT orig FROM hh.custom_cells WHERE title_id=?1 AND kind='name'");
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && ns < NAMESET_MAX)
      if (sqlite3_column_text(st, 0)) snprintf(set[ns++], 512, "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }

  int matched = 0, rc = 0;
  for (int i = 0; i < n && rc == 0; i++) {
    int in = 0;
    for (int k = 0; k < ns; k++) in |= !strcmp(set[k], cells[i].value);
    if (!in) continue;
    matched++;
    if (!strcmp(cells[i].value, name)) continue;
    /* an earlier custom name isn't an original; anything else is */
    if (!(prev[0] && !strcmp(cells[i].value, prev)) &&
        save_orig(h, tid, "name", &cells[i], cells[i].value, 0)) {
      rc = -1;
      break;
    }
    int ch = set_cell(h, &cells[i], cells[i].value, name);
    if (ch < 0) rc = -1;
    else *changed += ch;
  }
  free(cells);
  free(set);
  if (rc) return -1;
  if (!matched) {
    cerr(h, "Home Screen Manager couldn't find this tile's name in the home screen database, so it can't be renamed.");
    return -1;
  }
  return 0;
}

static void drop_empty(hh_t *h, const char *tid) {
  run(h, "DELETE FROM hh.custom WHERE title_id=?1 AND name IS NULL AND icon_file IS NULL "
         "AND bg_file IS NULL",
      tid, NULL, NULL, NULL);
}

static int can_customize(hh_t *h, const char *tid) {
  if (!hh_valid_tid(tid)) {
    cerr(h, "Missing or invalid title id.");
    return 0;
  }
  if (folders_is_folder(tid)) {
    cerr(h, "This is a folder. Use Edit folder to change its name and picture.");
    return 0;
  }
  if (h->nusers == 0) {
    cerr(h, "This firmware's home screen database has a layout Home Screen Manager doesn't know yet.");
    return 0;
  }
  return 1;
}

static int reset_name(hh_t *h, const char *tid) {
  char cur[256] = "";
  if (text1(h, "SELECT name FROM hh.custom WHERE title_id=?1", tid, cur, sizeof(cur))) return 0;
  sqlite3_stmt *st = prep(h, "SELECT tbl, col, keycol, keyval, orig FROM hh.custom_cells "
                             "WHERE title_id=?1 AND kind='name'");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  cell_t c;
  int rc = 0;
  while (rc == 0 && sqlite3_step(st) == SQLITE_ROW) {
    snprintf(c.tbl, sizeof(c.tbl), "%s", sqlite3_column_text(st, 0));
    snprintf(c.col, sizeof(c.col), "%s", sqlite3_column_text(st, 1));
    snprintf(c.keycol, sizeof(c.keycol), "%s", sqlite3_column_text(st, 2));
    snprintf(c.keyval, sizeof(c.keyval), "%s", sqlite3_column_text(st, 3));
    const char *orig = (const char *)sqlite3_column_text(st, 4);
    if (!safe_ident(c.tbl) || !safe_ident(c.col) || !safe_ident(c.keycol) || !orig) continue;
    if (set_cell(h, &c, cur, orig) < 0) rc = -1;
  }
  sqlite3_finalize(st);
  if (rc) return -1;
  if (run(h, "DELETE FROM hh.custom_cells WHERE title_id=?1 AND kind='name'", tid, NULL, NULL, NULL) < 0 ||
      run(h, "UPDATE hh.custom SET name=NULL, name_src=NULL, name_before=NULL WHERE title_id=?1", tid, NULL, NULL, NULL) < 0)
    return -1;
  drop_empty(h, tid);
  return 0;
}

/* tool = the name comes from Clean Up Game Names (and not from the tile editor, by hand) */
static int set_name_impl(hh_t *h, const char *tid, const char *name_in, int tool) {
  if (!can_customize(h, tid)) return -1;
  char name[CUSTOM_NAME_MAX * 4 + 8] = "";
  if (name_in && clean_name(name_in, name, sizeof(name))) {
    cerr(h, "Use a name of up to 64 characters.");
    return -1;
  }
  char orig[256] = "";
  if (text1(h, "SELECT orig_name FROM hh.custom WHERE title_id=?1", tid, orig, sizeof(orig)))
    hh_title_name(h, tid, orig, sizeof(orig));
  /* the name before the tool: a name you gave by hand is kept, so undoing the tool gives it back */
  char cur[256] = "", cursrc[16] = "", before[256] = "";
  text1(h, "SELECT name FROM hh.custom WHERE title_id=?1", tid, cur, sizeof(cur));
  text1(h, "SELECT name_src FROM hh.custom WHERE title_id=?1", tid, cursrc, sizeof(cursrc));
  text1(h, "SELECT name_before FROM hh.custom WHERE title_id=?1", tid, before, sizeof(before));
  const char *new_before = NULL;
  if (tool) new_before = (cur[0] && strcmp(cursrc, "tool")) ? cur : (!strcmp(cursrc, "tool") && before[0] ? before : NULL);

  if (run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0) return -1;
  int rc, changed = 0;
  if (!name[0] || !strcmp(name, orig)) {
    rc = reset_name(h, tid);
  } else {
    rc = apply_name(h, tid, name, &changed);
    if (rc == 0 &&
        run(h, "INSERT INTO hh.custom(title_id, name, orig_name, updated_at) "
               "VALUES(?1, ?2, ?3, datetime('now')) ON CONFLICT(title_id) DO UPDATE SET "
               "name=excluded.name, orig_name=coalesce(hh.custom.orig_name, excluded.orig_name), "
               "updated_at=excluded.updated_at",
            tid, name, orig, NULL) < 0)
      rc = -1;
    if (rc == 0 && run(h, "UPDATE hh.custom SET name_src=?2, name_before=?3 WHERE title_id=?1", tid, tool ? "tool" : NULL,
                       tool ? new_before : NULL, NULL) < 0)
      rc = -1;
  }
  if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) rc = -1;
  if (rc) {
    char keep[512];
    snprintf(keep, sizeof(keep), "%s", h->err);
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(h->err, sizeof(h->err), "%s", keep);
    return -1;
  }
  hh_log("rename %s: %s (%d value(s) changed)%s", tid, name[0] && strcmp(name, orig) ? name : "original name", changed, tool ? " [cleanup]" : "");
  return 0;
}

int custom_set_name(hh_t *h, const char *tid, const char *name_in) { return set_name_impl(h, tid, name_in, 0); }
int custom_set_name_tool(hh_t *h, const char *tid, const char *name_in) { return set_name_impl(h, tid, name_in, 1); }

/* ------------------------------------------------------------------------- */
/* pictures                                                                  */
/* ------------------------------------------------------------------------- */

static void copy_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/icons/%s.png", C.data_dir, tid);
}

/* Points every icon0Info cell of tid at meta. prev = our previous file (its
 * cells already have their original saved). */
static int point_icons(hh_t *h, const char *tid, const char *meta, const char *prev,
                       int *changed, int *found) {
  *changed = 0;
  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  if (!cells) return -1;
  int n = find_cells(h, tid, K_ICON, cells, MAX_CELLS);
  *found = n;
  int rc = 0;
  for (int i = 0; i < n && rc == 0; i++) {
    if (!strcmp(cells[i].value, meta)) continue;
    int ours = prev && prev[0] && !strcmp(cells[i].value, prev);
    /* anything that isn't ours is the latest original (the system may have
     * changed it since we saved one) */
    if (!ours && save_orig(h, tid, "icon", &cells[i], cells[i].value, 1)) rc = -1;
    int ch = rc ? -1 : set_cell(h, &cells[i], cells[i].value, meta);
    if (ch < 0) rc = -1;
    else *changed += ch;
  }
  free(cells);
  return rc;
}

static int reset_icon(hh_t *h, const char *tid) {
  char meta[512] = "", copy[400];
  if (text1(h, "SELECT icon_meta FROM hh.custom WHERE title_id=?1", tid, meta, sizeof(meta))) return 0;
  sqlite3_stmt *st = prep(h, "SELECT tbl, col, keycol, keyval, orig FROM hh.custom_cells "
                             "WHERE title_id=?1 AND kind='icon'");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  cell_t c;
  int rc = 0;
  while (rc == 0 && sqlite3_step(st) == SQLITE_ROW) {
    snprintf(c.tbl, sizeof(c.tbl), "%s", sqlite3_column_text(st, 0));
    snprintf(c.col, sizeof(c.col), "%s", sqlite3_column_text(st, 1));
    snprintf(c.keycol, sizeof(c.keycol), "%s", sqlite3_column_text(st, 2));
    snprintf(c.keyval, sizeof(c.keyval), "%s", sqlite3_column_text(st, 3));
    const char *orig = (const char *)sqlite3_column_text(st, 4);
    if (!safe_ident(c.tbl) || !safe_ident(c.col) || !safe_ident(c.keycol) || !orig) continue;
    if (set_cell(h, &c, meta, orig) < 0) rc = -1;
  }
  sqlite3_finalize(st);
  if (rc) return -1;
  if (run(h, "DELETE FROM hh.custom_cells WHERE title_id=?1 AND kind='icon'", tid, NULL, NULL, NULL) < 0 ||
      run(h, "UPDATE hh.custom SET icon_file=NULL, icon_meta=NULL WHERE title_id=?1", tid, NULL, NULL, NULL) < 0)
    return -1;
  drop_empty(h, tid);
  unlink(meta);
  copy_path(tid, copy, sizeof(copy));
  unlink(copy);
  return 0;
}

int custom_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len, int clear) {
  if (!can_customize(h, tid)) return -1;
  if (!img) {
    if (run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0) return -1;
    if (reset_icon(h, tid) || run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) {
      sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
      return -1;
    }
    hh_log("picture %s: original picture", tid);
    return 0;
  }

  unsigned char *png;
  size_t pl;
  if (icon_from_image(img, len, clear ? "none" : "dark", &png, &pl)) {
    cerr(h, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).");
    return -1;
  }
  char prev[512] = "", meta[600], dir[400], copy[400];
  text1(h, "SELECT icon_meta FROM hh.custom WHERE title_id=?1", tid, prev, sizeof(prev));
  snprintf(dir, sizeof(dir), "%s/%s", C.appmeta_dir, tid);
  mkdirs(dir);
  /* a new file name each time: the home screen caches pictures by path */
  long stamp = (long)time(NULL);
  do {
    snprintf(meta, sizeof(meta), "%s/hh_icon0_%ld.png", dir, stamp++);
  } while (!strcmp(meta, prev) || file_exists(meta));
  char cdir[400];
  snprintf(cdir, sizeof(cdir), "%s/icons", C.data_dir);
  mkdirs(cdir);
  copy_path(tid, copy, sizeof(copy));
  int wrc = write_file(meta, png, pl) || write_file(copy, png, pl);
  free(png);
  if (wrc) {
    unlink(meta);
    cerr(h, "Couldn't save the picture on the console.");
    return -1;
  }

  int changed = 0, found = 0, rc = run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0 ? -1 : 0;
  if (rc == 0) rc = point_icons(h, tid, meta, prev, &changed, &found);
  if (rc == 0 && found == 0) {
    cerr(h, "This tile's picture isn't stored in the home screen database, so Home Screen Manager can't change it.");
    rc = -1;
  }
  char orig[256] = "";
  hh_title_name(h, tid, orig, sizeof(orig));
  if (rc == 0 &&
      run(h, "INSERT INTO hh.custom(title_id, icon_file, icon_meta, orig_name, updated_at) "
             "VALUES(?1, ?2, ?3, ?4, datetime('now')) ON CONFLICT(title_id) DO UPDATE SET "
             "icon_file=excluded.icon_file, icon_meta=excluded.icon_meta, "
             "orig_name=coalesce(hh.custom.orig_name, excluded.orig_name), updated_at=excluded.updated_at",
          tid, copy, meta, orig) < 0)
    rc = -1;
  if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) rc = -1;
  if (rc) {
    char keep[512];
    snprintf(keep, sizeof(keep), "%s", h->err);
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(h->err, sizeof(h->err), "%s", keep);
    unlink(meta);
    if (!prev[0]) unlink(copy);
    return -1;
  }
  if (prev[0]) unlink(prev);
  hh_log("picture %s: %s (%d value(s) changed)", tid, meta, changed);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* backgrounds                                                               */
/* ------------------------------------------------------------------------- */

static void bg_prefix(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/%s/hh_bg_", C.appmeta_dir, tid);
}

/* The experimental "share one file between games": one converted file per picture and format, in a
 * folder of its own, and every game's background entry points at it. */
static void bg_shared_dir(char *out, size_t n) { snprintf(out, n, "%s/hsm_shared", C.appmeta_dir); }
static int bg_is_shared(const char *v) {
  char sh[420];
  snprintf(sh, sizeof(sh), "%s/hsm_shared/", C.appmeta_dir);
  return !strncmp(v, sh, strlen(sh));
}
/* a background cell that holds a file of ours: this tile's own, or a shared one */
/* An "original" that points at one of our own files is not an original (an older build could save
 * one when two games shared an entry). The first backup, taken before we changed anything, still has
 * the console's own value: use that instead. Returns 1 and fills out if the backup has a usable one. */
static int backup_orig(const cell_t *c, char *out, size_t n) {
  char path[500];
  snprintf(path, sizeof(path), "%s/backup/app.db.original", C.data_dir);
  if (!file_exists(path)) return 0;
  sqlite3 *db = NULL;
  int ok = 0;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
    char sql[400];
    snprintf(sql, sizeof(sql), "SELECT \"%s\" FROM \"%s\" WHERE \"%s\"=?1 LIMIT 1", c->col, c->tbl, c->keycol);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, c->keyval, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_TEXT) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v && v[0] && !strstr(v, "/hh_bg_") && !strstr(v, "/hsm_shared/")) {
          snprintf(out, n, "%s", v);
          ok = 1;
        }
      }
      sqlite3_finalize(st);
    }
  }
  if (db) sqlite3_close(db);
  return ok;
}

/* Ours: this tile's own file, a shared file, or the file of ANOTHER tile of ours. Several games can
 * share one concept (one set of background cells); a file one of them wrote there is not "the
 * console's own" for the others, and must never be saved as their original. */
static int bg_is_ours(const char *v, const char *pre) {
  if (!strncmp(v, pre, strlen(pre)) || bg_is_shared(v)) return 1;
  /* pre is "<appmeta>/<TITLE>/hh_bg_": the other tiles' files start with the same <appmeta>/ */
  const char *last = strrchr(pre, '/');
  const char *prev = NULL;
  for (const char *c = pre; last && c < last; c++)
    if (*c == '/') prev = c;
  if (!prev) return 0;
  size_t base = (size_t)(prev - pre) + 1;
  return !strncmp(v, pre, base) && strstr(v + base, "/hh_bg_") != NULL;
}

static unsigned crc32_buf(const unsigned char *b, size_t n) {
  static unsigned tab[256];
  static int ready;
  if (!ready) {
    for (unsigned i = 0; i < 256; i++) {
      unsigned c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      tab[i] = c;
    }
    ready = 1;
  }
  unsigned c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
/* the crc of the same picture is asked for once per game: remember the last answer */
static unsigned crc32_cached(const unsigned char *b, size_t n) {
  static const unsigned char *last_b;
  static size_t last_n;
  static unsigned last_c;
  if (b == last_b && n == last_n && b) return last_c;
  last_c = crc32_buf(b, n);
  last_b = b;
  last_n = n;
  return last_c;
}

static void bg_source(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/backgrounds/%s.jpg", C.data_dir, tid);
}

/* Original value saved for a cell, "" if none. */
static void saved_orig(hh_t *h, const char *tid, const char *kind, const cell_t *c, char *out, size_t n) {
  out[0] = '\0';
  sqlite3_stmt *st = prep(h, "SELECT orig FROM hh.custom_cells WHERE title_id=?1 AND kind=?2 AND tbl=?3 "
                             "AND col=?4 AND keycol=?5 AND keyval=?6");
  if (!st) return;
  const char *v[6] = {tid, kind, c->tbl, c->col, c->keycol, c->keyval};
  for (int i = 0; i < 6; i++) sqlite3_bind_text(st, i + 1, v[i], -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
    snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
}

/* Like saved_orig, but says whether an original was saved at all, and whether it was NULL. */
static int saved_orig_row(hh_t *h, const char *tid, const char *kind, const cell_t *c, char *out, size_t n, int *is_null) {
  out[0] = '\0';
  *is_null = 0;
  int found = 0;
  sqlite3_stmt *st = prep(h, "SELECT orig FROM hh.custom_cells WHERE title_id=?1 AND kind=?2 AND tbl=?3 "
                             "AND col=?4 AND keycol=?5 AND keyval=?6");
  if (!st) return 0;
  const char *v[6] = {tid, kind, c->tbl, c->col, c->keycol, c->keyval};
  for (int i = 0; i < 6; i++) sqlite3_bind_text(st, i + 1, v[i], -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    found = 1;
    if (sqlite3_column_type(st, 0) == SQLITE_NULL) *is_null = 1;
    else snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  return found;
}

/* Removes our background files for tid except the ones in keep. */
static void bg_cleanup(const char *tid, char keep[][600], int nkeep) {
  char dir[400], pre[64];
  snprintf(dir, sizeof(dir), "%s/%s", C.appmeta_dir, tid);
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  snprintf(pre, sizeof(pre), "hh_bg_");
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, pre, strlen(pre))) continue;
    char p[700];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    int kept = 0;
    for (int i = 0; i < nkeep; i++) kept |= !strcmp(keep[i], p);
    if (!kept) unlink(p);
  }
  closedir(d);
}

#define BG_FILES 4

/* Writes new background files (one per format the originals use) and
 * points every pic0Info/pic1Info cell of tid at them. The files are made
 * before the database is touched, so the home screen isn't held up. */
/* A bulk run converts the picture once for each file format and links that file into every tile
 * (a link takes no copying time and no extra space; if the drive can't link, the file is copied). */
#define BG_CACHE 8
static int g_bulk = 0, g_share = 0;
static long g_bulk_shared = 0;
static struct {
  tex_fmt_t f;
  char master[600];
  int used;
} g_bgc[BG_CACHE];
static long g_bulk_encode_ms = 0, g_bulk_reused = 0, g_bulk_linked = 0, g_bulk_copied = 0;

static long cb_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

void custom_bulk_begin(int share) {
  g_bulk = 1;
  g_share = share ? 1 : 0;
  g_bulk_shared = 0;
  memset(g_bgc, 0, sizeof(g_bgc));
  g_bulk_encode_ms = 0;
  g_bulk_reused = 0;
  g_bulk_linked = 0;
  g_bulk_copied = 0;
}

void custom_bulk_end(long *encode_ms, long *reused, long *linked, long *copied, long *shared) {
  if (shared) *shared = g_bulk_shared;
  g_share = 0;
  if (encode_ms) *encode_ms = g_bulk_encode_ms;
  if (reused) *reused = g_bulk_reused;
  if (linked) *linked = g_bulk_linked;
  if (copied) *copied = g_bulk_copied;
  g_bulk = 0;
  memset(g_bgc, 0, sizeof(g_bgc));
}

static int apply_bg_locked(hh_t *h, const char *tid, const unsigned char *img, size_t len, int *changed) {
  *changed = 0;
  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  if (!cells) return -1;
  int n = find_cells(h, tid, K_BG, cells, MAX_CELLS);
  if (!n) {
    free(cells);
    cerr(h, "This tile has no background in the home screen database, so Home Screen Manager can't change it.");
    return -1;
  }
  char pre[400], shdir[400];
  bg_prefix(tid, pre, sizeof(pre));
  bg_shared_dir(shdir, sizeof(shdir));
  int share = g_share;
  for (int i = 0; i < n; i++)
    if (bg_is_shared(cells[i].value)) share = 1; /* a tile that is already sharing keeps sharing */
  unsigned crc = share ? crc32_cached(img, len) : 0;

  /* the original each cell had, and the format to write for it */
  char (*orig)[512] = calloc((size_t)n, 512);
  int *which = calloc((size_t)n, sizeof(int));
  tex_fmt_t fmts[BG_FILES];
  char files[BG_FILES][600];
  int nf = 0, rc = 0;
  if (!orig || !which) rc = -1;
  long stamp = (long)time(NULL);
  for (;; stamp++) { /* a name not used before, so no cached copy is shown */
    char t[3][600];
    snprintf(t[0], 600, "%s%ld_0.dds", pre, stamp);
    snprintf(t[1], 600, "%s%ld_0.png", pre, stamp);
    snprintf(t[2], 600, "%s%ld_0.jpg", pre, stamp);
    if (!file_exists(t[0]) && !file_exists(t[1]) && !file_exists(t[2])) break;
  }
  for (int i = 0; i < n && rc == 0; i++) {
    if (bg_is_ours(cells[i].value, pre)) {
      saved_orig(h, tid, "bg", &cells[i], orig[i], 512);
      if (orig[i][0] && (strstr(orig[i], "/hh_bg_") || strstr(orig[i], "/hsm_shared/"))) {
        char good[512];
        if (backup_orig(&cells[i], good, sizeof(good))) {
          hh_log("background %s: the saved original of %s.%s was one of our own files; using the first backup's value instead", tid, cells[i].tbl, cells[i].col);
          snprintf(orig[i], 512, "%s", good);
          save_orig(h, tid, "bg", &cells[i], good, 1);
        } else orig[i][0] = '\0';
      }
      if (!orig[i][0] && strncmp(cells[i].value, pre, strlen(pre)) && !bg_is_shared(cells[i].value)) {
        /* the cell holds the file of another game that uses the same concept: the original it
           saved is this game's original too */
        sqlite3_stmt *st = prep(h, "SELECT orig FROM hh.custom_cells WHERE kind='bg' AND tbl=?1 AND col=?2 AND keycol=?3 "
                                   "AND keyval=?4 AND orig IS NOT NULL AND instr(orig, '/hh_bg_')=0 LIMIT 1");
        if (st) {
          const char *v[4] = {cells[i].tbl, cells[i].col, cells[i].keycol, cells[i].keyval};
          for (int q = 0; q < 4; q++) sqlite3_bind_text(st, q + 1, v[q], -1, SQLITE_TRANSIENT);
          if (sqlite3_step(st) == SQLITE_ROW) snprintf(orig[i], 512, "%s", (const char *)sqlite3_column_text(st, 0));
          sqlite3_finalize(st);
        }
        if (orig[i][0]) save_orig(h, tid, "bg", &cells[i], orig[i], 0);
      }
    } else snprintf(orig[i], 512, "%s", cells[i].value);
    which[i] = -1;
    tex_fmt_t f;
    if (!orig[i][0] || tex_probe(orig[i], &f)) continue; /* not a picture path: leave it */
    char desc[96];
    tex_describe(&f, desc, sizeof(desc));
    if (!tex_supported(&f)) {
      hh_log("background %s: %s uses %s", tid, orig[i], desc);
      snprintf(h->err, sizeof(h->err), "This background uses a picture format Home Screen Manager can't write yet (%s).", desc);
      rc = -1;
      break;
    }
    int k;
    for (k = 0; k < nf; k++)
      if (!memcmp(&fmts[k], &f, sizeof(f))) break;
    if (k == nf) {
      if (nf == BG_FILES) continue;
      fmts[nf] = f;
      if (share) {
        char d[96];
        tex_describe(&f, d, sizeof(d));
        for (char *q = d; *q; q++)
          if (!isalnum((unsigned char)*q)) *q = '_';
        snprintf(files[nf], sizeof(files[nf]), "%s/bg_%08x_%s.%s", shdir, crc, d, tex_ext(&f));
      } else {
        snprintf(files[nf], sizeof(files[nf]), "%s%ld_%d.%s", pre, stamp, nf, tex_ext(&f));
      }
      nf++;
    }
    which[i] = k;
  }
  if (rc == 0 && nf == 0) {
    cerr(h, "This tile's background isn't a picture file Home Screen Manager can replace.");
    rc = -1;
  }

  /* make the files */
  if (rc == 0) {
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/%s", C.appmeta_dir, tid);
    mkdirs(dir);
    for (int k = 0; k < nf && rc == 0; k++) {
      if (share) {
        if (file_exists(files[k])) continue; /* the shared file is there already: nothing to make */
        mkdirs(shdir);
      }
      if (g_bulk && !share) { /* made for an earlier tile in this run: link to that file */
        int c;
        for (c = 0; c < BG_CACHE; c++)
          if (g_bgc[c].used && !memcmp(&g_bgc[c].f, &fmts[k], sizeof(tex_fmt_t)) && file_exists(g_bgc[c].master)) break;
        int how = c < BG_CACHE ? (link(g_bgc[c].master, files[k]) == 0 ? 1 : (copy_file(g_bgc[c].master, files[k]) == 0 ? 2 : 0)) : 0;
        if (how) {
          g_bulk_reused++;
          if (how == 1) g_bulk_linked++;
          else g_bulk_copied++;
          hh_log("background %s: reused the converted file (%s, %s)", tid, files[k], how == 1 ? "linked" : "copied");
          continue;
        }
      }
      long t_enc = cb_now_ms();
      unsigned char *out;
      size_t ol;
      if (tex_encode(img, len, &fmts[k], &out, &ol)) {
        cerr(h, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).");
        rc = -1;
        break;
      }
      if (write_file(files[k], out, ol)) {
        cerr(h, "Couldn't save the background on the console.");
        rc = -1;
      }
      char desc[96];
      tex_describe(&fmts[k], desc, sizeof(desc));
      hh_log("background %s: wrote %s (%s, %zu bytes)", tid, files[k], desc, ol);
      free(out);
      if (g_bulk && !share && rc == 0) {
        g_bulk_encode_ms += cb_now_ms() - t_enc;
        for (int c = 0; c < BG_CACHE; c++)
          if (!g_bgc[c].used) {
            g_bgc[c].f = fmts[k];
            snprintf(g_bgc[c].master, sizeof(g_bgc[c].master), "%.590s", files[k]);
            g_bgc[c].used = 1;
            break;
          }
      }
    }
  }

  /* point the cells at them */
  if (rc == 0 && run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0) rc = -1;
  if (rc == 0) {
    for (int i = 0; i < n && rc == 0; i++) {
      if (which[i] < 0 || !strcmp(cells[i].value, files[which[i]])) continue;
      int ours = bg_is_ours(cells[i].value, pre);
      if (!ours && save_orig(h, tid, "bg", &cells[i], cells[i].value, 1)) rc = -1;
      int ch = rc ? -1 : set_cell(h, &cells[i], cells[i].value, files[which[i]]);
      if (ch < 0) rc = -1;
      else *changed += ch;
    }
    if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) rc = -1;
    if (rc) {
      char keep[512];
      snprintf(keep, sizeof(keep), "%s", h->err);
      sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
      snprintf(h->err, sizeof(h->err), "%s", keep);
    }
  }
  if (rc == 0) {
    bg_cleanup(tid, files, nf); /* earlier backgrounds of ours */
    if (share) g_bulk_shared++;
  } else if (!share) {
    for (int k = 0; k < nf; k++) unlink(files[k]);
  }
  free(cells);
  free(orig);
  free(which);
  return rc;
}

/* Only one background change runs at a time. Two at once on the same tile could delete each
 * other's files (the repair and a bulk run did that once). */
static pthread_mutex_t g_apply_mx = PTHREAD_MUTEX_INITIALIZER;
static int apply_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len, int *changed) {
  pthread_mutex_lock(&g_apply_mx);
  int rc = apply_bg_locked(h, tid, img, len, changed);
  pthread_mutex_unlock(&g_apply_mx);
  return rc;
}

/* Removes the shared files (and shared source pictures) that no game uses any more. */
static int cell_refs(hh_t *h, const char *path, const lay_t *lay, int nl) {
  int found = 0;
  for (int i = 0; i < nl && !found; i++) {
    char sql[300];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM main.\"%s\" WHERE \"%s\"=?1 LIMIT 1", lay[i].tbl, lay[i].col);
    sqlite3_stmt *st = prep(h, sql);
    if (!st) continue;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
  }
  return found;
}

static int g_gc_defer;
void custom_gc_defer(int on) { g_gc_defer = on; }

static void shared_gc(hh_t *h) {
  char dir[420];
  /* The layout copy is about 30 KB: kept on the heap, once, not on the stack of a worker thread. */
  lay_t *lay = malloc(sizeof(lay_t) * MAX_LAY);
  int nl = lay ? layout_for(h, K_BG, lay, MAX_LAY) : 0;
  bg_shared_dir(dir, sizeof(dir));
  DIR *d = lay ? opendir(dir) : NULL;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (strncmp(e->d_name, "bg_", 3)) continue;
      char p[700];
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      if (!cell_refs(h, p, lay, nl)) unlink(p);
    }
    closedir(d);
  }
  snprintf(dir, sizeof(dir), "%s/backgrounds", C.data_dir);
  free(lay);
  d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "shared_", 7)) continue;
    char p[700], used[8] = "";
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    sqlite3_stmt *st = prep(h, "SELECT 1 FROM hh.custom WHERE bg_file=?1 LIMIT 1");
    int in_use = 0;
    if (st) {
      sqlite3_bind_text(st, 1, p, -1, SQLITE_TRANSIENT);
      in_use = sqlite3_step(st) == SQLITE_ROW;
      sqlite3_finalize(st);
    }
    (void)used;
    if (!in_use) unlink(p);
  }
  closedir(d);
}

void custom_shared_gc(hh_t *h) { shared_gc(h); }

static int reset_bg(hh_t *h, const char *tid) {
  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  if (!cells) return -1;
  int n = find_cells(h, tid, K_BG, cells, MAX_CELLS), rc = 0;
  char pre[400];
  bg_prefix(tid, pre, sizeof(pre));
  if (run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0) rc = -1;
  for (int i = 0; i < n && rc == 0; i++) {
    if (!bg_is_ours(cells[i].value, pre)) continue;
    char orig[512];
    int is_null = 0;
    if (!saved_orig_row(h, tid, "bg", &cells[i], orig, sizeof(orig), &is_null)) continue;
    if (!is_null && (strstr(orig, "/hh_bg_") || strstr(orig, "/hsm_shared/"))) {
      char good[512];
      if (backup_orig(&cells[i], good, sizeof(good))) {
        hh_log("background %s: the saved original of %s.%s was one of our own files; putting the first backup's value back", tid, cells[i].tbl, cells[i].col);
        snprintf(orig, sizeof(orig), "%s", good);
      } else is_null = 1; /* better the console's default than a file we are about to delete */
    }
    if (!is_null && !orig[0]) continue;
    if (set_cell(h, &cells[i], cells[i].value, is_null ? NULL : orig) < 0) rc = -1;
  }
  if (rc == 0 &&
      (run(h, "DELETE FROM hh.custom_cells WHERE title_id=?1 AND kind='bg'", tid, NULL, NULL, NULL) < 0 ||
       run(h, "UPDATE hh.custom SET bg_file=NULL WHERE title_id=?1", tid, NULL, NULL, NULL) < 0))
    rc = -1;
  if (rc == 0) drop_empty(h, tid);
  if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) rc = -1;
  if (rc) sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
  free(cells);
  if (rc) return -1;
  bg_cleanup(tid, NULL, 0);
  char src[400];
  bg_source(tid, src, sizeof(src));
  unlink(src);
  if (!g_gc_defer) shared_gc(h);
  return 0;
}

/* Folder tiles: the name and the picture, straight into the home screen
 * database. A folder's files are the source of truth (the console's installer
 * reads them next time), so nothing is saved as an "original" and nothing is
 * re-applied at start-up; the only thing kept is the name of the last picture
 * file we made, to remove it when the next one replaces it. */
int custom_refresh_folder_tile(hh_t *h, const char *tid, const char *name, const unsigned char *png,
                               size_t len) {
  if (!hh_valid_tid(tid) || !folders_is_folder(tid)) {
    cerr(h, "That isn't a folder tile.");
    return -1;
  }
  if (h->nusers == 0) {
    cerr(h, "This firmware's home screen database has a layout Home Screen Manager doesn't know yet.");
    return -1;
  }
  if (!name || !name[0] || !png || !len) {
    cerr(h, "The folder has no name or picture to put in the database.");
    return -1;
  }

  /* the picture: a file name never used before, so no cached copy is shown */
  char prev[512] = "", meta[600], dir[400];
  text1(h, "SELECT icon_meta FROM hh.folder_meta WHERE title_id=?1", tid, prev, sizeof(prev));
  snprintf(dir, sizeof(dir), "%s/%s", C.appmeta_dir, tid);
  mkdirs(dir);
  long stamp = (long)time(NULL);
  do {
    snprintf(meta, sizeof(meta), "%s/hh_icon0_%ld.png", dir, stamp++);
  } while (!strcmp(meta, prev) || file_exists(meta));
  if (write_file(meta, png, len)) {
    unlink(meta);
    cerr(h, "Couldn't save the folder's picture on the console.");
    return -1;
  }

  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  if (!cells) {
    unlink(meta);
    cerr(h, "Out of memory.");
    return -1;
  }
  int rc = run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0 ? -1 : 0;
  int names = 0, icons = 0, changed = 0;
  if (rc == 0) {
    int n = find_cells(h, tid, K_NAME, cells, MAX_CELLS);
    for (int i = 0; i < n && rc == 0; i++) {
      if (!is_title_name_col(cells[i].col)) continue;
      names++;
      if (!strcmp(cells[i].value, name)) continue;
      int ch = set_cell(h, &cells[i], cells[i].value, name);
      if (ch < 0) rc = -1;
      else changed += ch;
    }
  }
  if (rc == 0) {
    int n = find_cells(h, tid, K_ICON, cells, MAX_CELLS);
    for (int i = 0; i < n && rc == 0; i++) {
      icons++;
      if (!strcmp(cells[i].value, meta)) continue;
      int ch = set_cell(h, &cells[i], cells[i].value, meta);
      if (ch < 0) rc = -1;
      else changed += ch;
    }
  }
  if (rc == 0 && names == 0) {
    cerr(h, "Home Screen Manager couldn't find this folder's name in the home screen database.");
    rc = -1;
  }
  if (rc == 0 && icons == 0) {
    cerr(h, "This folder's picture isn't stored in the home screen database, so it can't be changed there.");
    rc = -1;
  }
  if (rc == 0 &&
      run(h, "INSERT OR REPLACE INTO hh.folder_meta(title_id, icon_meta) VALUES(?1, ?2)", tid, meta, NULL,
          NULL) < 0)
    rc = -1;
  if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) rc = -1;
  free(cells);
  if (rc) {
    char keep[512];
    snprintf(keep, sizeof(keep), "%s", h->err);
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(h->err, sizeof(h->err), "%s", keep);
    unlink(meta);
    return -1;
  }
  if (prev[0]) unlink(prev);
  hh_log("folder %s: name and picture put in the home screen database (%d name cell(s), %d picture cell(s), %d value(s) changed)",
         tid, names, icons, changed);
  return 0;
}

int custom_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  if (!can_customize(h, tid)) return -1;
  if (!img) {
    if (reset_bg(h, tid)) return -1;
    hh_log("background %s: original background", tid);
    return 0;
  }
  int changed = 0;
  if (apply_bg(h, tid, img, len, &changed)) return -1;

  /* keep the picture (as a 4K JPEG) to remake the files if they go missing */
  char src[400], dir[400], orig[256] = "";
  snprintf(dir, sizeof(dir), "%s/backgrounds", C.data_dir);
  mkdirs(dir);
  bg_source(tid, src, sizeof(src));
  uint8_t *rgba = icon_load_cover(img, len, TEX_BG_W, TEX_BG_H);
  unsigned char *jpg = NULL;
  size_t jl = 0;
  if (rgba && img_encode_jpg(rgba, TEX_BG_W, TEX_BG_H, 90, &jpg, &jl) == 0) write_file(src, jpg, jl);
  free(rgba);
  free(jpg);
  hh_title_name(h, tid, orig, sizeof(orig));
  run(h, "INSERT INTO hh.custom(title_id, bg_file, orig_name, updated_at) "
         "VALUES(?1, ?2, ?3, datetime('now')) ON CONFLICT(title_id) DO UPDATE SET "
         "bg_file=excluded.bg_file, orig_name=coalesce(hh.custom.orig_name, excluded.orig_name), "
         "updated_at=excluded.updated_at",
      tid, src, orig, NULL);
  hh_log("background %s: %d value(s) changed", tid, changed);
  return 0;
}

int custom_bg_path(hh_t *h, const char *tid, char *out, size_t n) {
  if (!hh_valid_tid(tid)) return -1;
  if (text1(h, "SELECT bg_file FROM hh.custom WHERE title_id=?1", tid, out, n)) return -1;
  return file_exists(out) ? 0 : -1;
}

/* Puts our background back if the console reset a cell or our file is gone. */
static int bg_needs_fix(hh_t *h, const char *tid) {
  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  if (!cells) return 0;
  int n = find_cells(h, tid, K_BG, cells, MAX_CELLS), fix = 0;
  char pre[400];
  bg_prefix(tid, pre, sizeof(pre));
  for (int i = 0; i < n; i++) {
    const char *why = NULL;
    if (bg_is_ours(cells[i].value, pre)) {
      if (!file_exists(cells[i].value)) why = "our file is missing";
    } else {
      tex_fmt_t f;
      if (tex_probe(cells[i].value, &f) == 0) why = "it holds a file that isn't ours (the console put its own back)";
      /* a value that is not a picture path is left alone, as apply_bg leaves it */
    }
    if (why) {
      fix = 1;
      hh_log("custom %s: background needs fixing: %s.%s (%s=%s) = %s: %s", tid, cells[i].tbl, cells[i].col, cells[i].keycol, cells[i].keyval,
             cells[i].value, why);
    }
  }
  free(cells);
  return fix;
}

/* ------------------------------------------------------------------------- */
/* status, repair, reset                                                     */
/* ------------------------------------------------------------------------- */

int custom_get_json(hh_t *h, const char *tid, strbuf_t *sb) {
  if (!hh_valid_tid(tid)) {
    cerr(h, "Missing or invalid title id.");
    return -1;
  }
  char name[256] = "", orig[256] = "", cname[256] = "", icon[400] = "";
  hh_title_name(h, tid, name, sizeof(name));
  int has = text1(h, "SELECT orig_name FROM hh.custom WHERE title_id=?1", tid, orig, sizeof(orig)) == 0;
  text1(h, "SELECT name FROM hh.custom WHERE title_id=?1", tid, cname, sizeof(cname));
  text1(h, "SELECT icon_file FROM hh.custom WHERE title_id=?1", tid, icon, sizeof(icon));
  cell_t *cells = calloc(MAX_CELLS, sizeof(cell_t));
  int nname = cells ? find_cells(h, tid, K_NAME, cells, MAX_CELLS) : 0;
  int nicon = cells ? find_cells(h, tid, K_ICON, cells, MAX_CELLS) : 0;
  int nbg = cells ? find_cells(h, tid, K_BG, cells, MAX_CELLS) : 0;
  char bgf[400] = "";
  text1(h, "SELECT bg_file FROM hh.custom WHERE title_id=?1", tid, bgf, sizeof(bgf));
  free(cells);
  sb_printf(sb, "{\"id\":\"%s\",\"name\":", tid);
  sb_json_str(sb, name[0] ? name : tid);
  sb_puts(sb, ",\"orig_name\":");
  sb_json_str(sb, has && orig[0] ? orig : (name[0] ? name : tid));
  sb_printf(sb, ",\"renamed\":%s,\"custom_icon\":%s,\"custom_bg\":%s,\"can_rename\":%s,"
                "\"can_icon\":%s,\"can_bg\":%s}",
            cname[0] ? "true" : "false", icon[0] ? "true" : "false", bgf[0] ? "true" : "false",
            nname ? "true" : "false", (nicon || icon[0]) ? "true" : "false", nbg ? "true" : "false");
  return sb->oom ? -1 : 0;
}

int custom_reapply(hh_t *h, int *fixed) {
  *fixed = 0;
  if (h->nusers == 0) return 0;
  typedef struct {
    char tid[16], name[256], file[400], meta[512];
  } row_t;
  int cap = custom_rows(h) + 1;
  if (cap > 4096) cap = 4096;
  row_t *rows = calloc((size_t)cap, sizeof(row_t));
  if (!rows) return -1;
  int n = 0;
  sqlite3_stmt *st = prep(h, "SELECT title_id, name, icon_file, icon_meta FROM hh.custom");
  if (st) {
    while (sqlite3_step(st) == SQLITE_ROW && n < cap) {
      const char *v[4];
      for (int i = 0; i < 4; i++) v[i] = (const char *)sqlite3_column_text(st, i);
      if (!v[0] || !hh_valid_tid(v[0])) continue;
      snprintf(rows[n].tid, sizeof(rows[n].tid), "%s", v[0]);
      snprintf(rows[n].name, sizeof(rows[n].name), "%s", v[1] ? v[1] : "");
      snprintf(rows[n].file, sizeof(rows[n].file), "%s", v[2] ? v[2] : "");
      snprintf(rows[n].meta, sizeof(rows[n].meta), "%s", v[3] ? v[3] : "");
      n++;
    }
    sqlite3_finalize(st);
  }
  for (int i = 0; i < n; i++) {
    char bgf[400];
    if (text1(h, "SELECT bg_file FROM hh.custom WHERE title_id=?1", rows[i].tid, bgf, sizeof(bgf)) == 0 &&
        bg_needs_fix(h, rows[i].tid)) {
      FILE *f = fopen(bgf, "rb");
      unsigned char *b = NULL;
      long sz = 0;
      if (f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 32L * 1024 * 1024 && (b = malloc((size_t)sz)) &&
            fread(b, 1, (size_t)sz, f) != (size_t)sz) {
          free(b);
          b = NULL;
        }
        fclose(f);
      }
      int c4 = 0;
      if (b && apply_bg(h, rows[i].tid, b, (size_t)sz, &c4) == 0 && c4) {
        hh_log("custom %s: put the background back (%d value(s))", rows[i].tid, c4);
        *fixed += c4;
      }
      free(b);
    }
    if (run(h, "BEGIN", NULL, NULL, NULL, NULL) < 0) break; /* no lock unless something is written */
    int ch = 0, found = 0, rc = 0;
    if (rows[i].meta[0]) {
      if (!file_exists(rows[i].meta)) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", rows[i].meta);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        mkdirs(dir);
        if (copy_file(rows[i].file, rows[i].meta) == 0) ch++;
      }
      int c2 = 0;
      rc = point_icons(h, rows[i].tid, rows[i].meta, rows[i].meta, &c2, &found);
      ch += c2;
    }
    if (rc == 0 && rows[i].name[0]) {
      int c3 = 0;
      apply_name(h, rows[i].tid, rows[i].name, &c3); /* game may be gone: not an error */
      ch += c3;
    }
    if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) >= 0) {
      if (ch) hh_log("custom %s: put back %d value(s) the system changed", rows[i].tid, ch);
      *fixed += ch;
    } else {
      sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    }
  }
  free(rows);
  return 0;
}

void custom_forget_all(hh_t *h) {
  sqlite3_stmt *st = prep(h, "SELECT icon_file, icon_meta, bg_file, title_id FROM hh.custom");
  if (!st) return;
  while (sqlite3_step(st) == SQLITE_ROW) {
    for (int i = 0; i < 3; i++) {
      const char *p = (const char *)sqlite3_column_text(st, i);
      if (p && p[0]) unlink(p);
    }
    const char *tid = (const char *)sqlite3_column_text(st, 3);
    if (tid && hh_valid_tid(tid)) bg_cleanup(tid, NULL, 0);
  }
  sqlite3_finalize(st);
  char dir[420];
  bg_shared_dir(dir, sizeof(dir));
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      char p[700];
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      if (!strncmp(e->d_name, "bg_", 3)) unlink(p);
    }
    closedir(d);
  }
}

/* A tile's background from the picture already prepared for a bulk run (a 4K JPEG at
 * prepared_path). The converted files are shared between tiles, see custom_bulk_begin. */
int custom_set_bg_prepared(hh_t *h, const char *tid, const unsigned char *jpg, size_t len, const char *prepared_path) {
  if (!can_customize(h, tid)) return -1;
  int changed = 0;
  if (apply_bg(h, tid, jpg, len, &changed)) return -1;
  char src[400], dir[400], orig[256] = "";
  snprintf(dir, sizeof(dir), "%s/backgrounds", C.data_dir);
  mkdirs(dir);
  if (g_share) { /* one source picture for all the games that share */
    snprintf(src, sizeof(src), "%.360s/shared_%08x.jpg", dir, crc32_cached(jpg, len));
    if (!file_exists(src) && link(prepared_path, src) != 0) copy_file(prepared_path, src);
  } else {
    bg_source(tid, src, sizeof(src));
    unlink(src);
    if (link(prepared_path, src) != 0) copy_file(prepared_path, src);
  }
  hh_title_name(h, tid, orig, sizeof(orig));
  run(h, "INSERT INTO hh.custom(title_id, bg_file, orig_name, updated_at) "
         "VALUES(?1, ?2, ?3, datetime('now')) ON CONFLICT(title_id) DO UPDATE SET "
         "bg_file=excluded.bg_file, orig_name=coalesce(hh.custom.orig_name, excluded.orig_name), "
         "updated_at=excluded.updated_at",
      tid, src, orig, NULL);
  hh_log("background %s: %d value(s) changed", tid, changed);
  return 0;
}

int custom_can_bg(hh_t *h, const char *tid) {
  cell_t c;
  return hh_valid_tid(tid) && find_cells(h, tid, K_BG, &c, 1) > 0;
}

/* [{"id","can_bg","can_rename","orig_name"}, ...] for the tiles of the bulk screens. */
void custom_bulk_info_json(hh_t *h, char (*ids)[16], int n, strbuf_t *sb) {
  sb_puts(sb, "[");
  for (int i = 0; i < n; i++) {
    cell_t c;
    char name[256] = "", orig[256] = "";
    int bg = hh_valid_tid(ids[i]) && find_cells(h, ids[i], K_BG, &c, 1) > 0;
    int rn = hh_valid_tid(ids[i]) && find_cells(h, ids[i], K_NAME, &c, 1) > 0;
    hh_title_name(h, ids[i], name, sizeof(name));
    text1(h, "SELECT orig_name FROM hh.custom WHERE title_id=?1", ids[i], orig, sizeof(orig));
    char cname[256] = "", src[16] = "";
    text1(h, "SELECT name FROM hh.custom WHERE title_id=?1", ids[i], cname, sizeof(cname));
    text1(h, "SELECT name_src FROM hh.custom WHERE title_id=?1", ids[i], src, sizeof(src));
    sb_printf(sb, "%s{\"id\":\"%s\",\"can_bg\":%s,\"can_rename\":%s,\"by\":\"%s\",\"orig_name\":", i ? "," : "", ids[i], bg ? "true" : "false",
              rn ? "true" : "false", cname[0] ? (!strcmp(src, "tool") ? "tool" : "hand") : "");
    sb_json_str(sb, orig[0] ? orig : name);
    sb_puts(sb, "}");
  }
  sb_puts(sb, "]");
}

int custom_rows(hh_t *h) {
  sqlite3_stmt *st = prep(h, "SELECT count(*) FROM hh.custom");
  int n = 0;
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  return n;
}

int custom_has_bg(hh_t *h, const char *tid) {
  char bgf[400] = "";
  return text1(h, "SELECT bg_file FROM hh.custom WHERE title_id=?1", tid, bgf, sizeof(bgf)) == 0 && bgf[0];
}

/* Title ids with a custom name / a custom background. Returns how many were put in ids. */
static int ids_where(hh_t *h, const char *sql, char (*ids)[16], int max) {
  sqlite3_stmt *st = prep(h, sql);
  int n = 0;
  while (st && n < max && sqlite3_step(st) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(st, 0);
    if (v && hh_valid_tid(v)) snprintf(ids[n++], 16, "%.15s", v);
  }
  if (st) sqlite3_finalize(st);
  return n;
}
int custom_renamed_ids(hh_t *h, char (*ids)[16], int max) {
  return ids_where(h, "SELECT title_id FROM hh.custom WHERE name IS NOT NULL AND name<>'' ORDER BY title_id", ids, max);
}
int custom_bg_ids(hh_t *h, char (*ids)[16], int max) {
  return ids_where(h, "SELECT title_id FROM hh.custom WHERE bg_file IS NOT NULL AND bg_file<>'' ORDER BY title_id", ids, max);
}

/* Puts a name back after a cleanup. A name the tool changed goes back to what was there before the tool
 * (a name you gave by hand, or the original). A name you gave by hand is only touched when hand_too.
 * Returns 1 if the name was changed, 0 if not, -1 on an error. */
int custom_undo_names(hh_t *h, const char *tid, int hand_too) {
  char name[256] = "", src[16] = "", before[256] = "";
  if (text1(h, "SELECT name FROM hh.custom WHERE title_id=?1", tid, name, sizeof(name))) return 0;
  text1(h, "SELECT name_src FROM hh.custom WHERE title_id=?1", tid, src, sizeof(src));
  text1(h, "SELECT name_before FROM hh.custom WHERE title_id=?1", tid, before, sizeof(before));
  int tool = !strcmp(src, "tool");
  if (!tool && !hand_too) return 0;
  if (tool && before[0] && !hand_too) return custom_set_name(h, tid, before) == 0 ? 1 : -1;
  return custom_set_name(h, tid, NULL) == 0 ? 1 : -1;
}

/* ------------------------------------------------------------------------- */
/* the experiment: a background for a tile that has none                     */
/* ------------------------------------------------------------------------- */

typedef struct {
  char tbl[96], col[64], keycol[32], keyval[96];
  int rows, empty;
} slot_t;

/* every row, in every table with a background column, that belongs to the tile, empty or not */
static int find_slots(hh_t *h, const char *tid, slot_t *out, int max) {
  char cids[MAX_KEYS][96];
  int ncid = concept_ids(h, tid, cids, MAX_KEYS);
  lay_t lay[MAX_LAY];
  int nl = layout_for(h, K_BG, lay, MAX_LAY), n = 0;
  for (int li = 0; li < nl && n < max; li++) {
    const lay_t *L = &lay[li];
    const char *keys[MAX_KEYS];
    int nkeys = 0;
    if (L->has_tid) keys[nkeys++] = tid;
    else
      for (int i = 0; i < ncid; i++) keys[nkeys++] = cids[i];
    for (int k = 0; k < nkeys && n < max; k++) {
      char sql[500];
      snprintf(sql, sizeof(sql),
               "SELECT count(*), coalesce(sum(CASE WHEN \"%s\" IS NULL OR \"%s\"='' THEN 1 ELSE 0 END), 0) "
               "FROM main.\"%s\" WHERE \"%s\"=?1",
               L->col, L->col, L->tbl, L->keycol);
      sqlite3_stmt *st = prep(h, sql);
      if (!st) continue;
      sqlite3_bind_text(st, 1, keys[k], -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0) {
        slot_t *s2 = &out[n++];
        snprintf(s2->tbl, sizeof(s2->tbl), "%s", L->tbl);
        snprintf(s2->col, sizeof(s2->col), "%s", L->col);
        snprintf(s2->keycol, sizeof(s2->keycol), "%s", L->keycol);
        snprintf(s2->keyval, sizeof(s2->keyval), "%s", keys[k]);
        s2->rows = sqlite3_column_int(st, 0);
        s2->empty = sqlite3_column_int(st, 1);
      }
      sqlite3_finalize(st);
    }
  }
  return n;
}

/* The format to write for an empty slot: the one most other tiles use in the same table and column.
 * With none to copy from: by the kind of title (PS5 games 3840x2160 BC7, PS4 games 1920x1080 BC1). */
static void guess_bg_format(hh_t *h, const slot_t *s, const char *tid, tex_fmt_t *f, char *how, size_t hn) {
  tex_fmt_t seen[8];
  int cnt[8], ns = 0;
  char sql[400];
  snprintf(sql, sizeof(sql), "SELECT DISTINCT \"%s\" FROM main.\"%s\" WHERE \"%s\" IS NOT NULL AND \"%s\"<>'' LIMIT 60", s->col, s->tbl, s->col,
           s->col);
  sqlite3_stmt *st = prep(h, sql);
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(st, 0);
    tex_fmt_t t;
    if (!v || tex_probe(v, &t) || !tex_supported(&t) || t.w <= 0 || t.h <= 0) continue;
    int k;
    for (k = 0; k < ns; k++)
      if (!memcmp(&seen[k], &t, sizeof(t))) break;
    if (k == ns) {
      if (ns == 8) continue;
      seen[ns] = t;
      cnt[ns++] = 0;
    }
    cnt[k]++;
  }
  if (st) sqlite3_finalize(st);
  if (ns) {
    int best = 0;
    for (int k = 1; k < ns; k++)
      if (cnt[k] > cnt[best]) best = k;
    *f = seen[best];
    snprintf(how, hn, "the same as %d other tile(s) in %s.%s", cnt[best], s->tbl, s->col);
    return;
  }
  tex_fmt_t bc7 = {TEX_DDS, 3840, 2160, 1, 98, 0}, bc1 = {TEX_DDS, 1920, 1080, 1, 71, 1};
  *f = !strncmp(tid, "CUSA", 4) ? bc1 : bc7;
  snprintf(how, hn, "no other tile has a value there, so the default for this kind of title");
}

/* Fills the empty background slots of a tile with a picture of ours. write = 0 only reports (system tiles).
 * Returns 0 when done, 1 when only a report was made, -1 on an error (the text is in h->err). */
int custom_try_bg(hh_t *h, const char *tid, const unsigned char *jpg, size_t len, const char *prepared_path, int write) {
  if (!can_customize(h, tid)) return -1;
  cell_t one;
  if (find_cells(h, tid, K_BG, &one, 1) > 0) {
    cerr(h, "This tile already has a background entry, so it is in the normal list.");
    return -1;
  }
  slot_t slots[24];
  int ns = find_slots(h, tid, slots, 24), filled_slots = 0;
  for (int i = 0; i < ns; i++) {
    hh_log("experiment %s: %s.%s (%s=%s): %d row(s), %d empty", tid, slots[i].tbl, slots[i].col, slots[i].keycol, slots[i].keyval, slots[i].rows,
           slots[i].empty);
    if (slots[i].empty > 0) filled_slots++;
  }
  if (!ns || !filled_slots) {
    hh_log("experiment %s: %s", tid, ns ? "every slot has a value that isn't text" : "no table with a background column has a row for this tile");
    cerr(h, "The home screen database has no background slot at all for this tile, so there is nothing to fill in. The log has the details.");
    return -1;
  }
  if (!write) {
    hh_log("experiment %s: a system tile, report only (%d slot(s) found)", tid, filled_slots);
    snprintf(h->err, sizeof(h->err), "%d empty background slot(s) found for this tile. Nothing was written: system tiles are report only. The log has the details.", filled_slots);
    return 1;
  }
  tex_fmt_t fmts[BG_FILES];
  char files[BG_FILES][600], pre[400];
  int nf = 0, slot_file[24];
  long stamp = (long)time(NULL);
  bg_prefix(tid, pre, sizeof(pre));
  for (int i = 0; i < ns; i++) {
    slot_file[i] = -1;
    if (slots[i].empty <= 0) continue;
    tex_fmt_t f;
    char how[160], desc[96];
    guess_bg_format(h, &slots[i], tid, &f, how, sizeof(how));
    tex_describe(&f, desc, sizeof(desc));
    hh_log("experiment %s: %s.%s gets %s (%s)", tid, slots[i].tbl, slots[i].col, desc, how);
    int k;
    for (k = 0; k < nf; k++)
      if (!memcmp(&fmts[k], &f, sizeof(f))) break;
    if (k == nf) {
      if (nf == BG_FILES) continue;
      fmts[nf] = f;
      snprintf(files[nf], sizeof(files[nf]), "%s%ld_%d.%s", pre, stamp, nf, tex_ext(&f));
      nf++;
    }
    slot_file[i] = k;
  }
  char dir[400];
  snprintf(dir, sizeof(dir), "%s/%s", C.appmeta_dir, tid);
  mkdirs(dir);
  int rc = 0;
  for (int k = 0; k < nf && rc == 0; k++) {
    unsigned char *out;
    size_t ol;
    if (tex_encode(jpg, len, &fmts[k], &out, &ol)) {
      cerr(h, "The picture couldn't be converted.");
      rc = -1;
      break;
    }
    if (write_file(files[k], out, ol)) {
      cerr(h, "Couldn't save the background on the console.");
      rc = -1;
    }
    hh_log("experiment %s: wrote %s (%zu bytes)", tid, files[k], ol);
    free(out);
  }
  int cells_set = 0;
  if (rc == 0 && run(h, "BEGIN IMMEDIATE", NULL, NULL, NULL, NULL) < 0) rc = -1;
  for (int i = 0; i < ns && rc == 0; i++) {
    if (slot_file[i] < 0) continue;
    char sql[400];
    snprintf(sql, sizeof(sql), "UPDATE main.\"%.95s\" SET \"%.63s\"=?1 WHERE \"%.31s\"=?2 AND (\"%.63s\" IS NULL OR \"%.63s\"='')", slots[i].tbl,
             slots[i].col, slots[i].keycol, slots[i].col, slots[i].col);
    int ch = run(h, sql, files[slot_file[i]], slots[i].keyval, NULL, NULL);
    if (ch < 0) {
      rc = -1;
      break;
    }
    if (ch > 0) {
      cell_t c;
      snprintf(c.tbl, sizeof(c.tbl), "%.95s", slots[i].tbl);
      snprintf(c.col, sizeof(c.col), "%.63s", slots[i].col);
      snprintf(c.keycol, sizeof(c.keycol), "%.31s", slots[i].keycol);
      snprintf(c.keyval, sizeof(c.keyval), "%.95s", slots[i].keyval);
      c.value[0] = '\0';
      if (save_orig(h, tid, "bg", &c, NULL, 1)) rc = -1; /* the original was empty: NULL */
      cells_set += ch;
    }
  }
  if (rc == 0 && run(h, "COMMIT", NULL, NULL, NULL, NULL) < 0) rc = -1;
  if (rc) {
    char keep[512];
    snprintf(keep, sizeof(keep), "%s", h->err);
    sqlite3_exec(h->db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(h->err, sizeof(h->err), "%s", keep);
    for (int k = 0; k < nf; k++) unlink(files[k]);
    return -1;
  }
  char src[400], sdir[400], orig[256] = "";
  snprintf(sdir, sizeof(sdir), "%s/backgrounds", C.data_dir);
  mkdirs(sdir);
  bg_source(tid, src, sizeof(src));
  unlink(src);
  if (link(prepared_path, src) != 0) copy_file(prepared_path, src);
  hh_title_name(h, tid, orig, sizeof(orig));
  run(h, "INSERT INTO hh.custom(title_id, bg_file, orig_name, updated_at) "
         "VALUES(?1, ?2, ?3, datetime('now')) ON CONFLICT(title_id) DO UPDATE SET "
         "bg_file=excluded.bg_file, orig_name=coalesce(hh.custom.orig_name, excluded.orig_name), "
         "updated_at=excluded.updated_at",
      tid, src, orig, NULL);
  hh_log("experiment %s: filled %d cell(s) with %d file(s)", tid, cells_set, nf);
  return 0;
}
