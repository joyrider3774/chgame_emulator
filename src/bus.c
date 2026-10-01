/*
 * Everything on the bus that is not RAM or flash read straight from the CPU's
 * fast path: the peripherals, the flash controller, the system area.
 *
 * Nothing here is ticked. Each peripheral keeps the cycle its state was last
 * brought up to and works out the present from that when it is touched, and
 * the ones that raise interrupts tell chg_schedule() when they next will.
 */
#include <string.h>
#include <stdio.h>
#include "machine.h"
#include "sdspi.h"

#define PERIPH_WS 0     /* measured: peripheral loads and stores cost what RAM ones do */

/* ------------------------------------------------------------------------ */
/* PFIC                                                                      */
/* ------------------------------------------------------------------------ */

void pfic_set_line(ChgMachine *m, int irq, bool level)
{
    const uint64_t bit = 1ull << irq;
    const uint64_t was = m->pfic.line;
    if (level) m->pfic.line |= bit;
    else m->pfic.line &= ~bit;
    if (m->pfic.line != was && level)
        chg_kick(m);
}

static uint32_t pfic_read(ChgMachine *m, uint32_t off)
{
    ChgPfic *p = &m->pfic;
    if (off < 0x20)
        return off < 8 ? (uint32_t)(p->enabled >> (off * 8)) : 0;
    if (off >= 0x20 && off < 0x40) {
        off -= 0x20;
        return off < 8 ? (uint32_t)((p->pending | p->line) >> (off * 8)) : 0;
    }
    if (off == 0x40) return p->ithresdr;
    if (off == 0x48) return 0;
    if (off == 0x4c) {
        uint32_t v = (uint32_t)m->cpu.trap_depth;
        if (m->cpu.trap_depth) v |= 1u << 8;
        if ((p->pending | p->line) & p->enabled) v |= 1u << 9;
        return v;
    }
    if (off >= 0x50 && off < 0x54) return p->vtf_id[0] | p->vtf_id[1] << 8 | p->vtf_id[2] << 16 | (uint32_t)p->vtf_id[3] << 24;
    if (off >= 0x60 && off < 0x70) return p->vtf_addr[(off - 0x60) / 4];
    if (off >= 0x100 && off < 0x108) return (uint32_t)(p->enabled >> ((off - 0x100) * 8));
    if (off >= 0x300 && off < 0x308) {
        uint64_t act = 0;
        for (int d = 0; d < m->cpu.trap_depth; d++)
            if (m->cpu.trap_irq[d] >= 0) act |= 1ull << m->cpu.trap_irq[d];
        return (uint32_t)(act >> ((off - 0x300) * 8));
    }
    if (off >= 0x400 && off < 0x500) {
        off -= 0x400;
        return p->prio[off] | p->prio[off + 1] << 8 | p->prio[off + 2] << 16 | (uint32_t)p->prio[off + 3] << 24;
    }
    if (off == 0xd10) return p->sctlr;
    return 0;
}

static void pfic_write(ChgMachine *m, uint32_t off, uint32_t v, int size)
{
    ChgPfic *p = &m->pfic;
    if (off >= 0x400 && off < 0x500) {
        for (int i = 0; i < size; i++)
            p->prio[off - 0x400 + i] = (uint8_t)(v >> (8 * i));
        chg_kick(m);
        return;
    }
    const int word = (off & 0x7f) / 4;
    const uint64_t bits = word < 2 ? (uint64_t)v << (32 * word) : 0;
    switch (off & ~0x7fu) {
    case 0x100: p->enabled |= bits; break;
    case 0x180: p->enabled &= ~bits; break;
    case 0x200: p->pending |= bits; break;
    case 0x280: p->pending &= ~bits; break;
    default:
        if (off == 0x40) p->ithresdr = v & 0xff;
        else if (off == 0x48) {
            /* KEY3 with SYSRESET: NVIC_SystemReset() */
            if ((v >> 16) == 0xBEEF && (v & 0x80))
                m->reset_request = true;
        } else if (off >= 0x50 && off < 0x54) {
            for (uint32_t i = 0; i < (uint32_t)size && (off - 0x50) + i < 4; i++)
                p->vtf_id[(off - 0x50) + i] = (uint8_t)(v >> (8 * i));
        } else if (off >= 0x60 && off < 0x70) p->vtf_addr[(off - 0x60) / 4] = v;
        else if (off == 0xd10) {
            if (v & 0x80000000u) m->reset_request = true;
            p->sctlr = v & 0x7fffffffu;
        }
        break;
    }
    chg_kick(m);
}

/* ------------------------------------------------------------------------ */
/* SysTick                                                                   */
/* ------------------------------------------------------------------------ */

#define STK_STE   0x01
#define STK_STIE  0x02
#define STK_STCLK 0x04
#define STK_STRE  0x08
#define STK_MODE  0x10
#define STK_INIT  0x20
#define STK_SWIE  0x80000000u

static uint64_t stk_div(ChgMachine *m) { return (m->systick.ctlr & STK_STCLK) ? 1 : 8; }

/* the counter as it is at 'now' */
static uint64_t stk_count(ChgMachine *m, uint64_t now)
{
    ChgSysTick *s = &m->systick;
    if (!(s->ctlr & STK_STE))
        return s->cnt_base;
    const uint64_t ticks = (now - s->cycle_base) / stk_div(m);
    if (s->ctlr & STK_MODE) {
        /* down */
        if (s->ctlr & STK_STRE) {
            const uint64_t period = s->cmp + 1;
            const uint64_t done = (s->cmp - s->cnt_base + ticks) % period;
            return s->cmp - done;
        }
        return s->cnt_base - ticks;
    }
    if (s->ctlr & STK_STRE) {
        const uint64_t period = s->cmp + 1;
        if (s->cnt_base > s->cmp) {
            /* above the compare value: runs on until it wraps at 2^64, which
               is for practical purposes never */
            return s->cnt_base + ticks;
        }
        return (s->cnt_base + ticks) % period;
    }
    return s->cnt_base + ticks;
}

/* re-bases the counter at 'now' so a change of settings starts from there */
static void stk_rebase(ChgMachine *m, uint64_t now)
{
    m->systick.cnt_base = stk_count(m, now);
    m->systick.cycle_base = now;
}

