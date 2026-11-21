/* Where a system tile opens (PlayStation Store, PlayStation Plus, Welcome).
 *
 * A tile's launch link is a "deeplink": tbl_contentinfo.pprDeeplinkUri and the DEEPLINK_URI entry inside its
 * AppInfoJson in app.db, and a DEEPLINK_URI row in appinfo.db (tbl_appinfo). Changing those makes the tile open
 * something else (a web page, or another part of the console). The idea and the database layout come from
 * JailbreakStore by maj0r (MIT licence), which does this for the Store tile with one fixed link.
 * What a tile held before is saved in <data>/redirect/<id>.txt the first time it is changed, so it can be put back. */
#pragma once

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

void sysredirect_init(const char *data_dir, const char *appinfo_db_path);
int  sysredirect_is(const char *tid); /* 1 for a tile whose link can be changed */
int  sysredirect_custom(const char *tid);
int  sysredirect_custom_ids(char (*ids)[16], int max);
/* [{"id":..,"name":..,"available":..,"custom":..,"url":..,"original":..}, ...] */
void sysredirect_json(hh_t *h, strbuf_t *sb);
/* url must look like "scheme:..." (up to 400 characters). 0 on success, else err says why. */
int  sysredirect_set(hh_t *h, const char *tid, const char *url, char *err, size_t errsz);
int  sysredirect_reset(hh_t *h, const char *tid, char *err, size_t errsz);
/* At start-up: puts our link back on a tile the console has changed. Returns how many were put back. */
int  sysredirect_reapply(hh_t *h);
/* [{"name":..,"url":..}, ...] the links offered in the menu list */
void sysredirect_presets_json(strbuf_t *sb);
