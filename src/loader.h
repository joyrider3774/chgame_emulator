#ifndef CHG_LOADER_H
#define CHG_LOADER_H

#include <stddef.h>
#include <stdbool.h>

struct ChgMachine;
struct ChgBoard;

/* The board asked for (--board), or NULL to let what is loaded decide: a
   CHG package names its board, a bootloader image carries its board word,
   and anything else runs on the default board (the newest). Loading sets
   the machine's board (chg_set_board) before it puts the bootloader in */
extern const struct ChgBoard *chg_board_choice;
/* The board for a program that does not say which it is for (a .bin, .hex
   or .elf without --bootloader), when no board was asked for: NULL is the
   newest. Unlike chg_board_choice it never refuses a CHG package for
   another board (the web page's board menu) */
extern const struct ChgBoard *chg_board_fallback;

/* .chgame carts: the game to run (its id; NULL or "" for the first), and the
   folder the emulated card is made from, which the game's SD files are
   written into (NULL: they are not; the card is an image, or there is none).
   After a load chg_cart_sd_written says how many card files were written or
   changed, so the front end knows to read the card in again */
extern const char *chg_cart_game;
extern const char *chg_cart_sd_folder;
extern int chg_cart_sd_written;

/* The board the --bootloader image is for (its board word), NULL with the
   built-in bootloaders or an image for a board this emulator does not know */
const struct ChgBoard *chg_bootloader_board(void);

/* true (the default): an application is started the way a CHGame starts it,
   through the real bootloader, which is put at 0x0000 with the metadata page
   it checks. false: it is started at 0x3000 directly, with nothing below it */
extern bool chg_use_bootloader;

/* The bootloader put at 0x0000: a file of at most 12 KB (CHGame's SD menu
   bootloader, say) instead of the board's built-in one, which NULL brings
   back */
bool chg_set_bootloader(const char *path, char *err, size_t errlen);
/* The board as the bootloader leaves it with nothing installed: the
   bootloader at 0x0000, the program flash erased. It runs from 0x0000 */
void chg_load_bootloader_only(struct ChgMachine *m);

bool chg_load_program(struct ChgMachine *m, const char *path, char *err, size_t errlen);
void chg_save_path(const char *program, char *out, size_t outlen);
bool chg_load_save(struct ChgMachine *m, const char *path);
bool chg_write_save(struct ChgMachine *m, const char *path);

#endif
