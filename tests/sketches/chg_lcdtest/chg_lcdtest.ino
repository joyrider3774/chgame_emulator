/*
 * ST7735S behaviour check: the cases where the emulator follows the
 * datasheet but nothing had been compared with the panel. Each step draws
 * something and waits for A; compare the screen with the emulator's
 * (chg_headless with --press a@... or --shots, see CLAUDE.md).
 *
 *  1  reference: MADCTL 0xC8, quadrants red / green above blue / white
 *  2  MADCTL 0xE8 (MV set), the same quadrants: rotated or mirrored?
 *  3  MADCTL 0xC0 only, nothing redrawn: do red and blue swap at once?
 *  4  red and green ramps, 16 bit, through the colour table as it is
 *  5  RGBSET with the red half reversed, ramps again
 *  6  hardware reset and CHGfx init (no RGBSET), ramps: does the table survive?
 *  7  SWRESET with MADCTL 0xC8 and COLMOD 0x05 set, quadrants without
 *     setting either again: do both survive?
 *  8  quadrants, partial mode on rows 40-120: what shows outside them?
 *  9  normal mode, vertical scrolling over all 162 lines, SSA 40
 * 10  the same with ML set (MADCTL 0xD8)
 * 11  normal mode: scrolling off again
 * 12  12 bit ramps through the table CHGfx writes for 12 bpp
 * 13  the same with the red half of the table reversed
 * 14  16 bit ramps after a table with green all zero
 * 15  quadrants, partial mode on rows 100-150
 * 16  partial mode on rows 150 to 20 (wrapping)
 */
#include <CHGfx.h>

#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F
#define WHITE 0xFFFF

static void pressA(void)
{
    /* A is PB1 to ground: input with pull-up */
    GPIOB->CFGLR = (GPIOB->CFGLR & ~(0xFu << 4)) | (0x8u << 4);
    GPIOB->BSHR = 1u << 1;
    while (!(GPIOB->INDR & (1u << 1))) ;
    delay(30);
    while (GPIOB->INDR & (1u << 1)) ;
    delay(30);
}

static void cmd(uint8_t c) { gfx_select(); gfx_cmd(c); gfx_deselect(); }
static void cmd1(uint8_t c, uint8_t a) { gfx_select(); gfx_cmd(c); gfx_data8(a); gfx_deselect(); }
static void cmd16x2(uint8_t c, uint16_t a, uint16_t b)
{
    gfx_select(); gfx_cmd(c);
    gfx_data8(a >> 8); gfx_data8(a); gfx_data8(b >> 8); gfx_data8(b);
    gfx_deselect();
}

/* a 128x128 window in frame memory coordinates, as MADCTL addresses it */
static void window(uint16_t x0, uint16_t y0)
{
    cmd16x2(0x2A, x0, x0 + 127);
    cmd16x2(0x2B, y0, y0 + 127);
}

static void quadrants(void)
{
    gfx_select(); gfx_cmd(0x2C);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            const uint16_t c = y < 64 ? (x < 64 ? RED : GREEN) : (x < 64 ? BLUE : WHITE);
            gfx_data8(c >> 8); gfx_data8(c);
        }
    gfx_deselect();
}

/* red ramp in the top half, green ramp in the bottom half, dark at the left */
static void ramps(void)
{
    gfx_select(); gfx_cmd(0x2C);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            const uint16_t c = y < 64 ? (uint16_t)((x >> 2) << 11) : (uint16_t)((x >> 1) << 5);
            gfx_data8(c >> 8); gfx_data8(c);
        }
    gfx_deselect();
}

static void rgbset(bool reverse_red)
{
    gfx_select(); gfx_cmd(0x2D);
    for (int i = 0; i < 32; i++) {
        const int v = reverse_red ? 31 - i : i;
        gfx_data8((uint8_t)(v << 1 | v >> 4));
    }
    for (int i = 0; i < 64; i++) gfx_data8((uint8_t)i);
    for (int i = 0; i < 32; i++) gfx_data8((uint8_t)(i << 1 | i >> 4));
    gfx_deselect();
}

