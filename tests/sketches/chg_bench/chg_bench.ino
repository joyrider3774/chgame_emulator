// CHGame timing benchmark.
//
// Runs a handful of loops whose cost depends on how the core fetches code and
// data, times each with micros() and prints the results on the display, one
// line per test: the test number, then microseconds. Run it on a real CHGame
// and in the emulator and compare the two screens - that is how the
// emulator's cycle model is calibrated.
//
//   1  32768 bytes to SPI1 by hand, loop in flash (the old SpiSend)
//   2  the same loop, in RAM
//   3  32768 bytes to SPI1 by DMA
//   4  100000 iterations of an ALU loop in flash
//   5  the same loop in RAM
//   6  10000 32-bit divides
//   7  10000 byte loads from a flash table
//   8  16384 pixel byte swaps in RAM (writePixels' loop)
//   9  delay(100), as a check on the clock itself
//
// Built with the CHGame board, Peripherals: Game.

#include <Arduino.h>
#include <SPI.h>
extern "C" {
#include "ch32x035.h"
}

#define RAMFUNC __attribute__((section(".data.bench"), noinline))

static const uint8_t font3x5[][3] = {
    { 0x1f, 0x11, 0x1f }, { 0x00, 0x1f, 0x00 }, { 0x1d, 0x15, 0x17 }, { 0x15, 0x15, 0x1f },
    { 0x07, 0x04, 0x1f }, { 0x17, 0x15, 0x1d }, { 0x1f, 0x15, 0x1d }, { 0x01, 0x01, 0x1f },
    { 0x1f, 0x15, 0x1f }, { 0x17, 0x15, 0x1f },
};

static uint8_t frame[128 * 2];

static void cmd(uint8_t c)
{
    while (SPI1->STATR & SPI_STATR_BSY) ;
    digitalWrite(PB0, LOW);
    SPI1->DATAR = c;
    while (SPI1->STATR & SPI_STATR_BSY) ;
    digitalWrite(PB0, HIGH);
}

static void dat(uint8_t d)
{
    while (!(SPI1->STATR & SPI_STATR_TXE)) ;
    SPI1->DATAR = d;
}

static void window(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    cmd(0x2A); dat(0); dat(x0 + 2); dat(0); dat(x1 + 2);
    cmd(0x2B); dat(0); dat(y0 + 3); dat(0); dat(y1 + 3);
    cmd(0x2C);
}

static void fill(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t c)
{
    window(x, y, x + w - 1, y + h - 1);
    for (int i = 0; i < w * h; i++) { dat(c >> 8); dat(c); }
    while (SPI1->STATR & SPI_STATR_BSY) ;
}

static void digit(int x, int y, int d, uint16_t c)
{
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 5; row++)
            if (font3x5[d][col] >> row & 1)
                fill(x + col * 2, y + row * 2, 2, 2, c);
}

static void number(int x, int y, uint32_t v, uint16_t c)
{
    char buf[12];
    int n = 0;
    do { buf[n++] = v % 10; v /= 10; } while (v);
    while (n--) { digit(x, y, buf[n], c); x += 8; }
}

static void display_init(void)
{
    pinMode(PB11, OUTPUT); digitalWrite(PB11, HIGH);
    pinMode(PA4, OUTPUT); digitalWrite(PA4, HIGH);
    pinMode(PB0, OUTPUT); digitalWrite(PB0, HIGH);
    pinMode(PB12, OUTPUT);
    digitalWrite(PB12, HIGH); delay(10);
    digitalWrite(PB12, LOW); delay(10);
    digitalWrite(PB12, HIGH); delay(120);
    SPI.begin();
    SPI.beginTransaction(SPISettings(24000000, MSBFIRST, SPI_MODE0, SPI_TRANSMITONLY));
    digitalWrite(PA4, LOW);
    cmd(0x01); delay(150);
    cmd(0x11); delay(255);
    cmd(0x36); dat(0xC8);
    cmd(0x3A); dat(0x05);
    cmd(0x29); delay(100);
    fill(0, 0, 128, 128, 0x0000);
}

// ---- the tests ----------------------------------------------------------

__attribute__((noinline)) static void spi_by_hand(const uint8_t *data, size_t length)
{
    while (length--) {
        while (!(SPI1->STATR & SPI_STATR_TXE)) ;
        SPI1->DATAR = *data++;
    }
}

