/*
 * The CHGame's display: an ST7735S-compatible controller and the 128x128
 * glass behind it. Modelled on the Sitronix ST7735S datasheet V1.1
 * (2011-11-21; section numbers below are its own) where the CHGame's
 * controller agrees with it, and on that controller where it does not:
 * tests/sketches/chg_lcdread reads its registers and frame memory back over
 * the data line, tests/sketches/chg_lcdtest shows the rest on the glass
 * (both run on the device 2026-10-05).
 *
 * What the device showed:
 * - It runs the datasheet's 132x132 configuration (GM=01): MY mirrors the
 *   row counter over 132 rows (row = 131 - counter, 9.11.2), and the panel
 *   shows memory rows 0-131 only, so partial mode and scrolling work in
 *   those 132 lines. The memory itself has 162 rows: without MY, rows
 *   132-161 can be written and read back, they are just never shown.
 * - The glass shows the block at column 2, row 1 of the shown rows: CHGfx
 *   and the games reach it with MADCTL 0xC8 and a window offset of column
 *   2, row 3.
 * - RGBSET is accepted but changes nothing: 12 and 16-bit colours always
 *   reach the 18-bit memory through the same fixed expansion (measured).
 * - It answers RDDID with 83 76 0F, the bitwise inverse of the datasheet's
 *   ST7735S default.
 *
 * As the datasheet has it, and confirmed on the glass: MX/MY/MV mapping
 * (MX always mirrors the memory column, MY the memory row, also with MV,
 * 9.11.2), the RGB/BGR bit switching the panel's subpixel order at scan time
 * (it recolours what is already in memory), SWRESET keeping MADCTL and
 * COLMOD, partial mode's non-display area blank, scrolling in scan order
 * with ML reversing it.
 *
 * Everything between the frame memory and the glass happens when the panel is
 * scanned, so st7735_render applies it to whatever the memory holds at that
 * moment. MH (horizontal refresh order), gamma, frame rate and power
 * settings change nothing a static picture shows and are accepted without
 * effect. The read commands are not modelled: the CHGame only reads the
 * panel through a bit-banged test sketch.
 */
#include <string.h>
#include "machine.h"

#define MAD_MY  0x80
#define MAD_MX  0x40
#define MAD_MV  0x20
#define MAD_ML  0x10
#define MAD_BGR 0x08

/* Power applied: frame memory random (10.1.22), then the state of a
   hardware reset */
void st7735_power_on(ChgSt7735 *lcd)
{
    uint32_t r = 0x12345678u;
    for (int y = 0; y < ST7735_ROWS; y++)
        for (int x = 0; x < ST7735_COLS; x++) {
            r = r * 1664525u + 1013904223u;
            lcd->gram[y][x] = r >> 14;
        }
    st7735_reset(lcd);
}

/* What both resets set (reset table 9.15.2, GM=01). The frame memory keeps
   its contents */
static void common_reset(ChgSt7735 *lcd)
{
    lcd->cmd = 0;
    lcd->nparam = 0;
    lcd->writing = false;
    lcd->npix = 0;
    lcd->sleeping = true;
    lcd->display_on = false;
    lcd->inverted = false;
    lcd->idle = false;
    lcd->partial = false;
    lcd->scrolling = false;
    lcd->psl = 0;
    lcd->pel = ST7735_LINES - 1;
    lcd->tfa = 0;
    lcd->vsa = ST7735_LINES;
    lcd->bfa = 0;
    lcd->ssa = 0;
    lcd->xs = 0; lcd->xe = ST7735_LINES - 1;
    lcd->ys = 0; lcd->ye = ST7735_LINES - 1;
    lcd->cx = lcd->cy = 0;
}

/* RESX low: also MADCTL and COLMOD go back to their defaults. SWRESET
   leaves both as they were (10.1.29, 10.1.33; step 7 of chg_lcdtest) */
void st7735_reset(ChgSt7735 *lcd)
{
    lcd->madctl = 0;
    lcd->colmod = 0x06;
    common_reset(lcd);
}

/* one pixel at the address counter, then the counter moves on */
static void put_pixel(ChgSt7735 *lcd, uint32_t c)
{
    /* MV sends the column counter to the memory's rows and the row counter
       to its columns; MX then mirrors the memory column and MY the memory
       row, whichever counter drives them (9.11.2) */
    int col = lcd->cx, row = lcd->cy;
    if (lcd->madctl & MAD_MV) { int t = col; col = row; row = t; }
    if (lcd->madctl & MAD_MX) col = ST7735_COLS - 1 - col;
    if (lcd->madctl & MAD_MY) row = ST7735_LINES - 1 - row;
    /* outside the memory the data is ignored (10.1.20) */
    if ((unsigned)col < ST7735_COLS && (unsigned)row < ST7735_ROWS)
        lcd->gram[row][col] = c;
    /* the column counter runs to XE, then back to XS on the next row; past
       YE both start over (9.10) */
    if (++lcd->cx > lcd->xe) {
        lcd->cx = lcd->xs;
        if (++lcd->cy > lcd->ye) {
            lcd->cy = lcd->ys;
            lcd->frames_written++;
        }
    }
}

