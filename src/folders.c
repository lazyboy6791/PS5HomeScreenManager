#include "folders.h"
#include <sys/time.h>

#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "custom.h"
#include "icons.h"
#include "texture.h"
#include "log.h"
#include "platform.h"

#include <time.h>

/* Adding a tile needs the console's installer, which can take about 90 seconds
 * on newer firmware. While a folder is being saved the background check must
 * not "repair" its tile (it isn't in the console's database yet, so the check
 * would start a second install of the same tile). */
static volatile int g_busy;


void folders_hold(int on) {
  if (on) __sync_fetch_and_add(&g_busy, 1);
  else __sync_fetch_and_sub(&g_busy, 1);
}

int folders_busy(void) { return g_busy > 0; }

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)(t.tv_sec * 1000L + t.tv_nsec / 1000000L);
}

static struct {
  char data_dir[256];
  char app_dir[256];
  char app_db[300];
  int port;
} F;

void folders_set_db_path(const char *path) { snprintf(F.app_db, sizeof(F.app_db), "%s", path ? path : ""); }

#define GAMES_FULL 0x8A /* typical dispLocation of a Games tab tile */
#define MEDIA_FULL 0xBC /* typical dispLocation of a Media tab tile */

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void ferr(hh_t *h, const char *fmt, const char *a) {
  snprintf(h->err, sizeof(h->err), fmt, a ? a : "");
}

static sqlite3_stmt *prep(hh_t *h, const char *sql) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(h->db, sql, -1, &st, NULL) != SQLITE_OK) {
    ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
    return NULL;
  }
  return st;
}

static int run1(hh_t *h, const char *sql, const char *a, const char *b) {
  sqlite3_stmt *st = prep(h, sql);
  if (!st) return -1;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
    return -1;
  }
  return 0;
}

static long long count1(hh_t *h, const char *sql, const char *a) {
  sqlite3_stmt *st = prep(h, sql);
  long long n = 0;
  if (!st) return 0;
  if (a) sqlite3_bind_text(st, 1, a, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return n;
}

/* 1 if the game is in that folder. */
static int is_member(hh_t *h, const char *tid, const char *folder) {
  sqlite3_stmt *st = prep(h, "SELECT count(*) FROM hh.folder_items WHERE title_id=?1 AND folder=?2");
  int n = 0;
  if (!st) return 0;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, folder, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0) > 0;
  sqlite3_finalize(st);
  return n;
}

static int col_exists(hh_t *h, const char *table, const char *col) {
  sqlite3_stmt *st = prep(h, "SELECT count(*) FROM pragma_table_info(?1, 'main') WHERE name=?2");
  int n = 0;
  if (!st) return 0;
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, col, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return n > 0;
}

int folders_is_folder(const char *tid) {
  return tid && strncmp(tid, FOLDER_PREFIX, 4) == 0 && hh_valid_tid(tid);
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

static int read_file(const char *path, unsigned char **out, size_t *len) {
  struct stat st;
  if (stat(path, &st) || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  *out = malloc((size_t)st.st_size);
  if (!*out) {
    fclose(f);
    return -1;
  }
  *len = fread(*out, 1, (size_t)st.st_size, f);
  fclose(f);
  if (*len != (size_t)st.st_size) {
    free(*out);
    return -1;
  }
  return 0;
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

void folders_init(const folders_cfg_t *cfg) {
  snprintf(F.data_dir, sizeof(F.data_dir), "%s", cfg->data_dir);
  snprintf(F.app_dir, sizeof(F.app_dir), "%s", cfg->app_dir);
  F.port = cfg->port;
}

static void custom_icon_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/folders/%s.png", F.data_dir, tid);
}

/* the background as the tile uses it (BC7 DDS) and the picture (JPEG) */
static void bg_dds_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/folders/%s_pic0.dds", F.data_dir, tid);
}

static void bg_jpg_path(const char *tid, char *out, size_t n) {
  snprintf(out, n, "%s/folders/%s_bg.jpg", F.data_dir, tid);
}

int folders_bg_path(const char *tid, char *out, size_t outsz) {
  if (!folders_is_folder(tid)) return -1;
  bg_jpg_path(tid, out, outsz);
  struct stat st;
  return stat(out, &st) == 0 ? 0 : -1;
}

int folders_icon_path(const char *tid, char *out, size_t outsz) {
  if (!folders_is_folder(tid)) return -1;
  snprintf(out, outsz, "%s/%s/sce_sys/icon0.png", F.app_dir, tid);
  struct stat st;
  return stat(out, &st) == 0 ? 0 : -1;
}

/* Name + recency of a title as the home screen shows it. */
static void title_info(hh_t *h, const char *tid, char *name, size_t namesz, long long *recent) {
  name[0] = '\0';
  *recent = 0;
  char sql[600];
  for (int i = 0; i < h->nusers && !name[0]; i++) {
    char icon_t[96], con_t[96];
    snprintf(icon_t, sizeof(icon_t), "tbl_iconinfo_%s", h->users[i]);
    snprintf(con_t, sizeof(con_t), "tbl_concepticoninfo_%s", h->users[i]);
    int has_con = col_exists(h, con_t, "conceptName") && col_exists(h, icon_t, "localConceptId");
    int has_name = col_exists(h, icon_t, "titleName");
    int has_acc = col_exists(h, icon_t, "lastAccessIndex");
    snprintf(sql, sizeof(sql),
             "SELECT %s, %s FROM main.\"%s\" i %s%s%s WHERE i.titleId=?1 LIMIT 1",
             has_con ? (has_name ? "coalesce(nullif(c.conceptName,''), i.titleName)" : "c.conceptName")
                     : (has_name ? "i.titleName" : "NULL"),
             has_acc ? "i.lastAccessIndex" : "0", icon_t,
             has_con ? "LEFT JOIN main.\"" : "", has_con ? con_t : "",
             has_con ? "\" c ON c.localConceptId = i.localConceptId" : "");
    sqlite3_stmt *st = prep(h, sql);
    if (!st) continue;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
      const char *n = (const char *)sqlite3_column_text(st, 0);
      if (n) snprintf(name, namesz, "%s", n);
      *recent = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
  }
  if (!name[0]) snprintf(name, namesz, "%s", tid);
}

/* The console's uninstall answers 0 for a tile it doesn't know, yet its rows stay in the home screen database,
 * and the next install-all by another tool (ps4mount) shows the tile again. So the rows of one of OUR folder
 * tiles are taken out here too. Tables that don't exist are skipped quietly. */
static int table_exists(hh_t *h, const char *name) {
  return count1(h, "SELECT count(*) FROM main.sqlite_master WHERE type='table' AND name=?1", name) > 0;
}

static void purge_tile_rows(hh_t *h, const char *tid) {
  if (!folders_is_folder(tid)) return;
  char sql[240], cid[64], t[80];
  snprintf(cid, sizeof(cid), "cid:local:%s", tid);
  for (int i = 0; i < h->nusers; i++) {
    snprintf(t, sizeof(t), "tbl_iconinfo_%s", h->users[i]);
    if (table_exists(h, t)) {
      snprintf(sql, sizeof(sql), "DELETE FROM main.\"%s\" WHERE titleId=?1", t);
      run1(h, sql, tid, NULL);
    }
    snprintf(t, sizeof(t), "tbl_concepticoninfo_%s", h->users[i]);
    if (table_exists(h, t)) {
      snprintf(sql, sizeof(sql), "DELETE FROM main.\"%s\" WHERE localConceptId=?1", t);
      run1(h, sql, cid, NULL);
    }
  }
  if (table_exists(h, "tbl_conceptmetadata")) run1(h, "DELETE FROM main.tbl_conceptmetadata WHERE localConceptId=?1", cid, NULL);
  if (table_exists(h, "tbl_contentinfo")) run1(h, "DELETE FROM main.tbl_contentinfo WHERE titleId=?1", tid, NULL);
}

static int tile_present(hh_t *h, const char *tid) {
  char sql[200];
  for (int i = 0; i < h->nusers; i++) {
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1",
             h->users[i]);
    if (count1(h, sql, tid) > 0) return 1;
  }
  return 0;
}