RAMFUNC static void spi_by_hand_ram(const uint8_t *data, size_t length)
{
    while (length--) {
        while (!(SPI1->STATR & SPI_STATR_TXE)) ;
        SPI1->DATAR = *data++;
    }
}

static void spi_dma(const uint8_t *data, uint16_t length)
{
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    SPI1->CTLR2 &= (uint16_t)~SPI_I2S_DMAReq_Tx;
    DMA1_Channel3->CFGR = 0;
    DMA1->INTFCR = 0xF00;
    DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
    DMA1_Channel3->MADDR = (uint32_t)data;
    DMA1_Channel3->CNTR = length;
    DMA1_Channel3->CFGR = DMA_CFGR1_DIR | DMA_CFGR1_MINC | DMA_CFGR1_PL;
    SPI1->CTLR2 |= SPI_I2S_DMAReq_Tx;
    DMA1_Channel3->CFGR |= DMA_CFGR1_EN;
    while (!(DMA1->INTFR & DMA1_FLAG_TC3)) ;
    DMA1_Channel3->CFGR = 0;
    DMA1->INTFCR = 0xF00;
    SPI1->CTLR2 &= (uint16_t)~SPI_I2S_DMAReq_Tx;
    while (SPI1->STATR & SPI_STATR_BSY) ;
}

__attribute__((noinline)) static uint32_t alu_flash(uint32_t n)
{
    uint32_t a = 1, b = 7;
    while (n--) { a = a * 3 + b; b ^= a >> 3; }
    return a + b;
}

RAMFUNC static uint32_t alu_ram(uint32_t n)
{
    uint32_t a = 1, b = 7;
    while (n--) { a = a * 3 + b; b ^= a >> 3; }
    return a + b;
}

static volatile uint32_t divisor = 7;

__attribute__((noinline)) static uint32_t divides(uint32_t n)
{
    uint32_t a = 0xFFFFFFF0u, s = 0;
    while (n--) { s += a / divisor; a -= 13; }
    return s;
}

static const uint8_t table[256] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };

__attribute__((noinline)) static uint32_t flash_loads(uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += table[i & 255];
    return s;
}

static uint16_t pixels[64];

RAMFUNC static void swap_pixels(uint8_t *dst, const uint16_t *src, int32_t run)
{
    for (int32_t i = 0; i < run; i++) {
        *dst++ = (uint8_t)(src[i] >> 8);
        *dst++ = (uint8_t)src[i];
    }
}

volatile uint32_t sink;

void setup()
{
    display_init();
    uint32_t results[10];
    static uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = 0;

    uint32_t t;

    // the pixels go into a window that covers the whole screen, so what they
    // draw is overwritten by the results afterwards
    window(0, 0, 127, 127);
    t = micros(); for (int i = 0; i < 128; i++) spi_by_hand(buf, 256); results[1] = micros() - t;
    while (SPI1->STATR & SPI_STATR_BSY) ;
    window(0, 0, 127, 127);
    t = micros(); for (int i = 0; i < 128; i++) spi_by_hand_ram(buf, 256); results[2] = micros() - t;
    while (SPI1->STATR & SPI_STATR_BSY) ;
    window(0, 0, 127, 127);
    t = micros(); for (int i = 0; i < 128; i++) spi_dma(buf, 256); results[3] = micros() - t;

    t = micros(); sink = alu_flash(100000); results[4] = micros() - t;
    t = micros(); sink = alu_ram(100000); results[5] = micros() - t;
    t = micros(); sink = divides(10000); results[6] = micros() - t;
    t = micros(); sink = flash_loads(10000); results[7] = micros() - t;
    t = micros(); for (int i = 0; i < 256; i++) swap_pixels(frame, pixels, 64); results[8] = micros() - t;
    t = micros(); delay(100); results[9] = micros() - t;

    fill(0, 0, 128, 128, 0x0000);
    for (int i = 1; i <= 9; i++) {
        digit(2, 2 + (i - 1) * 14, i, 0xFFE0);
        number(20, 2 + (i - 1) * 14, results[i], 0xFFFF);
    }
}

void loop()
{
    digitalWrite(PB9, (millis() / 500) & 1);
    pinMode(PB9, OUTPUT);
}
