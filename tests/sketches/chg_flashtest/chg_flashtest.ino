// Writes a flash page the way the games' PlatformCHGame.cpp saves do, reads
// it back and fills the screen green when it matches, red when it does not.
// Run it twice: the second run also finds the first run's counter, and the
// screen shows that as a white bar per run so far (up to 16).

#include <Arduino.h>
#include <SPI.h>
extern "C" {
#include "ch32x035.h"
}

#define PAGE 0xF600u
#define RAMFUNC __attribute__((section(".srodata.flashtest"), noinline))

RAMFUNC static void page_write(uint32_t addr, const uint32_t *words)
{
    uint32_t saved;
    __asm volatile("csrr %0, 0x800" : "=r"(saved));
    __asm volatile("csrw 0x800, %0" : : "r"(saved & ~0x88u));
    FLASH->KEYR = 0x45670123u; FLASH->KEYR = 0xCDEF89ABu;
    FLASH->MODEKEYR = 0x45670123u; FLASH->MODEKEYR = 0xCDEF89ABu;
    FLASH->CTLR |= 0x00020000u; FLASH->ADDR = addr + 0x08000000u; FLASH->CTLR |= 0x40u;
    while (FLASH->STATR & 1u) ;
    FLASH->CTLR &= ~0x00020000u;
    FLASH->CTLR |= 0x00010000u; FLASH->CTLR |= 0x00080000u;
    while (FLASH->STATR & 1u) ;
    FLASH->CTLR &= ~0x00010000u;
    for (uint32_t i = 0; i < 64; i++) {
        FLASH->CTLR |= 0x00010000u;
        *(volatile uint32_t *)(addr + 0x08000000u + i * 4) = words[i];
        FLASH->CTLR |= 0x00040000u;
        while (FLASH->STATR & 1u) ;
        FLASH->CTLR &= ~0x00010000u;
    }
    FLASH->CTLR |= 0x00010000u; FLASH->ADDR = addr + 0x08000000u; FLASH->CTLR |= 0x40u;
    while (FLASH->STATR & 1u) ;
    FLASH->CTLR &= ~0x00010000u;
    FLASH->CTLR |= 0x00008000u;
    __asm volatile("csrw 0x800, %0" : : "r"(saved));
}

static void cmd(uint8_t c) { while (SPI1->STATR & 0x80) ; digitalWrite(PB0, LOW); SPI1->DATAR = c; while (SPI1->STATR & 0x80) ; digitalWrite(PB0, HIGH); }
static void dat(uint8_t d) { while (!(SPI1->STATR & 2)) ; SPI1->DATAR = d; }
static void fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c)
{
    cmd(0x2A); dat(0); dat(x + 2); dat(0); dat(x + w + 1);
    cmd(0x2B); dat(0); dat(y + 3); dat(0); dat(y + h + 2);
    cmd(0x2C);
    for (int i = 0; i < w * h; i++) { dat(c >> 8); dat(c); }
}

static uint32_t page[64];

void setup()
{
    pinMode(PA4, OUTPUT); pinMode(PB0, OUTPUT); pinMode(PB12, OUTPUT); pinMode(PB11, OUTPUT);
    digitalWrite(PB11, HIGH); digitalWrite(PB12, HIGH); delay(10); digitalWrite(PB12, LOW); delay(10);
    digitalWrite(PB12, HIGH); delay(120);
    SPI.begin();
    SPI.beginTransaction(SPISettings(24000000, MSBFIRST, SPI_MODE0, SPI_TRANSMITONLY));
    digitalWrite(PA4, LOW);
    cmd(0x11); delay(120); cmd(0x36); dat(0xC8); cmd(0x3A); dat(0x05); cmd(0x29);

    const uint32_t *old = (const uint32_t *)PAGE;
    uint32_t runs = (old[0] == 0x46544553u) ? old[1] : 0;
    page[0] = 0x46544553u;
    page[1] = runs + 1;
    for (int i = 2; i < 64; i++) page[i] = 0x01010101u * (uint32_t)i;
    page_write(PAGE, page);
    const bool ok = memcmp((const void *)PAGE, page, sizeof page) == 0;
    fill(0, 0, 128, 128, ok ? 0x07E0 : 0xF800);
    for (uint32_t i = 0; i < runs + 1 && i < 16; i++)
        fill(4 + i * 8, 100, 6, 20, 0xFFFF);
}

void loop() {}