/* 1 if the tile table can be read and holds tiles. A busy or half-written database answers with nothing,
 * which must not be taken for "all folder tiles are gone". */
static int db_answers(hh_t *h) {
  char sql[200];
  for (int i = 0; i < h->nusers; i++) {
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM main.\"tbl_iconinfo_%s\"", h->users[i]);
    sqlite3_stmt *st = prep(h, sql);
    if (!st) return 0;
    int ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0;
    sqlite3_finalize(st);
    if (ok) return 1;
  }
  return 0;
}

/* Puts the folder tile on the chosen tab and makes it visible.
 * Returns 1 if something had to change. */
static int place_tile(hh_t *h, const char *tid, const char *tab) {
  int games = strcmp(tab, "media") != 0;
  int set = games ? HH_BIT_GAMES : HH_BIT_MEDIA, clr = games ? HH_BIT_MEDIA : HH_BIT_GAMES;
  int full = games ? GAMES_FULL : MEDIA_FULL;
  char sql[400], expr[200];
  snprintf(expr, sizeof(expr),
           "CASE WHEN (dispLocation & ~6) = 0 THEN %d ELSE ((dispLocation | %d) & ~%d) END",
           full, set, clr);
  int changed = 0;
  for (int i = 0; i < h->nusers; i++) {
    const char *u = h->users[i];
    snprintf(sql, sizeof(sql),
             "SELECT count(*) FROM main.\"tbl_iconinfo_%s\" WHERE titleId=?1 AND "
             "(visible = 0%s)",
             u, h->has_disp[i] ? games ? " OR (dispLocation & 2) = 0 OR (dispLocation & 4) != 0"
                                       : " OR (dispLocation & 4) = 0 OR (dispLocation & 2) != 0"
                               : "");
    if (count1(h, sql, tid) == 0) continue;
    changed = 1;
    if (h->has_disp[i])
      snprintf(sql, sizeof(sql),
               "UPDATE main.\"tbl_iconinfo_%s\" SET visible=1, dispLocation=%s WHERE titleId=?1", u, expr);
    else
      snprintf(sql, sizeof(sql), "UPDATE main.\"tbl_iconinfo_%s\" SET visible=1 WHERE titleId=?1", u);
    run1(h, sql, tid, NULL);
  }
  if (changed && col_exists(h, "tbl_contentinfo", "dispLocation")) {
    snprintf(sql, sizeof(sql), "UPDATE main.tbl_contentinfo SET dispLocation=%s WHERE titleId=?1", expr);
    run1(h, sql, tid, NULL);
  }
  return changed;
}

/* ------------------------------------------------------------------------- */
/* tile files                                                                */
/* ------------------------------------------------------------------------- */

/* param.json: a launcher whose "game" is our folder page. force=0 only writes when the file differs. */
static int write_param(hh_t *h, const char *tid, const char *name, const char *path) {
  strbuf_t pj;
  sb_init(&pj);
  sb_printf(&pj, "{\n  \"titleId\": \"%s\",\n  \"applicationCategoryType\": 65536,\n"
                 "  \"deeplinkUri\": \"http://127.0.0.1:%d/folder/%s\",\n"
                 "  \"localizedParameters\": {\n    \"defaultLanguage\": \"en-US\",\n"
                 "    \"en-US\": {\n      \"titleName\": ",
            tid, F.port, tid);
  sb_json_str(&pj, name);
  sb_puts(&pj, "\n    }\n  }\n}\n");
  int rc = pj.oom ? -1 : write_file(path, pj.data, pj.len);
  sb_free(&pj);
  if (rc) {
    ferr(h, "Couldn't write %.400s.", path);
    return -1;
  }
  return 0;
}

static int write_tile(hh_t *h, const char *tid) {
  char name[FOLDER_NAME_MAX * 2] = "", color[16] = "blue";
  int custom = 0, style = 0;
  sqlite3_stmt *st = prep(h, "SELECT name, color, custom_icon, style FROM hh.folders WHERE title_id=?1");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(name, sizeof(name), "%s", (const char *)sqlite3_column_text(st, 0));
    snprintf(color, sizeof(color), "%s", (const char *)sqlite3_column_text(st, 1));
    custom = sqlite3_column_int(st, 2);
    style = sqlite3_column_int(st, 3);
  }
  sqlite3_finalize(st);
  if (!name[0]) {
    ferr(h, "Folder %s not found.", tid);
    return -1;
  }

  char dir[400], path[520];
  snprintf(dir, sizeof(dir), "%s/%s/sce_sys", F.app_dir, tid);
  mkdirs(dir);

  snprintf(path, sizeof(path), "%s/param.json", dir);
  if (write_param(h, tid, name, path)) return -1;

  /* icon0.png: custom picture or a collage of the first four games */
  unsigned char *png = NULL;
  size_t png_len = 0;
  char cpath[400];
  custom_icon_path(tid, cpath, sizeof(cpath));
  unsigned char *raw = NULL;
  size_t raw_len = 0;
  if (custom && read_file(cpath, &raw, &raw_len) == 0) {
    /* a transparent picture goes on the folder's colour */
    if (icon_flatten(raw, raw_len, color, &png, &png_len)) png = NULL;
    free(raw);
  }
  if (!png) {
    char paths[ICON_COLLAGE_MAX][512];
    const char *ptrs[ICON_COLLAGE_MAX] = {NULL};
    int n = 0;
    st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 ORDER BY pos LIMIT 9");
    if (st) {
      sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
      while (sqlite3_step(st) == SQLITE_ROW && n < ICON_COLLAGE_MAX) {
        const char *it = (const char *)sqlite3_column_text(st, 0);
        if (it && hh_icon_path(h, it, paths[n], sizeof(paths[n])) == 0) ptrs[n] = paths[n];
        n++;
      }
      sqlite3_finalize(st);
    }
    if (icon_collage(ptrs, n, color, style, &png, &png_len)) {
      ferr(h, "Couldn't draw the folder icon.%s", "");
      return -1;
    }
  }
  snprintf(path, sizeof(path), "%s/icon0.png", dir);
  int rc = write_file(path, png, png_len);
  free(png);
  if (rc) {
    ferr(h, "Couldn't write %.400s.", path);
    return -1;
  }

  /* pic0.dds: the background, if the folder has one */
  char bg[400];
  bg_dds_path(tid, bg, sizeof(bg));
  snprintf(path, sizeof(path), "%s/pic0.dds", dir);
  unsigned char *dds = NULL;
  size_t dds_len = 0;
  if (read_file(bg, &dds, &dds_len) == 0) {
    rc = write_file(path, dds, dds_len);
    free(dds);
    if (rc) {
      ferr(h, "Couldn't write %.400s.", path);
      return -1;
    }
  } else {
    unlink(path);
  }
  return 0;
}

/* Install later: while on, install_tile only writes the tile's files and remembers the folder. One call to
 * folders_install_pending() then registers them all. On the console the installer handles every tile
 * folder in one pass anyway (about 90 s), so doing it once beats doing it per folder, per picture and
 * per background. Per thread, so a menu save running at the same time isn't affected. */
static __thread int t_defer;
static __thread char t_pend[FOLDER_MAX][16];
static __thread int t_npend;

void folders_defer_install(int on) { t_defer = on; }

/* Ends a "write the files now, install in a later step" request: forgets what was postponed on this thread
 * (the later request installs the tile itself). */
void folders_forget_pending(void) {
  t_defer = 0;
  t_npend = 0;
}

/* A change stamp of what other tools touch while they register games: the home screen database and the
 * apps folder (ShadowMount+ stages a new game's folder in it, PS4 Mount and the console write the database). */