static void stk_plan(ChgMachine *m)
{
    ChgSysTick *s = &m->systick;
    s->next_match = UINT64_MAX;
    if (!(s->ctlr & STK_STE))
        return;
    const uint64_t div = stk_div(m);
    uint64_t ticks;
    const uint64_t c = s->cnt_base;
    if (s->ctlr & STK_MODE) {
        /* counting down, the flag is set at 0 */
        if (s->ctlr & STK_STRE) ticks = c ? c : s->cmp + 1;
        else if (c) ticks = c;
        else return;
    } else {
        if (c < s->cmp) ticks = s->cmp - c;
        else if ((s->ctlr & STK_STRE) && c == s->cmp) ticks = s->cmp + 1;
        else return;
    }
    /* the first tick after cycle_base lands at the next multiple of div */
    s->next_match = s->cycle_base + ticks * div;
}

uint64_t systick_next_event(ChgMachine *m) { return m->systick.next_match; }

static void stk_event(ChgMachine *m)
{
    ChgSysTick *s = &m->systick;
    if (m->cycles < s->next_match)
        return;
    const uint64_t at = s->next_match;
    s->sr |= 1;
    if (s->ctlr & STK_STIE)
        pfic_set_line(m, CHG_IRQ_SYSTICK, true);
    /* bring the base up to the match and plan the next one */
    s->cnt_base = stk_count(m, at);
    s->cycle_base = at;
    if (!(s->ctlr & STK_STRE) && !(s->ctlr & STK_MODE)) {
        s->next_match = UINT64_MAX;
        return;
    }
    /* with auto reload the counter stays on CMP for one tick, then restarts */
    stk_plan(m);
}

static uint32_t stk_read(ChgMachine *m, uint32_t off)
{
    ChgSysTick *s = &m->systick;
    switch (off) {
    case 0x00: return s->ctlr;
    case 0x04: return s->sr;
    case 0x08: return (uint32_t)stk_count(m, m->cycles);
    case 0x0c: return (uint32_t)(stk_count(m, m->cycles) >> 32);
    case 0x10: return (uint32_t)s->cmp;
    case 0x14: return (uint32_t)(s->cmp >> 32);
    }
    return 0;
}

static void stk_write(ChgMachine *m, uint32_t off, uint32_t v)
{
    ChgSysTick *s = &m->systick;
    stk_rebase(m, m->cycles);
    switch (off) {
    case 0x00:
        s->ctlr = v & ~STK_INIT & ~STK_SWIE;
        if (v & STK_INIT)
            s->cnt_base = (v & STK_MODE) ? s->cmp : 0;
        if (v & STK_SWIE)
            m->pfic.pending |= 1ull << CHG_IRQ_SW;
        break;
    case 0x04:
        s->sr = v & 1;
        break;
    case 0x08: s->cnt_base = (s->cnt_base & 0xffffffff00000000ull) | v; break;
    case 0x0c: s->cnt_base = (s->cnt_base & 0xffffffffull) | ((uint64_t)v << 32); break;
    case 0x10: s->cmp = (s->cmp & 0xffffffff00000000ull) | v; break;
    case 0x14: s->cmp = (s->cmp & 0xffffffffull) | ((uint64_t)v << 32); break;
    }
    pfic_set_line(m, CHG_IRQ_SYSTICK, (s->sr & 1) && (s->ctlr & STK_STIE));
    stk_plan(m);
    chg_kick(m);
}

/* ------------------------------------------------------------------------ */
/* GPIO                                                                      */
/* ------------------------------------------------------------------------ */

static inline unsigned pin_cfg(ChgMachine *m, int port, int pin)
{
    return (m->gpio[port].cfg[pin >> 3] >> ((pin & 7) * 4)) & 15;
}

/* What a pin in alternate function mode outputs */
static bool af_level(ChgMachine *m, int port, int pin)
{
    /* PA4 as SPI1's NSS, driven low while the SPI is on with SSOE */
    if (port == 0 && pin == 4)
        return !((m->spi.ctlr2 & 0x04) && (m->spi.ctlr1 & 0x40));
    /* PB10 as TIM1 CH2: the buzzer. Its level at this instant */
    if (port == 1 && pin == 10) {
        const ChgBuzzState *b = &m->buzzer.cur;
        if (!b->pwm)
            return b->level;
        const uint64_t ph = (m->cycles - b->origin) % b->period;
        return (ph < b->high) ? b->level : !b->level;
    }
    return false;
}

bool gpio_pin(ChgMachine *m, int port, int pin)
{
    const ChgGpioPort *g = &m->gpio[port];
    const unsigned cfg = pin_cfg(m, port, pin);
    if (cfg & 3) {
        /* an output. Open drain outputs can still be pulled low from outside */
        bool v = (cfg & 8) ? af_level(m, port, pin) : ((g->outdr >> pin) & 1);
        if (g->ext_low >> pin & 1) v = false;
        return v;
    }
    if (g->ext_low >> pin & 1)
        return false;
    switch (cfg >> 2) {
    case 2: return (g->outdr >> pin) & 1;     /* pull up (1) or down (0) */
    default: return false;                    /* analog or floating */
    }
}

/* Which button each port pin is, from the board's wiring */
static const struct { int port, pin; uint8_t btn; } button_pins[] = {
    { 1, 4, CHG_BTN_UP }, { 2, 14, CHG_BTN_DOWN }, { 1, 3, CHG_BTN_LEFT }, { 2, 15, CHG_BTN_RIGHT },
    { 1, 1, CHG_BTN_A }, { 1, 6, CHG_BTN_B }, { 1, 7, CHG_BTN_SELECT }, { 1, 8, CHG_BTN_START },
};

void chg_set_buttons(ChgMachine *m, uint8_t buttons)
{
    m->buttons = buttons;
    for (int p = 0; p < 3; p++) m->gpio[p].ext_low = 0;
    for (size_t i = 0; i < sizeof(button_pins) / sizeof(button_pins[0]); i++)
        if (buttons & button_pins[i].btn)
            m->gpio[button_pins[i].port].ext_low |= 1u << button_pins[i].pin;
}

/* The pins other hardware watches; called whenever a port's outputs may have changed */
static void gpio_changed(ChgMachine *m)
{
    const bool rst = gpio_pin(m, 1, 12);
    if (rst != m->lcd.rst_level) {
        m->lcd.rst_level = rst;
        if (!rst)
            st7735_reset(&m->lcd);
    }
    m->led = gpio_pin(m, 1, 9);
    buzzer_update(m);
}

static uint32_t gpio_read(ChgMachine *m, int port, uint32_t off)
{
    ChgGpioPort *g = &m->gpio[port];
    switch (off) {
    case 0x00: return g->cfg[0];
    case 0x04: return g->cfg[1];
    case 0x08: {
        uint32_t v = 0;
        for (int pin = 0; pin < 24; pin++)
            if (gpio_pin(m, port, pin)) v |= 1u << pin;
        return v;
    }
    case 0x0c: return g->outdr;
    case 0x18: return g->lckr;
    case 0x1c: return g->cfg[2];
    }
    return 0;
}

