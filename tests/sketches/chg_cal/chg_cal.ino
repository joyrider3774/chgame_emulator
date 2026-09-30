// Cycle model calibration for the CHGame emulator.
//
// Runs every loop in bench.S from flash and from RAM, against RAM, flash and
// peripheral registers where it makes sense, and reports cycles per loop
// iteration (times 100) over USB serial, once a second, as
//     name value
// The same numbers are kept in cal_results[] with cal_count set when done,
// which the emulator's headless tool can dump instead.

#include <Arduino.h>

typedef void (*BenchFn)(uint32_t n, volatile void *p);

#define DECL(x) extern "C" void f_##x(uint32_t, volatile void *); extern "C" void r_##x(uint32_t, volatile void *);
DECL(add32_8) DECL(add32_24) DECL(add16_16) DECL(add16_48) DECL(add32_8_mis)
DECL(lw8) DECL(lhu8) DECL(sw8) DECL(sb8) DECL(lwuse8) DECL(lwalu8)
DECL(divbig8) DECL(divsmall8) DECL(mul8) DECL(btaken8) DECL(bnot8) DECL(jal8)
#define DECLR(x) extern "C" void r_##x(uint32_t, volatile void *);
DECLR(swalu8) DECLR(sw2alu2) DECLR(sw1alu7) DECLR(lw2alu2) DECLR(lw4) DECLR(lw1alu7) DECLR(mis16)
DECLR(swap_al) DECLR(swap_mis)

static volatile uint8_t swapBuf[512] __attribute__((aligned(4)));

static volatile uint32_t ramBuf[16];
static const uint32_t flashBuf[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
#define PERIPH_READ  ((volatile void *)0x40013008u)   /* SPI1 STATR */
#define PERIPH_WRITE ((volatile void *)0x40010810u)   /* GPIOA BSHR, writing 0 changes nothing */

struct Test { const char *name; BenchFn fn; volatile void *arg; };

#define PAIR(x, arg) { "f_" #x, f_##x, arg }, { "r_" #x, r_##x, arg }
static const Test tests[] = {
    PAIR(add32_8, ramBuf), PAIR(add32_24, ramBuf), PAIR(add16_16, ramBuf), PAIR(add16_48, ramBuf),
    PAIR(add32_8_mis, ramBuf),
    PAIR(lw8, ramBuf), { "f_lw8_flash", f_lw8, (volatile void *)flashBuf }, { "r_lw8_flash", r_lw8, (volatile void *)flashBuf },
    { "f_lw8_per", f_lw8, PERIPH_READ }, { "r_lw8_per", r_lw8, PERIPH_READ },
    PAIR(lhu8, ramBuf), { "r_lhu8_flash", r_lhu8, (volatile void *)flashBuf },
    PAIR(sw8, ramBuf), { "f_sw8_per", f_sw8, PERIPH_WRITE }, { "r_sw8_per", r_sw8, PERIPH_WRITE },
    PAIR(sb8, ramBuf),
    PAIR(lwuse8, ramBuf), PAIR(lwalu8, ramBuf),
    PAIR(divbig8, ramBuf), PAIR(divsmall8, ramBuf), PAIR(mul8, ramBuf),
    PAIR(btaken8, ramBuf), PAIR(bnot8, ramBuf), PAIR(jal8, ramBuf),
    { "r_swalu8", r_swalu8, ramBuf }, { "r_swalu8_per", r_swalu8, PERIPH_WRITE },
    { "r_sw2alu2", r_sw2alu2, ramBuf }, { "r_sw1alu7", r_sw1alu7, ramBuf },
    { "r_lw2alu2", r_lw2alu2, ramBuf }, { "r_lw4", r_lw4, ramBuf }, { "r_lw1alu7", r_lw1alu7, ramBuf },
    { "r_mis16", r_mis16, ramBuf },
    /* 64 pixels an iteration: divide by 64 for a pixel */
    { "r_swap_al", r_swap_al, swapBuf }, { "r_swap_mis", r_swap_mis, swapBuf },
};
#define NTESTS (sizeof(tests) / sizeof(tests[0]))
#define ITER 20000u

volatile uint32_t cal_results[64];
volatile uint32_t cal_count;

void setup()
{
    pinMode(PB9, OUTPUT);
    delay(200);
    for (uint32_t i = 0; i < NTESTS; i++) {
        tests[i].fn(10, tests[i].arg);             /* once to settle */
        const uint32_t t0 = micros();
        tests[i].fn(ITER, tests[i].arg);
        const uint32_t us = micros() - t0;
        /* cycles per iteration * 100 = us * 48 * 100 / ITER */
        cal_results[i] = (uint32_t)((uint64_t)us * 4800u / ITER);
    }
    cal_count = NTESTS;
}

void loop()
{
    digitalWrite(PB9, HIGH);
    if (Serial.dtr()) {
        Serial.print("BEGIN ");
        Serial.println((int)NTESTS);
        for (uint32_t i = 0; i < NTESTS; i++) {
            Serial.print(tests[i].name);
            Serial.print(' ');
            Serial.println((int)cal_results[i]);
        }
        Serial.println("END");
    }
    delay(1000);
    digitalWrite(PB9, LOW);
    delay(100);
}