static long long activity_stamp(void) {
  struct stat a, b;
  long long v = 0;
  if (F.app_db[0] && stat(F.app_db, &a) == 0)
    v += (long long)a.st_mtim.tv_sec * 1000000007LL + a.st_mtim.tv_nsec + (long long)a.st_size;
  if (stat(F.app_dir, &b) == 0) v += (long long)b.st_mtim.tv_sec * 999983LL + b.st_mtim.tv_nsec;
  return v;
}

/* Before an install: if another tool is registering games right now (the database or the apps folder keeps
 * changing), wait until it has been still for 4 seconds, at most 30 seconds. Starting our own install in the
 * middle of its install-all could make the console skip games or tiles. On a quiet console this costs 1 second. */
static void wait_for_other_tools(void) {
  const char *skip = getenv("HH_NO_QUIET_WAIT");
  if (skip && skip[0]) return;
  { /* with a game open the database is never still, and the install is the plain one anyway */
    char gt[64];
    uint32_t gid;
    if (plat_running_game(gt, sizeof(gt), &gid) == 0) return;
  }
  long long last = activity_stamp();
  long t0 = now_ms(), still_since = t0;
  int waited_for_others = 0;
  for (;;) {
    usleep(500000);
    long long cur = activity_stamp();
    long now = now_ms();
    if (cur != last) {
      last = cur;
      still_since = now;
      if (!waited_for_others) hh_log("folder install: another tool is writing to the home screen database or the apps folder, waiting for it to finish first");
      waited_for_others = 1;
    }
    if (!waited_for_others && now - t0 >= 1000) return;       /* nothing going on */
    if (now - still_since >= 4000) break;                      /* it has been quiet */
    if (now - t0 >= 30000) {
      hh_log("folder install: the database was still changing after 30 s, going ahead");
      return;
    }
  }
  if (waited_for_others) hh_log("folder install: quiet again after %ld ms, going ahead", now_ms() - t0);
}

/* For finding out why an install is sometimes slow (93 s: the console went through every game): what the
 * console can see of this tile just before the install, and what happened to the same number lately. */
static char g_meta_dir[256]; /* (set below) */
static char g_del_id[8][16];
static time_t g_del_at[8];
static int g_del_next;
static long g_last_inst_end_ms; /* when the last folder install returned */
static long g_last_inst_took_ms;

static void note_folder_deleted(const char *tid) {
  snprintf(g_del_id[g_del_next], sizeof(g_del_id[0]), "%s", tid);
  g_del_at[g_del_next] = time(NULL);
  g_del_next = (g_del_next + 1) % 8;
}

static void log_install_state(hh_t *h, const char *tid) {
  char p[420], del[64] = "never deleted in this run";
  struct stat sb;
  time_t now = time(NULL);
  for (int i = 0; i < 8; i++)
    if (g_del_at[i] && !strcmp(g_del_id[i], tid)) {
      snprintf(del, sizeof(del), "deleted %ld s ago", (long)(now - g_del_at[i]));
      break;
    }
  snprintf(p, sizeof(p), "%s/%s", F.app_dir, tid);
  int dir = stat(p, &sb) == 0;
  snprintf(p, sizeof(p), "%s/%s", g_meta_dir, tid);
  int meta = g_meta_dir[0] && stat(p, &sb) == 0;
  int dirs = 0, hhdirs = 0;
  DIR *d = opendir(F.app_dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      dirs++;
      if (!strncmp(e->d_name, "HHFD", 4)) hhdirs++;
    }
    closedir(d);
  }
  char last[80] = "no earlier install in this run";
  if (g_last_inst_end_ms)
    snprintf(last, sizeof(last), "the last install ended %ld s ago and took %ld ms", (now_ms() - g_last_inst_end_ms) / 1000,
             g_last_inst_took_ms);
  hh_log("install check %s: tile in app.db before: %s; own folder %s, appmeta copy %s; %d entries in the apps folder (%d of them folders); %s; %s",
         tid, tile_present(h, tid) ? "yes" : "no", dir ? "yes" : "no", meta ? "yes" : "no", dirs, hhdirs, del, last);
}