static void gpio_write(ChgMachine *m, int port, uint32_t off, uint32_t v)
{
    ChgGpioPort *g = &m->gpio[port];
    /* pins going to the display change: finish what the SPI has on the wire first */
    spi_sync(m, m->cycles);
    switch (off) {
    case 0x00: g->cfg[0] = v; break;
    case 0x04: g->cfg[1] = v; break;
    case 0x0c: g->outdr = v & 0xffffff; break;
    case 0x10: g->outdr = (g->outdr | (v & 0xffff)) & ~(v >> 16); break;
    case 0x14: g->outdr &= ~(v & 0xffffff); break;
    case 0x18: g->lckr = v; break;
    case 0x1c: g->cfg[2] = v; break;
    case 0x20: g->outdr = (g->outdr | ((v & 0xff) << 16)) & ~(((v >> 16) & 0xff) << 16); break;
    }
    gpio_changed(m);
}

/* ------------------------------------------------------------------------ */
/* SPI1 and DMA1                                                             */
/* ------------------------------------------------------------------------ */

#define SPI_CPHA 0x0001
#define SPI_MSTR 0x0004
#define SPI_SPE  0x0040
#define SPI_LSB  0x0080
#define SPI_DFF  0x0800
#define SPI_RXDMAEN 0x01
#define SPI_TXDMAEN 0x02
#define SPI_RXNEIE 0x40
#define SPI_TXEIE  0x80

#define DMA_EN    0x0001
#define DMA_TCIE  0x0002
#define DMA_HTIE  0x0004
#define DMA_DIR   0x0010
#define DMA_CIRC  0x0020
#define DMA_PINC  0x0040
#define DMA_MINC  0x0080
#define DMA_M2M   0x4000

static uint32_t spi_frame_cycles(ChgMachine *m)
{
    const uint32_t br = (m->spi.ctlr1 >> 3) & 7;
    const uint32_t bits = (m->spi.ctlr1 & SPI_DFF) ? 16 : 8;
    return bits * (2u << br);
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xf0) >> 4 | (b & 0x0f) << 4);
    b = (uint8_t)((b & 0xcc) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xaa) >> 1 | (b & 0x55) << 1);
}

/* A frame has left the shift register: whoever is selected gets it */
static void spi_deliver(ChgMachine *m, uint16_t frame)
{
    /* the display listens while its CS (PA4) is low and it is not in reset */
    if (!gpio_pin(m, 0, 4) && m->lcd.rst_level) {
        const bool dc = gpio_pin(m, 1, 0);
        if (m->spi.ctlr1 & SPI_DFF) {
            uint8_t hi = (uint8_t)(frame >> 8), lo = (uint8_t)frame;
            if (m->spi.ctlr1 & SPI_LSB) { uint8_t t = rev8(lo); lo = rev8(hi); hi = t; }
            st7735_byte(&m->lcd, dc, hi);
            st7735_byte(&m->lcd, dc, lo);
        } else {
            uint8_t b = (uint8_t)frame;
            if (m->spi.ctlr1 & SPI_LSB) b = rev8(b);
            st7735_byte(&m->lcd, dc, b);
        }
    }
    /* The microSD card answers on MISO while its CS (PB11) is low; otherwise
       MISO floats high (the display never answers). A card that sees CS go
       high drops what it was saying. */
    uint16_t miso = (m->spi.ctlr1 & SPI_DFF) ? 0xffff : 0xff;
    const bool sd_sel = !gpio_pin(m, 1, 11);
    if (sd_sel) {
        if (m->spi.ctlr1 & SPI_DFF) {
            const uint8_t hi = sdspi_xfer(&m->sdspi, m->sd, (uint8_t)(frame >> 8));
            const uint8_t lo = sdspi_xfer(&m->sdspi, m->sd, (uint8_t)frame);
            miso = (uint16_t)(hi << 8 | lo);
        } else {
            miso = sdspi_xfer(&m->sdspi, m->sd, (uint8_t)frame);
        }
    } else if (m->sd_selected) {
        sdspi_deselect(&m->sdspi);
    }
    m->sd_selected = sd_sel;
    if (m->spi.rx_full)
        m->spi.statr_sticky |= 0x40;    /* OVR */
    m->spi.rx_full = true;
    m->spi.rx_data = miso;
}

static void dma_flag(ChgMachine *m, int ch, uint32_t flags)
{
    m->dma.intfr |= (flags | 1u) << (ch * 4);
    const ChgDmaChannel *c = &m->dma.ch[ch];
    const bool irq = ((m->dma.intfr >> (ch * 4 + 1)) & 1 && (c->cfgr & DMA_TCIE)) ||
                     ((m->dma.intfr >> (ch * 4 + 2)) & 1 && (c->cfgr & DMA_HTIE));
    if (ch < 7)
        pfic_set_line(m, CHG_IRQ_DMA1_CH1 + ch, irq);
}

static int dma_size(uint32_t cfgr, int shift)
{
    return 1 << ((cfgr >> shift) & 3);
}

static uint32_t dma_mem_read(ChgMachine *m, uint32_t addr, int size)
{
    int wait = 0;
    uint32_t off = addr - CHG_RAM_BASE;
    if (off <= CHG_RAM_SIZE - (uint32_t)size) {
        uint32_t v = 0;
        memcpy(&v, m->ram + off, (size_t)size);
        return v;
    }
    if (addr <= CHG_FLASH_SIZE - (uint32_t)size) {
        uint32_t v = 0;
        memcpy(&v, m->flash + addr, (size_t)size);
        return v;
    }
    return bus_read_slow(m, addr, size, &wait);
}

/* One item of channel ch, memory to peripheral: returns the value */
static uint32_t dma_take(ChgMachine *m, int ch)
{
    ChgDmaChannel *c = &m->dma.ch[ch];
    const int msize = dma_size(c->cfgr, 10);
    const uint32_t addr = c->maddr + ((c->cfgr & DMA_MINC) ? c->pos * (uint32_t)msize : 0);
    const uint32_t v = dma_mem_read(m, addr, msize);
    c->pos++;
    c->cntr--;
    if (c->cntr == c->count_reload / 2)
        dma_flag(m, ch, 4);
    if (c->cntr == 0) {
        if (c->cfgr & DMA_CIRC) {
            c->cntr = c->count_reload;
            c->pos = 0;
        }
        dma_flag(m, ch, 2);
    }
    return v;
}

static bool dma_ready(ChgMachine *m, int ch)
{
    const ChgDmaChannel *c = &m->dma.ch[ch];
    return (c->cfgr & DMA_EN) && c->cntr;
}

