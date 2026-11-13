/* Home Screen Manager - hide PS5 home screen tiles without deleting anything.
 *
 * Launch from Payload Manager. The ELF starts a small web server and opens
 * its menu in the PS5 browser. The same menu is reachable from any phone or
 * PC on the network at http://<PS5-IP>:8090/
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include "backup.h"
#include "datadir.h"
#include "hider.h"
#include "arrange.h"
#include "tabs.h"
#include "custom.h"
#include "http.h"
#include "json.h"
#include "launch.h"
#include "liveshell.h"
#include "platform.h"
#include "strbuf.h"
#include "folder_html.h"
#include "folders.h"
#include "log.h"
#include "updater.h"
#include "usbpics.h"
#include "webui_html.h"

#define HH_PORT 8090

#ifndef HH_DEFAULT_REPO
#define HH_DEFAULT_REPO ""
#endif

static char g_data_dir[256];
static char g_app_db[256];
static char g_state_db[300];
static char g_settings[300];
static char g_backup_dir[300];
static char g_backup_orig[340];
static char g_backup_launch[340];

typedef struct {
  int  open_browser;  /* 1 always open the menu, 0 never, 2 quiet on the first
                         start after boot (Autoload), menu on later starts */
  int  reapply;       /* re-hide tiles the system brought back, at launch */
  int  auto_check;    /* look for updates (GitHub/USB) at launch */
  int  live;          /* start LiveShell (live wallpaper) with Home Screen Manager */
  int  folders_first; /* keep the folders in the first spots of their tab */
  int  show_running;  /* a running game's tile stays on screen (so it can be closed from the row) */
  int  folder_header; /* folder page: show the folder's picture and name at the top */
  int  folder_bar;    /* folder page: show the row with the count and the sort button */
  int  folder_edit;   /* folder page: show the "Edit this folder in Home Screen Manager" link under the games */
  int  folder_debug;  /* folder page: show what the controller sends (for testing) */
  char repo[128];     /* GitHub "owner/name" for updates */
} settings_t;

/* Set when a folder was created or really changed and the home screen should be
 * restarted to show it. The background thread does it (after putting the tile
 * positions right), so two database passes never run at once. */
static volatile int g_refresh_request;
static volatile int g_import_running; /* an import is running: folder changes wait */
static void request_home_refresh(void) { g_refresh_request = 1; }

/* The repository used when none is saved (set at build time; empty = none). An empty value
 * saved in the settings means "use this one". */
static const char *g_default_repo = HH_DEFAULT_REPO;

static settings_t g_cfg = {1, 1, 1, 0, 0, 1, 1, 1, 1, 0, HH_DEFAULT_REPO};
static char g_pldmgr_dir[256];
static char g_usb_fmt[256];
static char g_github_api[128];
static char g_app_dir[256];
static char g_appmeta_dir[256];
static int  g_folders_fixed = 0;
static int  g_loader_port = 9021;
static int  g_released = 0;
static char g_updated_to[32];
static volatile int g_running = 1;
static int g_reapplied = 0;
static int g_custom_fixed = 0;
static int g_arrange_fixed = 0;

/* ------------------------------------------------------------------------- */
/* paths & settings                                                          */
/* ------------------------------------------------------------------------- */

static char g_data_note[400]; /* what happened to the data folder at start, for the log */

static void setup_paths(void) {
  const char *data = "/data/homescreenmanager";
  const char *legacy = "/data/homehider"; /* where the data was before the app was renamed */
  const char *app = "/system_data/priv/mms/app.db";
  const char *pldmgr = "/data/pldmgr/payloads";
  const char *usb = "/mnt/usb%d";
  const char *api = "https://api.github.com";
#ifdef HH_HOST
  if (getenv("HH_DATA_DIR")) data = getenv("HH_DATA_DIR");
  legacy = getenv("HH_LEGACY_DATA_DIR"); /* tests only: on a PC nothing is moved unless asked */
  if (getenv("HH_DEFAULT_REPO")) g_default_repo = getenv("HH_DEFAULT_REPO"); /* tests */
  if (getenv("HH_APP_DB")) app = getenv("HH_APP_DB");
  if (getenv("HH_PLDMGR_DIR")) pldmgr = getenv("HH_PLDMGR_DIR");
  /* test override: must hold exactly one %d and no other conversions */
  if (getenv("HH_USB_FMT")) {
    const char *f = getenv("HH_USB_FMT"), *pc = strchr(f, '%');
    if (pc && pc[1] == 'd' && !strchr(pc + 2, '%') && strlen(f) < sizeof(g_usb_fmt)) usb = f;
    else fprintf(stderr, "HH_USB_FMT ignored: needs one %%d and under %zu characters\n", sizeof(g_usb_fmt));
  }
  if (getenv("HH_GITHUB_API")) api = getenv("HH_GITHUB_API");
  if (getenv("HH_LOADER_PORT")) g_loader_port = atoi(getenv("HH_LOADER_PORT"));
#endif
  snprintf(g_pldmgr_dir, sizeof(g_pldmgr_dir), "%s", pldmgr);
  snprintf(g_usb_fmt, sizeof(g_usb_fmt), "%s", usb);
  snprintf(g_github_api, sizeof(g_github_api), "%s", api);
  const char *appdir = "/user/app";
#ifdef HH_HOST
  if (getenv("HH_USER_APP_DIR")) appdir = getenv("HH_USER_APP_DIR");
#endif
  snprintf(g_app_dir, sizeof(g_app_dir), "%s", appdir);
  const char *appmeta = "/user/appmeta";
#ifdef HH_HOST
  if (getenv("HH_APPMETA_DIR")) appmeta = getenv("HH_APPMETA_DIR");
#endif
  snprintf(g_appmeta_dir, sizeof(g_appmeta_dir), "%s", appmeta);
  /* the data folder was renamed: move the old one over (nothing is lost if that fails) */
  if (legacy && legacy[0] && datadir_move(legacy, data, g_data_note, sizeof(g_data_note)) < 0) data = legacy;
  snprintf(g_cfg.repo, sizeof(g_cfg.repo), "%s", g_default_repo);
  snprintf(g_data_dir, sizeof(g_data_dir), "%s", data);
  snprintf(g_app_db, sizeof(g_app_db), "%s", app);
  snprintf(g_state_db, sizeof(g_state_db), "%s/state.db", g_data_dir);
  snprintf(g_settings, sizeof(g_settings), "%s/settings.txt", g_data_dir);
  snprintf(g_backup_dir, sizeof(g_backup_dir), "%s/backup", g_data_dir);
  snprintf(g_backup_orig, sizeof(g_backup_orig), "%s/app.db.original", g_backup_dir);
  snprintf(g_backup_launch, sizeof(g_backup_launch), "%s/app.db.last-launch", g_backup_dir);
  mkdir(g_data_dir, 0777);
  mkdir(g_backup_dir, 0777);
}

static void load_settings(void) {
  FILE *f = fopen(g_settings, "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int v;
    line[strcspn(line, "\r\n")] = '\0';
    if (sscanf(line, "open_browser=%d", &v) == 1) g_cfg.open_browser = v == 2 ? 2 : v ? 1 : 0;
    if (sscanf(line, "reapply=%d", &v) == 1) g_cfg.reapply = v ? 1 : 0;
    if (sscanf(line, "auto_check=%d", &v) == 1) g_cfg.auto_check = v ? 1 : 0;
    if (sscanf(line, "live=%d", &v) == 1) g_cfg.live = v ? 1 : 0;
    if (sscanf(line, "folders_first=%d", &v) == 1) g_cfg.folders_first = v ? 1 : 0;
    if (sscanf(line, "show_running=%d", &v) == 1) g_cfg.show_running = v ? 1 : 0;
    if (sscanf(line, "folder_header=%d", &v) == 1) g_cfg.folder_header = v ? 1 : 0;
    if (sscanf(line, "folder_bar=%d", &v) == 1) g_cfg.folder_bar = v ? 1 : 0;
    if (sscanf(line, "folder_edit=%d", &v) == 1) g_cfg.folder_edit = v ? 1 : 0;
    if (sscanf(line, "folder_debug=%d", &v) == 1) g_cfg.folder_debug = v ? 1 : 0;
    if (!strncmp(line, "repo=", 5) && line[5] && strlen(line + 5) < sizeof(g_cfg.repo) &&
        upd_valid_repo(line + 5)) /* an empty value means "the default" */
      memcpy(g_cfg.repo, line + 5, strlen(line + 5) + 1);
  }
  fclose(f);
}

static int save_settings(void) {
  char tmp[320];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_settings);
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  fprintf(f, "open_browser=%d\nreapply=%d\nauto_check=%d\nlive=%d\nfolders_first=%d\nshow_running=%d\n"
             "folder_header=%d\nfolder_bar=%d\nfolder_edit=%d\nfolder_debug=%d\nrepo=%s\n",
          g_cfg.open_browser, g_cfg.reapply, g_cfg.auto_check, g_cfg.live, g_cfg.folders_first,
          g_cfg.show_running, g_cfg.folder_header, g_cfg.folder_bar, g_cfg.folder_edit, g_cfg.folder_debug, g_cfg.repo);
  fclose(f);
  return rename(tmp, g_settings);
}

static int file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* ------------------------------------------------------------------------- */
/* request handlers                                                          */
/* ------------------------------------------------------------------------- */

static void send_json(int fd, int status, strbuf_t *sb) {
  if (sb->oom) {
    http_send_text(fd, 500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
    return;
  }
  http_send(fd, status, "application/json", sb->data, sb->len, NULL);
}

static char g_cur_path[256];

static void send_error(int fd, int status, const char *msg) {
  hh_log("HTTP %d on %s: %s", status, g_cur_path, msg);
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":false,\"error\":");
  sb_json_str(&sb, msg);
  sb_puts(&sb, "}");
  send_json(fd, status, &sb);
  sb_free(&sb);
}

static void send_ok(int fd, const char *note) {
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"note\":");
  sb_json_str(&sb, note);
  sb_puts(&sb, "}");
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

