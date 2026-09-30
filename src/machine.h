/*
 * The CHGame as a whole: a WCH CH32X035G8U6 (QingKe V4C RISC-V core, 48 MHz,
 * 62 KB flash, 20 KB SRAM) and what is wired to its pins.
 *
 *   ST7735S 128x128  SPI1 (SCK PA5, MOSI PA7), CS PA4, D/C PB0, RESET PB12
 *   buttons          UP PB4, DOWN PC14, LEFT PB3, RIGHT PC15,
 *                    A PB1, B PB6, SELECT PB7, START PB8 - all to GND
 *   buzzer           PB10 (TIM1 CH2 with the partial remap, or plain GPIO)
 *   LED              PB9, active high
 *   microSD          SPI1, CS PB11 (not emulated)
 *
 * Every module keeps its state in here so a machine is one plain struct that
 * can be reset, and later snapshotted, as a unit.
 */
#ifndef CHG_MACHINE_H
#define CHG_MACHINE_H

#include <stdint.h>
#include <stdbool.h>

#define CHG_HCLK            48000000u

#define CHG_FLASH_SIZE      0x10000u        /* 62 KB user flash, rounded up so the tail reads 0xff */
#define CHG_FLASH_USER      0xF800u
#define CHG_FLASH_ALIAS     0x08000000u
#define CHG_RAM_BASE        0x20000000u
#define CHG_RAM_SIZE        0x5000u         /* 20 KB */
#define CHG_APP_START       0x3000u
#define CHG_PAGE_SIZE       256u

/* ------------------------------------------------------------------------ */
/* CPU                                                                        */
/* ------------------------------------------------------------------------ */

/* One predecoded instruction. op == 0 means "not decoded yet". */
typedef struct {
    uint8_t  op;
    uint8_t  rd;        /* 32 is a sink register, used whenever rd is x0 */
    uint8_t  rs1;
    uint8_t  rs2;
    int32_t  imm;
    uint8_t  len;       /* 2 or 4 */
    uint8_t  cost;      /* cycles to execute, once fetched */
    uint16_t wend;      /* the 32-bit word its last byte is in (flash: what must be fetched) */
} ChgInsn;

#define CHG_HPE_DEPTH 8

typedef struct {
    uint32_t x[33];                 /* x[32] swallows writes to x0 */
    uint32_t pc;

    uint32_t mstatus, mtvec, mscratch, mepc, mcause, mtval;
    uint32_t intsyscr;              /* 0x804: HWSTKEN, INESTEN, ... */
    uint32_t corecfgr;              /* 0xBC0 */
    uint32_t csr_misc[16];          /* anything else a program pokes at */

    /* The hardware prologue/epilogue stack. On QingKe V4 it lives inside the
       core, not in RAM, so software never sees it. */
    uint32_t hpe[CHG_HPE_DEPTH][16];
    int      trap_depth;
    bool     trap_hpe[CHG_HPE_DEPTH];
    int      trap_irq[CHG_HPE_DEPTH];   /* -1 for an exception */
    /* what a trap replaced, put back by its mret: with nesting on, a
       preempting interrupt must not lose the preempted handler's return */
    uint32_t trap_mepc[CHG_HPE_DEPTH], trap_mcause[CHG_HPE_DEPTH], trap_mstatus[CHG_HPE_DEPTH];

    /* timing state: the flash prefetcher, the divider, fractions of a cycle */
    uint32_t fetch_word;            /* the last flash word the prefetcher has (or is) fetched */
    uint64_t fetch_time;            /* the cycle that word arrives */
    uint64_t div_ready;             /* the cycle the divider's result is ready */
    uint8_t  div_rd;                /* the register it goes to */
    uint8_t  frac8;                 /* eighths of a cycle carried over */

    bool     wfi;
    bool     lr_valid;
    uint32_t lr_addr;

    /* the last fault, for the UI */
    bool     faulted;
    uint32_t fault_cause, fault_pc, fault_tval;
} ChgCpu;

/* ------------------------------------------------------------------------ */
/* Interrupt controller (PFIC) and SysTick                                   */
/* ------------------------------------------------------------------------ */

