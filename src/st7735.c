/*
 * The ST7735S display controller and the 128x128 glass behind it.
 *
 * The controller holds 132x162 pixels of frame memory. Where a pixel written
 * through CASET/RASET/RAMWR lands in it depends on MADCTL (MX, MY, MV), and
 * the glass only shows part of that memory: on the CHGame's 1.44" panel it is
 * the 128x128 block that the board's own library reaches with MADCTL 0xC8
 * and a window offset of column 2, row 3. The mapping below is physical, so
 * a program that uses another MADCTL or other offsets sees its picture moved
 * or mirrored exactly as the real panel would show it.
 */
#include <string.h>
#include "machine.h"

#define MAD_MY  0x80
#define MAD_MX  0x40
#define MAD_MV  0x20
#define MAD_BGR 0x08

void st7735_reset(ChgSt7735 *lcd)
{
    /* the frame memory keeps whatever it held; a real panel's is random at
       power on, which chg_init fills in */
    lcd->cmd = 0;
    lcd->nparam = 0;
    lcd->xs = 0; lcd->xe = ST7735_COLS - 1;
    lcd->ys = 0; lcd->ye = ST7735_ROWS - 1;
    lcd->cx = lcd->cy = 0;
    lcd->writing = false;
    lcd->npix = 0;
    lcd->madctl = 0;
    lcd->colmod = 0x06;
    lcd->sleeping = true;
    lcd->display_on = false;
    lcd->inverted = false;
    lcd->idle = false;
}

/* one pixel at the address counter, then the counter moves on */
static void put_pixel(ChgSt7735 *lcd, uint16_t c)
{
    /* the window is in the rotated space: with MV the 162 pixel side runs
       across. Mirrored addresses count back from the far edge of that space,
       then MV exchanges the two to reach the frame memory */
    const bool mv = (lcd->madctl & MAD_MV) != 0;
    unsigned col = lcd->cx, row = lcd->cy;
    if (lcd->madctl & MAD_MX) col = (mv ? ST7735_ROWS - 1 : ST7735_COLS - 1) - col;
    if (lcd->madctl & MAD_MY) row = (mv ? ST7735_COLS - 1 : ST7735_ROWS - 1) - row;
    if (mv) { unsigned t = col; col = row; row = t; }
    if (col < ST7735_COLS && row < ST7735_ROWS) {
        /* BGR clear means the controller swaps red and blue on the way to
           this panel, whose subpixels are BGR */
        if (!(lcd->madctl & MAD_BGR))
            c = (uint16_t)((c >> 11) | (c & 0x07e0) | (c << 11));
        lcd->gram[row][col] = c;
    }
    if (++lcd->cx > lcd->xe) {
        lcd->cx = lcd->xs;
        if (++lcd->cy > lcd->ye) {
            lcd->cy = lcd->ys;
            lcd->frames_written++;
        }
    }
}

static void pixel_byte(ChgSt7735 *lcd, uint8_t b)
{
    lcd->pix[lcd->npix++] = b;
    switch (lcd->colmod & 7) {
    case 5:     /* 16 bit */
        if (lcd->npix == 2) {
            put_pixel(lcd, (uint16_t)(lcd->pix[0] << 8 | lcd->pix[1]));
            lcd->npix = 0;
        }
        break;
    case 3:     /* 12 bit: two pixels in three bytes */
        if (lcd->npix == 3) {
            const unsigned p0 = (unsigned)lcd->pix[0] << 4 | lcd->pix[1] >> 4;
            const unsigned p1 = (unsigned)(lcd->pix[1] & 15) << 8 | lcd->pix[2];
            for (int i = 0; i < 2; i++) {
                const unsigned p = i ? p1 : p0;
                const unsigned r = p >> 8, g = (p >> 4) & 15, bl = p & 15;
                put_pixel(lcd, (uint16_t)((r << 12 | (r >> 3) << 11) | (g << 7 | (g >> 2) << 5) | (bl << 1 | bl >> 3)));
            }
            lcd->npix = 0;
        }
        break;
    default:    /* 18 bit: a byte a colour, top 6 bits used */
        if (lcd->npix == 3) {
            put_pixel(lcd, (uint16_t)((lcd->pix[0] >> 3) << 11 | (lcd->pix[1] >> 2) << 5 | lcd->pix[2] >> 3));
            lcd->npix = 0;
        }
        break;
    }
}