static void handle_status(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  char ip[64] = "";
  if (plat_get_ip(ip, sizeof(ip))) ip[0] = '\0';

  hh_t h;
  int ok = hh_open(&h, g_app_db, g_state_db) == 0;

  sb_printf(&sb, "{\"ok\":true,\"version\":\"%s\",\"port\":%d,\"ip\":", HH_VERSION, HH_PORT);
  sb_json_str(&sb, ip);
  sb_printf(&sb, ",\"settings\":{\"open_browser\":%d,\"reapply\":%s,\"auto_check\":%s,"
                 "\"folders_first\":%s,\"show_running\":%s,\"folder_header\":%s,\"folder_bar\":%s,"
                 "\"folder_edit\":%s,\"folder_debug\":%s,\"repo\":",
            g_cfg.open_browser, g_cfg.reapply ? "true" : "false",
            g_cfg.auto_check ? "true" : "false", g_cfg.folders_first ? "true" : "false",
            g_cfg.show_running ? "true" : "false", g_cfg.folder_header ? "true" : "false",
            g_cfg.folder_bar ? "true" : "false", g_cfg.folder_edit ? "true" : "false", g_cfg.folder_debug ? "true" : "false");
  sb_json_str(&sb, g_cfg.repo);
  sb_puts(&sb, ",\"default_repo\":");
  sb_json_str(&sb, g_default_repo);
  sb_puts(&sb, "}");
  sb_printf(&sb, ",\"released_locked\":%d,\"folders_fixed\":%d,\"updated_to\":", g_released,
            g_folders_fixed);
  sb_json_str(&sb, g_updated_to[0] ? g_updated_to : NULL);
  sb_puts(&sb, ",\"marker\":");
  sb_json_str(&sb, upd_marker());
  sb_printf(&sb, ",\"reapplied_at_launch\":%d", g_reapplied);
  sb_printf(&sb, ",\"custom_fixed\":%d", g_custom_fixed);
  sb_printf(&sb, ",\"arrange_fixed\":%d", g_arrange_fixed);
  sb_printf(&sb, ",\"backup\":%s", file_exists(g_backup_orig) ? "true" : "false");
  sb_puts(&sb, ",\"db\":{\"ok\":");
  sb_puts(&sb, ok && h.nusers > 0 ? "true" : "false");
  sb_printf(&sb, ",\"users\":%d,\"error\":", ok ? h.nusers : 0);
  if (!ok) sb_json_str(&sb, h.err);
  else if (h.nusers == 0) sb_json_str(&sb, "Unknown home screen database layout on this firmware.");
  else sb_puts(&sb, "null");
  sb_puts(&sb, "}}");
  if (ok) hh_close(&h);
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

static void handle_list(int fd) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"tiles\":");
  if (hh_list(&h, &sb)) {
    send_error(fd, 500, h.err);
  } else {
    sb_puts(&sb, "}");
    send_json(fd, 200, &sb);
  }
  sb_free(&sb);
  hh_close(&h);
}

static void name_of(const char *tid, char *out, size_t n) {
  out[0] = '\0';
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) return;
  hh_title_name(&h, tid, out, n);
  hh_close(&h);
}

/* close_running=1: close the game that is running first (the user said OK)
 * local=1: the page runs in the PS5's own browser (web links open in it) */
/* Opening a folder (or starting a game from one) makes the PS5 write a fresh
 * recency number on the folder's tile. With Keep folders first on, put the
 * folders back in order now rather than at the next background pass, so the
 * home screen reads the right order as early as possible. */
static void front_now(void) {
  if (!g_cfg.folders_first) return;
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db) != 0) return;
  int n = 0;
  folders_front_apply(&h, &n);
  hh_close(&h);
}

static void handle_launch(int fd, const char *query) {
  char tid[32], v[8];
  if (http_query_get(query, "id", tid, sizeof(tid)) || !hh_valid_tid(tid)) {
    send_error(fd, 400, "Missing or invalid title id.");
    return;
  }
  int close_running = !http_query_get(query, "close_running", v, sizeof(v)) && atoi(v);
  int local = !http_query_get(query, "local", v, sizeof(v)) && atoi(v);
  front_now(); /* a game started from a folder page */

  char name[160] = "", url[512] = "";
  int web = 0;
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db) == 0) {
    hh_title_name(&h, tid, name, sizeof(name));
    web = launch_web_url(&h, tid, url, sizeof(url)) == 0;
    hh_close(&h);
  }
  if (!name[0]) snprintf(name, sizeof(name), "%s", tid);

  if (web) {
    hh_log("launch %s: web link %s (%s)", tid, url, local ? "opened in this page" : "opened on the PS5");
    if (!local && plat_launch_browser(url)) {
      send_error(fd, 409, "Couldn't open the PS5 browser.");
      return;
    }
    strbuf_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"ok\":true,\"url\":");
    sb_json_str(&sb, url);
    sb_puts(&sb, ",\"note\":");
    char note[240];
    snprintf(note, sizeof(note), local ? "Opening %s" : "Opened %s on the PS5", name);
    sb_json_str(&sb, note);
    sb_puts(&sb, "}");
    send_json(fd, 200, &sb);
    sb_free(&sb);
    return;
  }

  int job = launch_start(tid, name, close_running);
  if (job < 0) {
    send_error(fd, 409, "Still starting the last game. Try again in a moment.");
    return;
  }
  char out[96];
  snprintf(out, sizeof(out), "{\"ok\":true,\"job\":%d}", job);
  http_send_text(fd, 200, "application/json", out);
}

static void handle_launch_status(int fd, const char *query) {
  char v[16] = "";
  http_query_get(query, "job", v, sizeof(v));
  strbuf_t sb;
  sb_init(&sb);
  launch_status_json(atoi(v), &sb, name_of);
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

/* ---- arrange the home screen ---- */

static void handle_arrange(int fd) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"arrange\":");
  if (arrange_json(&h, &sb)) send_error(fd, 500, h.err);
  else {
    sb_puts(&sb, "}");
    send_json(fd, 200, &sb);
  }
  sb_free(&sb);
  hh_close(&h);
}

static void handle_arrange_save(int fd, http_req_t *r) {
  unsigned char *body = NULL;
  size_t len = 0;
  if (http_read_body(fd, r, &body, &len, 64 * 1024)) {
    send_error(fd, 400, "Bad request.");
    return;
  }
  char *form = malloc(len + 1);
  if (!form) {
    free(body);
    send_error(fd, 500, "Out of memory.");
    return;
  }
  memcpy(form, body, len);
  form[len] = '\0';
  free(body);
  char *items = malloc(len + 1), *pinned = malloc(len + 1);
  if (!items || !pinned) {
    free(form);
    free(items);
    free(pinned);
    send_error(fd, 500, "Out of memory.");
    return;
  }
  items[0] = pinned[0] = '\0';
  http_query_get(form, "items", items, len + 1);
  http_query_get(form, "pinned", pinned, len + 1);
  free(form);
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    free(items);
    free(pinned);
    send_error(fd, 500, h.err);
    return;
  }
  if (arrange_set(&h, items, pinned)) send_error(fd, 409, h.err);
  else send_ok(fd, "Arrangement saved. The home screen shows it after you open and close any game, or restart.");
  free(items);
  free(pinned);
  hh_close(&h);
}

static void handle_arrange_clear(int fd) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  if (arrange_clear(&h)) send_error(fd, 409, h.err);
  else send_ok(fd, "Arrangement cleared. The home screen goes back to most-recent order.");
  hh_close(&h);
}

/* ---- move tiles between the Games and Media tabs ---- */

static void handle_tabs_moved(int fd) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"moved\":");
  if (tabs_moved_json(&h, &sb)) send_error(fd, 500, h.err);
  else {
    sb_puts(&sb, "}");
    send_json(fd, 200, &sb);
  }
  sb_free(&sb);
  hh_close(&h);
}

static void handle_tab_move(int fd, const char *query) {
  char tid[32], to[16] = "";
  if (http_query_get(query, "id", tid, sizeof(tid)) || !hh_valid_tid(tid)) {
    send_error(fd, 400, "Missing or invalid title id.");
    return;
  }
  http_query_get(query, "to", to, sizeof(to));
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  int reset = !strcmp(to, "reset") || !strcmp(to, "original");
  int rc;
  if (folders_is_folder(tid))
    rc = folders_set_tab(&h, tid, to); /* folders have no "original" tab */
  else
    rc = reset ? tabs_reset(&h, tid) : tabs_move(&h, tid, to);
  if (rc) send_error(fd, 409, h.err);
  else send_ok(fd, reset ? "Moved back to its original tab. The home screen shows it after you open and close any game, or restart."
                         : "Moved. The home screen shows it after you open and close any game, or restart.");
  hh_close(&h);
}

static void handle_folder_take_out(int fd, const char *query) {
  if (g_import_running) {
    send_error(fd, 409, "An import is running. Please wait until it has finished.");
    return;
  }
  char tid[32];
  if (http_query_get(query, "id", tid, sizeof(tid)) || !hh_valid_tid(tid)) {
    send_error(fd, 400, "Missing or invalid title id.");
    return;
  }
  char folder[32] = "";
  if (http_query_get(query, "folder", folder, sizeof(folder)) == 0 && folder[0] && !folders_is_folder(folder)) {
    send_error(fd, 400, "Invalid folder id.");
    return;
  }
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  if (folders_remove_member(&h, tid, folder)) {
    send_error(fd, 409, h.err);
  } else {
    char left[16] = "";
    folders_of_title(&h, tid, left, sizeof(left));
    send_ok(fd, left[0] ? "Taken out of that folder. It's still in another folder."
                        : "Taken out of the folder. It's back on its tab, and you can move it now.");
  }
  hh_close(&h);
}

/* ---- live wallpaper (LiveShell Engine by ZiZc3) ---- */

static volatile int g_live_starting = 0;

static void *live_start_later(void *arg) {
  int delay = (int)(long)arg;
  if (delay) sleep((unsigned)delay); /* let Autoload finish loading other payloads */
  if (!live_running()) live_start();
  g_live_starting = 0;
  return NULL;
}

static void live_start_async(int delay) {
  if (g_live_starting) return;
  g_live_starting = 1;
  pthread_t t;
  if (pthread_create(&t, NULL, live_start_later, (void *)(long)delay) == 0) pthread_detach(t);
  else g_live_starting = 0;
}