#define CHG_IRQ_COUNT       64
#define CHG_IRQ_SYSTICK     12
#define CHG_IRQ_SW          14
#define CHG_IRQ_DMA1_CH1    22
#define CHG_IRQ_SPI1        33
#define CHG_IRQ_TIM1_UP     35
#define CHG_IRQ_TIM1_CC     37
#define CHG_IRQ_TIM2_UP     38
#define CHG_IRQ_TIM3        60

typedef struct {
    uint64_t enabled;
    uint64_t pending;       /* set by software or by an edge */
    uint64_t line;          /* level of every peripheral's request */
    uint8_t  prio[256];
    uint32_t ithresdr;
    uint32_t sctlr;
    uint32_t vtf_addr[4];
    uint8_t  vtf_id[4];
} ChgPfic;

typedef struct {
    uint32_t ctlr, sr;
    uint64_t cmp;
    uint64_t cnt_base;      /* counter value at cycle_base */
    uint64_t cycle_base;
    uint64_t next_match;    /* cycle of the next compare match, or UINT64_MAX */
} ChgSysTick;

/* ------------------------------------------------------------------------ */
/* GPIO                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t cfg[3];        /* CFGLR, CFGHR, CFGXR: 4 bits a pin, 24 pins */
    uint32_t outdr;
    uint32_t lckr;
    uint32_t ext_low;       /* pins something outside pulls low (buttons) */
} ChgGpioPort;

/* ------------------------------------------------------------------------ */
/* SPI1 + DMA1                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t cfgr, cntr, paddr, maddr;
    uint32_t count_reload;
    uint32_t pos;           /* items transferred since the channel was enabled */
} ChgDmaChannel;

typedef struct {
    ChgDmaChannel ch[8];
    uint32_t intfr;
} ChgDma;

typedef struct {
    uint16_t ctlr1, ctlr2, crcr, hscr;
    uint16_t statr_sticky;  /* OVR, MODF, ... */
    bool     tx_full;
    uint16_t tx_data;
    bool     shifting;
    uint16_t shift_data;
    uint64_t shift_end;     /* cycle the frame in the shift register is done */
    uint64_t t;             /* how far the SPI state has been brought up to */
    bool     rx_full;
    uint16_t rx_data;
} ChgSpi;

/* ------------------------------------------------------------------------ */
/* Timers                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint16_t ctlr1, ctlr2, smcfgr, dmaintenr, intfr, chctlr1, chctlr2, ccer;
    uint16_t psc, atrlr, rptcr, ccr[4], bdtr, dmacfgr, spec;
    uint32_t cnt_base;      /* counter at cycle_base */
    uint64_t cycle_base;
    uint64_t next_update;   /* UINT64_MAX while no update interrupt is due */
    bool     advanced;      /* TIM1: outputs need MOE */
} ChgTimer;

/* ------------------------------------------------------------------------ */
/* Flash controller                                                          */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t actlr, statr, ctlr, addr, obr, wpr;
    int      key_stage, mode_key_stage;
    bool     locked, fast_locked;
    uint8_t  page_buf[CHG_PAGE_SIZE];
} ChgFlashCtl;

/* ------------------------------------------------------------------------ */
/* Buzzer: what drives PB10, logged with timestamps for the audio side       */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint64_t cycle;         /* when this takes effect */
    uint8_t  pwm;           /* 0: a steady level, 1: a PWM wave */
    uint8_t  level;         /* steady level, or the level while "active" for PWM */
    uint32_t period;        /* PWM: cycles a period */
    uint32_t high;          /* PWM: cycles a period is at 'level' from its start */
    uint64_t origin;        /* PWM: a cycle at which a period started */
} ChgBuzzState;

#define CHG_BUZZ_LOG 4096

typedef struct {
    ChgBuzzState cur;       /* what the pin does now */
    ChgBuzzState log[CHG_BUZZ_LOG];
    unsigned head, tail;
} ChgBuzzer;

/* ------------------------------------------------------------------------ */
/* ST7735S                                                                   */
/* ------------------------------------------------------------------------ */

#define ST7735_COLS 132
#define ST7735_ROWS 162

typedef struct {
    uint16_t gram[ST7735_ROWS][ST7735_COLS];    /* RGB565 as it is on the glass */
    uint8_t  cmd;
    int      nparam;
    uint8_t  param[16];
    uint16_t xs, xe, ys, ye;
    uint16_t cx, cy;
    bool     writing;
    uint8_t  pix[3];
    int      npix;
    uint8_t  madctl, colmod;
    bool     sleeping, display_on, inverted, idle;
    bool     rst_level;
    uint32_t frames_written;    /* times the address counter wrapped a whole window */
    uint32_t frame_starts;      /* times a RAMWR window went back up the screen: a new frame */
    uint16_t last_ys;
} ChgSt7735;

