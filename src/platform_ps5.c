#include "platform.h"

#include <time.h>

#include "log.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <errno.h>
#include <dirent.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

#include <ps5/kernel.h>

/* Classic notification request (works across firmware versions). */
typedef struct {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *req,
                                     size_t size, int blocking);
int sceUserServiceInitialize(void *);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
int sceUserServiceGetForegroundUser(int *user_id);

typedef struct {
  uint32_t sz;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} lnc_app_param_t;

int sceLncUtilLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
int sceLncUtilKillApp(uint32_t app_id);

/* system HTTPS stack (same calls as the SDK's http2_get sample) */
int sceNetInit(void);
int sceNetPoolCreate(const char *, int, int);
int sceSslInit(size_t);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2CreateTemplate(int, const char *, int, int);
int sceHttp2SetAutoRedirect(int, int);
int sceHttp2CreateRequestWithURL(int, const char *, const char *, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void *, size_t);
int sceHttp2GetStatusCode(int, int *);
int sceHttp2ReadData(int, void *, size_t);
int sceHttp2GetAllResponseHeaders(int, char **, size_t *);

/* app installer (same calls Payload Manager uses for its launcher tile) */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

/* Launcher calls that are looked up at run time: a payload that links a
 * function the firmware doesn't export fails to load at all. */
static void *sysfn(const char *name) {
  static uint32_t handle;
  static int have = -1;
  if (have < 0) have = kernel_dynlib_handle(-1, "libSceSystemService.sprx", &handle) == 0;
  if (!have) return NULL;
  intptr_t p = kernel_dynlib_dlsym(-1, handle, name);
  return (p == 0 || p == -1) ? NULL : (void *)p;
}

static int valid_app_id(int id) {
  return id != 0 && id != -1 && !((uint32_t)id & 0x80000000u);
}

/* Payload managers show a payload under the name of its process. Without one they only know "payload pid N".
 * Weak, so a console or SDK without the call still links and runs. */
extern int sceKernelSetProcessName(const char *name) __attribute__((weak));

void plat_set_process_name(const char *name) {
  if (sceKernelSetProcessName) sceKernelSetProcessName(name);
  syscall(464 /* SYS_thr_set_name */, -1, name); /* the name of this thread (the main one) */
}

void plat_init(void) {
  /* needed by sceSystemServiceLaunchWebBrowser; harmless if already done */
  sceUserServiceInitialize(0);
  /* the launcher wants this before LaunchApp/KillApp; "already done" is fine */
  int (*lnc_init)(void) = sysfn("sceLncUtilInitialize");
  if (lnc_init) lnc_init();
}

void plat_notify(const char *fmt, ...) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  printf("[HomeScreenManager] %s\n", req.message);
}

int plat_launch_browser(const char *url) {
  return sceSystemServiceLaunchWebBrowser(url, 0) == 0 ? 0 : -1;
}

void plat_describe_mount(const char *path, char *out, size_t n) {
  struct statfs sf;
  if (statfs(path, &sf)) {
    snprintf(out, n, "statfs failed (%s)", strerror(errno));
    return;
  }
  snprintf(out, n, "type %s, from %s, mounted on %s, %s, %llu KiB free", sf.f_fstypename, sf.f_mntfromname,
           sf.f_mntonname, (sf.f_flags & MNT_RDONLY) ? "READ-ONLY" : "writable",
           (unsigned long long)sf.f_bavail * sf.f_bsize / 1024);
}

int plat_launch_app(const char *title_id, uint32_t *app_id) {
  *app_id = 0;
  int user = 0;
  int rc = sceUserServiceGetForegroundUser(&user);
  if (rc != 0) return rc;
  lnc_app_param_t p;
  memset(&p, 0, sizeof(p));
  p.sz = sizeof(p);
  p.user_id = (uint32_t)user;
  /* success returns the new app's id (0x6xxxxxxx); errors have the top bit set */
  rc = sceLncUtilLaunchApp(title_id, NULL, &p);
  if ((uint32_t)rc & 0x80000000u) return rc;
  *app_id = (uint32_t)rc;
  return 0;
}

