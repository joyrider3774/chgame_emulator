/*
 * The CHGame's microSD card, in SPI mode (SD Physical Layer spec, chapter 7):
 * an SDHC card of whatever size the SdCard holds, block addressed.
 *
 * A command is 6 bytes, 01xxxxxx first; the card answers after one filler
 * byte (0xFF) with R1, plus R3/R7's four bytes (CMD58, CMD8) or a data block
 * (start token 0xFE, the data, two CRC bytes) for CMD9/10/17/18 and ACMD13/51.
 * CMD18 streams blocks until CMD12. A write (CMD24, CMD25) takes the host's
 * token (0xFE, 0xFC for each block of CMD25, 0xFD to end it), 512 bytes and
 * two CRC bytes, answers 0x05 (accepted) and is busy (0x00) for a byte or two.
 * CRCs are not checked, as with CRC off (the default in SPI mode).
 *
 * MISO is 0xFF whenever the card has nothing to say, as the pull-up makes it.
 */
#include <string.h>
#include "sdcard.h"
#include "sdspi.h"

#define R1_IDLE     0x01
#define R1_ILLEGAL  0x04

void sdspi_reset(ChgSdSpi *c)
{
    memset(c, 0, sizeof *c);
    c->idle = true;
}

void sdspi_deselect(ChgSdSpi *c)
{
    c->ncmd = 0;
    c->nout = c->pout = 0;
    c->reading = false;
}

static void put(ChgSdSpi *c, uint8_t b)
{
    if (c->nout < (int)sizeof c->out) c->out[c->nout++] = b;
}

static void start_response(ChgSdSpi *c)
{
    c->nout = c->pout = 0;
    put(c, 0xFF);                       /* NCR: one byte before the answer */
}

/* a data block: a filler byte, the start token, the data, two CRC bytes */
static void put_block(ChgSdSpi *c, const uint8_t *data, int n)
{
    put(c, 0xFF);
    put(c, 0xFE);
    for (int i = 0; i < n; i++) put(c, data[i]);
    put(c, 0xFF);
    put(c, 0xFF);
}

static bool read_block(struct SdCard *card, uint64_t lba, uint8_t out[512])
{
    if (!card || !sdcard_read(card, lba, out, 1)) {
        memset(out, 0xFF, 512);
        return false;
    }
    return true;
}

/* bit n of a big-endian 16-byte register (CSD, CID) */
static void reg_bits(uint8_t r[16], int start, int len, uint32_t v)
{
    for (int i = 0; i < len; i++)
        if (v >> i & 1) r[15 - (start + i) / 8] |= (uint8_t)(1u << ((start + i) % 8));
}

static void csd(struct SdCard *card, uint8_t r[16])
{
    memset(r, 0, 16);
    const uint64_t sectors = card ? sdcard_sectors(card) : 0;
    reg_bits(r, 126, 2, 1);                     /* CSD 2.0: SDHC/SDXC */
    reg_bits(r, 112, 8, 0x0E);                  /* TAAC */
    reg_bits(r, 96, 8, 0x32);                   /* 25 MHz */
    reg_bits(r, 84, 12, 0x5B5);                 /* command classes */
    reg_bits(r, 80, 4, 9);                      /* 512-byte reads */
    reg_bits(r, 48, 22, (uint32_t)(sectors / 1024 ? sectors / 1024 - 1 : 0));
    reg_bits(r, 46, 1, 1);
    reg_bits(r, 39, 7, 0x7F);
    reg_bits(r, 26, 3, 2);
    reg_bits(r, 22, 4, 9);                      /* 512-byte writes */
    reg_bits(r, 0, 1, 1);
}

static void cid(uint8_t r[16])
{
    static const uint8_t v[16] = { 0x43, 'C', 'H', 'C', 'H', 'G', 'S', 'D', 0x10,
                                   0x20, 0x26, 0x09, 0x30, 0x01, 0xA9, 0x01 };
    memcpy(r, v, 16);
}