static void handle_live(int fd) {
  char *state = NULL;
  size_t n = 0;
  int st = live_get("/api/state", &state, &n);
  strbuf_t sb;
  sb_init(&sb);
  sb_printf(&sb, "{\"ok\":true,\"enabled\":%s,\"running\":%s,\"starting\":%s,\"version\":\"%s\",\"port\":9050,\"state\":",
            g_cfg.live ? "true" : "false", st == 200 ? "true" : "false", g_live_starting ? "true" : "false",
            LIVESHELL_VERSION);
  if (st == 200 && state && state[0] == '{') sb_puts(&sb, state);
  else sb_puts(&sb, "null");
  sb_puts(&sb, "}");
  free(state);
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

/* Passes LiveShell's {"ok":..,"error":..} answer on. */
static void live_forward(int fd, const char *path) {
  char *b = NULL;
  size_t n = 0;
  int st = live_get(path, &b, &n);
  if (st < 0) send_error(fd, 409, "LiveShell isn't running. Turn Live wallpaper on first.");
  else if (st != 200 || !b || b[0] != '{') send_error(fd, 409, "LiveShell didn't accept that.");
  else if (strstr(b, "\"ok\":true")) send_ok(fd, "done");
  else {
    /* its own error text */
    json_tok_t t[16];
    int nt = json_parse(b, n, t, 16);
    char msg[300] = "LiveShell didn't accept that.";
    if (nt > 0) json_str(b, t, json_get(b, t, nt, 0, "error"), msg, sizeof(msg));
    send_error(fd, 409, msg);
  }
  free(b);
}

static void handle_live_video(int fd, const char *query, const char *what) {
  char path[1024] = "", url[64];
  if (http_query_get(query, "path", path, sizeof(path)) || !path[0]) {
    send_error(fd, 400, "Missing video.");
    return;
  }
  int i = live_video_index(path);
  if (i < 0) {
    send_error(fd, 404, live_running() ? "That video isn't in LiveShell's list any more." :
                                         "LiveShell isn't running. Turn Live wallpaper on first.");
    return;
  }
  snprintf(url, sizeof(url), "/api/%s?i=%d", what, i);
  hh_log("live wallpaper: %s %s", what, path);
  live_forward(fd, url);
}

static void handle_live_power(int fd, const char *query) {
  char v[8] = "";
  http_query_get(query, "on", v, sizeof(v));
  g_cfg.live = atoi(v) ? 1 : 0;
  save_settings();
  if (g_cfg.live) {
    if (live_running()) {
      send_ok(fd, "Live wallpaper is on.");
      return;
    }
    live_start_async(0);
    send_ok(fd, "Starting LiveShell. The wallpaper appears in a few seconds.");
  } else {
    if (live_running()) live_stop();
    send_ok(fd, "Live wallpaper is off. The home screen restarts with its normal background.");
  }
}

static void handle_live_thumb(int fd, const char *query) {
  char path[1024] = "", url[64];
  http_query_get(query, "path", path, sizeof(path));
  int i = path[0] ? live_video_index(path) : -1;
  char *b = NULL;
  size_t n = 0;
  snprintf(url, sizeof(url), "/thumb?i=%d", i);
  if (i >= 0 && live_get(url, &b, &n) == 200 && n > 8 && !memcmp(b, "\x89PNG", 4))
    http_send(fd, 200, "image/png", b, n, "Cache-Control: max-age=300\r\n");
  else
    http_send_text(fd, 404, "text/plain", "no preview");
  free(b);
}

/* ---- USB pictures ---- */

static void handle_usb_images(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"images\":");
  usbpics_list_json(&sb);
  sb_puts(&sb, "}");
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

static void handle_usb_thumb(int fd, const char *query) {
  char path[1024];
  unsigned char *png;
  size_t len;
  if (http_query_get(query, "path", path, sizeof(path)) || usbpics_thumb(path, &png, &len)) {
    http_send_text(fd, 404, "text/plain", "no preview");
    return;
  }
  http_send(fd, 200, "image/png", png, len, "Cache-Control: max-age=3600\r\n");
  free(png);
}

static void handle_hide(int fd, const char *query, int hide) {
  char tid[32];
  if (http_query_get(query, "id", tid, sizeof(tid)) || !hh_valid_tid(tid)) {
    send_error(fd, 400, "Missing or invalid title id.");
    return;
  }
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  char folder[16] = "";
  folders_of_title(&h, tid, folder, sizeof(folder));
  if (!hide && folder[0]) {
    send_error(fd, 409, "This game is in a folder. Take it out of its folders to show it again.");
    hh_close(&h);
    return;
  }
  int rc = hide ? hh_hide(&h, tid) : hh_unhide(&h, tid);
  if (rc) send_error(fd, 409, h.err);
  else send_ok(fd, hide ? "hidden" : "shown");
  hh_close(&h);
}

static void handle_unhide_all(int fd) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  int n = 0;
  if (hh_unhide_all(&h, &n)) send_error(fd, 409, h.err);
  else {
    char note[64];
    snprintf(note, sizeof(note), "%d tile(s) shown again", n);
    send_ok(fd, note);
  }
  hh_close(&h);
}

static void handle_settings(int fd, const char *query) {
  char v[8], repo[160];
  if (!http_query_get(query, "repo", repo, sizeof(repo))) {
    /* accept a pasted GitHub URL too */
    char *r = repo;
    if (!strncmp(r, "https://", 8)) r += 8;
    if (!strncmp(r, "github.com/", 11)) r += 11;
    size_t n = strlen(r);
    while (n && (r[n - 1] == '/' || r[n - 1] == ' ')) r[--n] = '\0';
    if (n > 4 && !strcmp(r + n - 4, ".git")) r[n - 4] = '\0';
    if (!upd_valid_repo(r)) {
      send_error(fd, 400, "Use the form owner/name, for example yourname/HomeScreenManager.");
      return;
    }
    if (n) memcpy(g_cfg.repo, r, strlen(r) + 1); /* upd_valid_repo caps it at 100 */
    else snprintf(g_cfg.repo, sizeof(g_cfg.repo), "%s", g_default_repo); /* cleared: back to the default */
    upd_set_repo(g_cfg.repo);
  }
  if (!http_query_get(query, "open_browser", v, sizeof(v)))
    g_cfg.open_browser = atoi(v) == 2 ? 2 : atoi(v) ? 1 : 0;
  if (!http_query_get(query, "reapply", v, sizeof(v))) g_cfg.reapply = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "auto_check", v, sizeof(v))) g_cfg.auto_check = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "show_running", v, sizeof(v))) g_cfg.show_running = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "folder_header", v, sizeof(v))) g_cfg.folder_header = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "folder_bar", v, sizeof(v))) g_cfg.folder_bar = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "folder_edit", v, sizeof(v))) g_cfg.folder_edit = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "folder_debug", v, sizeof(v))) g_cfg.folder_debug = atoi(v) ? 1 : 0;
  if (!http_query_get(query, "folders_first", v, sizeof(v))) {
    int on = atoi(v) ? 1 : 0;
    if (on != g_cfg.folders_first) {
      g_cfg.folders_first = on; /* first, so the background check follows */
      hh_t h;
      if (hh_open(&h, g_app_db, g_state_db) == 0) {
        int n = 0;
        if (on) folders_front_apply(&h, &n);
        else folders_front_clear(&h);
        hh_log("folders first: %s (%d row(s) set)", on ? "on" : "off", n);
        hh_close(&h);
      }
    }
  }
  if (save_settings()) send_error(fd, 500, "Could not save settings.");
  else send_ok(fd, "saved");
}

/* The log by itself, as plain text (the same lines that end the diagnostics). */
static void handle_log(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  hh_log_tail(&sb, 40000);
  if (sb.oom) http_send_text(fd, 500, "text/plain", "out of memory");
  else http_send(fd, 200, "text/plain; charset=utf-8", sb.data, sb.len, "Cache-Control: no-store\r\n");
  sb_free(&sb);
}

static void handle_diag(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  hh_t h;
  int ok = hh_open(&h, g_app_db, g_state_db) == 0;
  hh_diag(&h, &sb);
  if (ok) hh_close(&h);
  sb_printf(&sb, "\nBackups: original=%s last-launch=%s\n",
            file_exists(g_backup_orig) ? "yes" : "no",
            file_exists(g_backup_launch) ? "yes" : "no");
  {
    char hex[65];
    live_elf_sha256(hex);
    sb_printf(&sb, "LiveShell %s built in: %zu bytes, sha256 %s; live wallpaper %s, %s\n",
              LIVESHELL_VERSION, live_elf_size(), hex, g_cfg.live ? "on" : "off",
              live_running() ? "running" : "not running");
  }
  sb_puts(&sb, "\n== Log (newest last) ==\n");
  hh_log_tail(&sb, 12000);
  if (sb.oom) http_send_text(fd, 500, "text/plain", "out of memory");
  else http_send(fd, 200, "text/plain; charset=utf-8", sb.data, sb.len, NULL);
  sb_free(&sb);
}

static int send_file(int fd, const char *path, const char *ctype, const char *extra) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64L * 1024 * 1024) {
    fclose(f);
    return -1;
  }
  char *buf = malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    free(buf);
    return -1;
  }
  http_send(fd, 200, ctype, buf, got, extra);
  free(buf);
  return 0;
}

