/* Pictures for the system tiles (PlayStation Store, Game Library, ...).
 *
 * Unlike a game, a system tile takes its picture from a file the firmware ships: a 512x512 icon0.png inside the
 * tile's own app folder. Where that file is depends on the firmware: newer ones read the copy under
 * /system_ex/rnps/apps/<app>/appdb/<entry>/icon0.png, firmware that keeps "live" copies of these apps reads
 * /mnt/rnps/apps/<app>/appdb/<entry>/icon0.png and ignores the other one. A new picture is written to the live copy
 * when the console has one, otherwise to the shipped one. The first time a tile is changed the file it replaces is
 * copied to <data>/sysicon/<id>.orig (and its path to <id>.where); "original" puts it back where it came from. */
#pragma once

#include <stddef.h>

void sysicon_init(const char *data_dir, const char *live_root, const char *shipped_root);
int  sysicon_is(const char *tid);                 /* 1 for a tile whose picture can be changed this way */
int  sysicon_available(const char *tid);          /* 1 if the console has the tile's picture file */
int  sysicon_custom(const char *tid);             /* 1 if it carries a picture of ours */
int  sysicon_custom_ids(char (*ids)[16], int max);
/* The file the home screen reads for the tile (the live copy, else the shipped one). 0 if there is one. */
int  sysicon_path(const char *tid, char *out, size_t outsz);
/* img is any picture; clear = keep see-through parts transparent instead of a dark background. */
int  sysicon_write(const char *tid, const unsigned char *img, size_t len, int clear, char *err, size_t errsz);
int  sysicon_reset(const char *tid, char *err, size_t errsz);