static void command(ChgSt7735 *lcd, uint8_t cmd)
{
    lcd->cmd = cmd;
    lcd->nparam = 0;
    lcd->writing = false;
    lcd->npix = 0;
    switch (cmd) {
    case 0x01: st7735_reset(lcd); break;          /* SWRESET */
    case 0x10: lcd->sleeping = true; break;       /* SLPIN */
    case 0x11: lcd->sleeping = false; break;      /* SLPOUT */
    case 0x20: lcd->inverted = false; break;      /* INVOFF */
    case 0x21: lcd->inverted = true; break;       /* INVON */
    case 0x28: lcd->display_on = false; break;    /* DISPOFF */
    case 0x29: lcd->display_on = true; break;     /* DISPON */
    case 0x38: lcd->idle = false; break;          /* IDMOFF */
    case 0x39: lcd->idle = true; break;           /* IDMON */
    case 0x2C:                                    /* RAMWR */
        /* Games draw top to bottom, whether in one window or in strips, so
           a window starting above the last one is the next frame. Only used
           for the frame rate the stats show */
        if (lcd->ys <= lcd->last_ys)
            lcd->frame_starts++;
        lcd->last_ys = lcd->ys;
        lcd->writing = true;
        lcd->cx = lcd->xs;
        lcd->cy = lcd->ys;
        break;
    }
}

static void parameter(ChgSt7735 *lcd, uint8_t b)
{
    if (lcd->writing) {
        pixel_byte(lcd, b);
        return;
    }
    if (lcd->nparam < (int)sizeof(lcd->param))
        lcd->param[lcd->nparam] = b;
    lcd->nparam++;
    const uint8_t *p = lcd->param;
    switch (lcd->cmd) {
    case 0x2A:      /* CASET */
        if (lcd->nparam == 2) lcd->xs = (uint16_t)(p[0] << 8 | p[1]);
        if (lcd->nparam == 4) lcd->xe = (uint16_t)(p[2] << 8 | p[3]);
        break;
    case 0x2B:      /* RASET */
        if (lcd->nparam == 2) lcd->ys = (uint16_t)(p[0] << 8 | p[1]);
        if (lcd->nparam == 4) lcd->ye = (uint16_t)(p[2] << 8 | p[3]);
        break;
    case 0x36: if (lcd->nparam == 1) lcd->madctl = b; break;
    case 0x3A: if (lcd->nparam == 1) lcd->colmod = b; break;
    }
}

void st7735_byte(ChgSt7735 *lcd, bool dc, uint8_t byte)
{
    if (!dc) command(lcd, byte);
    else parameter(lcd, byte);
}

/* The glass as it looks now, 128x128 RGB565, top left first */
void st7735_render(const ChgSt7735 *lcd, uint16_t out[128 * 128])
{
    if (!lcd->display_on || lcd->sleeping || !lcd->rst_level) {
        /* a normally white panel with nothing driving it */
        for (int i = 0; i < 128 * 128; i++) out[i] = 0xffff;
        return;
    }
    /* Screen pixel (x, y) is frame memory column 129-x, row 158-y: with
       MADCTL 0xC8 that is window column x+2, row y+3, which is where CHGfx and
       the games put the picture. */
    for (int y = 0; y < 128; y++) {
        const uint16_t *src = lcd->gram[158 - y];
        uint16_t *dst = out + y * 128;
        for (int x = 0; x < 128; x++)
            dst[x] = src[129 - x];
    }
    if (lcd->inverted)
        for (int i = 0; i < 128 * 128; i++) out[i] = (uint16_t)~out[i];
    if (lcd->idle)
        for (int i = 0; i < 128 * 128; i++) {
            const uint16_t c = out[i];
            out[i] = (uint16_t)(((c & 0x8000) ? 0xf800 : 0) | ((c & 0x0400) ? 0x07e0 : 0) | ((c & 0x0010) ? 0x001f : 0));
        }
}
