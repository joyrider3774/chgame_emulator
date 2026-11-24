/*
 * The CHGame boards (hardware revisions) the emulator emulates: the same
 * CH32X035G8U6, memory map and flash layout on each, with the screen, the
 * card, the buttons, the buzzer and the LED on different pins. Everything
 * that depends on the board reads it from here, through the machine's
 * `board`, so a new revision is one entry in chg_boards[] (board.c) and its
 * bootloader in bootloader_image.c (tools/make_bootloader_image.py).
 *
 * The boards are those of CHGame's docs/hardware-revisions.md: each is a
 * device (rev0, rev1, ...) with a four-character target id that its CHG
 * packages and its bootloader image carry. Rev 1.5 is the device rev1: its
 * power section changes nothing a program sees on these pins.
 */
#ifndef CHG_BOARD_H
#define CHG_BOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* A GPIO pin: port 0 = A, 1 = B, 2 = C. CHG_NO_PIN: the board has none */
typedef struct { uint8_t port, pin; } ChgPin;
#define CHG_NO_PIN ((ChgPin){ 0xff, 0 })
#define CHG_PIN(port, pin) ((ChgPin){ (port), (pin) })
static inline bool chg_pin_none(ChgPin p) { return p.port == 0xff; }

typedef struct ChgBoard {
    const char *name;           /* the device: "rev0", "rev1" */
    const char *title;          /* "CHGame Rev0" */
    const char *aliases;        /* other names --board takes, space separated */
    uint32_t    target;         /* its target id, as CHG packages and bootloaders carry it */
    /* the buttons, in CHG_BTN_* bit order (UP DOWN LEFT RIGHT A B SELECT
       START), each pulling its pin to GND */
    ChgPin      button[8];
    ChgPin      lcd_cs, lcd_dc, lcd_rst;
    ChgPin      lcd_bl;         /* a switched backlight, active high; none: always lit */
    ChgPin      sd_cs;
    ChgPin      led;            /* active high */
    /* The buzzer's pin, and the timer channel that can drive it: timer 0 =
       TIM1, 1 = TIM2; its channel 1-4; and the AFIO PCFR1 remap field
       (shift, 3 bits) value that puts that channel on this pin */
    ChgPin      buzzer;
    uint8_t     buzz_tim, buzz_ch, buzz_remap_shift, buzz_remap;
    /* pins the board pulls up with a resistor (they read high when nothing
       drives them low); unused entries CHG_NO_PIN */
    ChgPin      pull_up[4];
    /* ADC channels the board feeds a known voltage, as the 12-bit code a
       conversion reads; any other channel reads noise like a floating pin.
       Unused entries have channel 0xff */
    struct { uint8_t channel; uint16_t code; } analog[2];
    /* the bootloader the board ships with, put at 0x0000 */
    const uint8_t *bootloader;
    const size_t  *bootloader_size;
} ChgBoard;

extern const ChgBoard chg_boards[];
extern const int chg_board_count;

/* The board a new machine is: the newest */
const ChgBoard *chg_board_default(void);
/* By name or alias, any case ("rev1", "Rev1.5", "1"); NULL if unknown */
const ChgBoard *chg_board_find(const char *name);
/* By target id; NULL for a board this emulator does not know */
const ChgBoard *chg_board_by_target(uint32_t target);
/* The board a bootloader image is for, from its board word (offset 0x14:
   a board's target id, 0 or erased for rev0, which had none); NULL when it
   names a board this emulator does not know */
const ChgBoard *chg_board_of_bootloader(const uint8_t *image, size_t size);
/* "rev0 (CHGame Rev0), rev1 (CHGame Rev1, also Rev 1.5)" for help texts */
void chg_board_list(char *out, size_t outlen);

#endif
