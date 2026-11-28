/* Backgrounds for the system tiles (PlayStation Store, Game Library, ...).
 *
 * The console keeps one background per system tile as a zstd file in /system_ex/vsh_asset
 * (bg_NPXS40047.dds.zst and so on): a 3840x2160 BC7 texture with its 148 byte DDS header, 8,294,548 bytes
 * once unpacked. A new background is made the same way (the same texture the game backgrounds use) and
 * written in the same place. The first time a tile is changed its original file is copied to
 * <data>/sysbg/<id>.orig; "original" puts that copy back. Only the thirteen tiles that have a known name are
 * handled; the three without a tile name are left alone. */
#pragma once

#include <stddef.h>

/* the id the menu uses for the console's default background (a file, not a tile of the home screen) */
#define SYSBG_DEFAULT_ID "NPXS00000"

void sysbg_init(const char *data_dir, const char *asset_dir);
int  sysbg_is(const char *tid);                 /* 1 for the thirteen tiles that can take a background */
const char *sysbg_name(const char *tid);        /* the tile's real name, or NULL */
int  sysbg_available(const char *tid);          /* 1 if the console has the tile's background file (and it is one of the thirteen) */
int  sysbg_custom(const char *tid);             /* 1 if it carries a background of ours (its original is saved) */
int  sysbg_ids(char (*ids)[16], int max);       /* the thirteen ids */
int  sysbg_custom_ids(char (*ids)[16], int max);/* the ones that carry a background of ours */

/* A picture made ready for writing: the zstd file and a 4K JPEG copy for previews. */
typedef struct {
  unsigned char *file;
  size_t file_len;
  unsigned char *jpg;
  size_t jpg_len;
} sysbg_blob_t;
int  sysbg_prepare(const unsigned char *img, size_t len, sysbg_blob_t *out, char *err, size_t errsz);
void sysbg_blob_free(sysbg_blob_t *b);

/* Writes a prepared picture as the tile's background (the original is saved first). */
int  sysbg_write(const char *tid, const sysbg_blob_t *b, char *err, size_t errsz);
/* Puts the original file back. */
int  sysbg_reset(const char *tid, char *err, size_t errsz);
/* The 4K JPEG of the chosen background, for the preview. 0 if there is one. */
int  sysbg_preview_path(const char *tid, char *out, size_t outsz);
