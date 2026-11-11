#include "hider.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Tiles offered in the menu. Title IDs from psdevwiki (PS5 Title_ID page). */
const hh_target_t HH_TARGETS[] = {
    {"NPXS40047", "PlayStation Store", "Games"},
    {"NPXS40075", "Media Gallery", "Games"},
    {"NPXS40071", "Game Library", "Games"},
    {"NPXS40139", "App Library", "Media"},
    {"NPXS40056", "All Apps", "Media"},
};
const int HH_TARGET_COUNT = (int)(sizeof(HH_TARGETS) / sizeof(HH_TARGETS[0]));

/* Tiles the home screen adds by itself whatever the database says (seen on
 * FW 4.51: hiding them in app.db has no effect), so they are not offered.
 * "All Apps" (NPXS40056) does hide normally and is NOT in this list. */
static const char *const kLocked[] = {"NPXS40071", "NPXS40139"};

int hh_is_locked(const char *tid) {
  for (size_t i = 0; i < sizeof(kLocked) / sizeof(kLocked[0]); i++)
    if (strcmp(tid, kLocked[i]) == 0) return 1;
  return 0;
}

#define BUSY_TIMEOUT_MS 10000

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void set_err(hh_t *h, const char *fmt, const char *detail) {
  snprintf(h->err, sizeof(h->err), fmt, detail ? detail : "");
}

int hh_valid_tid(const char *tid) {
  if (!tid || strlen(tid) != 9) return 0;
  for (int i = 0; i < 4; i++)
    if (!isupper((unsigned char)tid[i])) return 0;
  for (int i = 4; i < 9; i++)
    if (!isdigit((unsigned char)tid[i])) return 0;
  return 1;
}

static int valid_ident_suffix(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n >= HH_USER_LEN) return 0;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
  return 1;
}

static int exec(hh_t *h, const char *sql) {
  char *msg = NULL;
  if (sqlite3_exec(h->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
    set_err(h, "Database error: %s", msg ? msg : sqlite3_errmsg(h->db));
    sqlite3_free(msg);
    return -1;
  }
  return 0;
}

/* Prepares sql and binds up to three text args (NULL = not bound). */
static sqlite3_stmt *prep(hh_t *h, const char *sql, const char *a1,
                          const char *a2, const char *a3) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    set_err(h, "Database error: %s", sqlite3_errmsg(h->db));
    return NULL;
  }
  if (a1) sqlite3_bind_text(st, 1, a1, -1, SQLITE_TRANSIENT);
  if (a2) sqlite3_bind_text(st, 2, a2, -1, SQLITE_TRANSIENT);
  if (a3) sqlite3_bind_text(st, 3, a3, -1, SQLITE_TRANSIENT);
  return st;
}