/* Writes the files, registers the tile and puts it on its tab. */
static int install_tile(hh_t *h, const char *tid, char *note, size_t notesz) {
  if (write_tile(h, tid)) {
    hh_log("folder %s: writing tile files failed: %s", tid, h->err);
    return -1;
  }
  hh_log("folder %s: tile files written to %s/%s", tid, F.app_dir, tid);
  if (t_defer) {
    int known = 0;
    for (int i = 0; i < t_npend; i++)
      if (!strcmp(t_pend[i], tid)) known = 1;
    if (!known && t_npend < FOLDER_MAX) snprintf(t_pend[t_npend++], 16, "%s", tid);
    hh_log("folder %s: install postponed (%d folder(s) waiting for one install)", tid, t_npend);
    return 0;
  }
  wait_for_other_tools();
  log_install_state(h, tid);
  long t_inst = now_ms();
  int err = plat_app_install(tid, F.app_dir);
  g_last_inst_took_ms = now_ms() - t_inst;
  g_last_inst_end_ms = now_ms();
  hh_log("folder %s: app install returned 0x%08X after %ld ms", tid, (unsigned)err, g_last_inst_took_ms);
  if (err) {
    char msg[64];
    snprintf(msg, sizeof(msg), "0x%08X", (unsigned)err);
    ferr(h, "The console refused to add the folder tile (error %s).", msg);
    return -1;
  }
  char tab[16] = "games";
  sqlite3_stmt *st = prep(h, "SELECT tab FROM hh.folders WHERE title_id=?1");
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(tab, sizeof(tab), "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  /* the system adds the tile to app.db shortly after installing */
  int waited = 0;
  for (; waited < 40 && !tile_present(h, tid); waited++) usleep(200000);
  hh_log("folder %s: tile in app.db=%d after %d ms", tid, tile_present(h, tid), waited * 200);
  if (!tile_present(h, tid)) {
    if (note) snprintf(note, notesz, "The tile was installed but hasn't appeared yet. Restart the PS5 if it doesn't show up.");
    return 0;
  }
  int moved = place_tile(h, tid, tab);
  hh_log("folder %s: placed on %s tab (changed=%d)", tid, tab, moved);
  return 0;
}

static void folder_tab(hh_t *h, const char *tid, char *tab, size_t n);



/* After a bulk install: wait until the home screen database and the apps folder have been still for 8 seconds
 * (at most 150 s). The console is still working through the changed tiles until then. */
static void wait_console_settled(void) {
  const char *skip = getenv("HH_NO_QUIET_WAIT");
  if (skip && skip[0]) return;
  long long last = activity_stamp();
  long t0 = now_ms(), still = t0;
  for (;;) {
    usleep(500000);
    long long cur = activity_stamp();
    long now = now_ms();
    if (cur != last) {
      last = cur;
      still = now;
    }
    if (now - still >= 8000) break;
    if (now - t0 >= 150000) {
      hh_log("folders: the console was still busy after 150 s, going on");
      return;
    }
  }
  hh_log("folders: the console was quiet again after %ld ms", now_ms() - t0);
}

static long file_size_or(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* What the console made of a folder tile's pictures, for the log (and a stale database entry for a folder without a
 * background is cleared). */
static void diag_folder_pictures(hh_t *h, const char *tid) {
  char a[600], m1[600], m2[600];
  snprintf(a, sizeof(a), "%s/%s/sce_sys/pic0.dds", F.app_dir, tid);
  snprintf(m1, sizeof(m1), "%s/%s/pic0.dds", g_meta_dir, tid);
  snprintf(m2, sizeof(m2), "%s/%s/pic0.png", g_meta_dir, tid);
  long sa = file_size_or(a);
  hh_log("folders: %s pictures: tile pic0.dds %ld, appmeta pic0.dds %ld, pic0.png %ld", tid, sa, g_meta_dir[0] ? file_size_or(m1) : -2,
         g_meta_dir[0] ? file_size_or(m2) : -2);
  custom_folder_bg_check(h, tid, sa < 0);
}

/* Registers every folder tile that install_tile postponed, with one installer call, then puts each on
 * its tab. A folder the installer didn't pick up (a console where it installs one title at a time) gets
 * its own call. Returns the number of folders that could not be registered (0 = all fine). */
int folders_install_pending(hh_t *h) {
  int n = t_npend, bad = 0;
  char ids[FOLDER_MAX][16];
  memcpy(ids, t_pend, sizeof(ids));
  t_npend = 0;
  if (n == 0) return 0;
  hh_log("folders: registering %d folder tile(s) with one install", n);
  wait_for_other_tools();
  log_install_state(h, ids[0]);
  long t0 = now_ms();
  int err = plat_app_install_many((const char (*)[16])ids, n, F.app_dir);
  g_last_inst_took_ms = now_ms() - t0;
  g_last_inst_end_ms = now_ms();
  hh_log("folders: the install returned 0x%08X after %ld ms", (unsigned)err, g_last_inst_took_ms);
  for (int i = 0; i < n; i++) {
    int waited = 0;
    if (!err || i > 0) {
      int max = i == 0 ? 40 : 10; /* the first has waited for the others already */
      for (; waited < max && !tile_present(h, ids[i]); waited++) usleep(200000);
    }
    if (!tile_present(h, ids[i])) {
      int e2 = plat_app_install(ids[i], F.app_dir); /* its own call */
      hh_log("folders: %s was not picked up, installing it by itself: 0x%08X", ids[i], (unsigned)e2);
      for (waited = 0; !e2 && waited < 40 && !tile_present(h, ids[i]); waited++) usleep(200000);
    }
    if (!tile_present(h, ids[i])) {
      bad++;
      hh_log("folders: %s is not in app.db after the install", ids[i]);
      continue;
    }
    char tab[16];
    folder_tab(h, ids[i], tab, sizeof(tab));
    place_tile(h, ids[i], tab);
  }
  /* The install call returns after the console has dealt with one tile; with several changed tiles it keeps going
   * on its own for a while (up to a minute and a half), and a home screen restart before it is done leaves the
   * later tiles with their old pictures. So wait until it has been quiet for a while. */
  if (n > 1) wait_console_settled();
  for (int i = 0; i < n; i++) diag_folder_pictures(h, ids[i]);
  hh_log("folders: %d of %d folder tile(s) registered in %ld ms", n - bad, n, now_ms() - t0);
  return bad;
}

/* The tab the folder is set to live on. */
static void folder_tab(hh_t *h, const char *tid, char *tab, size_t n) {
  snprintf(tab, n, "games");
  sqlite3_stmt *st = prep(h, "SELECT tab FROM hh.folders WHERE title_id=?1");
  if (!st) return;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) snprintf(tab, n, "%s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
}

/* The folder's current name and the finished picture (written by write_tile)
 * into the home screen database. 0 on success; -1 with h->err if it can't. */
static int refresh_in_place(hh_t *h, const char *tid) {
  char name[FOLDER_NAME_MAX * 2] = "", path[520];
  sqlite3_stmt *st = prep(h, "SELECT name FROM hh.folders WHERE title_id=?1");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) snprintf(name, sizeof(name), "%s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  snprintf(path, sizeof(path), "%s/%s/sce_sys/icon0.png", F.app_dir, tid);
  unsigned char *png = NULL;
  size_t png_len = 0;
  if (read_file(path, &png, &png_len)) {
    ferr(h, "Couldn't read the folder's picture back.%s", "");
    return -1;
  }
  int rc = custom_refresh_folder_tile(h, tid, name, png, png_len);
  free(png);
  return rc;
}

/* Brings the folder's tile up to date after a change. A tile that is already
 * in the home screen database is updated there directly (seconds); a new tile,
 * or one that can't be updated that way, goes through the console's installer
 * (about 90 seconds on newer firmware). */
static int update_tile(hh_t *h, const char *tid, char *note, size_t notesz) {
  if (tile_present(h, tid)) {
    long t0 = now_ms();
    if (write_tile(h, tid) == 0) {
      hh_log("folder %s: tile files written to %s/%s", tid, F.app_dir, tid);
      if (refresh_in_place(h, tid) == 0) {
        char tab[16];
        folder_tab(h, tid, tab, sizeof(tab));
        int moved = place_tile(h, tid, tab);
        hh_log("folder %s: updated without the installer in %ld ms (placed on %s tab, changed=%d)", tid,
               now_ms() - t0, tab, moved);
        return 0;
      }
      hh_log("folder %s: couldn't update it in the database (%s), using the installer", tid, h->err);
    } else {
      hh_log("folder %s: writing tile files failed: %s", tid, h->err);
      return -1;
    }
  }
  return install_tile(h, tid, note, notesz);
}

/* ------------------------------------------------------------------------- */
/* JSON                                                                      */
/* ------------------------------------------------------------------------- */

static void folder_json(hh_t *h, const char *tid, strbuf_t *sb) {
  sqlite3_stmt *st = prep(h, "SELECT name, tab, color, custom_icon, style FROM hh.folders WHERE title_id=?1");
  if (!st) return;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    return;
  }
  sb_printf(sb, "{\"id\":\"%s\",\"name\":", tid);
  sb_json_str(sb, (const char *)sqlite3_column_text(st, 0));
  sb_puts(sb, ",\"tab\":");
  sb_json_str(sb, (const char *)sqlite3_column_text(st, 1));
  sb_puts(sb, ",\"color\":");
  sb_json_str(sb, (const char *)sqlite3_column_text(st, 2));
  sb_printf(sb, ",\"custom_icon\":%s,\"style\":%d", sqlite3_column_int(st, 3) ? "true" : "false", sqlite3_column_int(st, 4));
  sqlite3_finalize(st);
  char bgp[400];
  sb_printf(sb, ",\"custom_bg\":%s", folders_bg_path(tid, bgp, sizeof(bgp)) == 0 ? "true" : "false");
  char fname[200];
  long long frecent = 0;
  title_info(h, tid, fname, sizeof(fname), &frecent); /* the folder tile's home-screen order */
  sb_printf(sb, ",\"recent\":%lld", frecent);
  sb_printf(sb, ",\"installed\":%s,\"items\":[", tile_present(h, tid) ? "true" : "false");

  st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 ORDER BY pos");
  int first = 1;
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *it = (const char *)sqlite3_column_text(st, 0);
      char name[200];
      long long recent;
      title_info(h, it, name, sizeof(name), &recent);
      sb_printf(sb, "%s{\"id\":\"%s\",\"name\":", first ? "" : ",", it);
      sb_json_str(sb, name);
      sb_printf(sb, ",\"recent\":%lld}", recent);
      first = 0;
    }
    sqlite3_finalize(st);
  }
  sb_puts(sb, "]}");
}

/* Title ids of all folders Home Screen Manager made. Returns how many were put in ids. */
int folders_ids(hh_t *h, char (*ids)[16], int max) {
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folders ORDER BY created_at, title_id");
  int n = 0;
  while (st && n < max && sqlite3_step(st) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(st, 0);
    if (v) snprintf(ids[n++], 16, "%.15s", v);
  }
  if (st) sqlite3_finalize(st);
  return n;
}

int folders_list_json(hh_t *h, strbuf_t *sb) {
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folders ORDER BY created_at, title_id");
  if (!st) return -1;
  char ids[FOLDER_MAX][16];
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_MAX)
    snprintf(ids[n++], 16, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  sb_puts(sb, "[");
  for (int i = 0; i < n; i++) {
    if (i) sb_puts(sb, ",");
    folder_json(h, ids[i], sb);
  }
  sb_puts(sb, "]");
  return 0;
}