/* Brings the SPI (and the DMA feeding it) up to 'now' */
void spi_sync(ChgMachine *m, uint64_t now)
{
    ChgSpi *s = &m->spi;
    if (!(s->ctlr1 & SPI_SPE)) {
        s->t = now;
        return;
    }
    const uint32_t frame = spi_frame_cycles(m);
    for (;;) {
        /* the transmit DMA refills the buffer as soon as it is empty */
        if (!s->tx_full && (s->ctlr2 & SPI_TXDMAEN) && dma_ready(m, 2) && (m->dma.ch[2].cfgr & DMA_DIR)) {
            s->tx_data = (uint16_t)dma_take(m, 2);
            s->tx_full = true;
        }
        if (!s->shifting && s->tx_full) {
            s->shifting = true;
            s->shift_data = s->tx_data;
            s->tx_full = false;
            s->shift_end = (s->t > s->shift_end ? s->t : s->shift_end) + frame;
            continue;
        }
        if (s->shifting && s->shift_end <= now) {
            s->shifting = false;
            s->t = s->shift_end;
            spi_deliver(m, s->shift_data);
            /* the receive DMA empties the buffer */
            if ((s->ctlr2 & SPI_RXDMAEN) && dma_ready(m, 1) && !(m->dma.ch[1].cfgr & DMA_DIR)) {
                ChgDmaChannel *c = &m->dma.ch[1];
                const int msize = dma_size(c->cfgr, 10);
                const uint32_t addr = c->maddr + ((c->cfgr & DMA_MINC) ? c->pos * (uint32_t)msize : 0);
                int wait = 0;
                if (addr - CHG_RAM_BASE <= CHG_RAM_SIZE - (uint32_t)msize) {
                    bus_write_slow(m, addr, s->rx_data, msize, &wait);
                }
                s->rx_full = false;
                c->pos++;
                if (--c->cntr == 0) {
                    if (c->cfgr & DMA_CIRC) { c->cntr = c->count_reload; c->pos = 0; }
                    dma_flag(m, 1, 2);
                }
            }
            continue;
        }
        break;
    }
    if (!s->shifting)
        s->t = now;
    pfic_set_line(m, CHG_IRQ_SPI1,
                  ((s->ctlr2 & SPI_TXEIE) && !s->tx_full) || ((s->ctlr2 & SPI_RXNEIE) && s->rx_full));
}

/* when the SPI next needs attention without anyone asking: only while a DMA
   transfer whose end raises an interrupt is running */
static uint64_t spi_next_event(ChgMachine *m)
{
    const ChgSpi *s = &m->spi;
    if (!(s->ctlr1 & SPI_SPE) || !s->shifting)
        return UINT64_MAX;
    for (int ch = 1; ch <= 2; ch++)
        if (dma_ready(m, ch) && (m->dma.ch[ch].cfgr & (DMA_TCIE | DMA_HTIE)))
            return s->shift_end;
    if (s->ctlr2 & (SPI_TXEIE | SPI_RXNEIE))
        return s->shift_end;
    return UINT64_MAX;
}

static uint32_t spi_read(ChgMachine *m, uint32_t off)
{
    ChgSpi *s = &m->spi;
    spi_sync(m, m->cycles);
    switch (off) {
    case 0x00: return s->ctlr1;
    case 0x04: return s->ctlr2;
    case 0x08: {
        uint32_t v = s->statr_sticky;
        if (s->rx_full) v |= 0x01;
        if (!s->tx_full) v |= 0x02;
        if (s->shifting || s->tx_full) v |= 0x80;
        return v;
    }
    case 0x0c: {
        const uint16_t v = s->rx_data;
        s->rx_full = false;
        s->statr_sticky &= ~0x40;
        return v;
    }
    case 0x10: return s->crcr;
    case 0x1c: return s->hscr;
    }
    return 0;
}

static void spi_write(ChgMachine *m, uint32_t off, uint32_t v)
{
    ChgSpi *s = &m->spi;
    spi_sync(m, m->cycles);
    switch (off) {
    case 0x00:
        s->ctlr1 = (uint16_t)v;
        if (!(v & SPI_SPE)) { s->shifting = false; s->tx_full = false; }
        s->t = m->cycles;
        gpio_changed(m);
        break;
    case 0x04: s->ctlr2 = (uint16_t)v; gpio_changed(m); break;
    case 0x08: s->statr_sticky &= (uint16_t)v; break;
    case 0x0c:
        if (s->ctlr1 & SPI_SPE) {
            s->tx_data = (uint16_t)v;
            s->tx_full = true;
            if (!s->shifting) s->t = m->cycles;
        }
        break;
    case 0x10: s->crcr = (uint16_t)v; break;
    case 0x1c: s->hscr = (uint16_t)v; break;
    }
    spi_sync(m, m->cycles);
    chg_kick(m);
}

/* Memory to memory transfers happen at once */
static void dma_m2m(ChgMachine *m, int ch)
{
    ChgDmaChannel *c = &m->dma.ch[ch];
    const int psize = dma_size(c->cfgr, 8), msize = dma_size(c->cfgr, 10);
    int wait = 0;
    while (c->cntr) {
        const uint32_t pa = c->paddr + ((c->cfgr & DMA_PINC) ? c->pos * (uint32_t)psize : 0);
        const uint32_t ma = c->maddr + ((c->cfgr & DMA_MINC) ? c->pos * (uint32_t)msize : 0);
        if (c->cfgr & DMA_DIR) {
            uint32_t v = dma_mem_read(m, ma, msize);
            bus_write_slow(m, pa, v, psize, &wait);
        } else {
            uint32_t v = dma_mem_read(m, pa, psize);
            bus_write_slow(m, ma, v, msize, &wait);
        }
        c->pos++;
        c->cntr--;
    }
    dma_flag(m, ch, 2);
}

static uint32_t dma_read(ChgMachine *m, uint32_t off)
{
    spi_sync(m, m->cycles);
    if (off == 0x00) return m->dma.intfr;
    if (off == 0x04) return 0;
    if (off >= 0x08 && off < 0x08 + 8 * 0x14) {
        const int ch = (int)(off - 0x08) / 0x14;
        const ChgDmaChannel *c = &m->dma.ch[ch];
        switch ((off - 0x08) % 0x14) {
        case 0x00: return c->cfgr;
        case 0x04: return c->cntr;
        case 0x08: return c->paddr;
        case 0x0c: return c->maddr;
        }
    }
    return 0;
}