/* 5 bits to the memory's 6, as the device stores them: shifted up, with
   the low bit set only for full intensity */
static inline uint32_t c5(unsigned v) { return v << 1 | (v == 31); }
/* 4 bits to 6: the top two bits repeated */
static inline uint32_t c4(unsigned v) { return v << 2 | v >> 2; }

static void pixel_byte(ChgSt7735 *lcd, uint8_t b)
{
    lcd->pix[lcd->npix++] = b;
    switch (lcd->colmod & 7) {
    case 5:     /* 16 bit: RRRRRGGG GGGBBBBB (9.8.21) */
        if (lcd->npix == 2) {
            const unsigned c = (unsigned)lcd->pix[0] << 8 | lcd->pix[1];
            put_pixel(lcd, c5(c >> 11) << 12 | (uint32_t)((c >> 5) & 63) << 6 | c5(c & 31));
            lcd->npix = 0;
        }
        break;
    case 3:     /* 12 bit: two pixels in three bytes, RRRRGGGG BBBBRRRR GGGGBBBB (9.8.20);
                   each is written as soon as it is complete */
        if (lcd->npix == 2)
            put_pixel(lcd, c4(lcd->pix[0] >> 4) << 12 | c4(lcd->pix[0] & 15) << 6 | c4(lcd->pix[1] >> 4));
        else if (lcd->npix == 3) {
            put_pixel(lcd, c4(lcd->pix[1] & 15) << 12 | c4(lcd->pix[2] >> 4) << 6 | c4(lcd->pix[2] & 15));
            lcd->npix = 0;
        }
        break;
    default:    /* 18 bit: a byte a colour, bits 7:2 (9.8.22) */
        if (lcd->npix == 3) {
            put_pixel(lcd, (uint32_t)(lcd->pix[0] >> 2) << 12 | (uint32_t)(lcd->pix[1] >> 2) << 6 | lcd->pix[2] >> 2);
            lcd->npix = 0;
        }
        break;
    }
}