static void handle_icon(int fd, const char *query) {
  char tid[32], path[512];
  if (http_query_get(query, "id", tid, sizeof(tid)) || !hh_valid_tid(tid)) {
    http_send_text(fd, 404, "text/plain", "no icon");
    return;
  }
  hh_t h;
  int ok = hh_open(&h, g_app_db, g_state_db) == 0;
  if (!ok) h.db = NULL;
  int found = hh_icon_path(&h, tid, path, sizeof(path)) == 0;
  if (ok) hh_close(&h);
  if (found) {
    const char *dot = strrchr(path, '.');
    const char *ctype = (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")))
                            ? "image/jpeg" : "image/png";
    if (send_file(fd, path, ctype, "Cache-Control: max-age=3600\r\n") == 0) return;
  }
  http_send_text(fd, 404, "text/plain", "no icon");
}

static void handle_backup_download(int fd) {
  if (send_file(fd, g_backup_orig, "application/octet-stream",
                "Content-Disposition: attachment; filename=\"app.db.original\"\r\n"))
    http_send_text(fd, 404, "text/plain", "No backup yet.");
}

static void handle_restore(int fd) {
  char err[256] = "";
  if (!file_exists(g_backup_orig)) {
    send_error(fd, 404, "No original backup found.");
    return;
  }
  /* keep a copy of the current state before overwriting it */
  char before[360];
  snprintf(before, sizeof(before), "%s/app.db.before-restore", g_backup_dir);
  hh_copy_db(g_app_db, before, err, sizeof(err));
  {
    hh_t h;
    if (hh_open(&h, g_app_db, g_state_db) == 0) {
      folders_remove_all_tiles(&h); /* the original database has no folders */
      custom_forget_all(&h);        /* ...and the original names and pictures */
      hh_close(&h);
    }
  }
  if (hh_copy_db(g_backup_orig, g_app_db, err, sizeof(err))) {
    send_error(fd, 500, err);
    return;
  }
  hh_reset_state(g_state_db, err, sizeof(err));
  send_ok(fd, "Original database restored. Restart the PS5 to finish.");
}

static void handle_reload(int fd) {
  if (plat_reload_shellui()) send_error(fd, 500, "Could not find the home screen process.");
  else send_ok(fd, "Home screen restarting");
}

static void handle_update_status(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  upd_status_json(&sb);
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

static void handle_update_check(int fd, const char *query) {
  char v[8];
  int usb_only = !http_query_get(query, "usb_only", v, sizeof(v)) && atoi(v);
  if (upd_start_check(usb_only ? 0 : 1, 1, 0)) send_error(fd, 409, "An update task is already running.");
  else send_ok(fd, "checking");
}

static void handle_update_install(int fd, const char *query) {
  char src[16];
  if (http_query_get(query, "source", src, sizeof(src)) ||
      (strcmp(src, "github") && strcmp(src, "usb"))) {
    send_error(fd, 400, "Unknown update source.");
    return;
  }
  if (upd_start_install(src)) send_error(fd, 409, "An update task is already running.");
  else send_ok(fd, "installing");
}

static void handle_update_upload(int fd, http_req_t *r) {
  unsigned char *body = NULL;
  size_t len = 0;
  int rc = http_read_body(fd, r, &body, &len, UPD_MAX_ELF);
  if (rc == -2) {
    send_error(fd, 413, "Send a Home Screen Manager .elf file (up to 32 MB).");
    return;
  }
  if (rc) {
    send_error(fd, 400, "The upload was interrupted.");
    return;
  }
  char v[8], msg[320];
  int allow_older = !http_query_get(r->query, "allow_older", v, sizeof(v)) && atoi(v);
  if (upd_install_upload(body, len, allow_older, msg, sizeof(msg))) send_error(fd, 409, msg);
  else send_ok(fd, msg);
  free(body);
}

/* ---- folders ---- */

static void handle_folders(int fd) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"folders\":");
  if (folders_list_json(&h, &sb)) send_error(fd, 500, h.err);
  else {
    sb_puts(&sb, "}");
    send_json(fd, 200, &sb);
  }
  sb_free(&sb);
  hh_close(&h);
}

static void handle_folder(int fd, const char *query) {
  char id[32];
  hh_t h;
  if (http_query_get(query, "id", id, sizeof(id))) id[0] = '\0';
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"folder\":");
  if (folders_get_json(&h, id, &sb)) send_error(fd, 404, h.err);
  else {
    /* how the folder page is laid out (the page is one fixed file) */
    sb_printf(&sb, ",\"page\":{\"header\":%s,\"bar\":%s,\"edit\":%s,\"debug\":%s}}", g_cfg.folder_header ? "true" : "false",
              g_cfg.folder_bar ? "true" : "false", g_cfg.folder_edit ? "true" : "false", g_cfg.folder_debug ? "true" : "false");
    send_json(fd, 200, &sb);
  }
  sb_free(&sb);
  hh_close(&h);
}

/* ---- saving a folder in the background ----
 * Adding a folder's tile goes through the console's installer, which takes about
 * 90 seconds on newer firmware. With ?async=1 the save runs on its own thread and
 * the answer comes at once ({"job":N}); the menu then asks /api/folders/job until
 * it is done. Without it the save is answered when it has finished, as before. */
static void sb_error_json(strbuf_t *sb, const char *msg) {
  sb_puts(sb, "{\"ok\":false,\"error\":");
  sb_json_str(sb, msg);
  sb_puts(sb, "}");
}

/* the answer for a folder that was saved (the same for both ways of asking) */
static void sb_saved_json(strbuf_t *sb, hh_t *h, const char *out_id, const char *note, int changed) {
  sb_printf(sb, "{\"ok\":true,\"id\":\"%s\",\"changed\":%s,\"note\":", out_id, changed ? "true" : "false");
  sb_json_str(sb, note[0] ? note : NULL);
  sb_puts(sb, ",\"folder\":");
  folders_get_json(h, out_id, sb);
  sb_puts(sb, "}");
}

typedef struct {
  char id[32], name[256], tab[16], color[16];
  char *items;
  int  refresh;  /* restart the home screen when this is done (if anything changed) */
} fjob_args_t;

static struct {
  pthread_mutex_t mx;
  int  last;     /* number of the latest job */
  int  running;
  int  done;
  char *result;  /* the JSON answer of the latest job */
} FJ = {PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, NULL};

static void *folder_job_worker(void *arg) {
  fjob_args_t *a = arg;
  strbuf_t sb;
  sb_init(&sb);
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    sb_error_json(&sb, h.err);
  } else {
    char out_id[16] = "", note[200] = "";
    int changed = 1;
    if (folders_save(&h, a->id, a->name, a->tab, a->color, a->items, out_id, note, sizeof(note), &changed)) {
      hh_log("folder job: not saved: %s", h.err);
      sb_error_json(&sb, h.err);
    } else {
      sb_saved_json(&sb, &h, out_id, note, changed);
      if (a->refresh && changed) request_home_refresh();
    }
    hh_close(&h);
  }
  char *res = NULL;
  if (!sb.oom && sb.data) {
    res = malloc(sb.len + 1);
    if (res) {
      memcpy(res, sb.data, sb.len);
      res[sb.len] = '\0';
    }
  }
  if (!res) res = strdup("{\"ok\":false,\"error\":\"Out of memory.\"}");
  sb_free(&sb);
  pthread_mutex_lock(&FJ.mx);
  free(FJ.result);
  FJ.result = res;
  FJ.running = 0;
  FJ.done = 1;
  pthread_mutex_unlock(&FJ.mx);
  free(a->items);
  free(a);
  return NULL;
}

/* job number, -1 if another folder change is still running, -2 if out of memory */
static int folder_job_start(const char *id, const char *name, const char *tab, const char *color,
                            const char *items, int refresh) {
  pthread_mutex_lock(&FJ.mx);
  if (FJ.running) {
    pthread_mutex_unlock(&FJ.mx);
    return -1;
  }
  fjob_args_t *a = calloc(1, sizeof(*a));
  if (a) a->items = strdup(items);
  if (!a || !a->items) {
    if (a) free(a->items);
    free(a);
    pthread_mutex_unlock(&FJ.mx);
    return -2;
  }
  snprintf(a->id, sizeof(a->id), "%s", id);
  snprintf(a->name, sizeof(a->name), "%s", name);
  snprintf(a->tab, sizeof(a->tab), "%s", tab);
  snprintf(a->color, sizeof(a->color), "%s", color);
  a->refresh = refresh;
  pthread_t t;
  if (pthread_create(&t, NULL, folder_job_worker, a) != 0) {
    free(a->items);
    free(a);
    pthread_mutex_unlock(&FJ.mx);
    return -2;
  }
  pthread_detach(t);
  FJ.last++;
  FJ.running = 1;
  FJ.done = 0;
  free(FJ.result);
  FJ.result = NULL;
  int job = FJ.last;
  pthread_mutex_unlock(&FJ.mx);
  return job;
}

static void handle_folder_job(int fd, const char *query) {
  char v[16] = "";
  http_query_get(query, "job", v, sizeof(v));
  int job = atoi(v);
  strbuf_t sb;
  sb_init(&sb);
  int code = 200;
  pthread_mutex_lock(&FJ.mx);
  if (job <= 0 || job != FJ.last) {
    code = 404;
    sb_error_json(&sb, "Unknown folder job.");
  } else if (FJ.running || !FJ.done) {
    sb_puts(&sb, "{\"ok\":true,\"done\":false}");
  } else {
    sb_puts(&sb, "{\"ok\":true,\"done\":true,\"result\":");
    sb_puts(&sb, FJ.result ? FJ.result : "{\"ok\":false,\"error\":\"No result.\"}");
    sb_puts(&sb, "}");
  }
  pthread_mutex_unlock(&FJ.mx);
  send_json(fd, code, &sb);
  sb_free(&sb);
}

static void handle_folder_save(int fd, http_req_t *r) {
  if (g_import_running) {
    http_drain(fd, r);
    send_error(fd, 409, "An import is running. Please wait until it has finished.");
    return;
  }
  unsigned char *body = NULL;
  size_t len = 0;
  if (http_read_body(fd, r, &body, &len, 64 * 1024)) {
    send_error(fd, 400, "Bad request.");
    return;
  }
  char *form = malloc(len + 1);
  if (!form) {
    free(body);
    send_error(fd, 500, "Out of memory.");
    return;
  }
  memcpy(form, body, len);
  form[len] = '\0';
  free(body);

  char id[32] = "", name[256] = "", tab[16] = "games", color[16] = "blue";
  char *items = malloc(len + 1);
  if (!items) {
    free(form);
    send_error(fd, 500, "Out of memory.");
    return;
  }
  items[0] = '\0';
  http_query_get(form, "id", id, sizeof(id));
  http_query_get(form, "name", name, sizeof(name));
  http_query_get(form, "tab", tab, sizeof(tab));
  http_query_get(form, "color", color, sizeof(color));
  http_query_get(form, "items", items, len + 1);
  free(form);

  char av[8] = "", rv[8] = "";
  int refresh = !http_query_get(r->query, "refresh", rv, sizeof(rv)) && atoi(rv);
  if (!http_query_get(r->query, "async", av, sizeof(av)) && atoi(av)) {
    int job = folder_job_start(id, name, tab, color, items, refresh);
    free(items);
    if (job == -1) {
      send_error(fd, 409, "Another folder change is still being made. Please wait until it has finished.");
    } else if (job < 0) {
      send_error(fd, 500, "Out of memory.");
    } else {
      strbuf_t sb;
      sb_init(&sb);
      sb_printf(&sb, "{\"ok\":true,\"job\":%d,\"id\":", job);
      sb_json_str(&sb, id);
      sb_puts(&sb, "}");
      send_json(fd, 200, &sb);
      sb_free(&sb);
    }
    return;
  }

  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    free(items);
    send_error(fd, 500, h.err);
    return;
  }
  char out_id[16] = "", note[200] = "";
  int changed = 1;
  if (folders_save(&h, id, name, tab, color, items, out_id, note, sizeof(note), &changed)) {
    send_error(fd, 409, h.err);
  } else {
    strbuf_t sb;
    sb_init(&sb);
    sb_saved_json(&sb, &h, out_id, note, changed);
    send_json(fd, 200, &sb);
    sb_free(&sb);
    if (refresh && changed) request_home_refresh();
  }
  free(items);
  hh_close(&h);
}

