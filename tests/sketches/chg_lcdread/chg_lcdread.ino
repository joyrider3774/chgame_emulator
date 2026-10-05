/*
 * Reads the CHGame's display controller back over its bidirectional data
 * line, bit-banged (the SPI peripheral only sends): its ID registers, and
 * frame memory after writes in each colour mode, to see what the controller
 * stores. Results go to USB serial while DTR is on, between BEGIN and END,
 * one line per read: name, then the bytes read, in hex.
 *
 * Datasheet 9.4.4: SDA is sampled on SCL's rising edge; the controller
 * shifts read data out on the falling edge; 24 and 32-bit reads have one
 * dummy clock first. RAMRD's framing is not drawn there, so its reads are
 * printed raw, with a known pattern written first.
 */
#include <CHGfx.h>

#define SCL_BIT (1u << 5)   /* PA5 */
#define SDA_BIT (1u << 7)   /* PA7 */
#define CS_BIT  (1u << 4)   /* PA4 */
#define DC_BIT  (1u << 0)   /* PB0 */

static void pause(void) { delayMicroseconds(2); }

static void sda_out(bool out)
{
    /* PA7: push-pull output 50 MHz (0x3) or floating input (0x4) */
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFu << 28)) | ((out ? 0x3u : 0x4u) << 28);
}

static void bb_begin(void)
{
    SPI1->CTLR1 &= ~(1u << 6);          /* SPE off: the pins are ours */
    /* PA5 and PA4 push-pull outputs */
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFu << 20 | 0xFu << 16)) | (0x3u << 20 | 0x3u << 16);
    GPIOA->BSHR = CS_BIT;
    GPIOA->BCR = SCL_BIT;
    sda_out(true);
}

static void put_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        GPIOA->BCR = SCL_BIT;
        if (b & (1u << i)) GPIOA->BSHR = SDA_BIT; else GPIOA->BCR = SDA_BIT;
        pause();
        GPIOA->BSHR = SCL_BIT;
        pause();
    }
    GPIOA->BCR = SCL_BIT;
}

static uint8_t get_byte(void)
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        GPIOA->BCR = SCL_BIT;
        pause();
        GPIOA->BSHR = SCL_BIT;
        b = (uint8_t)(b << 1 | ((GPIOA->INDR & SDA_BIT) ? 1 : 0));
        pause();
    }
    GPIOA->BCR = SCL_BIT;
    return b;
}

static void dummy_clock(void)
{
    GPIOA->BCR = SCL_BIT; pause();
    GPIOA->BSHR = SCL_BIT; pause();
    GPIOA->BCR = SCL_BIT;
}

static void cs(bool low) { if (low) GPIOA->BCR = CS_BIT; else GPIOA->BSHR = CS_BIT; pause(); }
static void dc(bool data) { if (data) GPIOB->BSHR = DC_BIT; else GPIOB->BCR = DC_BIT; }

