/*
 * Getting a program into flash, and the program's own flash writes (its
 * saves) back out to disk.
 *
 *   .bin   what the Arduino IDE exports and what the CHGame bootloader takes:
 *          an application image linked at 0x3000. A file larger than the
 *          application region is taken as a whole-flash image from 0x0000,
 *          bootloader included, and then runs from 0x0000
 *   .hex   Intel HEX, placed where its addresses say
 *   .elf   the loadable segments, at their load addresses
 *
 * The save file sits beside the program as <name>.sav and holds every flash
 * page the program has written: a 4 byte address then 256 bytes, repeated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "machine.h"
#include "loader.h"

#define CHGAME_META_ADDR 0xF700u

extern const uint8_t chg_bootloader[];
extern const size_t chg_bootloader_size;

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = 0;
    *size = (size_t)n;
    return buf;
}

static bool ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != ext[i]) return false;
    return true;
}

static bool place(ChgMachine *m, uint32_t addr, const uint8_t *data, size_t len, char *err, size_t errlen)
{
    addr &= 0x00ffffffu;    /* the 0x08000000 alias is the same flash */
    if (addr + len > CHG_FLASH_USER) {
        snprintf(err, errlen, "%u bytes at 0x%X do not fit the 62 KB of flash", (unsigned)len, (unsigned)addr);
        return false;
    }
    memcpy(m->flash + addr, data, len);
    return true;
}

static int hexval(const char *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}

static bool load_hex(ChgMachine *m, const char *text, uint32_t *lowest, char *err, size_t errlen)
{
    uint32_t base = 0;
    *lowest = 0xffffffffu;
    const char *p = text;
    while (*p) {
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;
        const int n = hexval(p, 2), a = hexval(p + 2, 4), type = hexval(p + 6, 2);
        if (n < 0 || a < 0 || type < 0) { snprintf(err, errlen, "not an Intel HEX file"); return false; }
        uint8_t data[256];
        for (int i = 0; i < n; i++) {
            const int b = hexval(p + 8 + i * 2, 2);
            if (b < 0) { snprintf(err, errlen, "broken Intel HEX record"); return false; }
            data[i] = (uint8_t)b;
        }
        if (type == 0) {
            const uint32_t addr = base + (uint32_t)a;
            if (!place(m, addr, data, (size_t)n, err, errlen)) return false;
            if ((addr & 0x00ffffffu) < *lowest) *lowest = addr & 0x00ffffffu;
        } else if (type == 1) {
            break;
        } else if (type == 2 && n == 2) {
            base = (uint32_t)(data[0] << 8 | data[1]) << 4;
        } else if (type == 4 && n == 2) {
            base = (uint32_t)(data[0] << 8 | data[1]) << 16;
        }
        p += 8 + n * 2;
    }
    if (*lowest == 0xffffffffu) { snprintf(err, errlen, "the HEX file holds no data"); return false; }
    return true;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool load_elf(ChgMachine *m, const uint8_t *f, size_t size, uint32_t *lowest, char *err, size_t errlen)
{
    if (size < 52 || f[4] != 1 || f[5] != 1) { snprintf(err, errlen, "not a 32-bit little endian ELF"); return false; }
    if (rd16(f + 18) != 243) { snprintf(err, errlen, "this ELF is not for RISC-V"); return false; }
    const uint32_t phoff = rd32(f + 28);
    const uint16_t phentsize = rd16(f + 42), phnum = rd16(f + 44);
    *lowest = 0xffffffffu;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + i * phentsize;
        if (ph + 32 > f + size) break;
        if (rd32(ph) != 1) continue;               /* PT_LOAD */
        const uint32_t off = rd32(ph + 4), paddr = rd32(ph + 12), filesz = rd32(ph + 16);
        if (!filesz) continue;
        if (off + filesz > size) { snprintf(err, errlen, "the ELF is cut short"); return false; }
        const uint32_t a = paddr & 0x00ffffffu;
        if (paddr >= CHG_RAM_BASE && paddr < CHG_RAM_BASE + CHG_RAM_SIZE) continue;
        if (!place(m, a, f + off, filesz, err, errlen)) return false;
        if (a < *lowest) *lowest = a;
    }
    if (*lowest == 0xffffffffu) { snprintf(err, errlen, "the ELF has nothing to load into flash"); return false; }
    return true;
}

