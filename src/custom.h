/* Renaming tiles and giving them a different picture.
 *
 * The home screen takes a tile's name and picture from app.db:
 *   - name:    titleName / conceptName cells of the rows that belong to the
 *              title (tbl_iconinfo_<u>, tbl_concepticoninfo_<u>,
 *              tbl_contentinfo, ... found by titleId or concept id)
 *   - picture: the icon0Info path in those rows
 *   - background: the pic0Info / pic1Info paths (see texture.h for formats)
 * Home Screen Manager writes the new picture to <appmeta>/<title id>/hh_icon0_<n>.png
 * (a new file name each time, so the home screen can't show a cached copy),
 * points icon0Info at it and changes the name cells. Every original value is
 * saved in state.db, so "Use the original" puts back exactly what was there.
 * Nothing in the game's own files is changed.
 */
#pragma once

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

#define CUSTOM_NAME_MAX 64

typedef struct {
  const char *data_dir;     /* our copy of each picture: <data_dir>/icons */
  const char *appmeta_dir;  /* "/user/appmeta" */
} custom_cfg_t;

void custom_init(const custom_cfg_t *cfg);

/* name NULL or "" (or the original name) puts the original name back. */
int  custom_set_name(hh_t *h, const char *tid, const char *name);
/* img NULL puts the original picture back. clear = keep see-through parts
 * transparent instead of putting them on a dark background. */
int  custom_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len, int clear);
/* Home screen background (pic0/pic1). img NULL puts the original back. */
int  custom_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len);
/* Folder tiles only: puts a folder's current name and picture into the home
 * screen database without the console's installer (which takes about 90 seconds
 * on newer firmware). The folder's own files stay the source of truth, so no
 * "originals" are saved. png is the finished picture. Returns -1 (h->err says
 * why) if the tile's name or picture can't be found in the database, so the
 * caller can use the installer instead. */
int  custom_refresh_folder_tile(hh_t *h, const char *tid, const char *name, const unsigned char *png,
                                size_t len);

/* The chosen background picture (a JPEG copy), for previews. 0 if any. */
int  custom_bg_path(hh_t *h, const char *tid, char *out, size_t n);

/* {"id","name","orig_name","renamed","custom_icon","custom_bg","can_rename",
 *  "can_icon","can_bg"} */
int  custom_get_json(hh_t *h, const char *tid, strbuf_t *sb);

/* Launch-time repair: puts custom names/pictures back if the system
 * restored the originals. *fixed = number of values changed. */
int  custom_reapply(hh_t *h, int *fixed);

/* Bulk changes (several tiles at once). Between begin and end, the picture is converted once for
 * each file format and the result is linked into every tile. */
void custom_gc_defer(int on);       /* 1: do not clean shared files after every tile (bulk undo) */
void custom_shared_gc(hh_t *h);     /* remove shared files no tile uses any more */
void custom_bulk_begin(int share); /* share: one converted file for all the games (experimental) */
void custom_bulk_end(long *encode_ms, long *reused, long *linked, long *copied, long *shared);
int  custom_set_bg_prepared(hh_t *h, const char *tid, const unsigned char *jpg, size_t len, const char *prepared_path);
int  custom_can_bg(hh_t *h, const char *tid);
void custom_bulk_info_json(hh_t *h, char (*ids)[16], int n, strbuf_t *sb);
/* Number of tiles with a custom name, picture or background. */
int  custom_rows(hh_t *h);

/* Deletes Home Screen Manager's picture files (before the original database is
 * restored, which brings back the original names and pictures). */
void custom_forget_all(hh_t *h);
int  custom_has_bg(hh_t *h, const char *tid);
int  custom_renamed_ids(hh_t *h, char (*ids)[16], int max);
int  custom_bg_ids(hh_t *h, char (*ids)[16], int max);

/* A name from Clean Up Game Names (remembers that it did not come from the tile editor). */
int  custom_set_name_tool(hh_t *h, const char *tid, const char *name);
/* Puts a name back after a cleanup; names given by hand only when hand_too. 1 = changed, 0 = not, -1 = error. */
int  custom_undo_names(hh_t *h, const char *tid, int hand_too);
/* The experiment: fill the empty background slots of a tile that has none. write = 0 only reports.
 * 0 = done, 1 = only a report was made, -1 = error (text in h->err). */
int  custom_try_bg(hh_t *h, const char *tid, const unsigned char *jpg, size_t len, const char *prepared_path, int write);
