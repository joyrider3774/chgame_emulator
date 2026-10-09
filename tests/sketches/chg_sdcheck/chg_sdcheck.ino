/*
 * microSD check for a CHGame: the steps Ethan's Critters takes to open
 * CRITTERS.DAT and stream it, one by one, with their timings, so a card
 * the game calls "INSERT CARD" shows where it fails. Results go to USB
 * serial while DTR is on, between BEGIN and END, a line each.
 *
 * Built against the CHSd copy the game vendors (bateske/EthansCritters,
 * chgame/platform/.../CHSd: CHGame's CHSd with its stream() patch), with
 * one test-only change: sd::streamBr, the SPI1 baud divider stream() uses
 * (0 = 24 MHz as the game, 1 = 12 MHz, 2 = 6 MHz), so the same stream test
 * runs at three clocks.
 *
 *   INIT n ok us        sd::init(), up to three tries
 *   SDHC h              block addressing
 *   CID maker name      CMD10
 *   CSD hex             CMD9
 *   MOUNT rc            fat::mount (0 ok, -10 no FAT, -11 exFAT)
 *   FIND rc size        fat::find("CRITTERSDAT")
 *   RUNS n              its FAT runs (the game takes up to 8)
 *   READ0 ok us magic   block 0, one single-block read (12 MHz, polled)
 *   SINGLE fails maxus  30 single-block reads at random places
 *   STREAM mhz tmo fails maxus reinits   200 streams of 17 blocks (a frame's)
 */
#include <CHGfx.h>
#include <SdSpi.h>
#include <Fat.h>

namespace sd { extern uint8_t streamBr; }

static char out[4000];
static int olen;
static void put(const char *s) { while (*s && olen < (int)sizeof(out) - 1) out[olen++] = *s++; out[olen] = 0; }
static void num(int32_t v)
{
    char b[12];
    int i = 11;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    put(" ");
    put(b + i);
}
static void hex(uint8_t v)
{
    static const char H[] = "0123456789ABCDEF";
    char b[3] = { H[v >> 4], H[v & 15], 0 };
    put(b);
}
static void line(const char *k) { if (olen) put("\n"); put(k); }

static uint8_t buf[512] __attribute__((aligned(4)));
static uint8_t b0[512] __attribute__((aligned(4)));
static uint8_t b1[512] __attribute__((aligned(4)));
static uint32_t seed = 12345;
static uint32_t rnd(uint32_t n) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) % n; }
static void sink(const uint8_t *, void *) {}

// --- The card's identification, by hand, every answer logged ------------
// The same steps and the same SPI1 setup as CHSd's ident() (187.5 kHz, mode
// 0, CS on PB11, the panel's CS on PA4 high), done here so the card's raw
// answers can be seen when sd::init() says no.
static uint8_t x(uint8_t b)
{
    SPI1->DATAR = b;
    while (!(SPI1->STATR & 1u)) {}
    return (uint8_t)SPI1->DATAR;
}
// variant C: CS up and a clocked gap before every command, as some drivers do
static bool gapped = false;
static int rawLeft;             // commands whose raw answer bytes are still logged
static uint8_t c(uint8_t cmdi, uint32_t arg)
{
    if (gapped) {
        GPIOB->BSHR = 1u << 11;
        x(0xFF);
        GPIOB->BCR = 1u << 11;
        x(0xFF);
    }
    x((uint8_t)(0x40 | cmdi));
    for (int sh = 24; sh >= 0; sh -= 8) x((uint8_t)(arg >> sh));
    x(cmdi == 8 ? 0x87 : 0x95);
    uint8_t r;
    uint32_t k = 9;
    // every byte the card gives until R1, logged as [ff ff 01]
    const bool raw = rawLeft > 0;
    if (raw) { rawLeft--; put(" ["); }
    do { r = x(0xFF); if (raw) hex(r); } while ((r & 0x80) && --k);
    if (raw) put("]");
    return r;
}
// name: the variant's label; mode: CPOL/CPHA bits (0 = mode 0, 3 = mode 3)
static void ident(const char *name, uint32_t mode, bool gap)
{
    gapped = gap;
    rawLeft = 40;
    line("VARIANT "); put(name);
    GPIOA->BSHR = 1u << 6;                       // MISO pulled up, as CHSd does
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFul << 24)) | (0x8ul << 24);
    GPIOA->BSHR = 1u << 4;                       // panel deselected
    const uint16_t saved = (uint16_t)SPI1->CTLR1;
    const uint32_t ident = (1u << 2) | (1u << 8) | (1u << 9) | (7u << 3) | mode;
    while (SPI1->STATR & (1u << 7)) {}
    SPI1->CTLR1 = (uint16_t)ident;
    SPI1->CTLR1 = (uint16_t)(ident | (1u << 6));
    (void)SPI1->DATAR;
    GPIOB->BSHR = 1u << 11;
    for (int i = 0; i < 10; i++) x(0xFF);        // 80 clocks, CS high
    GPIOB->BCR = 1u << 11;
    uint8_t w = 0;
    uint32_t t0 = micros();
    do w = x(0xFF); while (w != 0xFF && micros() - t0 < 100000);
    line("H_BUSYWAIT"); put(" "); hex(w);
    line("H_CMD0");
    for (int i = 0; i < 5; i++) { put(" "); hex(c(0, 0)); }
    uint8_t r = c(8, 0x1AA);
    line("H_CMD8 "); hex(r); put(" ");
    for (int i = 0; i < 4; i++) hex(x(0xFF));
    line("H_ACMD41");
    t0 = micros();
    int tries = 0;
    do {
        uint8_t r55 = c(55, 0);
        uint8_t r41 = c(41, 1ul << 30);
        if (tries < 6) { put(" "); hex(r55); put("/"); hex(r41); }
        r = r41;
        tries++;
    } while (r && micros() - t0 < 1000000);
    num(tries); num((int32_t)(micros() - t0));
    r = c(58, 0);
    line("H_CMD58 "); hex(r); put(" ");
    for (int i = 0; i < 4; i++) hex(x(0xFF));
    GPIOB->BSHR = 1u << 11;
    x(0xFF);
    while (SPI1->STATR & (1u << 7)) {}
    SPI1->CTLR1 = (uint16_t)(saved & ~(1u << 6));
    SPI1->CTLR1 = saved;
    gapped = false;
}