int plat_app_id(const char *title_id, uint32_t *app_id) {
  int (*get)(const char *) = sysfn("sceLncUtilGetAppId");
  if (!get) return -1;
  int id = get(title_id);
  if (!valid_app_id(id)) return -1;
  *app_id = (uint32_t)id;
  return 0;
}

int plat_running_game(char *title_id, size_t n, uint32_t *app_id) {
  int (*big)(void) = sysfn("sceLncUtilGetAppIdOfRunningBigApp");
  if (!big) big = sysfn("sceSystemServiceGetAppIdOfRunningBigApp");
  int (*title)(uint32_t, char *) = sysfn("sceLncUtilGetAppTitleId");
  if (!big) return -1;
  int id = big();
  if (!valid_app_id(id)) return -1;
  char t[64];
  memset(t, 0, sizeof(t));
  if (!title || title((uint32_t)id, t) != 0) t[0] = '\0';
  t[sizeof(t) - 1] = '\0';
  snprintf(title_id, n, "%s", t);
  *app_id = (uint32_t)id;
  return 0;
}

int plat_kill_app(uint32_t app_id, char *report, size_t n) {
  int rc = sceLncUtilKillApp(app_id);
  if (rc == 0) {
    snprintf(report, n, "kill %08x -> ok", app_id);
    return 0;
  }
  int (*sskill)(int, int, int, int) = sysfn("sceSystemServiceKillApp");
  int rc2 = sskill ? sskill((int)app_id, -1, 0, 0) : -1;
  snprintf(report, n, "kill %08x -> 0x%08X, system kill -> 0x%08X", app_id, (unsigned)rc,
           (unsigned)rc2);
  return rc2 == 0 ? 0 : -1;
}

int plat_boot_id(char *out, size_t n) {
  struct timeval tv;
  size_t len = sizeof(tv);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &tv, &len, NULL, 0) != 0 || tv.tv_sec <= 0) return -1;
  snprintf(out, n, "%lld", (long long)tv.tv_sec);
  return 0;
}

int plat_get_ip(char *buf, size_t n) {
  struct ifaddrs *ifaddr, *ifa;
  if (getifaddrs(&ifaddr) == -1) return -1;
  int ret = -1;
  for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;
    if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), buf, n, NULL, 0,
                    NI_NUMERICHOST) == 0) {
      ret = 0;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return ret;
}