static void dma_write(ChgMachine *m, uint32_t off, uint32_t v)
{
    spi_sync(m, m->cycles);
    if (off == 0x04) {
        m->dma.intfr &= ~v;
        /* the global flag goes when the others of its channel have */
        for (int ch = 0; ch < 8; ch++) {
            if (!((m->dma.intfr >> (ch * 4)) & 0xe)) m->dma.intfr &= ~(1u << (ch * 4));
            const ChgDmaChannel *c = &m->dma.ch[ch];
            const bool irq = ((m->dma.intfr >> (ch * 4 + 1)) & 1 && (c->cfgr & DMA_TCIE)) ||
                             ((m->dma.intfr >> (ch * 4 + 2)) & 1 && (c->cfgr & DMA_HTIE));
            if (ch < 7) pfic_set_line(m, CHG_IRQ_DMA1_CH1 + ch, irq);
        }
        return;
    }
    if (off >= 0x08 && off < 0x08 + 8 * 0x14) {
        const int ch = (int)(off - 0x08) / 0x14;
        ChgDmaChannel *c = &m->dma.ch[ch];
        switch ((off - 0x08) % 0x14) {
        case 0x00: {
            const bool starting = !(c->cfgr & DMA_EN) && (v & DMA_EN);
            c->cfgr = v & 0x7fff;
            if (starting) {
                c->pos = 0;
                c->count_reload = c->cntr;
                if (c->cfgr & DMA_M2M) dma_m2m(m, ch);
            }
            break;
        }
        case 0x04: if (!(c->cfgr & DMA_EN)) c->cntr = v & 0xffff; break;
        case 0x08: c->paddr = v; break;
        case 0x0c: c->maddr = v; break;
        }
    }
    spi_sync(m, m->cycles);
    chg_kick(m);
}

/* ------------------------------------------------------------------------ */
/* Timers TIM1, TIM2, TIM3                                                   */
/* ------------------------------------------------------------------------ */

static uint32_t tim_period(const ChgTimer *t) { return (uint32_t)t->atrlr + 1; }
static uint32_t tim_tick(const ChgTimer *t) { return (uint32_t)t->psc + 1; }

static uint32_t tim_count(ChgMachine *m, const ChgTimer *t, uint64_t now)
{
    if (!(t->ctlr1 & 1))
        return t->cnt_base;
    const uint64_t ticks = (now - t->cycle_base) / tim_tick(t);
    return (uint32_t)((t->cnt_base + ticks) % tim_period(t));
}

static void tim_rebase(ChgMachine *m, ChgTimer *t)
{
    t->cnt_base = tim_count(m, t, m->cycles);
    t->cycle_base = m->cycles;
}

static int tim_irq(int n) { return n == 0 ? CHG_IRQ_TIM1_UP : n == 1 ? CHG_IRQ_TIM2_UP : CHG_IRQ_TIM3; }

static void tim_plan(ChgMachine *m, int n)
{
    ChgTimer *t = &m->tim[n];
    t->next_update = UINT64_MAX;
    if (!(t->ctlr1 & 1) || !(t->dmaintenr & 1))
        return;
    const uint64_t left = tim_period(t) - (t->cnt_base % tim_period(t));
    t->next_update = t->cycle_base + left * tim_tick(t);
}

static void tim_events(ChgMachine *m)
{
    for (int n = 0; n < 3; n++) {
        ChgTimer *t = &m->tim[n];
        if (m->cycles < t->next_update)
            continue;
        t->cnt_base = 0;
        t->cycle_base = t->next_update;
        t->intfr |= 1;
        pfic_set_line(m, tim_irq(n), true);
        tim_plan(m, n);
    }
}

static uint32_t tim_read(ChgMachine *m, int n, uint32_t off)
{
    ChgTimer *t = &m->tim[n];
    switch (off) {
    case 0x00: return t->ctlr1;
    case 0x04: return t->ctlr2;
    case 0x08: return t->smcfgr;
    case 0x0c: return t->dmaintenr;
    case 0x10: return t->intfr;
    case 0x14: return 0;
    case 0x18: return t->chctlr1;
    case 0x1c: return t->chctlr2;
    case 0x20: return t->ccer;
    case 0x24: return tim_count(m, t, m->cycles);
    case 0x28: return t->psc;
    case 0x2c: return t->atrlr;
    case 0x30: return t->rptcr;
    case 0x34: case 0x38: case 0x3c: case 0x40: return t->ccr[(off - 0x34) / 4];
    case 0x44: return t->bdtr;
    case 0x48: return t->dmacfgr;
    case 0x50: return t->spec;
    }
    return 0;
}

static void tim_write(ChgMachine *m, int n, uint32_t off, uint32_t v)
{
    ChgTimer *t = &m->tim[n];
    tim_rebase(m, t);
    v &= 0xffff;
    switch (off) {
    case 0x00: t->ctlr1 = (uint16_t)v; break;
    case 0x04: t->ctlr2 = (uint16_t)v; break;
    case 0x08: t->smcfgr = (uint16_t)v; break;
    case 0x0c: t->dmaintenr = (uint16_t)v; break;
    case 0x10: t->intfr &= (uint16_t)v; break;
    case 0x14:
        if (v & 1) {                /* UG: the counter restarts, shadows load */
            t->cnt_base = 0;
            t->cycle_base = m->cycles;
            if (!(t->ctlr1 & 0x04))  /* URS: an update from UG raises the flag unless URS */
                t->intfr |= 1;
        }
        break;
    case 0x18: t->chctlr1 = (uint16_t)v; break;
    case 0x1c: t->chctlr2 = (uint16_t)v; break;
    case 0x20: t->ccer = (uint16_t)v; break;
    case 0x24: t->cnt_base = v; break;
    case 0x28: t->psc = (uint16_t)v; break;
    case 0x2c: t->atrlr = (uint16_t)v; break;
    case 0x30: t->rptcr = (uint16_t)v; break;
    case 0x34: case 0x38: case 0x3c: case 0x40: t->ccr[(off - 0x34) / 4] = (uint16_t)v; break;
    case 0x44: t->bdtr = (uint16_t)v; break;
    case 0x48: t->dmacfgr = (uint16_t)v; break;
    case 0x50: t->spec = (uint16_t)v; break;
    }
    if (t->cnt_base >= tim_period(t))
        t->cnt_base %= tim_period(t);
    pfic_set_line(m, tim_irq(n), (t->intfr & 1) && (t->dmaintenr & 1));
    tim_plan(m, n);
    if (n == 0)
        buzzer_update(m);
    chg_kick(m);
}

/* ------------------------------------------------------------------------ */
/* The buzzer on PB10                                                        */
/* ------------------------------------------------------------------------ */