int folders_get_json(hh_t *h, const char *tid, strbuf_t *sb) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  folder_json(h, tid, sb);
  return 0;
}

int folders_of_title(hh_t *h, const char *tid, char *out, size_t outsz) {
  out[0] = '\0';
  sqlite3_stmt *st = prep(h, "SELECT folder FROM hh.folder_items WHERE title_id=?1 ORDER BY folder LIMIT 1");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) snprintf(out, outsz, "%.15s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* save / delete                                                             */
/* ------------------------------------------------------------------------- */

static int clean_name(const char *in, char *out, size_t outsz) {
  while (*in == ' ') in++;
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < outsz; p++) {
    if (*p < 0x20 || *p == 0x7f) continue;
    out[n++] = (char)*p;
  }
  while (n && out[n - 1] == ' ') n--;
  /* never cut a UTF-8 sequence in half */
  while (n && ((unsigned char)out[n - 1] & 0xC0) == 0x80 && n == outsz - 1) n--;
  out[n] = '\0';
  return n > 0 && n <= FOLDER_NAME_MAX ? 0 : -1;
}

/* 1 if the folder already has exactly this name, tab, colour and games in this
 * order (and its tile is in the home screen database), so saving would do nothing. */
static int folder_unchanged(hh_t *h, const char *tid, const char *name, const char *tab, const char *color,
                            int style, char (*items)[16], int n) {
  if (!tile_present(h, tid)) return 0;
  sqlite3_stmt *st = prep(h, "SELECT name, tab, color, style FROM hh.folders WHERE title_id=?1");
  if (!st) return 0;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  int same = 0;
  if (sqlite3_step(st) == SQLITE_ROW)
    same = !strcmp(name, (const char *)sqlite3_column_text(st, 0)) &&
           !strcmp(tab, (const char *)sqlite3_column_text(st, 1)) &&
           !strcmp(color, (const char *)sqlite3_column_text(st, 2)) &&
           (style < 0 || style == sqlite3_column_int(st, 3));
  sqlite3_finalize(st);
  if (!same) return 0;
  st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 ORDER BY pos");
  if (!st) return 0;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  int i = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (i >= n || strcmp(items[i], (const char *)sqlite3_column_text(st, 0))) {
      same = 0;
      break;
    }
    i++;
  }
  sqlite3_finalize(st);
  return same && i == n;
}

static int folders_save_impl(hh_t *h, const char *tid_in, const char *name_in, const char *tab,
                             const char *color, int style, const char *items_csv, char *out_tid,
                             char *note, size_t notesz, int *changed) {
  long t_start = now_ms();
  if (changed) *changed = 1;
  if (note) note[0] = '\0';
  hh_log("folder save: id='%s' tab=%s color=%s items='%.200s'", tid_in ? tid_in : "", tab ? tab : "",
         color ? color : "", items_csv ? items_csv : "");
  if (h->nusers == 0) {
    ferr(h, "The home screen database layout isn't supported.%s", "");
    return -1;
  }
  char name[FOLDER_NAME_MAX + 8];
  if (!name_in || clean_name(name_in, name, sizeof(name))) {
    ferr(h, "Give the folder a name (up to %s characters).", "48");
    return -1;
  }
  if (!tab || (strcmp(tab, "games") && strcmp(tab, "media"))) tab = "games";
  if (!icon_valid_color(color)) color = "blue";
  if (style >= ICON_STYLES || style < -1) style = 0;

  /* parse and check the games */
  char (*items)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  if (!items) return -1;
  int n = 0;
  const char *p = items_csv ? items_csv : "";
  while (*p && n < FOLDER_ITEMS_MAX) {
    size_t len = strcspn(p, ",");
    char t[32];
    if (len > 0 && len < sizeof(t)) {
      memcpy(t, p, len);
      t[len] = '\0';
      int dup = 0;
      for (int i = 0; i < n; i++) dup |= !strcmp(items[i], t);
      if (!dup && hh_valid_tid(t) && !folders_is_folder(t) && !hh_is_locked(t) && !hh_is_internal(t))
        snprintf(items[n++], 16, "%.15s", t);
    }
    p += len;
    if (*p == ',') p++;
  }

  /* folder id */
  char tid[16] = "";
  if (tid_in && tid_in[0]) {
    if (!folders_is_folder(tid_in) ||
        count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid_in) == 0) {
      free(items);
      ferr(h, "That folder doesn't exist.%s", "");
      return -1;
    }
    snprintf(tid, sizeof(tid), "%s", tid_in);
  } else {
    for (int i = 1; i <= FOLDER_MAX && !tid[0]; i++) {
      char cand[16];
      snprintf(cand, sizeof(cand), FOLDER_PREFIX "%05d", i);
      if (count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", cand) == 0 &&
          !tile_present(h, cand))
        snprintf(tid, sizeof(tid), "%s", cand);
    }
    if (!tid[0]) {
      free(items);
      ferr(h, "You already have the maximum of %s folders.", "99");
      return -1;
    }
  }

  /* an existing folder that is already exactly like this: nothing to do */
  if (tid_in && tid_in[0] && folder_unchanged(h, tid, name, tab, color, style, items, n)) {
    hh_log("folder %s: nothing changed, nothing done", tid);
    if (changed) *changed = 0;
    snprintf(out_tid, 16, "%s", tid);
    free(items);
    return 0;
  }

  /* 1. records (one transaction) */
  char (*removed)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  int *removed_hidden = calloc(FOLDER_ITEMS_MAX, sizeof(int));
  int nrem = 0;
  if (!removed || !removed_hidden) goto oom;
  if (run1(h, "BEGIN IMMEDIATE", NULL, NULL)) goto fail;
  {
    sqlite3_stmt *st = prep(h,
        "INSERT INTO hh.folders(title_id, name, tab, color, custom_icon, created_at, style) "
        "VALUES(?1, ?2, ?3, ?4, 0, datetime('now'), MAX(?5, 0)) ON CONFLICT(title_id) DO UPDATE SET "
        "name=excluded.name, tab=excluded.tab, color=excluded.color, style=CASE WHEN ?5 < 0 THEN style ELSE ?5 END");
    if (!st) goto rollback;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tab, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, color, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, style);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
      ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
      goto rollback;
    }

    /* games that left this folder */
    st = prep(h, "SELECT title_id, was_hidden FROM hh.folder_items WHERE folder=?1");
    if (!st) goto rollback;
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && nrem < FOLDER_ITEMS_MAX) {
      const char *it = (const char *)sqlite3_column_text(st, 0);
      int keep = 0;
      for (int i = 0; i < n && !keep; i++) keep = !strcmp(items[i], it);
      if (keep) continue;
      snprintf(removed[nrem], 16, "%s", it);
      removed_hidden[nrem++] = sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
    /* leaving this folder only: the game's rows in other folders stay */
    for (int i = 0; i < nrem; i++) {
      st = prep(h, "DELETE FROM hh.folder_items WHERE title_id=?1 AND folder=?2");
      if (!st) goto rollback;
      sqlite3_bind_text(st, 1, removed[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, tid, -1, SQLITE_TRANSIENT);
      rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE) {
        ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
        goto rollback;
      }
    }

    /* games in the folder now, in order. A game already in another folder
     * keeps that folder's was_hidden: it was hidden by the folder, not by the
     * user, so the hide list can't be used to work it out a second time. */
    for (int i = 0; i < n; i++) {
      st = prep(h,
          "INSERT INTO hh.folder_items(title_id, folder, pos, was_hidden) VALUES(?1, ?2, ?3, "
          " COALESCE((SELECT was_hidden FROM hh.folder_items WHERE title_id=?1 LIMIT 1),"
          "           (SELECT count(*) FROM hh.wanted WHERE title_id=?1))) "
          "ON CONFLICT(title_id, folder) DO UPDATE SET pos=excluded.pos");
      if (!st) goto rollback;
      sqlite3_bind_text(st, 1, items[i], -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, tid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(st, 3, i);
      rc = sqlite3_step(st);
      sqlite3_finalize(st);
      if (rc != SQLITE_DONE) {
        ferr(h, "Database error: %s", sqlite3_errmsg(h->db));
        goto rollback;
      }
    }
  }
  if (run1(h, "COMMIT", NULL, NULL)) goto rollback;

  /* 2. hide the games inside, show the ones that left */
  int missing = 0;
  for (int i = 0; i < n; i++)
    if (hh_hide(h, items[i])) missing++;
  for (int i = 0; i < nrem; i++)
    if (!removed_hidden[i] &&
        count1(h, "SELECT count(*) FROM hh.folder_items WHERE title_id=?1", removed[i]) == 0)
      hh_unhide(h, removed[i]); /* left its last folder */

  /* 3. the tile itself */
  int rc = update_tile(h, tid, note, notesz);
  if (rc) goto fail;
  hh_log("folder %s: saved, %d game(s), %d hide failure(s), %d removed (%ld ms in all)", tid, n, missing, nrem,
         now_ms() - t_start);
  if (missing && note && !note[0])
    snprintf(note, notesz, "%d game(s) couldn't be hidden from the home screen.", missing);

  snprintf(out_tid, 16, "%s", tid);
  free(items);
  free(removed);
  free(removed_hidden);
  return 0;

rollback:
  run1(h, "ROLLBACK", NULL, NULL);
  goto fail;
oom:
  ferr(h, "Out of memory.%s", "");
fail:
  hh_log("folder save failed: %s", h->err);
  free(items);
  free(removed);
  free(removed_hidden);
  return -1;
}