/* ------------------------------------------------------------------------ */
/* The machine                                                               */
/* ------------------------------------------------------------------------ */

enum {
    CHG_BTN_UP = 1 << 0, CHG_BTN_DOWN = 1 << 1, CHG_BTN_LEFT = 1 << 2, CHG_BTN_RIGHT = 1 << 3,
    CHG_BTN_A = 1 << 4, CHG_BTN_B = 1 << 5, CHG_BTN_SELECT = 1 << 6, CHG_BTN_START = 1 << 7,
};

typedef struct ChgMachine {
    uint64_t cycles;
    uint64_t instret;       /* instructions retired, for the stats */
    uint64_t stop_at;       /* the CPU returns once cycles reaches this */
    uint64_t next_event;

    ChgCpu cpu;

    uint8_t flash[CHG_FLASH_SIZE];
    uint8_t ram[CHG_RAM_SIZE];
    ChgInsn dc_flash[CHG_FLASH_SIZE / 2];
    ChgInsn dc_ram[CHG_RAM_SIZE / 2];

    /* generic backing for peripheral registers nothing models */
    uint8_t io[0x30000];
    uint8_t usb[0x400];

    ChgPfic pfic;
    ChgSysTick systick;
    ChgGpioPort gpio[3];
    uint32_t afio_pcfr1, afio_exticr[2], afio_ctlr;
    uint32_t rcc[11];
    ChgSpi spi;
    ChgDma dma;
    ChgTimer tim[3];        /* TIM1, TIM2, TIM3 */
    ChgFlashCtl flashctl;
    uint32_t adc[24];
    uint32_t adc_noise;

    ChgBuzzer buzzer;
    ChgSt7735 lcd;

    uint8_t buttons;        /* CHG_BTN_* currently held */
    bool    led;

    /* flash pages the program has written since load, for the save file */
    uint8_t flash_dirty[CHG_FLASH_SIZE / CHG_PAGE_SIZE];
    bool    flash_written;
    uint64_t flash_write_cycle;

    uint32_t entry;         /* where a reset starts executing */
    bool     reset_request;
    char     serial_out[256];
    int      serial_len;
} ChgMachine;

/* machine.c */
void     chg_init(ChgMachine *m);
void     chg_reset(ChgMachine *m, bool power_on);
void     chg_run(ChgMachine *m, uint64_t until);
void     chg_set_buttons(ChgMachine *m, uint8_t buttons);
void     chg_schedule(ChgMachine *m);
static inline void chg_kick(ChgMachine *m) { m->stop_at = 0; }

/* rv32.c */
void     cpu_reset(ChgMachine *m);
void     cpu_exec(ChgMachine *m);
bool     cpu_take_interrupt(ChgMachine *m);
bool     cpu_irq_waiting(ChgMachine *m);
void     cpu_invalidate_flash(ChgMachine *m, uint32_t addr, uint32_t len);
int      cpu_decode(uint32_t pc, uint32_t raw, ChgInsn *d, bool in_flash);
const char *cpu_op_name(int op);

/* bus.c */
uint32_t bus_read_slow(ChgMachine *m, uint32_t addr, int size, int *wait);
void     bus_write_slow(ChgMachine *m, uint32_t addr, uint32_t value, int size, int *wait);
void     bus_reset(ChgMachine *m, bool power_on);
void     bus_events(ChgMachine *m);

/* periph.c pieces used across modules */
void     pfic_set_line(ChgMachine *m, int irq, bool level);
uint64_t systick_next_event(ChgMachine *m);
void     spi_sync(ChgMachine *m, uint64_t now);
bool     gpio_pin(ChgMachine *m, int port, int pin);
void     buzzer_update(ChgMachine *m);

/* st7735.c */
void     st7735_reset(ChgSt7735 *lcd);
void     st7735_byte(ChgSt7735 *lcd, bool dc, uint8_t byte);
void     st7735_render(const ChgSt7735 *lcd, uint16_t out[128 * 128]);

#endif
