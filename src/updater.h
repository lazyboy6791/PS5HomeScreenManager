/* Home Screen Manager update check.
 *
 * Looks at the latest GitHub release of a repository and tells the menu (and a
 * notification at start) when a newer version exists. Home Screen Manager does
 * not install anything itself: a new version is loaded by hand, like the first one.
 */
#pragma once

#include <stddef.h>

#include "strbuf.h"

typedef struct {
  const char *github_api;   /* "https://api.github.com" */
} upd_config_t;

/* The marker embedded in this build ("HOMESCREENMANAGER-ELF-VERSION=x.y.z;"; builds before the rename used HOMEHIDER-ELF-VERSION, and this build carries both). */
const char *upd_marker(void);

void upd_init(const upd_config_t *cfg);
void upd_set_repo(const char *repo);            /* "owner/name" or "" */
int  upd_valid_repo(const char *repo);

/* Starts a background check of GitHub. Returns 0 if started, -1 if busy. */
int  upd_start_check(int notify_if_found);

void upd_status_json(strbuf_t *sb);

/* Exposed for tests/diagnostics. */
int  upd_version_cmp(const char *a, const char *b);
