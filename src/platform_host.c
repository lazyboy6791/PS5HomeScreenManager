/* Desktop stand-ins for the PS5 services, used by the host test build. */
#include "platform.h"
#include "log.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "third_party/sqlite/sqlite3.h"

void plat_init(void) {}

void plat_notify(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("[notify] ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  fflush(stdout);
}

int plat_launch_browser(const char *url) {
  printf("[browser] would open %s\n", url);
  fflush(stdout);
  return 0;
}

int plat_get_ip(char *buf, size_t n) {
  snprintf(buf, n, "127.0.0.1");
  return 0;
}

/* Pretend launcher. One game runs at a time, like on the console:
 *   HH_FAKE_RUNNING=PPSA02000      a game already running at start
 *   HH_FAKE_LAUNCH=TID=0x80940031  force an error code for a title */
static char g_game[16] = "";
static uint32_t g_game_id = 0;
static uint32_t g_next_id = 0x60000100;
static int g_game_init = 0;

static void game_init(void) {
  if (g_game_init) return;
  g_game_init = 1;
  const char *r = getenv("HH_FAKE_RUNNING");
  if (r && strlen(r) == 9) {
    snprintf(g_game, sizeof(g_game), "%s", r);
    g_game_id = g_next_id++;
  }
}

static int fake_code(const char *tid) {
  const char *m = getenv("HH_FAKE_LAUNCH");
  if (!m) return 0;
  const char *p = strstr(m, tid);
  if (!p || p[9] != '=') return 0;
  return (int)strtoul(p + 10, NULL, 16);
}

int plat_launch_app(const char *title_id, uint32_t *app_id) {
  game_init();
  *app_id = 0;
  int forced = fake_code(title_id);
  if (forced) {
    printf("[launch] %s -> 0x%08X (forced)\n", title_id, (unsigned)forced);
    fflush(stdout);
    return forced;
  }
  int game = strncmp(title_id, "NPXS", 4) != 0;
  if (game && g_game[0] && !strcmp(g_game, title_id)) {
    printf("[launch] %s is already running\n", title_id);
    fflush(stdout);
    return (int)LNC_ERR_ALREADY_RUNNING;
  }
  if (game && g_game[0]) {
    printf("[launch] %s refused: %s is running\n", title_id, g_game);
    fflush(stdout);
    return (int)LNC_ERR_KILL_NEEDED;
  }
  *app_id = g_next_id++;
  if (game) {
    snprintf(g_game, sizeof(g_game), "%s", title_id);
    g_game_id = *app_id;
  }
  printf("[launch] would start %s (app %08x)\n", title_id, *app_id);
  fflush(stdout);
  return 0;
}

int plat_app_id(const char *title_id, uint32_t *app_id) {
  game_init();
  if (g_game[0] && !strcmp(g_game, title_id)) {
    *app_id = g_game_id;
    return 0;
  }
  if (!strcmp(title_id, "NPXS40169")) { /* the browser showing the menu */
    *app_id = 0x60000001;
    return 0;
  }
  return -1;
}

int plat_running_game(char *title_id, size_t n, uint32_t *app_id) {
  /* tests: HH_RUNNING_FILE names a file holding the running game's title id
   * (empty or missing = none), so a game can "close" without a launch */
  const char *rf = getenv("HH_RUNNING_FILE");
  if (rf) {
    char b[32] = "";
    FILE *fp = fopen(rf, "r");
    if (fp) {
      if (fgets(b, sizeof(b), fp)) b[strcspn(b, "\r\n ")] = '\0';
      fclose(fp);
    }
    if (!b[0]) return -1;
    snprintf(title_id, n, "%s", b);
    *app_id = 0x1000;
    return 0;
  }
  game_init();
  if (!g_game[0]) return -1;
  snprintf(title_id, n, "%s", g_game);
  *app_id = g_game_id;
  return 0;
}

int plat_kill_app(uint32_t app_id, char *report, size_t n) {
  game_init();
  snprintf(report, n, "kill %08x -> ok", app_id);
  printf("[lnc] would close app %08x\n", app_id);
  fflush(stdout);
  if (app_id == g_game_id) {
    g_game[0] = '\0';
    g_game_id = 0;
  }
  return 0;
}

int plat_boot_id(char *out, size_t n) {
  const char *e = getenv("HH_BOOT_ID");
  if (e) {
    snprintf(out, n, "%s", e);
    return 0;
  }
  FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
  if (!f) return -1;
  if (!fgets(out, (int)n, f)) out[0] = '\0';
  fclose(f);
  out[strcspn(out, "\r\n")] = '\0';
  return out[0] ? 0 : -1;
}

int plat_reload_shellui(void) {
  printf("[shellui] would restart SceShellUI\n");
  fflush(stdout);
  return 0;
}

/* Plain-HTTP client for tests (the console build uses the system HTTPS stack). */
static int http_once(const char *url, int *status, char *location, size_t locsz,
                     unsigned char **out, size_t *out_len, size_t max,
                     volatile long long *progress) {
  if (strncmp(url, "http://", 7) != 0) return -2;
  char host[256], path[1024] = "/";
  const char *h = url + 7, *slash = strchr(h, '/');
  size_t hl = slash ? (size_t)(slash - h) : strlen(h);
  if (hl >= sizeof(host)) return -1;
  memcpy(host, h, hl);
  host[hl] = '\0';
  if (slash) snprintf(path, sizeof(path), "%s", slash);
  char port[8] = "80";
  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
    snprintf(port, sizeof(port), "%s", colon + 1);
  }
  struct addrinfo hints = {0}, *ai = NULL;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &ai) || !ai) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) {
    freeaddrinfo(ai);
    if (fd >= 0) close(fd);
    return -1;
  }
  freeaddrinfo(ai);
  char reqbuf[1400];
  int rl = snprintf(reqbuf, sizeof(reqbuf),
                    "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: HomeScreenManager-host\r\n\r\n", path, host);
  send(fd, reqbuf, (size_t)rl, 0);
  size_t cap = 65536, len = 0;
  unsigned char *b = malloc(cap);
  for (;;) {
    if (len == cap) {
      cap *= 2;
      b = realloc(b, cap);
    }
    ssize_t n = recv(fd, b + len, cap - len, 0);
    if (n <= 0) break;
    len += (size_t)n;
  }
  close(fd);
  unsigned char *body = NULL;
  for (size_t i = 0; i + 3 < len; i++)
    if (!memcmp(b + i, "\r\n\r\n", 4)) { body = b + i + 4; break; }
  if (!body) { free(b); return -1; }
  *status = atoi((char *)b + 9);
  location[0] = '\0';
  for (unsigned char *p = b; p < body; p++)
    if ((p == b || p[-1] == '\n') && !strncasecmp((char *)p, "location:", 9)) {
      char *v = (char *)p + 9;
      while (*v == ' ') v++;
      size_t n = strcspn(v, "\r\n");
      if (n < locsz) { memcpy(location, v, n); location[n] = '\0'; }
    }
  size_t blen = len - (size_t)(body - b);
  if (blen > max) { free(b); return -3; }
  *out = malloc(blen ? blen : 1);
  memcpy(*out, body, blen);
  *out_len = blen;
  if (progress) *progress = (long long)blen;
  free(b);
  return 0;
}