static void handle_folder_delete(int fd, const char *query) {
  if (g_import_running) {
    send_error(fd, 409, "An import is running. Please wait until it has finished.");
    return;
  }
  char id[32] = "";
  http_query_get(query, "id", id, sizeof(id));
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  if (folders_delete(&h, id)) send_error(fd, 409, h.err);
  else send_ok(fd, "Folder removed. Its games are back on the home screen.");
  hh_close(&h);
}

static void handle_folder_icon_set(int fd, http_req_t *r) {
  if (g_import_running) {
    http_drain(fd, r);
    send_error(fd, 409, "An import is running. Please wait until it has finished.");
    return;
  }
  char id[32] = "", v[8];
  http_query_get(r->query, "id", id, sizeof(id));
  int reset = !http_query_get(r->query, "reset", v, sizeof(v)) && atoi(v);
  char usb[1024] = "";
  http_query_get(r->query, "usb", usb, sizeof(usb));
  unsigned char *body = NULL;
  size_t len = 0;
  if (usb[0]) {
    http_drain(fd, r);
    if (usbpics_read(usb, &body, &len)) {
      send_error(fd, 404, "That picture isn't on the USB drive any more. Plug the drive back in and try again.");
      return;
    }
  } else if (!reset) {
    int rc = http_read_body(fd, r, &body, &len, USBPIC_MAX_BYTES);
    if (rc) {
      send_error(fd, rc == -2 ? 413 : 400, "Send a PNG, JPG, BMP, GIF or TGA picture (up to 32 MB).");
      return;
    }
  } else {
    http_drain(fd, r);
  }
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    free(body);
    send_error(fd, 500, h.err);
    return;
  }
  char rv[8] = "";
  int want_refresh = !http_query_get(r->query, "refresh", rv, sizeof(rv)) && atoi(rv);
  if (folders_set_icon(&h, id, reset ? NULL : body, len)) {
    send_error(fd, 409, h.err);
  } else {
    send_ok(fd, reset ? "Back to the automatic icon." : "Folder picture updated.");
    if (want_refresh) request_home_refresh();
  }
  free(body);
  hh_close(&h);
}

/* ---- names and pictures of any tile ---- */

static void handle_custom_get(int fd, const char *query) {
  char id[32] = "";
  http_query_get(query, "id", id, sizeof(id));
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"item\":");
  if (custom_get_json(&h, id, &sb)) send_error(fd, 400, h.err);
  else {
    sb_puts(&sb, "}");
    send_json(fd, 200, &sb);
  }
  sb_free(&sb);
  hh_close(&h);
}

static void handle_custom_name(int fd, http_req_t *r) {
  char id[32] = "", v[8], name[400] = "";
  http_query_get(r->query, "id", id, sizeof(id));
  int reset = !http_query_get(r->query, "reset", v, sizeof(v)) && atoi(v);
  unsigned char *body = NULL;
  size_t len = 0;
  if (reset || r->content_length <= 0) {
    http_drain(fd, r);
  } else if (http_read_body(fd, r, &body, &len, 8 * 1024)) {
    send_error(fd, 400, "Bad request.");
    return;
  }
  if (!reset && body) {
    char *form = malloc(len + 1);
    if (form) {
      memcpy(form, body, len);
      form[len] = '\0';
      http_query_get(form, "name", name, sizeof(name));
      free(form);
    }
  }
  free(body);
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    send_error(fd, 500, h.err);
    return;
  }
  if (custom_set_name(&h, id, reset ? NULL : name)) send_error(fd, 409, h.err);
  else send_ok(fd, reset || !name[0] ? "Original name back." : "Name saved.");
  hh_close(&h);
}

/* Picture for a POST: ?usb=<path>, the request body, or nothing (?reset=1).
 * Returns 0 and *body (malloc'd, NULL for reset); on error the reply is sent. */
static int read_picture(int fd, http_req_t *r, int reset, unsigned char **body, size_t *len) {
  char usb[1024] = "";
  *body = NULL;
  *len = 0;
  http_query_get(r->query, "usb", usb, sizeof(usb));
  if (usb[0]) {
    http_drain(fd, r);
    if (usbpics_read(usb, body, len)) {
      send_error(fd, 404, "That picture isn't on the USB drive any more. Plug the drive back in and try again.");
      return -1;
    }
  } else if (!reset) {
    int rc = http_read_body(fd, r, body, len, USBPIC_MAX_BYTES);
    if (rc) {
      send_error(fd, rc == -2 ? 413 : 400, "Send a PNG, JPG, BMP, GIF or TGA picture (up to 32 MB).");
      return -1;
    }
  } else {
    http_drain(fd, r);
  }
  return 0;
}

static void handle_custom_bg(int fd, http_req_t *r) {
  char id[32] = "", v[8];
  http_query_get(r->query, "id", id, sizeof(id));
  int reset = !http_query_get(r->query, "reset", v, sizeof(v)) && atoi(v);
  unsigned char *body;
  size_t len;
  if (read_picture(fd, r, reset, &body, &len)) return;
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    free(body);
    send_error(fd, 500, h.err);
    return;
  }
  if (custom_set_bg(&h, id, reset ? NULL : body, len)) send_error(fd, 409, h.err);
  else send_ok(fd, reset ? "Original background back." : "Background saved.");
  free(body);
  hh_close(&h);
}

static void send_jpg_or_404(int fd, int found, const char *path) {
  if (!found || send_file(fd, path, "image/jpeg", "Cache-Control: no-cache\r\n"))
    http_send_text(fd, 404, "text/plain", "no background");
}

static void handle_custom_bg_get(int fd, const char *query) {
  char id[32] = "", path[400];
  http_query_get(query, "id", id, sizeof(id));
  hh_t h;
  int found = 0;
  if (hh_open(&h, g_app_db, g_state_db) == 0) {
    found = custom_bg_path(&h, id, path, sizeof(path)) == 0;
    hh_close(&h);
  }
  send_jpg_or_404(fd, found, path);
}

static void handle_folder_bg_set(int fd, http_req_t *r) {
  if (g_import_running) {
    http_drain(fd, r);
    send_error(fd, 409, "An import is running. Please wait until it has finished.");
    return;
  }
  char id[32] = "", v[8];
  http_query_get(r->query, "id", id, sizeof(id));
  int reset = !http_query_get(r->query, "reset", v, sizeof(v)) && atoi(v);
  unsigned char *body;
  size_t len;
  if (read_picture(fd, r, reset, &body, &len)) return;
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    free(body);
    send_error(fd, 500, h.err);
    return;
  }
  char rv[8] = "";
  int want_refresh = !http_query_get(r->query, "refresh", rv, sizeof(rv)) && atoi(rv);
  if (folders_set_bg(&h, id, reset ? NULL : body, len)) {
    send_error(fd, 409, h.err);
  } else {
    send_ok(fd, reset ? "Background removed." : "Background saved.");
    if (want_refresh) request_home_refresh();
  }
  free(body);
  hh_close(&h);
}

static void handle_folder_bg_get(int fd, const char *query) {
  char id[32] = "", path[400];
  http_query_get(query, "id", id, sizeof(id));
  send_jpg_or_404(fd, folders_bg_path(id, path, sizeof(path)) == 0, path);
}

static void handle_custom_icon(int fd, http_req_t *r) {
  char id[32] = "", v[8], usb[1024] = "";
  http_query_get(r->query, "id", id, sizeof(id));
  int reset = !http_query_get(r->query, "reset", v, sizeof(v)) && atoi(v);
  int clear = !http_query_get(r->query, "clear", v, sizeof(v)) && atoi(v);
  http_query_get(r->query, "usb", usb, sizeof(usb));
  unsigned char *body = NULL;
  size_t len = 0;
  if (usb[0]) {
    http_drain(fd, r);
    if (usbpics_read(usb, &body, &len)) {
      send_error(fd, 404, "That picture isn't on the USB drive any more. Plug the drive back in and try again.");
      return;
    }
  } else if (!reset) {
    int rc = http_read_body(fd, r, &body, &len, USBPIC_MAX_BYTES);
    if (rc) {
      send_error(fd, rc == -2 ? 413 : 400, "Send a PNG, JPG, BMP, GIF or TGA picture (up to 32 MB).");
      return;
    }
  } else {
    http_drain(fd, r);
  }
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    free(body);
    send_error(fd, 500, h.err);
    return;
  }
  if (custom_set_icon(&h, id, reset ? NULL : body, len, clear)) send_error(fd, 409, h.err);
  else send_ok(fd, reset ? "Original picture back." : "Picture saved.");
  free(body);
  hh_close(&h);
}

static void handle_folder_icon(int fd, const char *query) {
  char id[32] = "", path[512];
  http_query_get(query, "id", id, sizeof(id));
  if (folders_icon_path(id, path, sizeof(path)) || send_file(fd, path, "image/png", ""))
    http_send_text(fd, 404, "text/plain", "no icon");
}

/* ------------------------------------------------------------------------- */
/* export and import of everything the manager manages                       */
/* ------------------------------------------------------------------------- */

#define IMPORT_MAX_BYTES (128u * 1024 * 1024)

static struct {
  backup_t *pending; /* an opened file waiting for the Import button */
  int token, job;
  volatile int done;
  backup_progress_t prog;
  char result[1600];
  time_t t0;
} g_imp;
static pthread_mutex_t g_imp_mx = PTHREAD_MUTEX_INITIALIZER;

