#ifndef AKA_SDCARD_H
#define AKA_SDCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The card's storage: 512-byte sectors, from one of
 *   an image file   read and written in place (a whole-card image, as a card
 *                   reader dumps it, MBR and all)
 *   a folder        a FAT32 card built in memory from the folder's files;
 *                   sdcard_sync writes what the program changed back
 */
typedef struct SdCard SdCard;

/* A card from an image file or a folder. An image file that does not exist is
   created, formatted FAT32 at size_mb (0: the default, 256 MB). */
SdCard  *sdcard_open(const char *path, uint32_t size_mb, char *err, size_t errlen);
void     sdcard_close(SdCard *c);           /* syncs first */
uint64_t sdcard_sectors(const SdCard *c);
bool     sdcard_read(SdCard *c, uint64_t lba, uint8_t *buf, uint32_t count);
bool     sdcard_write(SdCard *c, uint64_t lba, const uint8_t *buf, uint32_t count);
/* a folder card: the files written back into the folder; an image: flushed */
bool     sdcard_sync(SdCard *c);

/* Writes a card image of size_mb (0: big enough for the folder, at least
   64 MB) holding folder's files, or an empty one when folder is NULL. */
bool     sdcard_make_image(const char *out, const char *folder, uint32_t size_mb, char *err, size_t errlen);

#endif