/* 12 bit: two pixels in three bytes, the same ramps with 4-bit colours */
static void ramps12(void)
{
    gfx_select(); gfx_cmd(0x2C);
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x += 2) {
            const uint8_t v = (uint8_t)(x >> 3);
            const uint16_t c = y < 64 ? (uint16_t)(v << 8) : (uint16_t)(v << 4);
            gfx_data8((uint8_t)(c >> 4));
            gfx_data8((uint8_t)((c & 15) << 4 | c >> 8));
            gfx_data8((uint8_t)c);
        }
    gfx_deselect();
}

/* the table CHGfx writes for 12 bpp: 4 bits to 6 in the first 16 entries */
static void rgbset12(bool reverse_red)
{
    gfx_select(); gfx_cmd(0x2D);
    for (int i = 0; i < 32; i++) {
        const int v = reverse_red && i < 16 ? 15 - i : i;
        gfx_data8(i < 16 ? (uint8_t)(v << 2 | v >> 2) : 0x3F);
    }
    for (int i = 0; i < 64; i++) gfx_data8(i < 16 ? (uint8_t)(i << 2 | i >> 2) : 0x3F);
    for (int i = 0; i < 32; i++) gfx_data8(i < 16 ? (uint8_t)(i << 2 | i >> 2) : 0x3F);
    gfx_deselect();
}

static void rgbset_nogreen(void)
{
    gfx_select(); gfx_cmd(0x2D);
    for (int i = 0; i < 32; i++) gfx_data8((uint8_t)(i << 1 | i >> 4));
    for (int i = 0; i < 64; i++) gfx_data8(0);
    for (int i = 0; i < 32; i++) gfx_data8((uint8_t)(i << 1 | i >> 4));
    gfx_deselect();
}

static void scroll(uint8_t madctl)
{
    cmd1(0x36, madctl);
    gfx_select(); gfx_cmd(0x33);
    gfx_data8(0); gfx_data8(0); gfx_data8(0); gfx_data8(162); gfx_data8(0); gfx_data8(0);
    gfx_deselect();
    gfx_select(); gfx_cmd(0x37); gfx_data8(0); gfx_data8(40); gfx_deselect();
}

void setup()
{
    gfx_begin(GFX_DIV4, GFX_16BPP);

    /* 1 */
    cmd1(0x36, 0xC8); window(2, 3); quadrants(); pressA();
    /* 2: with MV the column counter runs down the memory's rows */
    cmd1(0x36, 0xE8); window(3, 2); quadrants(); pressA();
    /* 3 */
    cmd1(0x36, 0xC0); pressA();
    /* 4 */
    cmd1(0x36, 0xC8); window(2, 3); ramps(); pressA();
    /* 5 */
    rgbset(true); ramps(); pressA();
    /* 6 */
    gfx_begin(GFX_DIV4, GFX_16BPP);
    cmd1(0x36, 0xC8); window(2, 3); ramps(); pressA();
    /* 7 */
    rgbset(false);
    cmd1(0x36, 0xC8); cmd1(0x3A, 0x05);
    cmd(0x01); delay(150);
    cmd(0x11); delay(150);
    cmd(0x29);
    window(2, 3); quadrants(); pressA();
    /* 8 */
    gfx_begin(GFX_DIV4, GFX_16BPP);
    cmd1(0x36, 0xC8); window(2, 3); quadrants();
    cmd16x2(0x30, 40, 120); cmd(0x12); pressA();
    /* 9 */
    cmd(0x13); scroll(0xC8); pressA();
    /* 10 */
    scroll(0xD8); pressA();
    /* 11 */
    cmd1(0x36, 0xC8); cmd(0x13); pressA();
    /* 12: 12 bit ramps through the table 12 bpp CHGfx writes */
    cmd1(0x3A, 0x03); rgbset12(false); window(2, 3); ramps12(); pressA();
    /* 13: the same with the red half reversed */
    rgbset12(true); ramps12(); pressA();
    /* 14: 16 bit ramps after a table with green all zero */
    cmd1(0x3A, 0x05); rgbset_nogreen(); ramps(); pressA();
    /* 15: quadrants, partial mode on rows 100-150 only */
    rgbset(false); quadrants(); cmd16x2(0x30, 100, 150); cmd(0x12); pressA();
    /* 16: partial mode on rows 150 to 20, wrapping past the last row */
    cmd16x2(0x30, 150, 20); pressA();
    cmd(0x13);
}

void loop()
{
}
