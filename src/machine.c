/*
 * Reset and the main run loop.
 *
 * The CPU runs flat out until the next thing that is due: the end of the
 * slice the front end asked for, or the next peripheral event (a SysTick
 * match, a timer update, a DMA transfer whose end raises an interrupt).
 * Anything the program does that could change what is due or what may
 * interrupt it - a peripheral register write, a CSR write, mret - cuts the
 * slice short so it is looked at again straight away.
 */
#include <string.h>
#include "machine.h"

void chg_init(ChgMachine *m)
{
    memset(m, 0, sizeof(*m));
    memset(m->flash, 0xff, sizeof(m->flash));
    m->entry = CHG_APP_START;
    st7735_power_on(&m->lcd);
    chg_reset(m, true);
}

void chg_reset(ChgMachine *m, bool power_on)
{
    if (power_on) {
        /* SRAM comes up holding junk; a warm reset keeps it, which the boot
           request block at 0x20000000 relies on */
        uint32_t r = 0x9e3779b9u;
        for (uint32_t i = 0; i < CHG_RAM_SIZE; i += 4) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            memcpy(m->ram + i, &r, 4);
        }
    }
    /* The reset cause in RCC RSTSCKR. A power-on reads as pin, power-on and
       software reset together: on the board the factory boot code runs first
       and enters user flash with a software reset (measured, CHGame's
       bootloader, test/hil/RESULTS-2026-10-01.md), which is why the SD menu
       bootloader tells a power-on by PORRSTF alone. A software reset
       (PFIC SYSRESET: a program returning to the bootloader) adds its flag
       to whatever was not cleared. */
    if (power_on) m->reset_flags = 0x1C000000u;     /* PINRSTF | PORRSTF | SFTRSTF */
    else m->reset_flags |= 0x10000000u;             /* SFTRSTF */
    bus_reset(m, power_on);
    /* the card powers with the board; a warm reset of the MCU leaves it be */
    if (power_on) sdspi_reset(&m->sdspi);
    m->sd_selected = false;
    st7735_reset(&m->lcd);
    m->lcd.rst_level = true;
    cpu_reset(m);
    m->reset_request = false;
    m->led = false;
    buzzer_update(m);
    chg_schedule(m);
}

void chg_run(ChgMachine *m, uint64_t until)
{
    while (m->cycles < until) {
        if (m->reset_request) {
            chg_reset(m, false);
            continue;
        }
        if (m->cycles >= m->next_event)
            bus_events(m);
        cpu_take_interrupt(m);
        chg_schedule(m);

        uint64_t stop = until < m->next_event ? until : m->next_event;
        if (stop <= m->cycles)
            stop = m->cycles + 1;
        if (m->cpu.wfi) {
            /* asleep: nothing happens until the next event wakes it */
            if (cpu_irq_waiting(m) && (m->cpu.mstatus & 8)) {
                m->cpu.wfi = false;
                continue;
            }
            m->cycles = stop > m->cycles ? stop : m->cycles;
            if (cpu_irq_waiting(m))
                m->cpu.wfi = false;
            continue;
        }
        m->stop_at = stop;
        cpu_exec(m);
    }
    /* bring the display up to date for the frame about to be shown */
    spi_sync(m, m->cycles);
}