static void command(ChgSt7735 *lcd, uint8_t cmd)
{
    /* a command ends a RAMWR and starts a new parameter list; the
       parameters already received have been applied (9.5) */
    lcd->cmd = cmd;
    lcd->nparam = 0;
    lcd->writing = false;
    lcd->npix = 0;
    switch (cmd) {
    case 0x01: common_reset(lcd); break;          /* SWRESET */
    case 0x10: lcd->sleeping = true; break;       /* SLPIN */
    case 0x11: lcd->sleeping = false; break;      /* SLPOUT */
    case 0x12:                                    /* PTLON; ends scrolling (10.1.30) */
        lcd->partial = true;
        lcd->scrolling = false;
        break;
    case 0x13:                                    /* NORON */
        lcd->partial = false;
        lcd->scrolling = false;
        break;
    case 0x20: lcd->inverted = false; break;      /* INVOFF */
    case 0x21: lcd->inverted = true; break;       /* INVON */
    case 0x28: lcd->display_on = false; break;    /* DISPOFF */
    case 0x29: lcd->display_on = true; break;     /* DISPON */
    case 0x38: lcd->idle = false; break;          /* IDMOFF */
    case 0x39:                                    /* IDMON, not in partial mode (10.1.32) */
        if (!lcd->partial)
            lcd->idle = true;
        break;
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
    const int n = ++lcd->nparam;
    if (n <= (int)sizeof(lcd->param))
        lcd->param[n - 1] = b;
    const uint8_t *p = lcd->param;
    switch (lcd->cmd) {
    case 0x2A:      /* CASET */
        if (n == 2) lcd->xs = (uint16_t)(p[0] << 8 | p[1]);
        if (n == 4) lcd->xe = (uint16_t)(p[2] << 8 | p[3]);
        break;
    case 0x2B:      /* RASET */
        if (n == 2) lcd->ys = (uint16_t)(p[0] << 8 | p[1]);
        if (n == 4) lcd->ye = (uint16_t)(p[2] << 8 | p[3]);
        break;
    case 0x30:      /* PTLAR */
        if (n == 2) lcd->psl = (uint16_t)(p[0] << 8 | p[1]);
        if (n == 4) lcd->pel = (uint16_t)(p[2] << 8 | p[3]);
        break;
    case 0x33:      /* SCRLAR: only defines the areas */
        if (n == 2) lcd->tfa = (uint16_t)(p[0] << 8 | p[1]);
        if (n == 4) lcd->vsa = (uint16_t)(p[2] << 8 | p[3]);
        if (n == 6) lcd->bfa = (uint16_t)(p[4] << 8 | p[5]);
        break;
    case 0x37:      /* VSCSAD: starts scrolling; not in partial mode (10.1.30) */
        if (n == 2 && !lcd->partial) {
            lcd->ssa = (uint16_t)(p[0] << 8 | p[1]);
            lcd->scrolling = true;
        }
        break;
    case 0x36: if (n == 1) lcd->madctl = b; break;
    case 0x3A:      /* COLMOD, not in partial mode (10.1.33) */
        if (n == 1 && !lcd->partial) lcd->colmod = b;
        break;
    }
}

void st7735_byte(ChgSt7735 *lcd, bool dc, uint8_t byte)
{
    if (!dc) command(lcd, byte);
    else parameter(lcd, byte);
}

/* The memory row panel line g shows. Without scrolling line g is row g.
   Scrolling works in scan order, which ML reverses: the top fixed area is
   the first TFA lines scanned, then lines from SSA on, wrapping back to the
   top of the scrolling area at its end, then the bottom fixed area (9.11.6,
   10.1.26, 10.1.30; with ML=1 the datasheet's example 2 counts all three,
   SSA too, from the bottom). The scrolling area ends at the last line even
   when TFA+VSA says more: on the device SCRLAR 0/162/0 with SSA 40 wraps
   at line 132 (chg_lcdtest step 9) */
static int memory_row(const ChgSt7735 *lcd, int g)
{
    if (!lcd->scrolling)
        return g;
    const bool ml = (lcd->madctl & MAD_ML) != 0;
    int s = ml ? ST7735_LINES - 1 - g : g;
    const int tfa = lcd->tfa;
    int end = tfa + lcd->vsa;
    if (end > ST7735_LINES) end = ST7735_LINES;
    if (s >= tfa && s < end) {
        /* an SSA inside a fixed area gives an "undesirable image": this one */
        s = lcd->ssa + (s - tfa);
        if (s >= end)
            s -= end - tfa;
    }
    if (s < 0 || s >= ST7735_LINES)
        return -1;
    return ml ? ST7735_LINES - 1 - s : s;
}

/* Partial mode shows rows PSL to PEL, wrapping past the last line when PEL
   is below PSL (10.1.25) */
static bool shown(const ChgSt7735 *lcd, int row)
{
    if (!lcd->partial)
        return true;
    if (lcd->psl <= lcd->pel)
        return row >= lcd->psl && row <= lcd->pel;
    return row >= lcd->psl || row <= lcd->pel;
}

/* 6 bits to 8 */
static inline uint32_t c8(uint32_t v) { return v << 2 | v >> 4; }

/* The glass as it looks now, 128x128 XRGB8888, top left first */
void st7735_render(const ChgSt7735 *lcd, uint32_t out[128 * 128])
{
    /* a normally white panel with nothing driving it: asleep, display off
       ("blank page inserted", 10.1.18), held in reset, and partial mode's
       non-display area (white on the device) */
    const uint32_t blank = 0xffffff;
    if (!lcd->display_on || lcd->sleeping || !lcd->rst_level) {
        for (int i = 0; i < 128 * 128; i++) out[i] = blank;
        return;
    }
    /* RGB=0 drives the first subpixel of each pixel with red, but this
       panel's are blue-green-red: the switch is in the source driver, so it
       swaps what is already in memory too (chg_lcdtest step 3) */
    const bool swap = !(lcd->madctl & MAD_BGR);
    /* Screen pixel (x, y) is frame memory column 129-x on panel line 128-y:
       with MADCTL 0xC8 that is window column x+2, row y+3, which is where
       CHGfx and the games put the picture */
    for (int y = 0; y < 128; y++) {
        uint32_t *dst = out + y * 128;
        const int row = memory_row(lcd, 128 - y);
        if (row < 0 || !shown(lcd, row)) {
            for (int x = 0; x < 128; x++) dst[x] = blank;
            continue;
        }
        const uint32_t *src = lcd->gram[row];
        for (int x = 0; x < 128; x++) {
            uint32_t v = src[129 - x];
            if (lcd->inverted)
                v ^= 0x3ffff;
            /* idle: eight colours from the top bit of each (10.1.32) */
            if (lcd->idle)
                v = ((v & 0x20000) ? 0x3f000 : 0) | ((v & 0x800) ? 0xfc0 : 0) | ((v & 0x20) ? 0x3f : 0);
            uint32_t r = v >> 12, g = (v >> 6) & 63, b = v & 63;
            if (swap) { uint32_t t = r; r = b; b = t; }
            dst[x] = c8(r) << 16 | c8(g) << 8 | c8(b);
        }
    }
}
