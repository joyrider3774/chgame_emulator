/*
 * The boards, from the CHGame board package's variants
 * (variants/CH32X035/CHGame_Rev<n>/variant_CHGame_Rev<n>.h) and its
 * libraries' pin code (CHGfx, CHSd, the CHGame library's Input.cpp and
 * Audio.cpp), 0.4.0. Oldest first: the last one is the default.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "board.h"

extern const uint8_t chg_bootloader_rev0[], chg_bootloader_rev1[];
extern const size_t chg_bootloader_rev0_size, chg_bootloader_rev1_size;

/* plain braces: a static initializer takes no compound literals */
#define PA(n) { 0, n }
#define PB(n) { 1, n }
#define PC(n) { 2, n }
#define NONE  { 0xff, 0 }

const ChgBoard chg_boards[] = {
    {
        /* The first board (2026): named after its MCU, "CX35", before boards
           had ids. The backlight is always on, the buzzer a piezo on PB10
           driven by TIM1 CH2 with TIM1's partial remap 1 */
        .name = "rev0", .title = "CHGame Rev0", .aliases = "0",
        .target = 0x35335843u,                                  /* "CX35" */
        .button = { PB(4), PC(14), PB(3), PC(15), PB(1), PB(6), PB(7), PB(8) },
        .lcd_cs = PA(4), .lcd_dc = PB(0), .lcd_rst = PB(12), .lcd_bl = NONE,
        .sd_cs = PB(11), .led = PB(9),
        .buzzer = PB(10), .buzz_tim = 0, .buzz_ch = 2, .buzz_remap_shift = 15, .buzz_remap = 1,
        .pull_up = { NONE, NONE, NONE, NONE },
        .analog = { { 0xff, 0 }, { 0xff, 0 } },
        .bootloader = chg_bootloader_rev0, .bootloader_size = &chg_bootloader_rev0_size,
    },
    {
        /* Rev1 and Rev 1.5 (board package 0.4.0, 2026-11): rev0's chip on
           other pins, "an attempt at a stable pinout". The backlight is
           switched on PB9 (CHGfx lights it after the first frame), the
           buzzer a magnetic one behind a MOSFET on PA1, TIM2 CH2 without a
           remap. The SD card's DI is gated by its CS, which a card that
           ignores the bus while deselected (as the emulated one does) never
           notices. PA0 (ADC channel 0) senses VBUS through a 100k/1k divider:
           with no USB host here, no VBUS, code 0. CN2's I2C lines (PC18
           SDA, PC19 SCL) have 4.7k pull-ups, which Rev0's lack
           (CHBoardTest, Checks.cpp) */
        .name = "rev1", .title = "CHGame Rev1", .aliases = "1 rev1.5 rev15 1.5",
        .target = 0x31524743u,                                  /* "CGR1" */
        .button = { PB(0), PB(1), PC(3), PC(0), PB(11), PB(6), PB(7), PB(8) },
        .lcd_cs = PA(4), .lcd_dc = PB(4), .lcd_rst = PB(12), .lcd_bl = PB(9),
        .sd_cs = PB(3), .led = PB(10),
        .buzzer = PA(1), .buzz_tim = 1, .buzz_ch = 2, .buzz_remap_shift = 18, .buzz_remap = 0,
        .pull_up = { PC(18), PC(19), NONE, NONE },
        .analog = { { 0, 0 }, { 0xff, 0 } },
        .bootloader = chg_bootloader_rev1, .bootloader_size = &chg_bootloader_rev1_size,
    },
};
const int chg_board_count = (int)(sizeof(chg_boards) / sizeof(chg_boards[0]));

const ChgBoard *chg_board_default(void)
{
    return &chg_boards[chg_board_count - 1];
}

static bool same(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

const ChgBoard *chg_board_find(const char *name)
{
    if (!name || !*name) return NULL;
    const size_t n = strlen(name);
    for (int i = 0; i < chg_board_count; i++) {
        const ChgBoard *b = &chg_boards[i];
        if (strlen(b->name) == n && same(b->name, name, n)) return b;
        for (const char *a = b->aliases; *a; ) {
            const char *e = strchr(a, ' ');
            const size_t len = e ? (size_t)(e - a) : strlen(a);
            if (len == n && same(a, name, n)) return b;
            a += len;
            while (*a == ' ') a++;
        }
    }
    return NULL;
}

const ChgBoard *chg_board_by_target(uint32_t target)
{
    for (int i = 0; i < chg_board_count; i++)
        if (chg_boards[i].target == target) return &chg_boards[i];
    return NULL;
}

const ChgBoard *chg_board_of_bootloader(const uint8_t *image, size_t size)
{
    if (size < 0x18) return &chg_boards[0];
    const uint32_t word = (uint32_t)image[0x14] | (uint32_t)image[0x15] << 8 |
                          (uint32_t)image[0x16] << 16 | (uint32_t)image[0x17] << 24;
    if (word == 0 || word == 0xffffffffu) return &chg_boards[0];
    return chg_board_by_target(word);
}

void chg_board_list(char *out, size_t outlen)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < chg_board_count && n < outlen; i++)
        n += (size_t)snprintf(out + n, outlen - n, "%s%s (%s)", i ? ", " : "", chg_boards[i].name, chg_boards[i].title);
}
