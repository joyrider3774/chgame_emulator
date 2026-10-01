#ifndef CHG_SDSPI_H
#define CHG_SDSPI_H

#include <stdbool.h>
#include <stdint.h>

struct SdCard;

/* A microSD card in SPI mode: what it is in the middle of saying and hearing */
typedef struct {
    bool     idle;          /* until ACMD41 finishes initialisation */
    bool     app;           /* the last command was CMD55 */
    uint8_t  cmd[6];
    int      ncmd;
    uint8_t  out[600];      /* bytes the card will shift out next */
    int      nout, pout;
    bool     reading;       /* CMD18: blocks keep coming until CMD12 */
    int      wstate;        /* 0, 1 waiting for a data token, 2 taking a block */
    bool     multi_write;   /* CMD25 */
    uint8_t  blk[514];
    int      nblk;
    uint64_t lba;
} ChgSdSpi;

void    sdspi_reset(ChgSdSpi *c);
/* one byte each way while the card is selected */
uint8_t sdspi_xfer(ChgSdSpi *c, struct SdCard *card, uint8_t mosi);
/* chip select went high */
void    sdspi_deselect(ChgSdSpi *c);

#endif