void buzzer_update(ChgMachine *m)
{
    ChgBuzzState s;
    memset(&s, 0, sizeof(s));
    s.cycle = m->cycles;

    const unsigned cfg = pin_cfg(m, 1, 10);
    const bool remap = ((m->afio_pcfr1 >> 15) & 7) == 1;   /* TIM1 partial remap 1: CH2 on PB10 */
    if ((cfg & 3) && (cfg & 8) && remap) {
        const ChgTimer *t = &m->tim[0];
        const bool enabled = (t->ccer & 0x10) && (t->bdtr & 0x8000);
        const bool polarity = (t->ccer & 0x20) != 0;
        const unsigned mode = (t->chctlr1 >> 12) & 7;
        const uint32_t period = tim_period(t), ccr = t->ccr[1];
        const uint32_t cnt = tim_count(m, t, m->cycles);
        if (!enabled) {
            s.level = 0;
        } else if ((mode == 6 || mode == 7) && (t->ctlr1 & 1) && ccr > 0 && ccr < period) {
            /* PWM mode 1: active while CNT < CCR; mode 2 the other way round */
            s.pwm = 1;
            s.level = (uint8_t)((mode == 6) != polarity);
            s.period = period * tim_tick(t);
            s.high = ccr * tim_tick(t);
            s.origin = m->cycles - (uint64_t)cnt * tim_tick(t) - (m->cycles - t->cycle_base) % tim_tick(t);
        } else {
            bool active;
            switch (mode) {
            case 4: active = false; break;
            case 5: active = true; break;
            case 6: active = cnt < ccr; break;
            case 7: active = cnt >= ccr; break;
            default: active = false; break;
            }
            s.level = (uint8_t)(active != polarity);
        }
    } else {
        s.level = (uint8_t)gpio_pin(m, 1, 10);
    }

    ChgBuzzer *b = &m->buzzer;
    const ChgBuzzState *c = &b->cur;
    if (c->pwm == s.pwm && c->level == s.level &&
        (!s.pwm || (c->period == s.period && c->high == s.high && c->origin == s.origin)))
        return;
    b->cur = s;
    const unsigned next = (b->head + 1) % CHG_BUZZ_LOG;
    if (next == b->tail)
        return;     /* the audio side has fallen far behind; drop the change */
    b->log[b->head] = s;
    b->head = next;
}

/* ------------------------------------------------------------------------ */
/* Flash controller                                                          */
/* ------------------------------------------------------------------------ */

#define FL_PG       0x00000001u
#define FL_PER      0x00000002u
#define FL_MER      0x00000004u
#define FL_STRT     0x00000040u
#define FL_LOCK     0x00000080u
#define FL_FLOCK    0x00008000u
#define FL_PAGE_PG  0x00010000u
#define FL_PAGE_ER  0x00020000u
#define FL_BUF_LOAD 0x00040000u
#define FL_BUF_RST  0x00080000u

static void flash_touched(ChgMachine *m, uint32_t addr, uint32_t len)
{
    cpu_invalidate_flash(m, addr, len);
    for (uint32_t p = addr / CHG_PAGE_SIZE; p <= (addr + len - 1) / CHG_PAGE_SIZE && p < CHG_FLASH_SIZE / CHG_PAGE_SIZE; p++)
        m->flash_dirty[p] = 1;
    m->flash_written = true;
    m->flash_write_cycle = m->cycles;
}

static void flash_erase(ChgMachine *m, uint32_t addr, uint32_t size)
{
    addr &= ~(size - 1);
    if (addr >= CHG_FLASH_USER) return;
    memset(m->flash + addr, 0xff, size);
    flash_touched(m, addr, size);
}

static void flash_ctl_write(ChgMachine *m, uint32_t off, uint32_t v)
{
    ChgFlashCtl *f = &m->flashctl;
    switch (off) {
    case 0x00: f->actlr = v; break;
    case 0x04:
        if (f->key_stage == 0 && v == 0x45670123u) f->key_stage = 1;
        else if (f->key_stage == 1 && v == 0xCDEF89ABu) { f->key_stage = 0; f->locked = false; }
        else f->key_stage = 0;
        break;
    case 0x0c: f->statr &= ~(v & 0x30); break;      /* EOP, WRPRTERR: write 1 to clear */
    case 0x10: {
        const uint32_t was = f->ctlr;
        if (v & FL_LOCK) { f->locked = true; f->fast_locked = true; }
        if (v & FL_FLOCK) f->fast_locked = true;
        f->ctlr = v & ~(FL_STRT | FL_BUF_LOAD | FL_BUF_RST);
        if (f->locked) break;
        if ((v & FL_BUF_RST) && !f->fast_locked)
            memset(f->page_buf, 0xff, sizeof(f->page_buf));
        if (v & FL_STRT) {
            const uint32_t a = f->addr & 0x00ffffffu;
            if ((v & FL_PAGE_ER) && !f->fast_locked) flash_erase(m, a, CHG_PAGE_SIZE);
            else if ((v & FL_PAGE_PG) && !f->fast_locked) {
                const uint32_t page = a & ~(CHG_PAGE_SIZE - 1);
                if (page < CHG_FLASH_USER) {
                    /* programming can only clear bits of what is there */
                    for (uint32_t i = 0; i < CHG_PAGE_SIZE; i++)
                        m->flash[page + i] &= f->page_buf[i];
                    flash_touched(m, page, CHG_PAGE_SIZE);
                }
            } else if (v & FL_PER) flash_erase(m, a, 1024);
            else if (v & FL_MER) flash_erase(m, 0, CHG_FLASH_USER);
            f->statr |= 0x20;
            /* an erase takes milliseconds on the part; the core stalls reading
               flash meanwhile, which is modelled as that many cycles */
            if (v & (FL_PAGE_ER | FL_PER | FL_MER)) m->cycles += CHG_HCLK / 1000 * 2;
            else m->cycles += CHG_HCLK / 1000;
        }
        (void)was;
        break;
    }
    case 0x14: f->addr = v; break;
    case 0x1c: break;
    case 0x20: f->wpr = v; break;
    case 0x24:
        if (f->mode_key_stage == 0 && v == 0x45670123u) f->mode_key_stage = 1;
        else if (f->mode_key_stage == 1 && v == 0xCDEF89ABu) { f->mode_key_stage = 0; f->fast_locked = false; }
        else f->mode_key_stage = 0;
        break;
    }
}

static uint32_t flash_ctl_read(ChgMachine *m, uint32_t off)
{
    ChgFlashCtl *f = &m->flashctl;
    switch (off) {
    case 0x00: return f->actlr;
    case 0x0c: return f->statr;
    case 0x10: return f->ctlr | (f->locked ? FL_LOCK : 0) | (f->fast_locked ? FL_FLOCK : 0);
    case 0x14: return f->addr;
    case 0x1c: return 0x03fffffcu;     /* option bytes loaded, nothing protected */
    case 0x20: return 0xffffffffu;
    }
    return 0;
}