const char *plat_http_last_step(void) { return ""; }

int plat_http_get(const char *url, unsigned char **out, size_t *out_len,
                  size_t max, volatile long long *progress) {
  char cur[1024], loc[1024];
  snprintf(cur, sizeof(cur), "%s", url);
  *out = NULL;
  *out_len = 0;
  for (int hop = 0; hop < 6; hop++) {
    int status = 0;
    unsigned char *b = NULL;
    size_t bl = 0;
    int rc = http_once(cur, &status, loc, sizeof(loc), &b, &bl, max, progress);
    if (rc) return rc;
    if (status >= 300 && status < 400 && loc[0]) {
      free(b);
      snprintf(cur, sizeof(cur), "%s", loc);
      continue;
    }
    if (status != 200) {
      free(b);
      return status;
    }
    *out = b;
    *out_len = bl;
    return 200;
  }
  return -4;
}

/* Plays SceShellCore: registering a tile adds rows to app.db (on the Media
 * tab, like a real launcher tile); removing it deletes them. */
static sqlite3 *open_app_db(void) {
  const char *p = getenv("HH_APP_DB");
  sqlite3 *db = NULL;
  if (!p || sqlite3_open_v2(p, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db, 5000);
  return db;
}

int plat_app_install(const char *title_id, const char *app_dir) {
  /* tests: HH_INSTALL_DELAY_MS makes the install slow, like the console's on newer firmware */
  const char *dl = getenv("HH_INSTALL_DELAY_MS");
  int delay_ms = dl ? atoi(dl) : 0;
  hh_log("install %s: host test build%s", title_id, delay_ms > 0 ? " (slow install simulated)" : "");
  if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);
  char path[600], name[200] = "";
  snprintf(path, sizeof(path), "%s/%s/sce_sys/param.json", app_dir, title_id);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char json[4096];
  size_t n = fread(json, 1, sizeof(json) - 1, f);
  fclose(f);
  json[n] = '\0';
  char *t = strstr(json, "\"titleName\":");
  if (t) {
    t += 12;
    while (*t == ' ' || *t == '"') t++;
    size_t i = 0;
    while (*t && *t != '"' && i < sizeof(name) - 1) name[i++] = *t++;
    name[i] = '\0';
  }
  snprintf(path, sizeof(path), "%s/%s/sce_sys/icon0.png", app_dir, title_id);
  struct stat st;
  if (stat(path, &st) || st.st_size < 100) return -2;
  sqlite3 *db = open_app_db();
  if (!db) return -3;
  sqlite3_stmt *q;
  char sql[300];
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'",
                         -1, &q, NULL) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      sqlite3_stmt *ins;
      const char *tbl = (const char *)sqlite3_column_text(q, 0);
      /* like the console: the tile's picture path is registered where the table has a place for it */
      int has_icon = 0;
      char chk[300];
      snprintf(chk, sizeof(chk), "SELECT count(*) FROM pragma_table_info('%s') WHERE name='icon0Info'", tbl);
      sqlite3_stmt *c2;
      if (sqlite3_prepare_v2(db, chk, -1, &c2, NULL) == SQLITE_OK) {
        if (sqlite3_step(c2) == SQLITE_ROW) has_icon = sqlite3_column_int(c2, 0) > 0;
        sqlite3_finalize(c2);
      }
      snprintf(sql, sizeof(sql),
               has_icon ? "INSERT OR REPLACE INTO \"%s\"(titleId, titleName, dispLocation, visible, installStatus, icon0Info) "
                          "VALUES(?1, ?2, COALESCE((SELECT dispLocation FROM \"%s\" WHERE titleId=?1), 188), 1, 2, ?3)"
                        : "INSERT OR REPLACE INTO \"%s\"(titleId, titleName, dispLocation, visible, installStatus) "
                          "VALUES(?1, ?2, COALESCE((SELECT dispLocation FROM \"%s\" WHERE titleId=?1), 188), 1, 2)",
               tbl, tbl);
      if (sqlite3_prepare_v2(db, sql, -1, &ins, NULL) == SQLITE_OK) {
        char icon[600];
        const char *am = getenv("HH_APPMETA_DIR");
        snprintf(icon, sizeof(icon), "%s/%s/icon0.png", am ? am : "/user/appmeta", title_id);
        sqlite3_bind_text(ins, 1, title_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, name, -1, SQLITE_TRANSIENT);
        if (has_icon) sqlite3_bind_text(ins, 3, icon, -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_finalize(ins);

        /* and, like the console, a concept row that holds the name and the picture path */
        if (!strncmp(tbl, "tbl_iconinfo_", 13)) {
          char ctbl[160], cid[64], q2[500];
          snprintf(ctbl, sizeof(ctbl), "tbl_concepticoninfo_%s", tbl + 13);
          snprintf(cid, sizeof(cid), "cid:local:%s", title_id);
          snprintf(q2, sizeof(q2),
                   "SELECT (SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s') "
                   "AND (SELECT count(*) FROM pragma_table_info('%s') WHERE name='localConceptId')", ctbl, tbl);
          sqlite3_stmt *c3;
          int concept_ok = 0;
          if (sqlite3_prepare_v2(db, q2, -1, &c3, NULL) == SQLITE_OK) {
            if (sqlite3_step(c3) == SQLITE_ROW) concept_ok = sqlite3_column_int(c3, 0) > 0;
            sqlite3_finalize(c3);
          }
          if (concept_ok) {
            sqlite3_stmt *u3;
            snprintf(q2, sizeof(q2), "UPDATE \"%s\" SET localConceptId=?1 WHERE titleId=?2", tbl);
            if (sqlite3_prepare_v2(db, q2, -1, &u3, NULL) == SQLITE_OK) {
              sqlite3_bind_text(u3, 1, cid, -1, SQLITE_TRANSIENT);
              sqlite3_bind_text(u3, 2, title_id, -1, SQLITE_TRANSIENT);
              sqlite3_step(u3);
              sqlite3_finalize(u3);
            }
            snprintf(q2, sizeof(q2), "DELETE FROM \"%s\" WHERE localConceptId=?1", ctbl);
            if (sqlite3_prepare_v2(db, q2, -1, &u3, NULL) == SQLITE_OK) {
              sqlite3_bind_text(u3, 1, cid, -1, SQLITE_TRANSIENT);
              sqlite3_step(u3);
              sqlite3_finalize(u3);
            }
            snprintf(q2, sizeof(q2), "INSERT INTO \"%s\"(localConceptId, conceptName, icon0Info) VALUES(?1, ?2, ?3)", ctbl);
            if (sqlite3_prepare_v2(db, q2, -1, &u3, NULL) == SQLITE_OK) {
              sqlite3_bind_text(u3, 1, cid, -1, SQLITE_TRANSIENT);
              sqlite3_bind_text(u3, 2, name, -1, SQLITE_TRANSIENT);
              sqlite3_bind_text(u3, 3, icon, -1, SQLITE_TRANSIENT);
              sqlite3_step(u3);
              sqlite3_finalize(u3);
            }
          }
        }
      }
    }
    sqlite3_finalize(q);
  }
  sqlite3_close(db);
  printf("[appinst] installed %s (%s)\n", title_id, name);
  fflush(stdout);
  return 0;
}