// SCK (PA5) and MOSI (PA7) as alternate-function push-pull with the given
// output speed nibble: 0xB = 50 MHz (as CHGfx sets them), 0x9 = 10 MHz,
// 0xA = 2 MHz. Slower edges ring less on a long clock line
static void spiPins(uint32_t nibble)
{
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFul << 20) | (0xFul << 28))) | (nibble << 20) | (nibble << 28);
}

static void run()
{
    // A failed identification leaves this card silent until it loses power,
    // so the game's own steps go first, through the CHSd copy whose cmd()
    // now clocks one idle byte (N_RC) before every command, with the pins as
    // CHGfx sets them
    line("LIBRARY with an idle byte before each command");
    bool ok = false;
    for (int i = 1; i <= 3 && !ok; i++) {
        uint32_t t = micros();
        ok = sd::init();
        line("INIT"); num(i); num(ok); num((int32_t)(micros() - t));
    }
    if (!ok) return;
    line("SDHC"); num(sd::isSdhc());
    uint8_t r[16];
    if (sd::readReg(10, r)) {
        line("CID"); num(r[0]); put(" ");
        for (int i = 3; i < 8; i++) { char c[2] = { (char)((r[i] >= 32 && r[i] < 127) ? r[i] : '?'), 0 }; put(c); }
    }
    if (sd::readReg(9, r)) { line("CSD "); for (int i = 0; i < 16; i++) hex(r[i]); }

    int8_t rc = fat::mount(buf);
    line("MOUNT"); num(rc);
    if (rc) return;
    fat::File f;
    rc = fat::find("CRITTERSDAT", f, buf);
    line("FIND"); num(rc); num((int32_t)f.size);
    if (rc) return;
    static fat::Run runs[8];
    int8_t n = fat::runs(f, runs, 8, buf);
    line("RUNS"); num(n);
    if (n <= 0) return;
    const uint32_t blocks = f.size / 512;

    uint32_t t = micros();
    ok = fat::read(runs, (uint32_t)n, 0, buf);
    line("READ0"); num(ok); num((int32_t)(micros() - t)); put(" "); for (int i = 0; i < 4; i++) hex(buf[i]);
    if (!ok) sd::init();

    uint32_t fails = 0, maxus = 0;
    for (int i = 0; i < 30; i++) {
        t = micros();
        bool k = fat::read(runs, (uint32_t)n, rnd(blocks), buf);
        uint32_t dt = micros() - t;
        if (dt > maxus) maxus = dt;
        if (!k) { fails++; sd::init(); }
    }
    line("SINGLE"); num((int32_t)fails); num((int32_t)maxus);

    static const struct { uint8_t br, mhz; uint32_t tmo; } TESTS[] = {
        { 0, 24, 1000000 }, { 0, 24, 32000 }, { 1, 12, 32000 }, { 2, 6, 32000 },
    };
    for (unsigned j = 0; j < sizeof(TESTS) / sizeof(TESTS[0]); j++) {
        sd::streamBr = TESTS[j].br;
        sd::setStreamTimeout(TESTS[j].tmo);
        uint32_t reinits = 0;
        fails = maxus = 0;
        for (int i = 0; i < 200; i++) {
            t = micros();
            bool k = fat::stream(runs, (uint8_t)n, rnd(blocks - 17), 17, b0, b1, sink, nullptr);
            uint32_t dt = micros() - t;
            if (dt > maxus) maxus = dt;
            if (!k) {
                fails++;
                if (!sd::recover()) { reinits++; sd::init(); }
            }
        }
        line("STREAM"); num(TESTS[j].mhz); num((int32_t)TESTS[j].tmo); num((int32_t)fails); num((int32_t)maxus); num((int32_t)reinits);
    }
    sd::streamBr = 0;
}

void setup()
{
    gfx_begin(GFX_DIV2, GFX_16BPP);     // SPI1 and its pins, as the game has them
    delay(300);
    run();
    line("DONE");
}

void loop()
{
    if (Serial.dtr()) {
        Serial.println("BEGIN");
        Serial.println(out);
        Serial.println("END");
    }
    delay(1000);
}