/* A write into the flash address space: goes to the page buffer or programs a half word */
static void flash_mem_write(ChgMachine *m, uint32_t addr, uint32_t v, int size)
{
    ChgFlashCtl *f = &m->flashctl;
    addr &= 0x00ffffffu;
    if (f->locked || addr >= CHG_FLASH_USER)
        return;
    if ((f->ctlr & FL_PAGE_PG) && !f->fast_locked) {
        for (int i = 0; i < size; i++)
            f->page_buf[(addr + (uint32_t)i) & (CHG_PAGE_SIZE - 1)] = (uint8_t)(v >> (8 * i));
        return;
    }
    if (f->ctlr & FL_PG) {
        for (int i = 0; i < size && addr + (uint32_t)i < CHG_FLASH_USER; i++)
            m->flash[addr + (uint32_t)i] &= (uint8_t)(v >> (8 * i));
        flash_touched(m, addr, (uint32_t)size);
        m->cycles += 48 * 30;       /* ~30 us a half word */
        f->statr |= 0x20;
    }
}

/* ------------------------------------------------------------------------ */
/* ADC                                                                       */
/* ------------------------------------------------------------------------ */

static uint32_t adc_read(ChgMachine *m, uint32_t off)
{
    const int r = (int)off / 4;
    if (r == 19) {                      /* RDATAR: an unconnected pin reads noise */
        m->adc[0] &= ~2u;
        return m->adc[19];
    }
    return r < 24 ? m->adc[r] : 0;
}