int plat_app_uninstall(const char *title_id) {
  sqlite3 *db = open_app_db();
  if (!db) return -3;
  sqlite3_stmt *q;
  char sql[300];
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'",
                         -1, &q, NULL) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      sqlite3_stmt *del;
      const char *tbl = (const char *)sqlite3_column_text(q, 0);
      /* the concept row the fake installer added goes too (it is only there for tables that have one) */
      if (!strncmp(tbl, "tbl_iconinfo_", 13)) {
        char cs[320];
        snprintf(cs, sizeof(cs), "DELETE FROM \"tbl_concepticoninfo_%s\" WHERE localConceptId=?1", tbl + 13);
        char cid[64];
        snprintf(cid, sizeof(cid), "cid:local:%s", title_id);
        if (sqlite3_prepare_v2(db, cs, -1, &del, NULL) == SQLITE_OK) {
          sqlite3_bind_text(del, 1, cid, -1, SQLITE_TRANSIENT);
          sqlite3_step(del);
          sqlite3_finalize(del);
        }
      }
      snprintf(sql, sizeof(sql), "DELETE FROM \"%s\" WHERE titleId=?1", tbl);
      if (sqlite3_prepare_v2(db, sql, -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, title_id, -1, SQLITE_TRANSIENT);
        sqlite3_step(del);
        sqlite3_finalize(del);
      }
    }
    sqlite3_finalize(q);
  }
  sqlite3_close(db);
  printf("[appinst] removed %s\n", title_id);
  fflush(stdout);
  return 0;
}
