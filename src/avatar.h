/* Profile pictures (avatars) of the console's users.
 *
 * The home screen reads a user's picture from /system_data/priv/cache/profile/0x<USER ID>/ : avatar64.dds,
 * avatar128.dds, avatar260.dds and avatar440.dds (DXT5, no mips, 128 byte header), and it wants an online.json in the
 * same folder (its content does not matter, so an empty {} is made only when there is none, which leaves a PSN
 * user's names alone). The method is the one Prospero Themes uses (measured by its author on 13.00, reported to work
 * on 13.60). Only these five file names are ever written, read or removed, in folders named 0x + eight hex digits.
 *
 * The first time a user's picture is changed the files that are there are copied to <data>/avatar/<ID>/ with a small
 * state file; "original" puts exactly that back (and removes what was not there before). A later change keeps the
 * first backup. The home screen shows a new picture after it restarts. */
#pragma once

#include <stddef.h>

#include "strbuf.h"

void avatar_init(const char *data_dir, const char *profile_root);
/* 1 if the console has the profile folder (so a picture can be changed at all) */
int  avatar_available(void);
/* {"available":bool,"profiles":[{"id":"0x1A2B3C4D","custom":bool,"has_picture":bool,"name":"..","auto":bool,"pool":[1,2]}]} */
void avatar_json(strbuf_t *sb);
/* 1 if id has the shape 0x + eight hex digits */
int  avatar_valid_id(const char *id);
/* The picture the user has now, as PNG (decoded from avatar128.dds, else the avatar.png file). 0 on success. */
int  avatar_current_png(const char *id, unsigned char **png, size_t *len);
/* img is any picture (PNG, JPG, ...); it is cropped from the middle to a square. 0 on success, else err is set. */
int  avatar_apply(const char *id, const unsigned char *img, size_t len, char *err, size_t errsz);
/* Puts back what was there before the first change. 0 on success (also when nothing was changed). */
int  avatar_reset(const char *id, char *err, size_t errsz);
int  avatar_custom(const char *id);
int  avatar_custom_ids(char (*ids)[16], int max);

/* Several pictures per user. They are kept (512 px squares) in <data>/avatar/pool/<ID>/. With "auto" on and at least two
 * pictures, avatar_rotate_all() writes a different one at random for every such user (the home screen shows it after
 * its next restart). "Use the original" turns auto off and keeps the pictures. Up to AV_POOL_MAX per user. */
#define AV_POOL_MAX 12
int  avatar_pool_add(const char *id, const unsigned char *img, size_t len, char *err, size_t errsz);
int  avatar_pool_remove(const char *id, int num);
int  avatar_pool_get(const char *id, int num, unsigned char **png, size_t *len);
int  avatar_pool_auto_set(const char *id, int on);
int  avatar_pool_use(const char *id, int num, char *err, size_t errsz);
int  avatar_pool_random(const char *id, char *err, size_t errsz);
int  avatar_rotate_all(void);
void avatar_pool_clear_all(void);