bool chg_use_bootloader = true;

static uint32_t crc32_iso_hdlc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* What a real CHGame has around an application: the bootloader in the first
   12 KB, and the metadata page the uploader writes last (magic, version,
   length, CRC-32 of the image), which the bootloader checks before it jumps
   to 0x3000 */
static void install_bootloader(ChgMachine *m)
{
    memcpy(m->flash, chg_bootloader, chg_bootloader_size);

    /* the image runs to its last programmed byte, a whole number of words */
    uint32_t end = CHGAME_META_ADDR;
    while (end > CHG_APP_START && m->flash[end - 1] == 0xff)
        end--;
    uint32_t length = (end - CHG_APP_START + 3) & ~3u;
    if (length == 0) length = 4;

    uint8_t *meta = m->flash + CHGAME_META_ADDR;
    memset(meta, 0xff, CHG_PAGE_SIZE);
    put32(meta + 0, 0x4D474843u);      /* CHGAME_META_MAGIC "CHGM" */
    put32(meta + 4, 1);                /* CHGAME_META_VERSION */
    put32(meta + 8, length);
    put32(meta + 12, crc32_iso_hdlc(m->flash + CHG_APP_START, length));
    put32(meta + 16, 0);               /* app_version */
    put32(meta + 20, 0);
    put32(meta + 24, 0);
    put32(meta + 28, 0);
}

bool chg_load_program(ChgMachine *m, const char *path, char *err, size_t errlen)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) { snprintf(err, errlen, "cannot read %s", path); return false; }

    memset(m->flash, 0xff, sizeof(m->flash));
    memset(m->flash_dirty, 0, sizeof(m->flash_dirty));
    m->flash_written = false;

    bool ok;
    uint32_t lowest = CHG_APP_START;
    if (size >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        ok = load_elf(m, data, size, &lowest, err, errlen);
    } else if (ends_with(path, ".hex")) {
        ok = load_hex(m, (const char *)data, &lowest, err, errlen);
    } else if (size > CHG_FLASH_USER - 256 - CHG_APP_START) {
        lowest = 0;
        ok = place(m, 0, data, size, err, errlen);
    } else {
        ok = place(m, CHG_APP_START, data, size, err, errlen);
    }
    free(data);
    if (!ok) return false;

    if (lowest < CHG_APP_START) {
        /* a whole-flash image brings its own bootloader, or is not an
           application at all: it runs from its own reset vector */
        m->entry = 0;
    } else if (chg_use_bootloader) {
        install_bootloader(m);
        m->entry = 0;
    } else {
        m->entry = CHG_APP_START;
    }
    return true;
}

void chg_save_path(const char *program, char *out, size_t outlen)
{
    snprintf(out, outlen, "%s", program);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    char *bslash = strrchr(out, '\\');
    if (bslash > slash) slash = bslash;
    if (dot && (!slash || dot > slash)) *dot = 0;
    strncat(out, ".sav", outlen - strlen(out) - 1);
}

bool chg_load_save(ChgMachine *m, const char *path)
{
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) return false;
    for (size_t o = 0; o + 4 + CHG_PAGE_SIZE <= size; o += 4 + CHG_PAGE_SIZE) {
        const uint32_t addr = rd32(data + o);
        if (addr % CHG_PAGE_SIZE || addr + CHG_PAGE_SIZE > CHG_FLASH_USER) continue;
        memcpy(m->flash + addr, data + o + 4, CHG_PAGE_SIZE);
        m->flash_dirty[addr / CHG_PAGE_SIZE] = 1;
    }
    free(data);
    return true;
}

bool chg_write_save(ChgMachine *m, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    for (uint32_t p = 0; p < CHG_FLASH_USER / CHG_PAGE_SIZE; p++) {
        if (!m->flash_dirty[p]) continue;
        const uint32_t addr = p * CHG_PAGE_SIZE;
        const uint8_t hdr[4] = { (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24) };
        fwrite(hdr, 1, 4, f);
        fwrite(m->flash + addr, 1, CHG_PAGE_SIZE, f);
    }
    fclose(f);
    m->flash_written = false;
    return true;
}
