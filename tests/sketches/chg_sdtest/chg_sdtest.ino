/*
 * microSD write test for the emulator (and a real CHGame): mounts the card,
 * makes a directory, writes a file over several blocks, reads it back.
 * Results in sd_result[] for chg_headless --mem (address from nm):
 *   [0] 1 mounted  [1] bytes written  [2] 1 read back equal  [3] 1 mkdir
 *   [4] 0xC0DE when done
 * Needs the Arduino SD library with the CH32 pin map fix (see CLAUDE.md).
 */
#include <SPI.h>
#include <SD.h>

#define PIN_SD_CS PB11
#define LEN 3000

volatile uint32_t sd_result[5];

static uint8_t pattern(uint32_t i) { return (uint8_t)(i * 7 + (i >> 8)); }

void setup()
{
    pinMode(PA4, OUTPUT);           /* the display's CS stays high */
    digitalWrite(PA4, HIGH);
    sd_result[0] = SD.begin(PIN_SD_CS) ? 1 : 0;
    if (sd_result[0]) {
        sd_result[3] = SD.mkdir("TESTDIR") ? 1 : 0;
        SD.remove("TESTDIR/WRITE.BIN");
        File f = SD.open("TESTDIR/WRITE.BIN", FILE_WRITE);
        uint32_t n = 0;
        if (f) {
            for (uint32_t i = 0; i < LEN; i++) n += f.write(pattern(i));
            f.close();
        }
        sd_result[1] = n;
        f = SD.open("TESTDIR/WRITE.BIN", FILE_READ);
        bool same = f && f.size() == LEN;
        for (uint32_t i = 0; same && i < LEN; i++) same = f.read() == pattern(i);
        if (f) f.close();
        sd_result[2] = same ? 1 : 0;
    }
    sd_result[4] = 0xC0DE;
}

void loop() {}