int plat_reload_shellui(void) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) || !size) return -1;
  size += size / 4;
  char *buf = malloc(size);
  if (!buf) return -1;
  if (sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  pid_t target = -1;
  for (char *p = buf; p + sizeof(int) <= buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    if (ki->ki_structsize <= 0) break;
    if (strcmp(ki->ki_comm, "SceShellUI") == 0) target = ki->ki_pid;
    p += ki->ki_structsize;
  }
  free(buf);
  if (target <= 0) return -1;
  /* SceShellCore restarts the home screen automatically */
  return kill(target, SIGKILL) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* HTTPS download                                                            */
/* ------------------------------------------------------------------------- */

static int g_http_tmpl = -1;
static int g_http_ctx = -1;
static const char *g_http_step = ""; /* where a failed request stopped, for the log */

/* functions of the HTTP library that may be missing from the link stubs, found by name when they are needed */
static void *http2_sym(const char *name) {
  uint32_t h;
  if (kernel_dynlib_handle(-1, "libSceHttp2.sprx", &h) != 0) return NULL;
  intptr_t p = kernel_dynlib_dlsym(-1, h, name);
  return (p != 0 && p != -1) ? (void *)p : NULL;
}

/* time limits, so that a changed network can't leave a request waiting for ever (microseconds) */
static void http_timeouts(int id) {
  static const char *names[] = {"sceHttp2SetConnectTimeOut", "sceHttp2SetSendTimeOut", "sceHttp2SetRecvTimeOut"};
  static const unsigned secs[] = {15, 30, 30};
  for (int i = 0; i < 3; i++) {
    int (*fn)(int, uint32_t) = (int (*)(int, uint32_t))http2_sym(names[i]);
    if (fn) fn(id, secs[i] * 1000000u);
  }
}

const char *plat_http_last_step(void) { return g_http_step; }

static int http_setup(void) {
  if (g_http_tmpl >= 0) return 0;
  g_http_step = "network setup";
  sceNetInit(); /* fine if the loader already did this */
  int pool = sceNetPoolCreate("homescreenmanager", 64 * 1024, 0);
  if (pool < 0) return pool;
  g_http_step = "secure connection (SSL) setup";
  int ssl = sceSslInit(256 * 1024);
  if (ssl < 0) return ssl;
  g_http_step = "HTTP setup";
  int ctx = sceHttp2Init(pool, ssl, 256 * 1024, 2);
  if (ctx < 0) return ctx;
  g_http_step = "HTTP template";
  int tmpl = sceHttp2CreateTemplate(ctx, "HomeScreenManager-PS5", 3, 1);
  if (tmpl < 0) return tmpl;
  sceHttp2SetAutoRedirect(tmpl, 1);
  http_timeouts(tmpl);
  g_http_ctx = ctx;
  g_http_tmpl = tmpl;
  return 0;
}

/* Location header from a 3xx response (fallback if auto-redirect is off). */
static int redirect_target(int req, char *out, size_t outsz) {
  char *hdr = NULL;
  size_t hsz = 0;
  if (sceHttp2GetAllResponseHeaders(req, &hdr, &hsz) != 0 || !hdr) return -1;
  for (size_t i = 0; i + 9 < hsz; i++) {
    if ((i == 0 || hdr[i - 1] == '\n') && strncasecmp(hdr + i, "location:", 9) == 0) {
      const char *v = hdr + i + 9;
      while (*v == ' ') v++;
      size_t n = 0;
      while (v + n < hdr + hsz && v[n] != '\r' && v[n] != '\n') n++;
      if (n == 0 || n >= outsz) return -1;
      memcpy(out, v, n);
      out[n] = '\0';
      return 0;
    }
  }
  return -1;
}

static int http_get_impl(const char *url, unsigned char **out, size_t *out_len,
                         size_t max, volatile long long *progress) {
  *out = NULL;
  *out_len = 0;
  g_http_step = "";
  int rc = http_setup();
  if (rc) return rc < 0 ? rc : -1;
  int tmpl_id = g_http_tmpl;

  char cur[1024];
  snprintf(cur, sizeof(cur), "%s", url);
  for (int hop = 0; hop < 6; hop++) {
    g_http_step = "creating the request";
    int req = sceHttp2CreateRequestWithURL(tmpl_id, "GET", cur, 0);
    if (req < 0) return req;
    g_http_step = "connecting and sending the request (secure handshake)";
    if ((rc = sceHttp2SendRequest(req, NULL, 0)) != 0) {
      sceHttp2DeleteRequest(req);
      return rc < 0 ? rc : -1;
    }
    g_http_step = "reading the answer";
    int status = 0;
    if (sceHttp2GetStatusCode(req, &status) != 0) {
      sceHttp2DeleteRequest(req);
      return -1;
    }
    if (status >= 300 && status < 400) {
      int ok = redirect_target(req, cur, sizeof(cur)) == 0;
      sceHttp2DeleteRequest(req);
      if (!ok) return status;
      continue;
    }
    if (status != 200) {
      sceHttp2DeleteRequest(req);
      return status;
    }
    size_t cap = 64 * 1024, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
      sceHttp2DeleteRequest(req);
      return -1;
    }
    for (;;) {
      if (len == cap) {
        if (cap >= max) {
          free(buf);
          sceHttp2DeleteRequest(req);
          return -3;
        }
        size_t ncap = cap * 2 > max ? max : cap * 2;
        unsigned char *nb = realloc(buf, ncap);
        if (!nb) {
          free(buf);
          sceHttp2DeleteRequest(req);
          return -1;
        }
        buf = nb;
        cap = ncap;
      }
      int n = sceHttp2ReadData(req, buf + len, cap - len);
      if (n < 0) {
        free(buf);
        sceHttp2DeleteRequest(req);
        return n;
      }
      if (n == 0) break;
      len += (size_t)n;
      if (progress) *progress = (long long)len;
    }
    sceHttp2DeleteRequest(req);
    *out = buf;
    *out_len = len;
    return 200;
  }
  return -4; /* too many redirects */
}

