#ifndef CHG_LOADER_H
#define CHG_LOADER_H

#include <stddef.h>
#include <stdbool.h>

struct ChgMachine;

/* true (the default): an application is started the way a CHGame starts it,
   through the real bootloader, which is put at 0x0000 with the metadata page
   it checks. false: it is started at 0x3000 directly, with nothing below it */
extern bool chg_use_bootloader;

/* The bootloader put at 0x0000: a file of at most 12 KB (CHCasino's SD menu
   bootloader, say) instead of the built-in one, which NULL brings back */
bool chg_set_bootloader(const char *path, char *err, size_t errlen);
/* The board as the bootloader leaves it with nothing installed: the
   bootloader at 0x0000, the program flash erased. It runs from 0x0000 */
void chg_load_bootloader_only(struct ChgMachine *m);

bool chg_load_program(struct ChgMachine *m, const char *path, char *err, size_t errlen);
void chg_save_path(const char *program, char *out, size_t outlen);
bool chg_load_save(struct ChgMachine *m, const char *path);
bool chg_write_save(struct ChgMachine *m, const char *path);

#endif