static void adc_write(ChgMachine *m, uint32_t off, uint32_t v)
{
    const int r = (int)off / 4;
    if (r >= 24) return;
    if (r == 0) { m->adc[0] &= v; return; }
    m->adc[r] = v;
    if (r == 2) {
        m->adc[2] &= ~0x0000000cu;      /* calibration done at once */
        if (v & 0x00400000u) {          /* SWSTART */
            m->adc[2] &= ~0x00400000u;
            m->adc_noise = (m->adc_noise * 1664525u + 1013904223u) ^ (uint32_t)m->cycles;
            m->adc[19] = 1800 + (m->adc_noise >> 22) % 400;
            m->adc[0] |= 2 | 0x10;      /* EOC, STRT */
            m->cycles += 50;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------ */

static uint32_t sys_read(ChgMachine *m, uint32_t addr)
{
    /* ESIG: flash size in KB, then the unique id */
    if (addr == 0x1FFFF7E0) return 62 | (0xFFFFu << 16);
    if (addr == 0x1FFFF7E8) return 0x43484731u;
    if (addr == 0x1FFFF7EC) return 0x0035E3A1u;
    if (addr == 0x1FFFF7F0) return 0x20260930u;
    if (addr >= 0x1FFFF800 && addr < 0x1FFFF840) return 0x00FF00FFu;   /* option bytes, erased pairs */
    if (addr == 0x1FFFF704) return 0x03510601u;    /* chip id, CH32X035G8U6 */
    return 0xFFFFFFFFu;
}

uint32_t bus_read_slow(ChgMachine *m, uint32_t addr, int size, int *wait)
{
    const uint32_t shift = (addr & 3) * 8;

    /* misaligned accesses to memory are split into bytes */
    if ((addr & (uint32_t)(size - 1)) && addr < 0x40000000u) {
        uint32_t v = 0;
        for (int i = 0; i < size; i++)
            v |= bus_read_slow(m, addr + (uint32_t)i, 1, wait) << (8 * i);
        return v;
    }

    if (addr < CHG_FLASH_SIZE || (addr >= CHG_FLASH_ALIAS && addr < CHG_FLASH_ALIAS + CHG_FLASH_SIZE)) {
        const uint32_t a = addr & 0xffff;
        uint32_t v = 0;
        memcpy(&v, m->flash + a, (size_t)(a + (uint32_t)size <= CHG_FLASH_SIZE ? size : 1));
        return v;
    }
    if (addr - CHG_RAM_BASE < CHG_RAM_SIZE) {
        uint32_t v = 0;
        const uint32_t off = addr - CHG_RAM_BASE;
        memcpy(&v, m->ram + off, (size_t)(off + (uint32_t)size <= CHG_RAM_SIZE ? size : 1));
        return v;
    }

    *wait += PERIPH_WS;
    const uint32_t word = addr & ~3u;
    uint32_t v;
    if (word >= 0xE000E000u && word < 0xE000F000u) v = pfic_read(m, word - 0xE000E000u);
    else if (word >= 0xE000F000u && word < 0xE000F100u) v = stk_read(m, word - 0xE000F000u);
    else if (word >= 0x1FFF0000u && word < 0x20000000u) v = sys_read(m, word);
    else if (word >= 0x40000000u && word < 0x40030000u) {
        const uint32_t off = word - 0x40000000u;
        switch (off & ~0x3ffu) {
        case 0x10800: v = gpio_read(m, 0, off & 0x3ff); break;
        case 0x10C00: v = gpio_read(m, 1, off & 0x3ff); break;
        case 0x11000: v = gpio_read(m, 2, off & 0x3ff); break;
        case 0x10000:
            switch (off & 0x3ff) {
            case 0x04: v = m->afio_pcfr1; break;
            case 0x08: v = m->afio_exticr[0]; break;
            case 0x0c: v = m->afio_exticr[1]; break;
            case 0x18: v = m->afio_ctlr; break;
            default: v = 0; break;
            }
            break;
        case 0x13000: v = spi_read(m, off & 0x3ff); break;
        case 0x20000: v = dma_read(m, off & 0x3ff); break;
        case 0x12C00: v = tim_read(m, 0, off & 0x3ff); break;
        case 0x00000: v = tim_read(m, 1, off & 0x3ff); break;
        case 0x00400: v = tim_read(m, 2, off & 0x3ff); break;
        case 0x22000: v = flash_ctl_read(m, off & 0x3ff); break;
        case 0x12400: v = adc_read(m, off & 0x3ff); break;
        case 0x21000: {
            const int r = (int)(off & 0x3ff) / 4;
            v = r < 11 ? m->rcc[r] : 0;
            if (r == 0) v |= 0x03;                          /* HSION, HSIRDY */
            if (r == 1) v = (v & ~0x0cu) | ((v & 3) << 2);  /* SWS follows SW */
            break;
        }
        case 0x13800: case 0x04400: case 0x04800: case 0x04C00:
            /* USARTs: always ready to send */
            v = ((off & 0x3ff) == 0) ? 0xC0 : 0;
            break;
        case 0x23400:
            memcpy(&v, m->usb + (off & 0x3ff), 4);
            break;
        default:
            memcpy(&v, m->io + off, 4);
            break;
        }
    } else {
        v = 0;
    }
    v >>= shift;
    if (size == 1) v &= 0xff;
    else if (size == 2) v &= 0xffff;
    return v;
}

void bus_write_slow(ChgMachine *m, uint32_t addr, uint32_t value, int size, int *wait)
{
    if ((addr & (uint32_t)(size - 1)) && addr < 0x40000000u) {
        for (int i = 0; i < size; i++)
            bus_write_slow(m, addr + (uint32_t)i, value >> (8 * i), 1, wait);
        return;
    }
    if (addr - CHG_RAM_BASE < CHG_RAM_SIZE) {
        const uint32_t off = addr - CHG_RAM_BASE;
        const size_t n = off + (uint32_t)size <= CHG_RAM_SIZE ? (size_t)size : 1;
        memcpy(m->ram + off, &value, n);
        uint32_t first = off >> 1;
        if (first) first--;
        for (uint32_t i = first; i <= (off + (uint32_t)n - 1) >> 1 && i < CHG_RAM_SIZE / 2; i++)
            m->dc_ram[i].op = 0;
        return;
    }
    if (addr < CHG_FLASH_SIZE || (addr >= CHG_FLASH_ALIAS && addr < CHG_FLASH_ALIAS + CHG_FLASH_SIZE)) {
        flash_mem_write(m, addr, value, size);
        return;
    }

    *wait += PERIPH_WS;
    const uint32_t shift = (addr & 3) * 8;
    const uint32_t word = addr & ~3u;
    /* narrow writes land in their lane of the register */
    const uint32_t v = value << shift;

    if (word >= 0xE000E000u && word < 0xE000F000u) {
        if (word - 0xE000E000u >= 0x400 && word - 0xE000E000u < 0x500)
            pfic_write(m, addr - 0xE000E000u, value, size);
        else
            pfic_write(m, word - 0xE000E000u, v, 4);
        return;
    }
    if (word >= 0xE000F000u && word < 0xE000F100u) { stk_write(m, word - 0xE000F000u, v); return; }
    if (word < 0x40000000u || word >= 0x40030000u)
        return;

    const uint32_t off = word - 0x40000000u;
    switch (off & ~0x3ffu) {
    case 0x10800: gpio_write(m, 0, off & 0x3ff, v); return;
    case 0x10C00: gpio_write(m, 1, off & 0x3ff, v); return;
    case 0x11000: gpio_write(m, 2, off & 0x3ff, v); return;
    case 0x10000:
        switch (off & 0x3ff) {
        case 0x04: m->afio_pcfr1 = v; break;
        case 0x08: m->afio_exticr[0] = v; break;
        case 0x0c: m->afio_exticr[1] = v; break;
        case 0x18: m->afio_ctlr = v; break;
        }
        gpio_changed(m);
        return;
    case 0x13000: spi_write(m, off & 0x3ff, v); return;
    case 0x20000: dma_write(m, off & 0x3ff, v); return;
    case 0x12C00: tim_write(m, 0, off & 0x3ff, v); return;
    case 0x00000: tim_write(m, 1, off & 0x3ff, v); return;
    case 0x00400: tim_write(m, 2, off & 0x3ff, v); return;
    case 0x22000: flash_ctl_write(m, off & 0x3ff, v); return;
    case 0x12400: adc_write(m, off & 0x3ff, v); return;
    case 0x21000: {
        const int r = (int)(off & 0x3ff) / 4;
        if (r < 11) m->rcc[r] = v;
        return;
    }
    case 0x13800: case 0x04400: case 0x04800: case 0x04C00:
        if ((off & 0x3ff) == 4) {
            /* USART data: collected into lines for the console */
            const char ch = (char)(value & 0xff);
            if (ch == '\n' || m->serial_len >= (int)sizeof(m->serial_out) - 1) {
                m->serial_out[m->serial_len] = 0;
                printf("[serial] %s\n", m->serial_out);
                m->serial_len = 0;
            } else if (ch != '\r') {
                m->serial_out[m->serial_len++] = ch;
            }
        }
        return;
    case 0x23400:
        memcpy(m->usb + (off & 0x3ff) + (addr & 3), &value, (size_t)size);
        return;
    default:
        memcpy(m->io + off + (addr & 3), &value, (size_t)size);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* Reset and events                                                          */
/* ------------------------------------------------------------------------ */

void bus_reset(ChgMachine *m, bool power_on)
{
    (void)power_on;
    memset(&m->pfic, 0, sizeof(m->pfic));
    memset(&m->systick, 0, sizeof(m->systick));
    m->systick.next_match = UINT64_MAX;
    memset(m->gpio, 0, sizeof(m->gpio));
    for (int p = 0; p < 3; p++) {
        /* every pin a floating input */
        m->gpio[p].cfg[0] = m->gpio[p].cfg[1] = m->gpio[p].cfg[2] = 0x44444444u;
    }
    chg_set_buttons(m, m->buttons);
    m->afio_pcfr1 = m->afio_exticr[0] = m->afio_exticr[1] = m->afio_ctlr = 0;
    memset(m->rcc, 0, sizeof(m->rcc));
    m->rcc[0] = 0x00000083u;
    memset(&m->spi, 0, sizeof(m->spi));
    memset(&m->dma, 0, sizeof(m->dma));
    memset(m->tim, 0, sizeof(m->tim));
    for (int n = 0; n < 3; n++) {
        m->tim[n].atrlr = 0xffff;
        m->tim[n].next_update = UINT64_MAX;
    }
    m->tim[0].advanced = true;
    memset(&m->flashctl, 0, sizeof(m->flashctl));
    m->flashctl.locked = m->flashctl.fast_locked = true;
    m->flashctl.actlr = 0;
    memset(m->adc, 0, sizeof(m->adc));
    memset(m->io, 0, sizeof(m->io));
    memset(m->usb, 0, sizeof(m->usb));
    m->lcd.rst_level = true;    /* the reset line is floating high until driven */
    memset(&m->buzzer.cur, 0, sizeof(m->buzzer.cur));
    m->buzzer.cur.cycle = m->cycles;
}

/* Everything whose time has come */
void bus_events(ChgMachine *m)
{
    stk_event(m);
    tim_events(m);
    spi_sync(m, m->cycles);
}

void chg_schedule(ChgMachine *m)
{
    uint64_t next = m->systick.next_match;
    for (int n = 0; n < 3; n++)
        if (m->tim[n].next_update < next) next = m->tim[n].next_update;
    const uint64_t s = spi_next_event(m);
    if (s < next) next = s;
    m->next_event = next;
}