int plat_http_get(const char *url, unsigned char **out, size_t *out_len,
                  size_t max, volatile long long *progress) {
  return http_get_impl(url, out, out_len, max, progress);
}

/* ------------------------------------------------------------------------- */
/* tiles                                                                     */
/* ------------------------------------------------------------------------- */

static long ms_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)(t.tv_sec * 1000L + t.tv_nsec / 1000000L);
}

/* ---- fast install of one tile: let the console see only that tile --------------------------------------
 * sceAppInstUtilAppInstallAll walks every folder of /user/app (about 0.7 s per game, registered or not), so with a
 * big library one folder tile takes minutes. PS4 Mount avoids that by showing the console an empty staging folder
 * over /user/app with only the new title mounted into it. The same is done here: the tile's own directory is
 * mounted a second time at a hold path, the staging folder goes over /user/app, the tile goes back in at its place,
 * and the install-all then sees one title. Everything is undone afterwards, and any failure falls back to the
 * normal install-all. Not done while a game runs, or when something else already shows /user/app in a special way. */
static char g_stage[256];

void plat_set_stage_dir(const char *dir) { snprintf(g_stage, sizeof(g_stage), "%s", dir ? dir : ""); }

#define HH_IOV(x) {(void *)(x), strlen(x) + 1}

static int hh_nullfs(const char *from, const char *to) {
  struct iovec iov[] = {HH_IOV("fstype"), HH_IOV("nullfs"), HH_IOV("from"), HH_IOV(from), HH_IOV("fspath"), HH_IOV(to)};
  if (nmount(iov, 6, 0)) {
    hh_log("fast install: mount %s -> %s failed: %s", from, to, strerror(errno));
    return -1;
  }
  return 0;
}

/* 1 when the directory is a mount point of its own (statfs names the mount it belongs to) */
static int hh_is_mount_point(const char *path, char *from, size_t fromsz) {
  struct statfs sf;
  if (statfs(path, &sf)) return 0;
  if (from && fromsz) snprintf(from, fromsz, "%s", sf.f_mntfromname);
  return strcmp(sf.f_mntonname, path) == 0;
}

static void hh_unmount(const char *path) {
  for (int i = 0; i < 12; i++) {
    if (!unmount(path, 0)) return;
    usleep(250000); /* the system keeps a folder open for a moment after the install returned */
  }
  if (unmount(path, MNT_FORCE)) hh_log("fast install: unmount %s failed: %s", path, strerror(errno));
}

/* a game that runs keeps its folder in use: ask the console which game is open (PS4 Mount does the same).
 * Looking for any /mnt/sandbox mount was wrong: system apps have those all the time. */
static int hh_game_running(void) {
  char t[64];
  uint32_t id;
  return plat_running_game(t, sizeof(t), &id) == 0;
}

/* a daemon that stopped while /user/app showed the staging folder: show everything again */
void plat_unmask_leftover(void) {
  char from[300];
  if (!g_stage[0]) return;
  if (hh_is_mount_point("/user/app", from, sizeof(from)) && !strncmp(from, g_stage, strlen(g_stage))) {
    hh_log("fast install: /user/app was still showing only a tile from an earlier run, restoring it");
    DIR *d = opendir(g_stage);
    struct dirent *e;
    while (d && (e = readdir(d))) {
      if (e->d_name[0] == '.' || strlen(e->d_name) != 8) continue;
      char t[400];
      snprintf(t, sizeof(t), "/user/app/%s", e->d_name);
      if (hh_is_mount_point(t, NULL, 0)) hh_unmount(t);
    }
    if (d) closedir(d);
    hh_unmount("/user/app");
  }
}