static void command(ChgSdSpi *c, struct SdCard *card)
{
    const uint8_t idx = c->cmd[0] & 0x3F;
    const uint32_t arg = (uint32_t)c->cmd[1] << 24 | (uint32_t)c->cmd[2] << 16 | (uint32_t)c->cmd[3] << 8 | c->cmd[4];
    const bool app = c->app;
    c->app = false;
    uint8_t r1 = c->idle ? R1_IDLE : 0;
    uint8_t reg[64];

    c->reading = false;
    start_response(c);
    if (!card) {
        /* no card in the slot: nothing answers, MISO stays high */
        c->nout = c->pout = 0;
        return;
    }
    if (app) {
        switch (idx) {
        case 41:                                /* SD_SEND_OP_COND: ready at once */
            c->idle = false;
            put(c, 0);
            return;
        case 13:                                /* SD_STATUS */
            put(c, r1); put(c, 0);
            memset(reg, 0, 64);
            reg[0] = 0x80;                      /* 4-bit bus capable */
            put_block(c, reg, 64);
            return;
        case 51:                                /* SCR: spec 3.0, 1 and 4-bit buses */
            put(c, r1);
            memset(reg, 0, 8);
            reg[0] = 0x02; reg[1] = 0x35; reg[2] = 0x80;
            put_block(c, reg, 8);
            return;
        case 22: case 23: case 42:
            put(c, r1);
            return;
        }
    }
    switch (idx) {
    case 0:                                     /* GO_IDLE_STATE */
        c->idle = true;
        c->wstate = 0;
        put(c, R1_IDLE);
        break;
    case 1:                                     /* SEND_OP_COND (MMC style init) */
        c->idle = false;
        put(c, 0);
        break;
    case 8:                                     /* SEND_IF_COND: 2.7-3.6 V, the pattern back */
        put(c, r1); put(c, 0); put(c, 0); put(c, (uint8_t)(arg >> 8 & 0x0F)); put(c, (uint8_t)arg);
        break;
    case 55:                                    /* APP_CMD */
        c->app = true;
        put(c, r1);
        break;
    case 58:                                    /* READ_OCR: powered up, SDHC */
        put(c, r1); put(c, 0xC0); put(c, 0xFF); put(c, 0x80); put(c, 0x00);
        break;
    case 9:                                     /* SEND_CSD */
        put(c, r1);
        csd(card, reg);
        put_block(c, reg, 16);
        break;
    case 10:                                    /* SEND_CID */
        put(c, r1);
        cid(reg);
        put_block(c, reg, 16);
        break;
    case 12:                                    /* STOP_TRANSMISSION: a stuff byte, R1, busy */
        put(c, 0xFF); put(c, r1); put(c, 0x00);
        break;
    case 13:                                    /* SEND_STATUS: R2 */
        put(c, r1); put(c, 0);
        break;
    case 16: case 59: case 23: case 32: case 33: case 38:
        put(c, r1);
        break;
    case 17: case 18: {                         /* READ_SINGLE / MULTIPLE_BLOCK */
        uint8_t b[512];
        put(c, r1);
        c->lba = arg;
        read_block(card, c->lba++, b);
        put_block(c, b, 512);
        c->reading = idx == 18;
        break;
    }
    case 24: case 25:                           /* WRITE_BLOCK / MULTIPLE_BLOCK */
        put(c, r1);
        c->lba = arg;
        c->wstate = 1;
        c->multi_write = idx == 25;
        break;
    default:
        put(c, r1 | R1_ILLEGAL);
        break;
    }
}

uint8_t sdspi_xfer(ChgSdSpi *c, struct SdCard *card, uint8_t mosi)
{
    /* what goes out was decided before this byte came in */
    uint8_t miso = 0xFF;
    if (c->pout < c->nout) {
        miso = c->out[c->pout++];
    } else if (c->reading && card) {
        /* CMD18: the next block, after the gap */
        uint8_t b[512];
        c->nout = c->pout = 0;
        read_block(card, c->lba++, b);
        put_block(c, b, 512);
        miso = c->out[c->pout++];
    }

    /* a block the host is writing */
    if (c->wstate == 1) {
        if (mosi == 0xFE || (c->multi_write && mosi == 0xFC)) {
            c->wstate = 2;
            c->nblk = 0;
            return miso;
        }
        if (c->multi_write && mosi == 0xFD) {
            /* stop token: busy a moment, then done */
            c->wstate = 0;
            c->nout = c->pout = 0;
            put(c, 0xFF); put(c, 0x00); put(c, 0x00);
            return miso;
        }
        if (mosi == 0xFF) return miso;          /* still waiting for the token */
        c->wstate = 0;                          /* a command instead */
    } else if (c->wstate == 2) {
        c->blk[c->nblk++] = mosi;
        if (c->nblk == 514) {
            if (card) sdcard_write(card, c->lba++, c->blk, 1);
            c->nout = c->pout = 0;
            put(c, 0x05);                       /* data accepted */
            put(c, 0x00); put(c, 0x00);         /* busy programming */
            c->wstate = c->multi_write ? 1 : 0;
        }
        return miso;
    }

    /* commands */
    if (c->ncmd == 0) {
        if ((mosi & 0xC0) == 0x40) c->cmd[c->ncmd++] = mosi;
    } else {
        c->cmd[c->ncmd++] = mosi;
        if (c->ncmd == 6) {
            c->ncmd = 0;
            command(c, card);
        }
    }
    return miso;
}