/* a command and its parameters */
static void wr(uint8_t c, const uint8_t *p, int n)
{
    cs(true);
    dc(false); put_byte(c);
    dc(true);
    for (int i = 0; i < n; i++) put_byte(p[i]);
    cs(false);
}
static void wr0(uint8_t c) { wr(c, NULL, 0); }
static void wr1(uint8_t c, uint8_t a) { wr(c, &a, 1); }
static void win(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1)
{
    const uint8_t a[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t b[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    wr(0x2A, a, 4);
    wr(0x2B, b, 4);
}

/* a read command: n bytes after an optional dummy clock */
static void rd(uint8_t c, bool dummy, uint8_t *out, int n)
{
    cs(true);
    dc(false); put_byte(c);
    sda_out(false);
    dc(true);
    if (dummy) dummy_clock();
    for (int i = 0; i < n; i++) out[i] = get_byte();
    cs(false);
    sda_out(true);
}

#define MAXLINES 24
static char names[MAXLINES][12];
static uint8_t data[MAXLINES][32];
static int lens[MAXLINES], nlines;

static void record(const char *name, uint8_t c, bool dummy, int n)
{
    if (nlines >= MAXLINES) return;
    strncpy(names[nlines], name, 11);
    rd(c, dummy, data[nlines], n);
    lens[nlines++] = n;
}

/* RAMRD of a window, raw */
static void ramrd(const char *name, uint16_t x0, uint16_t x1, uint16_t y, int n = 16)
{
    win(x0, x1, y, y);
    record(name, 0x2E, false, n);
}

void setup()
{
    gfx_begin(GFX_DIV4, GFX_16BPP);
    bb_begin();

    record("RDDID", 0x04, true, 3);
    record("RDDST", 0x09, true, 4);
    record("RDDPM", 0x0A, false, 1);
    record("RDDMADCTL", 0x0B, false, 1);
    record("RDDCOLMOD", 0x0C, false, 1);
    record("RDID1", 0xDA, false, 1);
    record("RDID2", 0xDB, false, 1);
    record("RDID3", 0xDC, false, 1);

    /* 18 bit pattern in row 5: red, green, blue, A8 54 24 */
    wr1(0x36, 0x00);
    wr1(0x3A, 0x66);
    static const uint8_t pat[12] = { 0xFC,0,0, 0,0xFC,0, 0,0,0xFC, 0xA8,0x54,0x24 };
    win(0, 3, 5, 5); wr(0x2C, pat, 12);
    ramrd("ROW5", 0, 3, 5);

    /* memory height: rows 131, 140 and 161 written, then read */
    static const uint8_t p131[3] = { 0x10,0x20,0x30 }, p140[3] = { 0x40,0x50,0x60 }, p161[3] = { 0x70,0x80,0x90 };
    win(0, 0, 131, 131); wr(0x2C, p131, 3);
    win(0, 0, 140, 140); wr(0x2C, p140, 3);
    win(0, 0, 161, 161); wr(0x2C, p161, 3);
    ramrd("ROW131", 0, 3, 131);
    ramrd("ROW140", 0, 3, 140);
    ramrd("ROW161", 0, 3, 161);
    ramrd("ROW8", 0, 3, 8);     /* 140 - 132, if writes wrap */
    ramrd("ROW29", 0, 3, 29);   /* 161 - 132 */

    /* 16 bit through the table as it is: r=16, g=32, b=16, white */
    static const uint8_t p16[8] = { 0x80,0x00, 0x04,0x00, 0x00,0x10, 0xFF,0xFF };
    wr1(0x3A, 0x55); win(0, 3, 6, 6); wr(0x2C, p16, 8);
    wr1(0x3A, 0x66); ramrd("ROW6_16", 0, 3, 6);

    /* RGBSET with red reversed, then the same 16 bit pixels */
    uint8_t lut[128];
    for (int i = 0; i < 32; i++) { lut[i] = (uint8_t)((31 - i) << 1 | (31 - i) >> 4); lut[96 + i] = (uint8_t)(i << 1 | i >> 4); }
    for (int i = 0; i < 64; i++) lut[32 + i] = (uint8_t)i;
    wr(0x2D, lut, 128);
    wr1(0x3A, 0x55); win(0, 3, 7, 7); wr(0x2C, p16, 8);
    wr1(0x3A, 0x66); ramrd("ROW7_16LUT", 0, 3, 7);

    /* 12 bit, two pixels: (15,0,0) and (8,4,2) */
    static const uint8_t p12[3] = { 0xF0, 0x08, 0x42 };
    wr1(0x3A, 0x53); win(0, 1, 9, 9); wr(0x2C, p12, 3);
    wr1(0x3A, 0x66); ramrd("ROW9_12", 0, 1, 9);

    record("RDDCOLMOD2", 0x0C, false, 1);

    /* how far MY mirrors: row counter 0 written with MY set */
    static const uint8_t pmy[3] = { 0x14, 0x28, 0x3C }, pmx[3] = { 0x44, 0x88, 0xCC };
    wr1(0x36, 0x80); win(0, 0, 0, 0); wr(0x2C, pmy, 3);
    /* how far MX mirrors: column counter 0 of row 20 written with MX set */
    wr1(0x36, 0x40); win(0, 0, 20, 20); wr(0x2C, pmx, 3);
    wr1(0x36, 0x00);
    ramrd("MY131", 0, 3, 131);
    ramrd("MY161", 0, 3, 161);
    ramrd("MX128", 128, 131, 20);

    /* 16 bit to 18: red 1, 2, 15, 17, 30, then blue 1, 15, 17 */
    static const uint8_t p16b[16] = { 0x08,0x00, 0x10,0x00, 0x78,0x00, 0x88,0x00, 0xF0,0x00, 0x00,0x01, 0x00,0x0F, 0x00,0x11 };
    wr1(0x3A, 0x55); win(0, 7, 10, 10); wr(0x2C, p16b, 16);
    wr1(0x3A, 0x66); ramrd("ROW10_16", 0, 7, 10, 28);
}

void loop()
{
    if (Serial.dtr()) {
        Serial.println("BEGIN");
        for (int i = 0; i < nlines; i++) {
            Serial.print(names[i]);
            for (int j = 0; j < lens[i]; j++) {
                Serial.print(' ');
                if (data[i][j] < 16) Serial.print('0');
                Serial.print(data[i][j], HEX);
            }
            Serial.println();
        }
        Serial.println("END");
    }
    delay(1000);
}