/* returns 1 when the staging folder now covers /user/app with only these tiles in it */
static int hh_mask_user_app(const char (*tids)[16], int n, const char *app_dir) {
  char root[320];
  if (!g_stage[0]) {
    hh_log("fast install: not possible, no staging folder set");
    return 0;
  }
  if (hh_game_running()) {
    hh_log("fast install: not used, a game is open (it keeps its folder in use)");
    return 0;
  }
  {
    /* Another tool (PS4 Mount) may be showing only its own new games over /user/app right now, for about 8
     * seconds per batch. Wait for it to finish before putting our own cover on, so the two never overlap. */
    struct statfs sf;
    int waited = 0;
    while (waited < 120 && !statfs("/user/app", &sf) && !strcmp(sf.f_fstypename, "nullfs") && !strcmp(sf.f_mntonname, "/user/app")) {
      if (waited == 0) hh_log("fast install: /user/app is covered by another tool (%s), waiting for it to finish", sf.f_mntfromname);
      usleep(500000);
      waited++;
    }
    if (waited) hh_log("fast install: waited %d ms for the other tool", waited * 500);
    if (!statfs("/user/app", &sf)) {
      hh_log("fast install: /user/app is on %s (type %s, mount point %s)", sf.f_mntfromname, sf.f_fstypename, sf.f_mntonname);
      if (!strcmp(sf.f_fstypename, "nullfs") && !strcmp(sf.f_mntonname, "/user/app")) {
        hh_log("fast install: not used, /user/app is still covered by another tool after %d s", waited / 2);
        return 0;
      }
    }
  }
  snprintf(root, sizeof(root), "%s/root", g_stage);
  char tmp[400];
  snprintf(tmp, sizeof(tmp), "%s", g_stage);
  mkdir(tmp, 0777);
  snprintf(tmp, sizeof(tmp), "%s/hold", g_stage);
  mkdir(tmp, 0777);
  mkdir(root, 0777);
  int held = 0; /* tiles whose own files are at a second path (hold/<tid>) so far */
  for (int i = 0; i < n; i++) {
    char mp[400], hold[400], orig[400];
    snprintf(mp, sizeof(mp), "%s/%s", root, tids[i]);
    snprintf(hold, sizeof(hold), "%s/hold/%s", g_stage, tids[i]);
    snprintf(orig, sizeof(orig), "%s/%s", app_dir, tids[i]);
    mkdir(mp, 0777);
    mkdir(hold, 0777);
    if (hh_nullfs(orig, hold)) goto undo_holds; /* the tile's own files, at a second path */
    held++;
  }
  if (hh_nullfs(root, "/user/app")) goto undo_holds; /* the console now sees an empty folder... */
  for (int i = 0; i < n; i++) {                       /* ...and the tiles in it */
    char hold[400], target[400];
    snprintf(hold, sizeof(hold), "%s/hold/%s", g_stage, tids[i]);
    snprintf(target, sizeof(target), "/user/app/%s", tids[i]);
    if (hh_nullfs(hold, target)) {
      for (int k = 0; k < i; k++) {
        snprintf(target, sizeof(target), "/user/app/%s", tids[k]);
        hh_unmount(target);
      }
      hh_unmount("/user/app");
      goto undo_holds;
    }
  }
  return 1;
undo_holds:
  for (int k = 0; k < held; k++) {
    char hold[400];
    snprintf(hold, sizeof(hold), "%s/hold/%s", g_stage, tids[k]);
    hh_unmount(hold);
  }
  return 0;
}

static void hh_unmask_user_app(const char (*tids)[16], int n) {
  char from[300] = "";
  for (int i = 0; i < n; i++) {
    char target[400];
    snprintf(target, sizeof(target), "/user/app/%s", tids[i]);
    if (hh_is_mount_point(target, NULL, 0)) hh_unmount(target);
  }
  /* only take away our own cover: if another tool (PS4 Mount) put its own on top meanwhile, leave it alone */
  if (hh_is_mount_point("/user/app", from, sizeof(from)) && !strncmp(from, g_stage, strlen(g_stage)))
    hh_unmount("/user/app");
  else
    hh_log("fast install: /user/app is covered by something else now (%s), leaving it alone", from);
  for (int i = 0; i < n; i++) {
    char hold[400];
    snprintf(hold, sizeof(hold), "%s/hold/%s", g_stage, tids[i]);
    hh_unmount(hold);
  }
}

