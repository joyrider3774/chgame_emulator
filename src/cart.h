/*
 * .chgame carts (CHGame's spec/chgame.md, version 1): a ZIP with a manifest,
 * info.json, naming one or more games, each with a binary per board it runs
 * on (`binaries`: [{"device": "rev1", "filename": ...}]) and the files it
 * puts on the SD card's root (`sdcard`: a folder in the ZIP).
 *
 * The emulator runs one game of a cart: its binary for the board, with its
 * card files written into the emulated card's folder. A cart's menu (its
 * background, cover, folders, `launch`) is for a card prepared for CHGame's
 * SD menu bootloader, which is not done here.
 */
#ifndef CHG_CART_H
#define CHG_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ChgCart ChgCart;

/* true for data that starts like a ZIP file (a local file header) */
bool        chg_is_zip(const uint8_t *data, size_t size);

/* The cart in data (copied): its ZIP directory and info.json read and
   checked. NULL with err filled for anything that is not a cart this reads */
ChgCart    *chg_cart_open(const uint8_t *data, size_t size, char *err, size_t errlen);
void        chg_cart_free(ChgCart *c);

int         chg_cart_games(const ChgCart *c);
/* The game with this id, or the first for NULL or ""; -1 if there is none */
int         chg_cart_find(const ChgCart *c, const char *id);
const char *chg_cart_id(const ChgCart *c, int game);
const char *chg_cart_title(const ChgCart *c, int game);
/* The device names it has a binary for ("rev0 rev1"), for messages */
void        chg_cart_devices(const ChgCart *c, int game, char *out, size_t outlen);
bool        chg_cart_has(const ChgCart *c, int game, const char *device);

/* The game's binary for the device, malloc'ed; NULL with err filled */
uint8_t    *chg_cart_binary(const ChgCart *c, int game, const char *device, size_t *len,
                            char *err, size_t errlen);

/* The game's SD files written into folder (its root is the card's), folders
   made as needed; a file already there with the same bytes is left alone.
   Returns how many files were written or changed, -1 on an error */
int         chg_cart_write_sdcard(const ChgCart *c, int game, const char *folder,
                                  char *err, size_t errlen);

#endif
