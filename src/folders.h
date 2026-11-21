/* Folders: one home screen tile that opens a page listing several games.
 *
 * A folder is a small launcher app (like Payload Manager's tile) installed at
 * <app_dir>/HHFDnnnnn with a param.json whose deeplink opens
 * http://127.0.0.1:<port>/folder/HHFDnnnnn in the PS5 browser. The games put
 * in a folder are hidden from the home screen with the normal hide mechanism,
 * and shown again when they leave their last folder (unless they were hidden
 * before). A game can be in any number of folders.
 */
#pragma once

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

#define FOLDER_PREFIX   "HHFD"
#define FOLDER_MAX      99
#define FOLDER_ITEMS_MAX 300
#define FOLDER_NAME_MAX 48

typedef struct {
  const char *data_dir;  /* custom icons live in <data_dir>/folders */
  const char *app_dir;   /* "/user/app" */
  int port;              /* menu port, for the tile's deeplink */
} folders_cfg_t;

void folders_init(const folders_cfg_t *cfg);
int  folders_is_folder(const char *tid);
/* The directory the folder tiles' files live in (/user/app on the console). */
const char *folders_app_dir(void);
void folders_set_appmeta_dir(const char *dir);
/* The home screen database, watched for other tools' activity before an install. */
void folders_set_db_path(const char *path);

/* JSON array of folders with their items. */
int  folders_list_json(hh_t *h, strbuf_t *sb);
/* JSON object for one folder; -1 if it doesn't exist. */
int  folders_get_json(hh_t *h, const char *tid, strbuf_t *sb);

/* Creates (tid empty) or updates a folder. items_csv = "PPSA01325,CUSA00900".
 * out_tid receives the folder id. note gets a warning, if any. 0 on success. */
int  folders_save(hh_t *h, const char *tid, const char *name, const char *tab,
                  const char *color, int style, const char *items_csv, char *out_tid,
                  char *note, size_t notesz, int *changed);
/* style = the collage style of the automatic picture (0 grid, 1 stack, 2 feature, 3 single);
 * -1 keeps the folder's present one. */
/* The automatic picture for these games (comma separated ids), colour and style, without saving anything.
 * *png is malloc'd. 0 on success. */
int  folders_preview(hh_t *h, const char *items_csv, const char *color, int style,
                     unsigned char **png, size_t *png_len);
/* *changed (may be NULL) is set to 0 when tid is an existing folder that is
 * already exactly like this (same name, tab, colour and games in the same order,
 * tile in the home screen database): then nothing at all is done. */
/* 1 while a folder is being saved (or its picture or background changed). The
 * console's installer can take about 90 seconds, and the background repair of
 * folder tiles waits for it. */
int  folders_busy(void);
/* Marks a long job (an import) as running, so the background repair waits like it does for a save. */
void folders_hold(int on);
/* While on (for the calling thread), creating a folder or changing its picture or background only writes
 * the tile's files. folders_install_pending() then registers all of them with one install. Returns the
 * number of folders the console did not register. */
void folders_defer_install(int on);
void folders_forget_pending(void);
int  folders_install_pending(hh_t *h);


int  folders_delete(hh_t *h, const char *tid);
int  folders_ids(hh_t *h, char (*ids)[16], int max);
/* Moves a folder's tile to the Games or Media tab. */
int  folders_set_tab(hh_t *h, const char *tid, const char *tab);
/* Takes one app out of a folder. It goes back on its tab when it is out of its
 * last folder. folder may be NULL or "" if the app is in only one folder. */
int  folders_remove_member(hh_t *h, const char *tid, const char *folder);
/* img == NULL goes back to the automatic collage. */
int  folders_set_icon(hh_t *h, const char *tid, const unsigned char *img, size_t len);
/* Home screen background for the folder tile (sce_sys/pic0.dds). img NULL
 * removes it. */
int  folders_set_bg(hh_t *h, const char *tid, const unsigned char *img, size_t len);
/* The chosen background picture (JPEG), for previews. 0 if there is one. */
int  folders_bg_path(const char *tid, char *out, size_t outsz);
/* Path of the tile's current icon0.png. */
int  folders_icon_path(const char *tid, char *out, size_t outsz);

/* Launch-time repair: reinstall missing tiles, put tiles back on their tab.
 * *fixed = number of folders that needed it. */
int  folders_reapply(hh_t *h, int *fixed);
/* The watchdog's version: missing folders must stay missing for a while, and the database must have been
 * quiet for `quiet` seconds, before they are reinstalled (another tool may be registering games). */
int  folders_reapply_patient(hh_t *h, int *fixed, long quiet);
/* Removes folder tiles (ids HHFD...) that no folder of ours is behind, with their files. */
int  folders_clear_stale_bg(hh_t *h); /* returns how many database entries were cleared */
int  folders_sweep_orphans(hh_t *h, int *removed);
int  folders_sweep_tiles_removed(void); /* tiles the last sweeps took off the home screen (reset on read) */
/* Uninstalls every folder tile (used before restoring the original database). */
void folders_remove_all_tiles(hh_t *h);
/* The first folder holding tid ("" if none); a game can be in several. */
int  folders_of_title(hh_t *h, const char *tid, char *out, size_t outsz);