int plat_app_install(const char *title_id, const char *app_dir) {
  char one[1][16];
  snprintf(one[0], sizeof(one[0]), "%s", title_id);
  return plat_app_install_many((const char (*)[16])one, 1, app_dir);
}

int plat_app_install_many(const char (*ids)[16], int nids, const char *app_dir) {
  const char *title_id = ids[0];
  long t0 = ms_now();
  int err = sceAppInstUtilInitialize();
  if (err) {
    hh_log("install %s: sceAppInstUtilInitialize failed 0x%08X", title_id, (unsigned)err);
    return err;
  }
  long t1 = ms_now();
  /* resolved at run time, like Payload Manager, so a missing export on some
   * firmware can't stop the whole payload from loading */
  int (*install_dir)(const char *, const char *, void *) = NULL;
  uint32_t handle;
  int hrc = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle);
  int by_code = 0, by_name = 0;
  if (!hrc) {
    /* 1. by its code (Wudg3Xe3heE is sceAppInstUtilAppInstallTitleDir) */
    install_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    by_code = install_dir != NULL;
    /* 2. by its plain name, the way other console functions are found on newer firmware */
    if (!install_dir) {
      intptr_t p = kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppInstallTitleDir");
      if (p != 0 && p != -1) {
        install_dir = (void *)p;
        by_name = 1;
      }
    }
  }
  hh_log("install %s: looking for the fast install: by code Wudg3Xe3heE %s, by name sceAppInstUtilAppInstallTitleDir %s (library lookup returned %d)",
         title_id, by_code ? "found" : "not found", by_name ? "found" : (by_code ? "not needed" : "not found"), hrc);
  /* A control: the install-all function is known to exist (it is called below). If both of
   * these lookups find it, the lookups work on this firmware and a "not found" above is real. */
  int ctl_code = 0, ctl_name = 0;
  if (!hrc) {
    intptr_t c1 = (intptr_t)kernel_dynlib_resolve(-1, handle, "+scQA5stvjs");
    intptr_t c2 = (intptr_t)kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppInstallAll");
    ctl_code = c1 != 0 && c1 != -1;
    ctl_name = c2 != 0 && c2 != -1;
  }
  hh_log("install %s: control lookup of sceAppInstUtilAppInstallAll, which is known to exist: by code %s, by name %s",
         title_id, ctl_code ? "found" : "not found", ctl_name ? "found" : "not found");
  hh_log("install %s: %s", title_id,
         install_dir ? "installing this title's folder only (fast way)"
                     : "SLOW FALLBACK: the title-folder function wasn't found on this firmware, installing ALL apps");
  char dir[300];
  snprintf(dir, sizeof(dir), "%s/", app_dir);
  long t2 = ms_now();
  int masked = 0;
  if (!install_dir) {
    masked = hh_mask_user_app(ids, nids, app_dir);
    hh_log("install %s: %s (%d tile(s))", title_id, masked ? "the console sees only the changed tile(s) for the install (fast way, as PS4 Mount does)"
                                                          : "could not show the console only these tiles, installing with everything visible", nids);
  }
  if (install_dir) {
    err = 0;
    for (int i = 0; i < nids && !err; i++) err = install_dir(ids[i], dir, 0);
  } else {
    err = sceAppInstUtilAppInstallAll(0);
  }
  if (masked) hh_unmask_user_app(ids, nids);
  long t3 = ms_now();
  sceAppInstUtilTerminate();
  long t4 = ms_now();
  hh_log("install %s: initialize %ld ms, install %ld ms (returned 0x%08X), terminate %ld ms", title_id,
         t1 - t0, t3 - t2, (unsigned)err, t4 - t3);
  return err;
}

int plat_app_uninstall(const char *title_id) {
  int err = sceAppInstUtilInitialize();
  if (err) return err;
  err = sceAppInstUtilAppUnInstall(title_id);
  sceAppInstUtilTerminate();
  return err;
}