int folders_save(hh_t *h, const char *tid_in, const char *name_in, const char *tab, const char *color, int style,
                 const char *items_csv, char *out_tid, char *note, size_t notesz, int *changed) {
  __sync_fetch_and_add(&g_busy, 1);
  int rc = folders_save_impl(h, tid_in, name_in, tab, color, style, items_csv, out_tid, note, notesz, changed);
  __sync_fetch_and_sub(&g_busy, 1);
  return rc;
}

static char g_meta_dir[256];
void folders_set_appmeta_dir(const char *d) { snprintf(g_meta_dir, sizeof(g_meta_dir), "%s", d ? d : ""); }
static void rm_tree(const char *path, int depth);
/* the console keeps a copy of a tile's data under appmeta too; it can bring the tile back later */
static void remove_meta_dir(const char *tid) {
  if (!g_meta_dir[0] || !folders_is_folder(tid)) return;
  char p[600];
  snprintf(p, sizeof(p), "%s/%s", g_meta_dir, tid);
  rm_tree(p, 0);
}

static void remove_tile_files(const char *tid) {
  char p[500];
  snprintf(p, sizeof(p), "%s/%s/sce_sys/param.json", F.app_dir, tid);
  unlink(p);
  snprintf(p, sizeof(p), "%s/%s/sce_sys/icon0.png", F.app_dir, tid);
  unlink(p);
  snprintf(p, sizeof(p), "%s/%s/sce_sys/pic0.dds", F.app_dir, tid);
  unlink(p);
  snprintf(p, sizeof(p), "%s/%s/sce_sys", F.app_dir, tid);
  rmdir(p);
  snprintf(p, sizeof(p), "%s/%s", F.app_dir, tid);
  rmdir(p);
}

int folders_set_tab(hh_t *h, const char *tid, const char *tab) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  if (!tab || (strcmp(tab, "games") && strcmp(tab, "media"))) {
    ferr(h, "Choose the Games or Media tab.%s", "");
    return -1;
  }
  if (run1(h, "UPDATE hh.folders SET tab=?2 WHERE title_id=?1", tid, tab)) return -1;
  place_tile(h, tid, tab);
  hh_log("folder %s: moved to the %s tab", tid, tab);
  return 0;
}

/* Takes one app out of a folder. It goes back on its tab once it is out of its
 * last folder. folder may be NULL/"" when the app is in only one. */
int folders_remove_member(hh_t *h, const char *tid, const char *folder_in) {
  char folder[16] = "";
  if (folders_of_title(h, tid, folder, sizeof(folder)) || !folder[0]) {
    ferr(h, "That app isn't in a folder.%s", "");
    return -1;
  }
  if (folder_in && folder_in[0]) {
    if (!folders_is_folder(folder_in) || !is_member(h, tid, folder_in)) {
      ferr(h, "That app isn't in that folder.%s", "");
      return -1;
    }
    snprintf(folder, sizeof(folder), "%.15s", folder_in);
  } else if (count1(h, "SELECT count(*) FROM hh.folder_items WHERE title_id=?1", tid) > 1) {
    ferr(h, "This app is in more than one folder. Choose which folder to take it out of.%s", "");
    return -1;
  }
  /* the remaining members, keeping their order */
  char (*keep)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  if (!keep) return -1;
  int n = 0;
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 AND title_id<>?2 ORDER BY pos");
  if (st) {
    sqlite3_bind_text(st, 1, folder, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_ITEMS_MAX)
      snprintf(keep[n++], 16, "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  int rc;
  if (n == 0) {
    rc = folders_delete(h, folder); /* last one out: the folder goes too */
  } else {
    char name[FOLDER_NAME_MAX * 4 + 8] = "", tab[8] = "games", color[16] = "blue";
    sqlite3_stmt *f = prep(h, "SELECT name, tab, color FROM hh.folders WHERE title_id=?1");
    if (f) {
      sqlite3_bind_text(f, 1, folder, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(f) == SQLITE_ROW) {
        snprintf(name, sizeof(name), "%s", sqlite3_column_text(f, 0));
        snprintf(tab, sizeof(tab), "%s", sqlite3_column_text(f, 1));
        snprintf(color, sizeof(color), "%s", sqlite3_column_text(f, 2));
      }
      sqlite3_finalize(f);
    }
    strbuf_t items;
    sb_init(&items);
    for (int i = 0; i < n; i++) sb_printf(&items, "%s%s", i ? "," : "", keep[i]);
    char out_id[16], note[160];
    rc = items.oom ? -1 : folders_save(h, folder, name, tab, color, -1, items.data, out_id, note, sizeof(note), NULL);
    sb_free(&items);
  }
  free(keep);
  if (rc == 0) hh_log("folder %s: took %s out", folder, tid);
  return rc;
}

int folders_delete(hh_t *h, const char *tid) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  /* show the games again (unless they were hidden before joining, or are
   * still in another folder) */
  char (*show)[16] = calloc(FOLDER_ITEMS_MAX, 16);
  int n = 0;
  if (!show) return -1;
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folder_items WHERE folder=?1 AND was_hidden=0 "
                         "AND title_id NOT IN (SELECT title_id FROM hh.folder_items WHERE folder<>?1)");
  if (st) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_ITEMS_MAX)
      snprintf(show[n++], 16, "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  if (run1(h, "DELETE FROM hh.folder_items WHERE folder=?1", tid, NULL) ||
      run1(h, "DELETE FROM hh.folder_meta WHERE title_id=?1", tid, NULL) ||
      run1(h, "DELETE FROM hh.folders WHERE title_id=?1", tid, NULL)) {
    free(show);
    return -1;
  }
  for (int i = 0; i < n; i++) hh_unhide(h, show[i]);
  free(show);

  int un = plat_app_uninstall(tid);
  purge_tile_rows(h, tid);
  hh_log("folder %s: deleted (uninstall 0x%08X, %d game(s) shown)", tid, (unsigned)un, n);
  note_folder_deleted(tid);
  remove_tile_files(tid);
  remove_meta_dir(tid);
  char cpath[400];
  custom_icon_path(tid, cpath, sizeof(cpath));
  unlink(cpath);
  bg_dds_path(tid, cpath, sizeof(cpath));
  unlink(cpath);
  bg_jpg_path(tid, cpath, sizeof(cpath));
  unlink(cpath);
  return 0;
}