static int dir_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int usb_root(int i, char *out, size_t n) {
  if ((size_t)snprintf(out, n, g_usb_fmt, i) >= n) return 0;
  return dir_exists(out);
}

static void backup_file_name(char *out, size_t n) {
  time_t now = time(NULL);
  struct tm tm;
  char stamp[32] = "";
  if (localtime_r(&now, &tm)) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M", &tm);
  snprintf(out, n, "HomeScreenManager-backup-%s.zip", stamp);
}

static int make_backup(strbuf_t *sb, char *err, size_t errsz) {
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    snprintf(err, errsz, "%.150s", h.err);
    return -1;
  }
  int rc = backup_export(&h, g_settings, sb);
  hh_close(&h);
  if (rc) snprintf(err, errsz, "Couldn't make the backup file.");
  return rc;
}

static void handle_export_download(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  char err[200], name[96], hdr[200];
  if (make_backup(&sb, err, sizeof(err))) {
    sb_free(&sb);
    send_error(fd, 500, err);
    return;
  }
  backup_file_name(name, sizeof(name));
  snprintf(hdr, sizeof(hdr), "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
  http_send(fd, 200, "application/zip", sb.data, sb.len, hdr);
  sb_free(&sb);
}

static void handle_export_usb(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  char err[200], name[96], root[260], path[400];
  if (make_backup(&sb, err, sizeof(err))) {
    sb_free(&sb);
    send_error(fd, 500, err);
    return;
  }
  backup_file_name(name, sizeof(name));
  int saved = 0;
  for (int i = 0; i < 8 && !saved; i++) {
    if (!usb_root(i, root, sizeof(root))) continue;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    size_t w = fwrite(sb.data, 1, sb.len, f);
    int closed = fclose(f) == 0;
    struct stat st;
    if (w == sb.len && closed && stat(path, &st) == 0 && (size_t)st.st_size == sb.len) saved = 1;
    else unlink(path);
  }
  if (!saved) {
    sb_free(&sb);
    send_error(fd, 409, "Couldn't write to a USB drive. Plug one in (exFAT or FAT32), then try again.");
    return;
  }
  hh_log("export: backup written to %s (%zu bytes)", path, sb.len);
  strbuf_t js;
  sb_init(&js);
  sb_puts(&js, "{\"ok\":true,\"path\":");
  sb_json_str(&js, path);
  sb_printf(&js, ",\"size\":%zu}", sb.len);
  send_json(fd, 200, &js);
  sb_free(&js);
  sb_free(&sb);
}

static int ends_with_zip(const char *s) {
  size_t n = strlen(s);
  return n > 4 && !strcasecmp(s + n - 4, ".zip");
}

static void handle_import_usb_files(int fd) {
  strbuf_t sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"ok\":true,\"files\":[");
  int first = 1;
  for (int i = 0; i < 8; i++) {
    char root[260];
    if (!usb_root(i, root, sizeof(root))) continue;
    DIR *d = opendir(root);
    if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!ends_with_zip(e->d_name) || strncasecmp(e->d_name, "HomeScreenManager-backup", 24)) continue;
      char path[400];
      struct stat st;
      if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, e->d_name) >= sizeof(path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        continue;
      sb_printf(&sb, "%s{\"path\":", first ? "" : ",");
      sb_json_str(&sb, path);
      sb_puts(&sb, ",\"name\":");
      sb_json_str(&sb, e->d_name);
      sb_printf(&sb, ",\"size\":%lld}", (long long)st.st_size);
      first = 0;
    }
    closedir(d);
  }
  sb_puts(&sb, "]}");
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

/* only a .zip on one of the USB drives, with no ".." in the way */
static int usb_zip_path_ok(const char *path) {
  if (!ends_with_zip(path) || strstr(path, "..")) return 0;
  for (int i = 0; i < 8; i++) {
    char root[260];
    if (!usb_root(i, root, sizeof(root))) continue;
    size_t rl = strlen(root);
    if (!strncmp(path, root, rl) && path[rl] == '/') return 1;
  }
  return 0;
}

