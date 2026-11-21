/* The home screen music (EXPERIMENTAL).
 *
 * The console plays /system_ex/vsh_asset/bgm_home.at9 on the home screen: an ATRAC9 file (48 kHz, 6 channels, looping).
 * A song (WAV or MP3) is decoded, leveled to the loudness of the console's own music, faded at both ends so the loop
 * doesn't click, encoded with the ATRAC9 encoder (atrac9.c), checked by decoding it again, and only then does it
 * replace the file. The first time the console's own file is copied to <data>/music/original.at9; "original" puts it
 * back, unless something else (a firmware update, another tool) has changed the file since: then it is left alone.
 * Only this one file is ever touched. */
#pragma once

#include <stddef.h>

#include "strbuf.h"

void sysmusic_init(const char *data_dir, const char *asset_dir);
int  sysmusic_available(void); /* 1 if the console has the home music file */
int  sysmusic_custom(void);    /* 1 if the file is ours (a song was applied and its original is saved) */
/* {"available":..,"custom":..,"name":..,"seconds":..,"cut":..,"cut_at":..,"level_db":..,"volume_db":..,"changed":..} */
void sysmusic_json(strbuf_t *sb);

/* Called with 0..1 now and then; non-zero cancels. */
typedef int (*sysmusic_progress_fn)(void *ctx, double fraction, const char *what);

/* The whole job: decode `data`, level it (volume_db on top), encode, verify, save the original, replace the file.
 * name is shown in the menu. 0 on success (the old file is untouched on any failure); err says why otherwise. */
int sysmusic_apply(const unsigned char *data, size_t len, const char *name, double volume_db, sysmusic_progress_fn progress, void *ctx,
                   char *err, size_t errsz);
/* Puts the original back. *note (if not NULL) gets a sentence when nothing was put back because the file had been changed. */
int sysmusic_reset(char *note, size_t notesz, char *err, size_t errsz);