static int folders_set_icon_impl(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  char cpath[400], dir[400];
  custom_icon_path(tid, cpath, sizeof(cpath));
  if (img) {
    unsigned char *png;
    size_t pl;
    if (icon_from_image(img, len, NULL, &png, &pl)) {
      ferr(h, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).%s", "");
      return -1;
    }
    snprintf(dir, sizeof(dir), "%s/folders", F.data_dir);
    mkdirs(dir);
    int rc = write_file(cpath, png, pl);
    free(png);
    if (rc) {
      ferr(h, "Couldn't save the picture.%s", "");
      return -1;
    }
  } else {
    unlink(cpath);
  }
  if (run1(h, "UPDATE hh.folders SET custom_icon=?2 WHERE title_id=?1", tid, img ? "1" : "0"))
    return -1;
  return update_tile(h, tid, NULL, 0);
}

static int folders_set_bg_impl(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  if (!folders_is_folder(tid) ||
      count1(h, "SELECT count(*) FROM hh.folders WHERE title_id=?1", tid) == 0) {
    ferr(h, "That folder doesn't exist.%s", "");
    return -1;
  }
  char dds_path[400], jpg_path[400], dir[400];
  bg_dds_path(tid, dds_path, sizeof(dds_path));
  bg_jpg_path(tid, jpg_path, sizeof(jpg_path));
  if (img) {
    /* a 4K BC7 texture, like the ones the console makes for its own apps */
    tex_fmt_t f = {TEX_DDS, TEX_BG_W, TEX_BG_H, 1, 98, 0};
    unsigned char *dds = NULL, *jpg = NULL;
    size_t dl = 0, jl = 0;
    if (tex_encode(img, len, &f, &dds, &dl)) {
      ferr(h, "That picture couldn't be read. Use a PNG, JPG, BMP, GIF or TGA picture (HEIC and WebP aren't supported).%s", "");
      return -1;
    }
    uint8_t *rgba = icon_load_cover(img, len, TEX_BG_W, TEX_BG_H);
    if (rgba) img_encode_jpg(rgba, TEX_BG_W, TEX_BG_H, 90, &jpg, &jl);
    free(rgba);
    snprintf(dir, sizeof(dir), "%s/folders", F.data_dir);
    mkdirs(dir);
    int rc = write_file(dds_path, dds, dl) || (jpg && write_file(jpg_path, jpg, jl));
    free(dds);
    free(jpg);
    if (rc) {
      ferr(h, "Couldn't save the background.%s", "");
      return -1;
    }
    hh_log("folder %s: background set (%zu bytes)", tid, dl);
  } else {
    unlink(dds_path);
    unlink(jpg_path);
    hh_log("folder %s: background removed", tid);
    /* In a bulk restore (install postponed) the tile is already registered: taking the picture away only needs
     * the tile's picture file gone and the database entry cleared. The install (about 100 s, and the home screen
     * moves games around meanwhile) is not needed for that. */
    char tile_pic[420];
    snprintf(tile_pic, sizeof(tile_pic), "%s/%s/sce_sys/pic0.dds", F.app_dir, tid);
    if (t_defer && tile_present(h, tid)) {
      unlink(tile_pic);
      custom_folder_bg_check(h, tid, 1);
      hh_log("folder %s: background taken off the tile without an install", tid);
      return 0;
    }
  }
  return install_tile(h, tid, NULL, 0);
}

int folders_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  __sync_fetch_and_add(&g_busy, 1);
  int rc = folders_set_icon_impl(h, tid, img, len);
  __sync_fetch_and_sub(&g_busy, 1);
  return rc;
}

int folders_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len) {
  __sync_fetch_and_add(&g_busy, 1);
  int rc = folders_set_bg_impl(h, tid, img, len);
  __sync_fetch_and_sub(&g_busy, 1);
  return rc;
}

/* The name the folder is set to have. */
static void folder_name(hh_t *h, const char *tid, char *name, size_t n) {
  name[0] = '\0';
  sqlite3_stmt *st = prep(h, "SELECT name FROM hh.folders WHERE title_id=?1");
  if (!st) return;
  sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) snprintf(name, n, "%s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
}

/* Name right again on a tile that is there (0/1: something was changed). */
static int fix_tile_name(hh_t *h, const char *tid) {
  char name[FOLDER_NAME_MAX * 2];
  folder_name(h, tid, name, sizeof(name));
  /* the tile's own file keeps the name too: a database rebuild by the console would bring an old one back */
  if (name[0]) {
    char path[520], want[FOLDER_NAME_MAX * 2 + 8];
    unsigned char *old = NULL;
    size_t ol = 0;
    snprintf(path, sizeof(path), "%s/%s/sce_sys/param.json", F.app_dir, tid);
    strbuf_t q;
    sb_init(&q);
    sb_json_str(&q, name);
    snprintf(want, sizeof(want), "%s", q.data ? q.data : "");
    sb_free(&q);
    if (read_file(path, &old, &ol) == 0) {
      char *z = realloc(old, ol + 1);
      if (z) {
        old = (unsigned char *)z;
        old[ol] = 0;
        if (want[0] && !strstr((char *)old, want) && write_param(h, tid, name, path) == 0)
          hh_log("folder %s: the tile's own file had an old name, now \"%s\"", tid, name);
      }
      free(old);
    }
  }
  return custom_fix_folder_name(h, tid, name) > 0;
}

static int reapply_impl(hh_t *h, int *fixed, int patient, long quiet) {
  *fixed = 0;
  if (h->nusers == 0) return 0;
  if (g_busy > 0) return 0; /* a folder is being saved: its tile is on its way (see g_busy) */
  sqlite3_stmt *st = prep(h, "SELECT title_id, tab FROM hh.folders");
  if (!st) return -1;
  char ids[FOLDER_MAX][16], tabs[FOLDER_MAX][8];
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_MAX) {
    snprintf(ids[n], 16, "%s", sqlite3_column_text(st, 0));
    snprintf(tabs[n], 8, "%s", sqlite3_column_text(st, 1));
    n++;
  }
  sqlite3_finalize(st);
  /* A folder tile that looks missing starts the console's installer, which takes about 90 seconds and
   * puts every custom name and background through a repair. So a tile is only called missing when the
   * database answers properly, and when it is still missing a moment later. */
  char missing[FOLDER_MAX];
  int nmiss = 0;
  for (int i = 0; i < n; i++) {
    char p[500];
    struct stat sb;
    snprintf(p, sizeof(p), "%s/%s/sce_sys/param.json", F.app_dir, ids[i]);
    int tile = tile_present(h, ids[i]), file = stat(p, &sb) == 0;
    missing[i] = !tile || !file;
    if (missing[i]) {
      nmiss++;
      hh_log("folder repair: %s looks missing (tile in app.db: %s, param.json: %s)", ids[i], tile ? "yes" : "no", file ? "yes" : "no");
    }
  }
  /* While the watchdog runs, another tool may be re-registering games (PS4 Mount does this for every game
   * it adds, which can take minutes for many games): the console then hides the folder tiles until that is
   * done. So missing folders are only repaired when they have been missing for a while AND the home screen
   * database has been quiet for a while (nobody else is registering games any more), instead of starting a
   * 90 second install of our own in the middle of it. After a long time the repair goes ahead anyway. */
  static time_t since;
  if (patient) {
    if (!nmiss) {
      /* nothing is missing: still put every tile back on its tab (another tool's re-registering moves
       * them), which is a cheap check that only writes when a tile is off its place */
      since = 0;
      for (int i = 0; i < n; i++)
        if (place_tile(h, ids[i], tabs[i]) | fix_tile_name(h, ids[i])) (*fixed)++;
      return 0;
    }
    long wait = 45, calm = 30, most = 900;
    const char *e1 = getenv("HH_REPAIR_GRACE"), *e2 = getenv("HH_REPAIR_QUIET"), *e3 = getenv("HH_REPAIR_MAX");
    if (e1 && *e1) wait = atol(e1);
    if (e2 && *e2) calm = atol(e2);
    if (e3 && *e3) most = atol(e3);
    time_t now = time(NULL);
    if (!since) {
      since = now;
      hh_log("folder repair: waiting in case another tool is registering games (at least %ld s, and until the database is quiet for %ld s) before reinstalling %d folder(s)", wait, calm, nmiss);
    }
    long age = (long)(now - since);
    if (age < wait || (quiet < calm && age < most)) return 0;
    since = 0;
  }
  if (nmiss) {
    if (!db_answers(h)) {
      hh_log("folder repair: the database didn't answer properly, trying again later");
      return 0;
    }
    usleep(2500000);
    nmiss = 0;
    for (int i = 0; i < n; i++) {
      char p[500];
      struct stat sb;
      snprintf(p, sizeof(p), "%s/%s/sce_sys/param.json", F.app_dir, ids[i]);
      if (missing[i] && (tile_present(h, ids[i]) && stat(p, &sb) == 0)) {
        hh_log("folder repair: %s is there after all", ids[i]);
        missing[i] = 0;
      }
      nmiss += missing[i];
    }
  }
  folders_defer_install(1); /* several missing tiles need only one install */
  for (int i = 0; i < n; i++) {
    if (missing[i]) {
      if (install_tile(h, ids[i], NULL, 0) == 0) (*fixed)++;
    } else if (place_tile(h, ids[i], tabs[i]) | fix_tile_name(h, ids[i])) {
      (*fixed)++;
    }
  }
  folders_defer_install(0);
  folders_install_pending(h);
  return 0;
}

