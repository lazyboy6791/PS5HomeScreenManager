/* Export and import of everything Home Screen Manager manages: folders (with their own
 * pictures and backgrounds), hidden tiles, custom tile names, pictures and backgrounds, the saved
 * home screen order and the settings. The file is a ZIP with uncompressed entries, so it also
 * opens on a PC or phone. Import adds to what is there: nothing is deleted. */
#ifndef BACKUP_H
#define BACKUP_H

#include <stddef.h>

#include "hider.h"
#include "strbuf.h"

typedef struct {
  const char *data_dir;    /* where the pictures are kept */
  const char *app_version;
} backup_cfg_t;
void backup_init(const backup_cfg_t *cfg);

/* Builds the backup file into out. Returns 0 on success. */
int backup_export(hh_t *h, const char *settings_path, strbuf_t *out);

/* An opened backup file. On success it owns the bytes (malloc'd) and frees them in backup_close;
 * on failure it returns NULL, err says why, and the caller still owns the bytes. */
typedef struct backup backup_t;
backup_t *backup_open(unsigned char *zip, size_t len, char *err, size_t err_size);
void backup_close(backup_t *b);

/* What the file holds and what would be skipped (games that aren't installed here), as JSON. */
void backup_summary_json(hh_t *h, const backup_t *b, strbuf_t *sb);

typedef struct {
  volatile int folder_i, folder_n; /* "folder 3 of 7" */
  char phase[200];                 /* what is going on right now */
  int folders_made, folders_skipped, hidden_done, custom_done, order_done, settings_done, problems;
  char notes[600];                 /* the first problems, for the result */
} backup_progress_t;

/* Applies the backup: settings, folders, hidden tiles, custom names/pictures/backgrounds, order.
 * apply_settings(text, len) is called with the saved settings (may be NULL).
 * Creating each folder can take about 90 seconds on newer firmware. Returns 0 if it ran
 * (problems are counted in p), -1 if it couldn't start (err says why). */
int backup_apply(hh_t *h, backup_t *b, backup_progress_t *p, int (*apply_settings)(const char *, size_t),
                 char *err, size_t err_size);

#endif