/* Runs a statement that returns no rows. */
static int run(hh_t *h, const char *sql, const char *a1, const char *a2,
                const char *a3) {
  sqlite3_stmt *st = prep(h, sql, a1, a2, a3);
  if (!st) return -1;
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    set_err(h, "Database error: %s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

/* Returns the first column of the first row as an integer, or dflt. */
static long long scalar(hh_t *h, const char *sql, const char *a1,
                        const char *a2, long long dflt) {
  sqlite3_stmt *st = prep(h, sql, a1, a2, NULL);
  if (!st) return dflt;
  long long v = dflt;
  if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static int has_table(hh_t *h, const char *schema, const char *table) {
  char sql[160];
  snprintf(sql, sizeof(sql),
           "SELECT count(*) FROM %s.sqlite_master WHERE type='table' AND name=?1",
           schema);
  return scalar(h, sql, table, NULL, 0) > 0;
}

static int has_col(hh_t *h, const char *schema, const char *table, const char *col) {
  sqlite3_stmt *st = prep(h,
      "SELECT count(*) FROM pragma_table_info(?1, ?2) WHERE name=?3",
      table, schema, col);
  long long n = 0;
  if (st) {
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  return n > 0;
}

/* Builds a quoted, comma separated list of columns that exist in both
 * main.tbl_group_<u> and hh.grp_<u>, skipping an INTEGER PRIMARY KEY column
 * of the console table (a rowid alias the console assigns itself). */
static int group_cols(hh_t *h, const char *user, strbuf_t *out) {
  char main_t[96], bak_t[96];
  snprintf(main_t, sizeof(main_t), "tbl_group_%s", user);
  snprintf(bak_t, sizeof(bak_t), "grp_%s", user);
  sqlite3_stmt *st = prep(h,
      "SELECT m.name FROM pragma_table_info(?1, 'main') m "
      "JOIN pragma_table_info(?2, 'hh') b ON b.name = m.name "
      "WHERE NOT (m.pk = 1 AND upper(m.type) = 'INTEGER') ORDER BY m.cid",
      main_t, bak_t, NULL);
  if (!st) return -1;
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(st, 0);
    if (!c || strchr(c, '"')) continue;
    sb_printf(out, "%s\"%s\"", n ? "," : "", c);
    n++;
  }
  sqlite3_finalize(st);
  if (n == 0) {
    set_err(h, "Could not match MyHome backup columns%s", "");
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* open / close                                                              */
/* ------------------------------------------------------------------------- */

static void discover_users(hh_t *h) {
  h->nusers = 0;
  sqlite3_stmt *st = prep(h,
      "SELECT name FROM main.sqlite_master WHERE type='table' "
      "AND name LIKE 'tbl\\_iconinfo\\_%' ESCAPE '\\' ORDER BY name",
      NULL, NULL, NULL);
  if (!st) return;
  while (sqlite3_step(st) == SQLITE_ROW && h->nusers < HH_MAX_USERS) {
    const char *name = (const char *)sqlite3_column_text(st, 0);
    if (!name) continue;
    const char *u = name + strlen("tbl_iconinfo_");
    if (!valid_ident_suffix(u)) continue;
    snprintf(h->users[h->nusers], HH_USER_LEN, "%s", u);
    h->nusers++;
  }
  sqlite3_finalize(st);

  /* keep only users whose icon table has the columns we rely on */
  int kept = 0;
  for (int i = 0; i < h->nusers; i++) {
    char t[96], g[96];
    snprintf(t, sizeof(t), "tbl_iconinfo_%s", h->users[i]);
    if (!has_col(h, "main", t, "titleId") || !has_col(h, "main", t, "visible"))
      continue;
    if (kept != i) memcpy(h->users[kept], h->users[i], HH_USER_LEN);
    h->has_disp[kept] = has_col(h, "main", t, "dispLocation");
    snprintf(g, sizeof(g), "tbl_group_%s", h->users[kept]);
    h->has_group[kept] = has_table(h, "main", g) &&
                         has_col(h, "main", g, "itemId") &&
                         has_col(h, "main", g, "groupName");
    kept++;
  }
  h->nusers = kept;
}

int hh_open(hh_t *h, const char *app_db, const char *state_db) {
  memset(h, 0, sizeof(*h));
  snprintf(h->app_db_path, sizeof(h->app_db_path), "%s", app_db);

  struct stat sbuf;
  if (stat(app_db, &sbuf) != 0) {
    set_err(h, "Home screen database not found: %s", app_db);
    return -1;
  }
  /* never create app.db: READWRITE without CREATE */
  if (sqlite3_open_v2(app_db, &h->db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    set_err(h, "Cannot open app.db: %s", h->db ? sqlite3_errmsg(h->db) : "?");
    if (h->db) sqlite3_close(h->db);
    h->db = NULL;
    return -1;
  }
  sqlite3_busy_timeout(h->db, BUSY_TIMEOUT_MS);
  sqlite3_exec(h->db, "PRAGMA temp_store=MEMORY;", NULL, NULL, NULL);

  /* ATTACH inherits app.db's no-create flag, so create state.db first */
  sqlite3 *sdb = NULL;
  char *msg = NULL;
  if (sqlite3_open_v2(state_db, &sdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK ||
      sqlite3_exec(sdb,
                   "CREATE TABLE IF NOT EXISTS hidden("
                   " title_id TEXT NOT NULL, user TEXT NOT NULL,"
                   " orig_visible INTEGER, orig_disp INTEGER, hidden_at TEXT,"
                   " PRIMARY KEY(title_id, user));"
                   "CREATE TABLE IF NOT EXISTS wanted("
                   " title_id TEXT PRIMARY KEY, hidden_at TEXT);"
                   "CREATE TABLE IF NOT EXISTS folders("
                   " title_id TEXT PRIMARY KEY, name TEXT NOT NULL,"
                   " tab TEXT NOT NULL DEFAULT 'games', color TEXT NOT NULL DEFAULT 'blue',"
                   " custom_icon INTEGER NOT NULL DEFAULT 0, created_at TEXT);"
                   "CREATE TABLE IF NOT EXISTS folder_items("
                   " title_id TEXT NOT NULL, folder TEXT NOT NULL,"
                   " pos INTEGER NOT NULL DEFAULT 0, was_hidden INTEGER NOT NULL DEFAULT 0,"
                   " PRIMARY KEY(title_id, folder));"
                   "CREATE TABLE IF NOT EXISTS custom("
                   " title_id TEXT PRIMARY KEY, name TEXT, orig_name TEXT,"
                   " icon_file TEXT, icon_meta TEXT, updated_at TEXT, bg_file TEXT);"
                   "CREATE TABLE IF NOT EXISTS custom_cells("
                   " title_id TEXT NOT NULL, kind TEXT NOT NULL, tbl TEXT NOT NULL, col TEXT NOT NULL,"
                   " keycol TEXT NOT NULL, keyval TEXT NOT NULL, orig TEXT,"
                   " PRIMARY KEY(title_id, kind, tbl, col, keycol, keyval));"
                   "CREATE TABLE IF NOT EXISTS arranged("
                   " title_id TEXT PRIMARY KEY, tab TEXT NOT NULL, pos INTEGER NOT NULL,"
                   " pinned INTEGER NOT NULL DEFAULT 0, updated_at TEXT);"
                   "CREATE TABLE IF NOT EXISTS arrange_orig("
                   " title_id TEXT NOT NULL, user TEXT NOT NULL,"
                   " orig_access INTEGER, orig_pinned INTEGER, orig_idx INTEGER,"
                   " PRIMARY KEY(title_id, user));"
                   "CREATE TABLE IF NOT EXISTS tab_moved("
                   " title_id TEXT PRIMARY KEY, dest TEXT NOT NULL, created_at TEXT);"
                   "CREATE TABLE IF NOT EXISTS tab_orig("
                   " title_id TEXT NOT NULL, user TEXT NOT NULL, orig_disp INTEGER,"
                   " PRIMARY KEY(title_id, user));",
                   NULL, NULL, &msg) != SQLITE_OK) {
    set_err(h, "Cannot create state.db: %s",
            msg ? msg : (sdb ? sqlite3_errmsg(sdb) : state_db));
    sqlite3_free(msg);
    if (sdb) sqlite3_close(sdb);
    hh_close(h);
    return -1;
  }
  /* columns added in later versions (fails harmlessly when present) */
  sqlite3_exec(sdb, "ALTER TABLE custom ADD COLUMN bg_file TEXT", NULL, NULL, NULL);
  sqlite3_exec(sdb, "ALTER TABLE arrange_orig ADD COLUMN orig_idx INTEGER", NULL, NULL, NULL);
  sqlite3_close(sdb);

  sqlite3_stmt *st = prep(h, "ATTACH DATABASE ?1 AS hh", state_db, NULL, NULL);
  if (!st || sqlite3_step(st) != SQLITE_DONE) {
    set_err(h, "Cannot open state.db: %s", sqlite3_errmsg(h->db));
    if (st) sqlite3_finalize(st);
    hh_close(h);
    return -1;
  }
  sqlite3_finalize(st);

  /* migration for old single-folder-per-title storage */
  sqlite3_stmt *m = prep(h,
      "SELECT count(*) FROM hh.sqlite_master WHERE type='table' AND name='folder_items'",
      NULL, NULL, NULL);
  if (m) {
    int have_old = 0;
    if (sqlite3_step(m) == SQLITE_ROW) have_old = sqlite3_column_int(m, 0) > 0;
    sqlite3_finalize(m);
    if (have_old) {
      sqlite3_exec(h->db,
                   "CREATE TABLE IF NOT EXISTS hh.folder_items_new("
                   " title_id TEXT NOT NULL, folder TEXT NOT NULL,"
                   " pos INTEGER NOT NULL DEFAULT 0, was_hidden INTEGER NOT NULL DEFAULT 0,"
                   " PRIMARY KEY(title_id, folder));"
                   "INSERT OR IGNORE INTO hh.folder_items_new(title_id, folder, pos, was_hidden) "
                   "SELECT title_id, folder, pos, was_hidden FROM hh.folder_items;"
                   "DROP TABLE hh.folder_items;"
                   "ALTER TABLE hh.folder_items_new RENAME TO folder_items;",
                   NULL, NULL, NULL);
    }
  }

  discover_users(h);
  return 0;
}

void hh_close(hh_t *h) {
  if (h->db) sqlite3_close(h->db);
  h->db = NULL;
}

static int need_users(hh_t *h) {
  if (h->nusers > 0) return 0;
  set_err(h, "This firmware's home screen database has a layout Home Hider "
             "doesn't know yet (no usable tbl_iconinfo_* tables). Open "
             "Advanced > Diagnostics and share the output.%s", "");
  return -1;
}

/* ------------------------------------------------------------------------- */
/* hide / unhide                                                             */
/* ------------------------------------------------------------------------- */
