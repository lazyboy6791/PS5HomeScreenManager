#include "updater.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "hider.h"
#include "json.h"
#include "log.h"
#include "platform.h"

/* Kept in the binary so other builds (and this updater) can read our version. */
__attribute__((used)) static const char MARKER[] = "HOMESCREENMANAGER-ELF-VERSION=" HH_VERSION ";";
/* The marker of the builds from before the rename. Those builds look for it to read the version
 * of a file (to offer it as an update), so this build carries it as well. */
__attribute__((used)) static const char MARKER_BEFORE_RENAME[] = "HOMEHIDER-ELF-VERSION=" HH_VERSION ";";
const char *upd_marker(void) { return MARKER; }

static struct {
  char github_api[128];
} C;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static char g_repo[128];

static struct {
  int busy;
  char phase[16];
  char message[320];

  int gh_checked, gh_ok, gh_newer;
  char gh_error[200], gh_version[64], gh_notes[1600], gh_page[300];
} S;

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void set_phase(const char *phase, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_phase(const char *phase, const char *fmt, ...) {
  pthread_mutex_lock(&g_mx);
  snprintf(S.phase, sizeof(S.phase), "%s", phase);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(S.message, sizeof(S.message), fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&g_mx);
}

static int parse_ver(const char *s, int v[4]) {
  memset(v, 0, 4 * sizeof(int));
  if (!s) return -1;
  while (*s == ' ') s++;
  if (*s == 'v' || *s == 'V') s++;
  while (*s == '.' || *s == ' ' || *s == '-' || *s == '_') s++; /* a tag like "v.2.2.2" or "v 2.2.2" */
  if (!isdigit((unsigned char)*s)) return -1;
  for (int i = 0; i < 4; i++) {
    if (!isdigit((unsigned char)*s)) break;
    long n = 0;
    while (isdigit((unsigned char)*s) && n < 100000) n = n * 10 + (*s++ - '0');
    v[i] = (int)n;
    if (*s != '.') break;
    s++;
  }
  return 0;
}

int upd_version_cmp(const char *a, const char *b) {
  int x[4], y[4];
  parse_ver(a, x);
  parse_ver(b, y);
  for (int i = 0; i < 4; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

static int ends_with_ci(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcasecmp(s + a - b, suf) == 0;
}

static int contains_ci(const char *s, const char *needle) {
  size_t n = strlen(needle);
  for (; *s; s++)
    if (strncasecmp(s, needle, n) == 0) return 1;
  return 0;
}


/* ------------------------------------------------------------------------- */
/* checks                                                                    */
/* ------------------------------------------------------------------------- */

/* "v2.2.2", "V2.2.2", "v.2.2.2", "v 2.2.2" -> "2.2.2" */
static const char *tag_version(const char *tag) {
  if (*tag == 'v' || *tag == 'V') tag++;
  while (*tag == '.' || *tag == ' ' || *tag == '-' || *tag == '_') tag++;
  return tag;
}

static void check_github(void) {
  char repo[128];
  pthread_mutex_lock(&g_mx);
  snprintf(repo, sizeof(repo), "%s", g_repo);
  S.gh_checked = 1;
  S.gh_ok = S.gh_newer = 0;
  S.gh_error[0] = S.gh_version[0] = S.gh_notes[0] = S.gh_page[0] = '\0';
  pthread_mutex_unlock(&g_mx);

  char err[200] = "";
  if (!repo[0]) {
    hh_log("update check: no GitHub repository is set, GitHub was not checked");
    pthread_mutex_lock(&g_mx);
    snprintf(S.gh_error, sizeof(S.gh_error), "No GitHub repository set.");
    pthread_mutex_unlock(&g_mx);
    return;
  }

  set_phase("checking", "Checking GitHub (%s)…", repo);
  char url[400];
  snprintf(url, sizeof(url), "%s/repos/%s/releases/latest", C.github_api, repo);
  unsigned char *b = NULL;
  size_t len = 0;
  int status = plat_http_get(url, &b, &len, 2u * 1024 * 1024, NULL);
  if (status != 200) {
    if (status == 404) snprintf(err, sizeof(err), "No published release found in %s.", repo);
    else if (status < 0) snprintf(err, sizeof(err), "Couldn't reach GitHub: this console can't make the secure connection it needs. Look at the release page on GitHub by hand.");
    else snprintf(err, sizeof(err), "GitHub answered HTTP %d.", status);
    if (status < 0)
      hh_log("update check: GitHub %s: couldn't connect (code 0x%08X, failed while: %s)", repo, (unsigned)status,
             plat_http_last_step()[0] ? plat_http_last_step() : "unknown");
    else hh_log("update check: GitHub %s: %s", repo, err);
    free(b);
    pthread_mutex_lock(&g_mx);
    snprintf(S.gh_error, sizeof(S.gh_error), "%s", err);
    pthread_mutex_unlock(&g_mx);
    return;
  }

  int max = (int)(len / 2 + 16);
  json_tok_t *t = malloc((size_t)max * sizeof(json_tok_t));
  int n = t ? json_parse((const char *)b, len, t, max) : -1;
  const char *js = (const char *)b;
  char tag[64] = "", notes[1600] = "", page[300] = "";
  int best_score = -1;
  if (n > 0 && t[0].type == JSON_OBJ) {
    json_str(js, t, json_get(js, t, n, 0, "tag_name"), tag, sizeof(tag));
    json_str(js, t, json_get(js, t, n, 0, "body"), notes, sizeof(notes));
    json_str(js, t, json_get(js, t, n, 0, "html_url"), page, sizeof(page));
    int assets = json_get(js, t, n, 0, "assets");
    for (int i = 0;; i++) {
      int a = json_at(t, n, assets, i);
      if (a < 0) break;
      char name[128] = "";
      if (json_str(js, t, json_get(js, t, n, a, "name"), name, sizeof(name)) != 0) continue;
      if (!ends_with_ci(name, ".elf")) continue;
      int score = (contains_ci(name, "homescreenmanager") || contains_ci(name, "homehider")) ? 2 : 1;
      if (score <= best_score) continue;
      best_score = score;
    }
  }
  free(t);
  free(b);

  pthread_mutex_lock(&g_mx);
  if (n <= 0) {
    snprintf(S.gh_error, sizeof(S.gh_error), "GitHub sent a reply Home Screen Manager couldn't read.");
  } else if (!tag[0]) {
    snprintf(S.gh_error, sizeof(S.gh_error), "The latest release has no version tag.");
  } else if (best_score < 0) {
    snprintf(S.gh_error, sizeof(S.gh_error), "Release %s has no .elf file attached.", tag);
    snprintf(S.gh_version, sizeof(S.gh_version), "%s", tag_version(tag));
  } else {
    S.gh_ok = 1;
    snprintf(S.gh_version, sizeof(S.gh_version), "%s", tag_version(tag));
    snprintf(S.gh_notes, sizeof(S.gh_notes), "%s", notes);
    snprintf(S.gh_page, sizeof(S.gh_page), "%s", page);
    S.gh_newer = upd_version_cmp(S.gh_version, HH_VERSION) > 0;
  }
  hh_log("update check: GitHub %s: %s", repo,
         S.gh_ok ? (S.gh_newer ? "a newer version is available" : "no newer version") : S.gh_error);
  pthread_mutex_unlock(&g_mx);
}

/* ------------------------------------------------------------------------- */
/* the check                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
  int notify;
} job_t;

static void *worker(void *arg) {
  job_t j = *(job_t *)arg;
  free(arg);
  check_github();
  pthread_mutex_lock(&g_mx);
  int gh = S.gh_newer;
  char gv[64];
  snprintf(gv, sizeof(gv), "%s", S.gh_version);
  pthread_mutex_unlock(&g_mx);
  if (gh) {
    set_phase("available", "Home Screen Manager v%s is available.", gv);
    if (j.notify)
      plat_notify("Home Screen Manager v%s is available on GitHub and the ProsperoStore", gv);
  } else {
    set_phase("idle", "You have the latest version (v%s).", HH_VERSION);
  }
  pthread_mutex_lock(&g_mx);
  S.busy = 0;
  pthread_mutex_unlock(&g_mx);
  return NULL;
}

static int start_job(const job_t *src) {
  pthread_mutex_lock(&g_mx);
  if (S.busy) {
    pthread_mutex_unlock(&g_mx);
    return -1;
  }
  S.busy = 1;
  pthread_mutex_unlock(&g_mx);
  job_t *j = malloc(sizeof(*j));
  pthread_t t;
  if (j) {
    *j = *src;
    if (pthread_create(&t, NULL, worker, j) == 0) {
      pthread_detach(t);
      return 0;
    }
    free(j);
  }
  pthread_mutex_lock(&g_mx);
  S.busy = 0;
  pthread_mutex_unlock(&g_mx);
  return -1;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

void upd_init(const upd_config_t *cfg) {
  snprintf(C.github_api, sizeof(C.github_api), "%s", cfg->github_api);
  snprintf(S.phase, sizeof(S.phase), "idle");
}

int upd_valid_repo(const char *r) {
  if (!r) return 0;
  if (!r[0]) return 1; /* empty = not set */
  size_t n = strlen(r);
  if (n > 100) return 0;
  const char *slash = strchr(r, '/');
  if (!slash || slash == r || !slash[1] || strchr(slash + 1, '/')) return 0;
  for (const char *p = r; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.' && *p != '/')
      return 0;
  return strstr(r, "..") == NULL;
}

void upd_set_repo(const char *repo) {
  pthread_mutex_lock(&g_mx);
  snprintf(g_repo, sizeof(g_repo), "%s", upd_valid_repo(repo) ? repo : "");
  pthread_mutex_unlock(&g_mx);
}

int upd_start_check(int notify_if_found) {
  job_t j = {0};
  j.notify = notify_if_found;
  return start_job(&j);
}

void upd_status_json(strbuf_t *sb) {
  pthread_mutex_lock(&g_mx);
  sb_printf(sb, "{\"ok\":true,\"current\":\"%s\",\"repo\":", HH_VERSION);
  sb_json_str(sb, g_repo);
  sb_printf(sb, ",\"busy\":%s,\"phase\":", S.busy ? "true" : "false");
  sb_json_str(sb, S.phase);
  sb_puts(sb, ",\"message\":");
  sb_json_str(sb, S.message);
  sb_printf(sb, ",\"github\":{\"checked\":%s,\"ok\":%s,\"newer\":%s,\"error\":",
            S.gh_checked ? "true" : "false", S.gh_ok ? "true" : "false",
            S.gh_newer ? "true" : "false");
  sb_json_str(sb, S.gh_error[0] ? S.gh_error : NULL);
  sb_puts(sb, ",\"version\":");
  sb_json_str(sb, S.gh_version);
  sb_puts(sb, ",\"notes\":");
  sb_json_str(sb, S.gh_notes);
  sb_puts(sb, ",\"page\":");
  sb_json_str(sb, S.gh_page);
  sb_puts(sb, "}}");
  pthread_mutex_unlock(&g_mx);
}
