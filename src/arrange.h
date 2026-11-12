/* Pre-arranging the home screen: a fixed order for tiles, and pinning.
 *
 * The PS5 sorts each tab's tiles by how recently they were used
 * (tbl_iconinfo_<u>.lastAccessIndex, counted by tbl_version.access_index).
 * There is no manual reorder in the stock UI, so Home Hider forces an order
 * by writing lastAccessIndex values above the counter: arranged tiles then
 * sit in front, in the chosen order, and normal play (which gets the next,
 * smaller index) doesn't disturb them. "Pin" also adds the tile to the
 * console's own MyHome group.
 *
 * Every original value is saved in state.db, so the arrangement can be undone
 * exactly. It is re-applied at each launch, like hidden tiles, in case the
 * system rebuilt the order.
 */
#pragma once

#include "hider.h"
#include "strbuf.h"

/* Sets the order. items_csv = title ids front-to-back, e.g.
 * "PPSA01325,CUSA00900". pinned_csv = the subset to also pin (any order).
 * An empty items_csv clears the arrangement. 0 on success. */
int  arrange_set(hh_t *h, const char *items_csv, const char *pinned_csv);

/* Undoes the arrangement (restores lastAccessIndex and MyHome). 0 on success. */
int  arrange_clear(hh_t *h);

/* Re-applies the saved arrangement. *fixed = tiles whose order was set. */
int  arrange_apply(hh_t *h, int *fixed);

/* JSON: {"on":bool,"items":[{"id","tab","pinned"}...]} in order. */
int  arrange_json(hh_t *h, strbuf_t *sb);

/* Keeps every folder in the first spots of its tab, ahead of all games and
 * apps (the folders still reorder among themselves by recent use). The block
 * sits just above the PS5's own numbers and moves up only as they do, and the
 * PS5's counter is raised to the top of the block so that a folder the PS5 has
 * just opened is numbered above the other folders. *fixed = rows changed. */
int  folders_front_apply(hh_t *h, int *fixed);

/* Switches that off: folders get the PS5's own numbers back. 0 on success. */
int  folders_front_clear(hh_t *h);