/* Removes a directory and everything in it. Only used on folder tile directories of ours (HHFD...). */
static void rm_tree(const char *path, int depth) {
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char p[600];
      snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
      struct stat sb;
      if (lstat(p, &sb) != 0) continue;
      if (S_ISDIR(sb.st_mode)) {
        if (depth < 6) rm_tree(p, depth + 1);
      } else {
        unlink(p);
      }
    }
    closedir(d);
  }
  rmdir(path);
}

/* A folder tile with no folder behind it (one that was deleted, or left from an older version) comes
 * back as a blank tile every time another tool re-registers the games, because the console finds its
 * directory again. This removes the tile and the whole directory, for ids that start with HHFD and are
 * not in the list of folders. */
static int g_sweep_tiles;
int folders_sweep_tiles_removed(void) {
  int n = g_sweep_tiles;
  g_sweep_tiles = 0;
  return n;
}

int folders_sweep_orphans(hh_t *h, int *removed) {
  *removed = 0;
  if (h->nusers == 0 || g_busy > 0) return 0;
  char cand[64][16];
  int nc = 0;
  DIR *d = opendir(F.app_dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) && nc < 64)
      if (folders_is_folder(e->d_name) && strlen(e->d_name) < 16) snprintf(cand[nc++], 16, "%s", e->d_name);
    closedir(d);
  }
  for (int u = 0; u < h->nusers; u++) {
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT titleId FROM main.\"tbl_iconinfo_%s\" WHERE titleId LIKE 'HHFD%%'", h->users[u]);
    sqlite3_stmt *q = prep(h, sql);
    while (q && sqlite3_step(q) == SQLITE_ROW && nc < 64) {
      const char *t = (const char *)sqlite3_column_text(q, 0);
      int dup = 0;
      for (int i = 0; t && i < nc; i++) dup |= !strcmp(cand[i], t);
      if (t && !dup && folders_is_folder(t) && strlen(t) < 16) snprintf(cand[nc++], 16, "%s", t);
    }
    if (q) sqlite3_finalize(q);
  }
  /* The folders are read AFTER the directories and tiles were listed: a folder that is being created has
   * its record first and its files afterwards, so anything listed above has its record by now. Reading
   * them in the other order could take a folder that was just created for a leftover one. */
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folders");
  if (!st) return -1;
  char known[FOLDER_MAX][16];
  int nk = 0, rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW && nk < FOLDER_MAX)
    snprintf(known[nk++], 16, "%.15s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && nk < FOLDER_MAX) return -1; /* the list could not be read: remove nothing */

  for (int i = 0; i < nc; i++) {
    int isknown = 0;
    for (int k = 0; k < nk; k++) isknown |= !strcmp(known[k], cand[i]);
    if (isknown || g_busy > 0) continue;
    char dir[2100];
    snprintf(dir, sizeof(dir), "%s/%s", F.app_dir, cand[i]);
    struct stat sb;
    char mp[600] = "";
    if (g_meta_dir[0]) snprintf(mp, sizeof(mp), "%s/%s", g_meta_dir, cand[i]);
    int had_dir = stat(dir, &sb) == 0, had_meta = mp[0] && stat(mp, &sb) == 0, had_tile = tile_present(h, cand[i]);
    int un = plat_app_uninstall(cand[i]);
    purge_tile_rows(h, cand[i]);
    rm_tree(dir, 0);
    remove_meta_dir(cand[i]);
    hh_log("folder %s: no folder of ours behind this tile, removed it and its files (uninstall 0x%08X; found: directory %s, appmeta copy %s, tile in app.db %s; tile in app.db afterwards: %s)",
           cand[i], (unsigned)un, had_dir ? "yes" : "no", had_meta ? "yes" : "no", had_tile ? "yes" : "no",
           tile_present(h, cand[i]) ? "yes" : "no");
    if (had_tile) g_sweep_tiles++; /* it was on the home screen: that needs a redraw */
    (*removed)++;
  }
  return 0;
}


int folders_preview(hh_t *h, const char *items_csv, const char *color, int style,
                    unsigned char **png, size_t *png_len) {
  char paths[ICON_COLLAGE_MAX][512];
  const char *ptrs[ICON_COLLAGE_MAX] = {NULL};
  int n = 0;
  const char *p = items_csv ? items_csv : "";
  while (*p && n < ICON_COLLAGE_MAX) {
    char tid[16];
    size_t k = 0;
    while (p[k] && p[k] != ',' && k < sizeof(tid) - 1) {
      tid[k] = p[k];
      k++;
    }
    tid[k] = '\0';
    p += k;
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
    if (hh_icon_path(h, tid, paths[n], sizeof(paths[n])) == 0) ptrs[n] = paths[n];
    n++;
  }
  if (!icon_valid_color(color)) color = "blue";
  if (style < 0 || style >= ICON_STYLES) style = 0;
  return icon_collage(ptrs, n, color, style, png, png_len);
}

const char *folders_app_dir(void) { return F.app_dir; }

int folders_reapply(hh_t *h, int *fixed) { return reapply_impl(h, fixed, 0, 0); }
int folders_reapply_patient(hh_t *h, int *fixed, long quiet) { return reapply_impl(h, fixed, 1, quiet); }

void folders_remove_all_tiles(hh_t *h) {
  sqlite3_stmt *st = prep(h, "SELECT title_id FROM hh.folders");
  if (!st) return;
  char ids[FOLDER_MAX][16];
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW && n < FOLDER_MAX)
    snprintf(ids[n++], 16, "%s", sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  for (int i = 0; i < n; i++) {
    plat_app_uninstall(ids[i]);
    remove_tile_files(ids[i]);
  }
}

/* A folder without a background whose database entry still points at a file that is gone (left by taking a background
 * away) shows black for a while on the home screen: clear such entries. */
int folders_clear_stale_bg(hh_t *h) {
  char ids[FOLDER_MAX][16];
  int n = folders_ids(h, ids, FOLDER_MAX), cleared = 0;
  for (int i = 0; i < n; i++) {
    char bgp[400];
    if (folders_bg_path(ids[i], bgp, sizeof(bgp)) == 0) continue;
    cleared += custom_folder_bg_check(h, ids[i], 1);
  }
  return cleared;
}