static void handle_import_inspect(int fd, http_req_t *r) {
  char usb[400] = "";
  http_query_get(r->query, "usb", usb, sizeof(usb));
  unsigned char *body = NULL;
  size_t len = 0;
  if (g_import_running) {
    http_drain(fd, r);
    send_error(fd, 409, "An import is running. Please wait until it has finished.");
    return;
  }
  if (usb[0]) {
    http_drain(fd, r);
    struct stat st;
    FILE *f = usb_zip_path_ok(usb) && stat(usb, &st) == 0 && S_ISREG(st.st_mode) && (size_t)st.st_size <= IMPORT_MAX_BYTES
                  ? fopen(usb, "rb")
                  : NULL;
    if (!f) {
      send_error(fd, 400, "That backup file isn't on a USB drive any more. Plug the drive back in and try again.");
      return;
    }
    body = malloc((size_t)st.st_size + 1);
    len = body ? fread(body, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!body || len != (size_t)st.st_size) {
      free(body);
      send_error(fd, 500, "Couldn't read the backup file from the USB drive.");
      return;
    }
  } else {
    int rc = http_read_body(fd, r, &body, &len, IMPORT_MAX_BYTES);
    if (rc) {
      send_error(fd, rc == -2 ? 413 : 400, "Send the backup file (a .zip of up to 128 MB).");
      return;
    }
  }
  char err[240] = "";
  backup_t *b = backup_open(body, len, err, sizeof(err));
  if (!b) {
    free(body);
    send_error(fd, 400, err[0] ? err : "That isn't a Home Screen Manager backup file.");
    return;
  }
  hh_t h;
  if (hh_open(&h, g_app_db, g_state_db)) {
    backup_close(b);
    send_error(fd, 500, h.err);
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  backup_summary_json(&h, b, &sb);
  hh_close(&h);
  pthread_mutex_lock(&g_imp_mx);
  backup_close(g_imp.pending);
  g_imp.pending = b;
  int token = ++g_imp.token;
  pthread_mutex_unlock(&g_imp_mx);
  if (sb.len && sb.data[sb.len - 1] == '}') sb.len--; /* add the token to the object */
  sb_printf(&sb, ",\"token\":%d}", token);
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

/* the backup's settings.txt replaces the current settings file and is loaded */
static int import_settings(const char *text, size_t len) {
  char tmp[420];
  snprintf(tmp, sizeof(tmp), "%s.tmp", g_settings);
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  size_t w = fwrite(text, 1, len, f);
  int closed = fclose(f) == 0;
  if (w != len || !closed || rename(tmp, g_settings) != 0) {
    unlink(tmp);
    return -1;
  }
  load_settings();
  upd_set_repo(g_cfg.repo);
  return 0;
}

static void *import_worker(void *arg) {
  backup_t *b = arg;
  hh_t h;
  char err[240] = "";
  int rc = -1;
  if (hh_open(&h, g_app_db, g_state_db) == 0) {
    folders_hold(1);
    rc = backup_apply(&h, b, &g_imp.prog, import_settings, err, sizeof(err));
    folders_hold(0);
    hh_close(&h);
  } else {
    snprintf(err, sizeof(err), "%.200s", h.err);
  }
  backup_close(b);
  strbuf_t sb;
  sb_init(&sb);
  const backup_progress_t *p = &g_imp.prog;
  if (rc == 0) {
    sb_printf(&sb, "{\"ok\":true,\"folders_made\":%d,\"folders_skipped\":%d,\"hidden\":%d,\"custom\":%d,\"order\":%d,\"settings\":%s,\"problems\":%d,\"notes\":",
              p->folders_made, p->folders_skipped, p->hidden_done, p->custom_done, p->order_done, p->settings_done ? "true" : "false",
              p->problems);
    sb_json_str(&sb, p->notes);
    sb_puts(&sb, "}");
    hh_log("import: %d folder(s) made, %d skipped, %d hidden, %d custom, order %d, %d problem(s)%s%s", p->folders_made,
           p->folders_skipped, p->hidden_done, p->custom_done, p->order_done, p->problems, p->notes[0] ? ": " : "", p->notes);
  } else {
    sb_puts(&sb, "{\"ok\":false,\"error\":");
    sb_json_str(&sb, err);
    sb_puts(&sb, "}");
    hh_log("import: not done: %s", err);
  }
  snprintf(g_imp.result, sizeof(g_imp.result), "%s", sb.data ? sb.data : "{\"ok\":false}");
  sb_free(&sb);
  if (rc == 0 && (p->folders_made || p->hidden_done || p->custom_done || p->order_done || p->settings_done)) request_home_refresh();
  g_imp.done = 1;
  g_import_running = 0;
  return NULL;
}

static void handle_import_apply(int fd, const char *query) {
  char v[16];
  int token = http_query_get(query, "token", v, sizeof(v)) ? 0 : atoi(v);
  if (g_import_running || folders_busy()) {
    send_error(fd, 409, "Another change is still being made. Please wait until it has finished.");
    return;
  }
  pthread_mutex_lock(&g_imp_mx);
  backup_t *b = g_imp.pending;
  int ok = b && token == g_imp.token;
  if (ok) g_imp.pending = NULL;
  pthread_mutex_unlock(&g_imp_mx);
  if (!ok) {
    send_error(fd, 409, "Choose the backup file again, then press Import.");
    return;
  }
  memset(&g_imp.prog, 0, sizeof(g_imp.prog));
  snprintf(g_imp.prog.phase, sizeof(g_imp.prog.phase), "Starting…");
  g_imp.result[0] = '\0';
  g_imp.done = 0;
  g_imp.t0 = time(NULL);
  g_imp.job++;
  g_import_running = 1;
  pthread_t t;
  if (pthread_create(&t, NULL, import_worker, b) != 0) {
    g_import_running = 0;
    backup_close(b);
    send_error(fd, 500, "Couldn't start the import.");
    return;
  }
  pthread_detach(t);
  strbuf_t sb;
  sb_init(&sb);
  sb_printf(&sb, "{\"ok\":true,\"job\":%d}", g_imp.job);
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

static void handle_import_job(int fd, const char *query) {
  char v[16];
  int job = http_query_get(query, "job", v, sizeof(v)) ? 0 : atoi(v);
  if (job == 0 || job != g_imp.job) {
    send_error(fd, 404, "Unknown import job.");
    return;
  }
  strbuf_t sb;
  sb_init(&sb);
  int done = g_imp.done;
  sb_printf(&sb, "{\"ok\":true,\"done\":%s,\"elapsed\":%ld,\"folder_i\":%d,\"folder_n\":%d,\"phase\":", done ? "true" : "false",
            (long)(time(NULL) - g_imp.t0), g_imp.prog.folder_i, g_imp.prog.folder_n);
  sb_json_str(&sb, g_imp.prog.phase);
  if (done) sb_printf(&sb, ",\"result\":%s", g_imp.result[0] ? g_imp.result : "{\"ok\":false}");
  sb_puts(&sb, "}");
  send_json(fd, 200, &sb);
  sb_free(&sb);
}

static void handle_client(int fd) {
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  http_req_t r;
  if (http_read_request(fd, &r)) return;
  int post = strcmp(r.method, "POST") == 0;
  const char *p = r.path;
  snprintf(g_cur_path, sizeof(g_cur_path), "%s %.200s", r.method, r.path);
  int upload = post && !strcmp(p, "/api/update/upload");
  int fsave = post && !strcmp(p, "/api/folders/save");
  int ficon = post && !strcmp(p, "/api/folders/icon");
  int cname = post && !strcmp(p, "/api/custom/name");
  int cicon = post && !strcmp(p, "/api/custom/icon");
  int cbg = post && !strcmp(p, "/api/custom/bg");
  int fbg = post && !strcmp(p, "/api/folders/bg");
  int asave = post && !strcmp(p, "/api/arrange/save");
  int iinspect = post && !strcmp(p, "/api/import/inspect"); /* reads the backup file itself */
  if (!upload && !fsave && !ficon && !cname && !cicon && !cbg && !fbg && !asave && !iinspect) http_drain(fd, &r);

  if (!strcmp(p, "/") || !strcmp(p, "/index.html")) {
    http_send(fd, 200, "text/html; charset=utf-8", WEBUI_HTML, WEBUI_HTML_LEN, NULL);
  } else if (!strcmp(p, "/api/status")) {
    handle_status(fd);
  } else if (!strncmp(p, "/folder/", 8)) {
    front_now(); /* the PS5 has just opened this folder */
    http_send(fd, 200, "text/html; charset=utf-8", FOLDER_HTML, FOLDER_HTML_LEN, NULL);
  } else if (!strcmp(p, "/api/folders")) {
    handle_folders(fd);
  } else if (!strcmp(p, "/api/folder")) {
    handle_folder(fd, r.query);
  } else if (!strcmp(p, "/api/folder_icon")) {
    handle_folder_icon(fd, r.query);
  } else if (!strcmp(p, "/api/usb_images")) {
    handle_usb_images(fd);
  } else if (!strcmp(p, "/api/usb_thumb")) {
    handle_usb_thumb(fd, r.query);
  } else if (!strcmp(p, "/api/list")) {
    handle_list(fd);
  } else if (!strcmp(p, "/api/diag")) {
    handle_diag(fd);
  } else if (!strcmp(p, "/api/log")) {
    handle_log(fd);
  } else if (!strcmp(p, "/api/icon")) {
    handle_icon(fd, r.query);
  } else if (!strcmp(p, "/api/custom")) {
    handle_custom_get(fd, r.query);
  } else if (!strcmp(p, "/api/custom_bg")) {
    handle_custom_bg_get(fd, r.query);
  } else if (!strcmp(p, "/api/folder_bg")) {
    handle_folder_bg_get(fd, r.query);
  } else if (!strcmp(p, "/api/arrange")) {
    handle_arrange(fd);
  } else if (!strcmp(p, "/api/tabs")) {
    handle_tabs_moved(fd);
  } else if (!strcmp(p, "/api/live")) {
    handle_live(fd);
  } else if (!strcmp(p, "/api/live/thumb")) {
    handle_live_thumb(fd, r.query);
  } else if (!strcmp(p, "/api/launch_status")) {
    handle_launch_status(fd, r.query);
  } else if (!strcmp(p, "/api/folders/job")) {
    handle_folder_job(fd, r.query);
  } else if (!strcmp(p, "/api/import/job")) {
    handle_import_job(fd, r.query);
  } else if (!strcmp(p, "/api/import/usb_files")) {
    handle_import_usb_files(fd);
  } else if (!strcmp(p, "/api/export")) {
    handle_export_download(fd);
  } else if (!strcmp(p, "/api/update/status")) {
    handle_update_status(fd);
  } else if (!strcmp(p, "/api/backup")) {
    handle_backup_download(fd);
  } else if (!post && !strncmp(p, "/api/", 5)) {
    send_error(fd, 405, "Use POST for this action.");
  } else if (http_is_cross_origin(&r)) {
    send_error(fd, 403, "Requests from other web sites are not allowed.");
  } else if (!strcmp(p, "/api/export/usb")) {
    handle_export_usb(fd);
  } else if (!strcmp(p, "/api/import/inspect")) {
    handle_import_inspect(fd, &r);
  } else if (!strcmp(p, "/api/import/apply")) {
    handle_import_apply(fd, r.query);
  } else if (!strcmp(p, "/api/hide")) {
    handle_hide(fd, r.query, 1);
  } else if (!strcmp(p, "/api/unhide")) {
    handle_hide(fd, r.query, 0);
  } else if (upload) {
    handle_update_upload(fd, &r);
  } else if (!strcmp(p, "/api/update/check")) {
    handle_update_check(fd, r.query);
  } else if (!strcmp(p, "/api/update/install")) {
    handle_update_install(fd, r.query);
  } else if (fsave) {
    handle_folder_save(fd, &r);
  } else if (ficon) {
    handle_folder_icon_set(fd, &r);
  } else if (cname) {
    handle_custom_name(fd, &r);
  } else if (cicon) {
    handle_custom_icon(fd, &r);
  } else if (cbg) {
    handle_custom_bg(fd, &r);
  } else if (fbg) {
    handle_folder_bg_set(fd, &r);
  } else if (!strcmp(p, "/api/folders/delete")) {
    handle_folder_delete(fd, r.query);
  } else if (!strcmp(p, "/api/folders/take_out")) {
    handle_folder_take_out(fd, r.query);
  } else if (!strcmp(p, "/api/clientlog")) {
    char msg[300] = "";
    http_query_get(r.query, "msg", msg, sizeof(msg));
    hh_log("menu: %s", msg);
    send_ok(fd, "logged");
  } else if (!strcmp(p, "/api/launch")) {
    handle_launch(fd, r.query);
  } else if (asave) {
    handle_arrange_save(fd, &r);
  } else if (!strcmp(p, "/api/arrange/clear")) {
    handle_arrange_clear(fd);
  } else if (!strcmp(p, "/api/tab")) {
    handle_tab_move(fd, r.query);
  } else if (!strcmp(p, "/api/live/power")) {
    handle_live_power(fd, r.query);
  } else if (!strcmp(p, "/api/live/select")) {
    handle_live_video(fd, r.query, "select");
  } else if (!strcmp(p, "/api/live/delete")) {
    handle_live_video(fd, r.query, "delete");
  } else if (!strcmp(p, "/api/unhide_all")) {
    handle_unhide_all(fd);
  } else if (!strcmp(p, "/api/settings")) {
    handle_settings(fd, r.query);
  } else if (!strcmp(p, "/api/restore")) {
    handle_restore(fd);
  } else if (!strcmp(p, "/api/reload_shell")) {
    handle_reload(fd);
  } else if (!strcmp(p, "/api/quit")) {
    send_ok(fd, "bye");
    g_running = 0;
  } else {
    http_send_text(fd, 404, "text/plain", "Not found");
  }
}

/* ------------------------------------------------------------------------- */
/* startup                                                                   */
/* ------------------------------------------------------------------------- */

/* Asks an already running Home Screen Manager to exit so we can take over the port.
 * Returns 1 if one was running. */
static int stop_previous_instance(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  int found = 0;
  if (s < 0) return 0;
  struct timeval tv = {1, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(HH_PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
    const char *req = "POST /api/quit HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
    send(s, req, strlen(req), 0);
    char sink[256];
    recv(s, sink, sizeof(sink), 0);
    found = 1;
  }
  close(s);
  return found;
}

/* 1 if this is the first start since the console was turned on (that is
 * when Autoload runs payloads). Remembers this boot for the next start. */
static int first_start_since_boot(void) {
  char boot[64] = "", seen[64] = "", path[320];
  if (plat_boot_id(boot, sizeof(boot)) || !boot[0]) return 1;
  snprintf(path, sizeof(path), "%s/last_boot.txt", g_data_dir);
  FILE *f = fopen(path, "r");
  if (f) {
    if (!fgets(seen, sizeof(seen), f)) seen[0] = '\0';
    fclose(f);
    seen[strcspn(seen, "\r\n")] = '\0';
  }
  int first = strcmp(seen, boot) != 0;
  if (first && (f = fopen(path, "w"))) {
    fprintf(f, "%s\n", boot);
    fclose(f);
  }
  return first;
}

static int open_server(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(HH_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  for (int tries = 0; tries < 30; tries++) {  /* up to ~3 s for the old instance */
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 64) == 0) return s;
    usleep(100000);
  }
  close(s);
  return -1;
}

/* Hides again the tiles the PS5 brought back. A game that is running is left
 * alone (when "Show a running game" is on): its tile stays on screen, in front
 * of the folders, so it can be closed from the home row. It is hidden again by
 * the first pass after the game is closed. */
static int running_title(char *out, size_t n) {
  if (!g_cfg.show_running) return -1;
  uint32_t rid = 0;
  if (plat_running_game(out, n, &rid) != 0 || strlen(out) >= 16 || !hh_valid_tid(out)) return -1;
  return 0;
}

static void rehide_now(hh_t *h, int *fixed) { hh_reapply(h, fixed, running_title); }

/* The title id of the game that is running right now ("" if none). */
static void game_now(char *out, size_t n) {
  uint32_t rid = 0;
  out[0] = '\0';
  if (plat_running_game(out, n, &rid) != 0 || strlen(out) >= 16 || !hh_valid_tid(out)) out[0] = '\0';
}

static long ms_since(const struct timespec *t0) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)((t.tv_sec - t0->tv_sec) * 1000L + (t.tv_nsec - t0->tv_nsec) / 1000000L);
}

/* One round of keeping the home screen right: hide again what the PS5 brought
 * back, put folders, the folder block, the chosen order and tab moves back. */
static void watchdog_pass(hh_t *h) {
  if (h->nusers == 0) return;
  int rehidden = 0, folders = 0, arranged = 0;
  if (g_cfg.reapply) rehide_now(h, &rehidden);
  folders_reapply(h, &folders);
  int fronted = 0;
  if (g_cfg.folders_first) folders_front_apply(h, &fronted);
  arrange_apply(h, &arranged);
  int tabbed = 0;
  tabs_apply(h, &tabbed);
  if (rehidden || folders || arranged || tabbed || fronted)
    hh_log("auto re-hide: %d tile(s), %d folder(s), %d arranged, %d tab-moved, %d kept first — the home screen heals itself",
           rehidden, folders, arranged, tabbed, fronted);
}

/* The system makes a tile visible again after its game is played, which
 * un-hides games that are inside folders (and anything else on the hide
 * list), cluttering the home screen until Home Screen Manager runs again. While Home
 * Hider is running, this thread re-hides them by itself every few seconds,
 * and keeps folders and the chosen order in place, so the home screen heals
 * without re-running the payload.
 *
 * The home screen reads the database again some moments after a game is
 * closed, so the fix has to be in by then. The thread therefore looks at the
 * running game four times a second, and the moment a game is gone it checks
 * everything straight away. The PS5 writes its own update for the game a
 * second or two after it closed (it shows the tile again), and the home screen
 * takes that write, not our fix before it. So for the next few seconds the
 * closed game's tile is watched closely and hidden again the instant it
 * comes back, and everything is checked again a few times. */
static void *rehide_watchdog(void *arg) {
  (void)arg;
  char prev[32];
  game_now(prev, sizeof(prev));
  while (g_running) {
    int closed = 0, refresh = 0;
    char closed_tid[32] = "";
    for (int t = 0; t < 6000 && g_running; t += 250) {
      usleep(250000);
      char now[32];
      game_now(now, sizeof(now));
      if (prev[0] && !now[0]) {
        hh_log("%s is closed: checking the home screen right away", prev);
        snprintf(closed_tid, sizeof(closed_tid), "%s", prev);
        closed = 1;
      }
      snprintf(prev, sizeof(prev), "%s", now);
      if (g_refresh_request) refresh = 1; /* a folder was created or changed */
      if (closed || refresh) break;
    }
    if (!g_running) break;
    hh_t h;
    if (hh_open(&h, g_app_db, g_state_db) != 0) continue;
    watchdog_pass(&h);
    if (closed) {
      hh_state_t st;
      int watch = g_cfg.reapply && h.nusers > 0 && hh_get_state(&h, closed_tid, &st) == 0 && st.wanted;
      struct timespec t0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      for (int i = 1; i <= 200 && g_running; i++) { /* 5 seconds, in steps of 25 ms */
        usleep(25000);
        char now[32];
        game_now(now, sizeof(now));
        if (now[0]) break; /* a game is running again: back to the normal rhythm */
        if (watch && hh_get_state(&h, closed_tid, &st) == 0 && st.present && (st.visible || st.in_myhome)) {
          long seen = ms_since(&t0);
          int rc = hh_hide(&h, closed_tid);
          hh_log("show running: the PS5 showed %s again %ld ms after the close; hidden again within %ld ms%s",
                 closed_tid, seen, ms_since(&t0) - seen, rc ? " (failed)" : "");
        }
        if (i % 60 == 0) watchdog_pass(&h); /* and everything again at 1.5 s, 3 s and 4.5 s */
      }
    }
    if (refresh) {
      /* A folder was created or really changed. The console shuffles tiles while it works and
       * may write a little after it is done, so: put the tile positions right, wait, put them
       * right again, and only then restart the home screen, so that it reads the right layout. */
      g_refresh_request = 0;
      hh_log("home screen refresh: the folder change is done, putting the tile positions right first");
      for (int i = 0; i < 8 && g_running; i++) usleep(250000);
      watchdog_pass(&h);
      hh_close(&h);
      int rc = plat_reload_shellui();
      hh_log("home screen refresh: restarting the home screen process%s", rc ? " (the process wasn't found)" : "");
      continue;
    }
    hh_close(&h);
  }
  return NULL;
}

/* After asking an older copy to quit: wait (up to 3 seconds) until it no longer answers,
 * so that its data folder isn't being written while it is moved or opened. */
static void wait_previous_instance_gone(void) {
  for (int i = 0; i < 30; i++) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct timeval tv = {0, 200000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(HH_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int up = connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(s);
    if (!up) return;
    usleep(100000);
  }
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  plat_init();
  int was_running = stop_previous_instance(); /* first: the data folder may be moved next */
  if (was_running) wait_previous_instance_gone();
  setup_paths();
  load_settings();
  {
    backup_cfg_t bcfg = {g_data_dir, HH_VERSION};
    backup_init(&bcfg);
  }
  {
    char lp[320];
    snprintf(lp, sizeof(lp), "%s/log.txt", g_data_dir);
    hh_log_init(lp);
    hh_log("Home Screen Manager %s starting", HH_VERSION);
    if (g_data_note[0]) hh_log("%s", g_data_note);
  }
  {
    char tc[320], pics[320];
    snprintf(tc, sizeof(tc), "%s/thumbs", g_data_dir);
    snprintf(pics, sizeof(pics), "%s/pictures", g_data_dir);
    usbpics_init(g_usb_fmt, tc, pics);
  }
  folders_cfg_t fcfg = {g_data_dir, g_app_dir, HH_PORT};
  folders_init(&fcfg);
  launch_cfg_t lcfg = {g_app_dir, g_appmeta_dir};
  launch_init(&lcfg);
  custom_cfg_t ccfg = {g_data_dir, g_appmeta_dir};
  custom_init(&ccfg);
  {
    live_cfg_t lv = {g_loader_port, 9050};
#ifdef HH_HOST
    if (getenv("HH_LIVESHELL_PORT")) lv.api_port = atoi(getenv("HH_LIVESHELL_PORT"));
#endif
    live_init(&lv);
  }

  /* safety copies of the home screen database */
  char err[256] = "";
  if (!file_exists(g_backup_orig) && hh_copy_db(g_app_db, g_backup_orig, err, sizeof(err)))
    plat_notify("Home Screen Manager: backup failed (%s)", err);
  hh_copy_db(g_app_db, g_backup_launch, err, sizeof(err));

  {
    hh_t h;
    if (hh_open(&h, g_app_db, g_state_db) == 0) {
      /* older versions let these be hidden although it has no effect */
      hh_release_locked(&h, &g_released);
      if (g_cfg.reapply) rehide_now(&h, &g_reapplied);
      folders_reapply(&h, &g_folders_fixed); /* folder tiles always come back */
      if (g_cfg.folders_first) { int ff = 0; folders_front_apply(&h, &ff); } /* in the first spots */
      custom_reapply(&h, &g_custom_fixed);   /* and so do new names and pictures */
      arrange_apply(&h, &g_arrange_fixed);   /* and the chosen home-screen order */
      { int tf = 0; tabs_apply(&h, &tf); }   /* and tiles moved to the other tab */
      hh_log("startup: users=%d reapplied=%d folders_fixed=%d custom_fixed=%d arrange_fixed=%d",
             h.nusers, g_reapplied, g_folders_fixed, g_custom_fixed, g_arrange_fixed);
      hh_close(&h);
    }
  }

  upd_config_t ucfg = {g_data_dir, g_backup_dir, g_pldmgr_dir, g_usb_fmt, g_github_api,
                       g_loader_port};
  upd_init(&ucfg);
  upd_set_repo(g_cfg.repo);

  /* started by our own updater? then the browser is already open */
  int just_updated = 0;
  {
    char flag[300];
    snprintf(flag, sizeof(flag), "%s/.updated", g_data_dir);
    FILE *f = fopen(flag, "r");
    if (f) {
      just_updated = 1;
      snprintf(g_updated_to, sizeof(g_updated_to), "%s", HH_VERSION);
      fclose(f);
      unlink(flag);
    }
  }

  int srv = open_server();
  if (srv < 0) {
    plat_notify("Home Screen Manager: port %d is busy.\nRestart the PS5 and try again.", HH_PORT);
    return 1;
  }

  char ip[64] = "";
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/", HH_PORT);
  if (just_updated)
    plat_notify("Home Screen Manager updated to v%s", HH_VERSION);
  else if (plat_get_ip(ip, sizeof(ip)) == 0)
    plat_notify("Home Screen Manager %s running\nMenu: http://%s:%d%s", HH_VERSION, ip, HH_PORT,
                g_reapplied ? "\n(re-hid tiles the system restored)" : "");
  else
    plat_notify("Home Screen Manager %s running on port %d", HH_VERSION, HH_PORT);

  if (g_cfg.auto_check && !just_updated) upd_start_check(g_cfg.repo[0] != '\0', 1, 1);

  /* live wallpaper: start the built-in LiveShell unless it is already running.
   * A short wait lets the ELF loader settle at boot; the rest of the time to
   * a visible wallpaper is LiveShell's own ShellUI setup. */
  if (g_cfg.live) live_start_async(just_updated ? 0 : 1);

  int first_boot_start = first_start_since_boot();
  int open_menu = !just_updated &&
                  (g_cfg.open_browser == 1 ||
                   (g_cfg.open_browser == 2 && (was_running || !first_boot_start)));
  hh_log("startup: menu mode %d, already running %d, first start since boot %d -> %s",
         g_cfg.open_browser, was_running, first_boot_start, open_menu ? "open menu" : "quiet");
  if (open_menu && plat_launch_browser(url))
    plat_notify("Home Screen Manager: couldn't open the browser.\nUse http://%s:%d from a phone/PC.",
                ip[0] ? ip : "<PS5-IP>", HH_PORT);

  /* keep the home screen de-cluttered after games are played */
  {
    pthread_t wd;
    if (pthread_create(&wd, NULL, rehide_watchdog, NULL) == 0) pthread_detach(wd);
  }

  while (g_running) {
    struct pollfd pfd = {srv, POLLIN, 0};
    int r = poll(&pfd, 1, 500);
    if (r <= 0) continue;
    int c = accept(srv, NULL, NULL);
    if (c < 0) continue;
    handle_client(c);
    close(c);
  }
  close(srv);
  plat_notify("Home Screen Manager closed");
  return 0;
}
