/* Moving the data folder when the app was renamed: /data/homehider became
 * /data/homescreenmanager. Nothing is lost if it goes wrong. */
#ifndef DATADIR_H
#define DATADIR_H

#include <stddef.h>

/* Moves old_dir to new_dir, if new_dir doesn't exist yet and old_dir does.
 *   1. everything is copied into a temporary folder next to new_dir,
 *   2. every file is read back and compared by its SHA-256,
 *   3. the temporary folder is renamed to new_dir,
 *   4. only then is old_dir removed.
 * The old own copy "HomeHider.elf" is renamed to "HomeScreenManager.elf" on the way.
 * Returns 0 if there was nothing to do (new_dir exists, or old_dir doesn't),
 *         1 if the data was moved,
 *        -1 if it couldn't be: old_dir is left exactly as it was and should keep
 *           being used. note gets one line for the log in every case but 0. */
int datadir_move(const char *old_dir, const char *new_dir, char *note, size_t note_size);

#endif
