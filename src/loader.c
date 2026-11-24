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
 *   .chg   a game package for CHGame's SD menu bootloader ("CHG1": a
 *          512-byte header, then the .bin), checked as that bootloader
 *          checks it; its program goes to 0x3000 like a .bin
 *   .chgame a cart (cart.c, CHGame's spec/chgame.md): one game of it (the
 *          first, or chg_cart_game), its binary for the board at 0x3000,
 *          its SD files into the card's folder (chg_cart_sd_folder)
 *
 * Which board it runs on (board.h): the one asked for (chg_board_choice,
 * --board), else the one a CHG package is for (its target id), else the one
 * a --bootloader image is for, else chg_board_fallback (the web page's
 * board menu), else the newest. A .bin, .hex or .elf does not say which
 * board it was built for.
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
#include "cart.h"

#define CHGAME_META_ADDR 0xF700u

const ChgBoard *chg_board_choice;
const ChgBoard *chg_board_fallback;
const char *chg_cart_game;
const char *chg_cart_sd_folder;
int chg_cart_sd_written;

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

/* the bootloader in use: the built-in one, or a file chg_set_bootloader read */
static uint8_t *custom_boot;
static size_t custom_boot_size;
static const ChgBoard *custom_boot_board;

const ChgBoard *chg_bootloader_board(void)
{
    return custom_boot ? custom_boot_board : NULL;
}

bool chg_set_bootloader(const char *path, char *err, size_t errlen)
{
    free(custom_boot);
    custom_boot = NULL;
    custom_boot_size = 0;
    custom_boot_board = NULL;
    if (!path) return true;
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (!data) { snprintf(err, errlen, "cannot read the bootloader %s", path); return false; }
    if (size == 0 || size > CHG_APP_START) {
        snprintf(err, errlen, "%s: a bootloader is at most %u bytes (0x0000-0x2FFF), this is %u",
                 path, (unsigned)CHG_APP_START, (unsigned)size);
        free(data);
        return false;
    }
    custom_boot = data;
    custom_boot_size = size;
    /* a bootloader is built for one board's pins, and since board package
       0.4.0 says which at 0x14 (0 for rev0) */
    custom_boot_board = chg_board_of_bootloader(data, size);
    return true;
}

/* The board a program without a board of its own runs on */
static const ChgBoard *board_for(void)
{
    if (chg_board_choice) return chg_board_choice;
    if (chg_bootloader_board()) return chg_bootloader_board();
    if (chg_board_fallback) return chg_board_fallback;
    return chg_board_default();
}

static void put_bootloader(ChgMachine *m)
{
    memset(m->flash, 0xff, CHG_APP_START);
    if (custom_boot) memcpy(m->flash, custom_boot, custom_boot_size);
    else memcpy(m->flash, m->board->bootloader, *m->board->bootloader_size);
}

void chg_load_bootloader_only(ChgMachine *m)
{
    chg_set_board(m, board_for());
    memset(m->flash, 0xff, sizeof(m->flash));
    memset(m->flash_dirty, 0, sizeof(m->flash_dirty));
    m->flash_written = false;
    put_bootloader(m);
    m->entry = 0;
}

/* What a real CHGame has around an application: the bootloader in the first
   12 KB, and the metadata page the uploader writes last (magic, version,
   length, CRC-32 of the image), which the bootloader checks before it jumps
   to 0x3000. length: the image as uploaded (a .bin or a package's payload,
   padded with 0xFF to whole words, as chgame-upload and the SD menu
   bootloader write it, so the menu knows it as installed), or 0: up to its
   last programmed byte (.hex, .elf) */
static void install_bootloader(ChgMachine *m, uint32_t length)
{
    put_bootloader(m);

    if (!length) {
        uint32_t end = CHGAME_META_ADDR;
        while (end > CHG_APP_START && m->flash[end - 1] == 0xff)
            end--;
        length = (end - CHG_APP_START + 3) & ~3u;
        if (length == 0) length = 4;
    }

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

/* A .CHG game package (CHGame's docs/chg-format.md), what the SD menu
   bootloader installs from the card: a 512-byte header, then the program
   image padded to whole words. Checked as that bootloader checks it, in its
   order; the payload is returned in place */
#define CHG_PKG_HEADER 512u
static bool chg_package(const uint8_t *d, size_t size, const uint8_t **payload, uint32_t *n,
                        const ChgBoard **board, char *err, size_t errlen)
{
    if (size < CHG_PKG_HEADER || rd32(d) != 0x31474843u) {                  /* "CHG1" */
        snprintf(err, errlen, "not a CHG package"); return false;
    }
    if (crc32_iso_hdlc(d, 0x1FC) != rd32(d + 0x1FC)) {
        snprintf(err, errlen, "the CHG package's header is damaged"); return false;
    }
    if (rd16(d + 4) != 1 || rd16(d + 6) != CHG_PKG_HEADER) {
        snprintf(err, errlen, "CHG package format %u is not supported", rd16(d + 4)); return false;
    }
    /* The board it is for (its target id) and the layout (0x3000 + 0xF700).
       A bootloader installs only its own board's packages (the SD menu shows
       ERROR 5 for another's), and so does the emulator for the board asked
       for; without one it takes the package's board */
    const uint32_t target = rd32(d + 8);
    *board = chg_board_by_target(target);
    if (!*board) {
        snprintf(err, errlen, "this CHG package is for a board this emulator does not know (target %c%c%c%c)",
                 d[8], d[9], d[10], d[11]);
        return false;
    }
    if (chg_board_choice && *board != chg_board_choice) {
        snprintf(err, errlen, "this CHG package is for %s, not %s (--board %s)",
                 (*board)->title, chg_board_choice->title, chg_board_choice->name);
        return false;
    }
    if (rd32(d + 12) != 0x003000F7u) {
        snprintf(err, errlen, "this CHG package's layout is not the CHGame's"); return false;
    }
    *n = rd32(d + 16);
    if (!*n || *n > CHGAME_META_ADDR - CHG_APP_START || (*n & 3) || size < CHG_PKG_HEADER + *n) {
        snprintf(err, errlen, "the CHG package's program size is wrong"); return false;
    }
    *payload = d + CHG_PKG_HEADER;
    if (rd32(*payload + 8) == 0x4C424843u) {                                /* "CHBL" */
        snprintf(err, errlen, "this CHG package holds a bootloader, not a program"); return false;
    }
    if (crc32_iso_hdlc(*payload, *n) != rd32(d + 20)) {
        snprintf(err, errlen, "the CHG package's program is damaged (CRC)"); return false;
    }
    return true;
}

/* A .chgame cart: the game's binary for the board at 0x3000, and its SD
   files into the card's folder. The board is the one asked for (the cart
   must have it), else the --bootloader image's, else the page's choice,
   else the newest board the cart has a binary for: unlike a .bin, a cart
   says which boards each binary is for */
static bool load_cart(ChgMachine *m, const uint8_t *data, size_t size, uint32_t *length,
                      const ChgBoard **board, char *err, size_t errlen)
{
    ChgCart *c = chg_cart_open(data, size, err, errlen);
    if (!c) return false;
    bool ok = false;
    uint8_t *image = NULL;
    const int game = chg_cart_find(c, chg_cart_game);
    if (game < 0) {
        size_t n = (size_t)snprintf(err, errlen, "the cart has no game %s; it has:", chg_cart_game);
        for (int i = 0; i < chg_cart_games(c) && n < errlen; i++)
            n += (size_t)snprintf(err + n, errlen - n, " %s", chg_cart_id(c, i));
        goto out;
    }
    const ChgBoard *b = chg_board_choice;
    if (!b) {
        const ChgBoard *prefer[2] = { chg_bootloader_board(), chg_board_fallback };
        for (int i = 0; i < 2 && !b; i++)
            if (prefer[i] && chg_cart_has(c, game, prefer[i]->name)) b = prefer[i];
        for (int i = chg_board_count - 1; i >= 0 && !b; i--)
            if (chg_cart_has(c, game, chg_boards[i].name)) b = &chg_boards[i];
    }
    if (!b) {
        char devs[128];
        chg_cart_devices(c, game, devs, sizeof devs);
        snprintf(err, errlen, "%s has no binary for a board this emulator knows (it has: %s)",
                 chg_cart_title(c, game), devs[0] ? devs : "none");
        goto out;
    }
    size_t len;
    image = chg_cart_binary(c, game, b->name, &len, err, errlen);
    if (!image) goto out;
    if (len >= 12 && rd32(image + 8) == 0x4C424843u) {                      /* "CHBL" */
        snprintf(err, errlen, "the cart's %s binary is a bootloader, not a program", b->name);
        goto out;
    }
    if (!place(m, CHG_APP_START, image, len, err, errlen)) goto out;
    *length = ((uint32_t)len + 3) & ~3u;
    *board = b;
    chg_cart_sd_written = 0;
    if (chg_cart_sd_folder) {
        const int n = chg_cart_write_sdcard(c, game, chg_cart_sd_folder, err, errlen);
        if (n < 0) goto out;
        chg_cart_sd_written = n;
    }
    ok = true;
out:
    free(image);
    chg_cart_free(c);
    return ok;
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
    uint32_t lowest = CHG_APP_START, length = 0;
    const ChgBoard *board = board_for();
    chg_cart_sd_written = 0;
    if (size >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        ok = load_elf(m, data, size, &lowest, err, errlen);
    } else if (chg_is_zip(data, size)) {
        ok = load_cart(m, data, size, &length, &board, err, errlen);
    } else if (size >= 4 && rd32(data) == 0x31474843u) {
        const uint8_t *payload;
        ok = chg_package(data, size, &payload, &length, &board, err, errlen) &&
             place(m, CHG_APP_START, payload, length, err, errlen);
    } else if (ends_with(path, ".hex")) {
        ok = load_hex(m, (const char *)data, &lowest, err, errlen);
    } else if (size > CHG_FLASH_USER - 256 - CHG_APP_START) {
        lowest = 0;
        ok = place(m, 0, data, size, err, errlen);
    } else {
        ok = place(m, CHG_APP_START, data, size, err, errlen);
        length = ((uint32_t)size + 3) & ~3u;
    }
    free(data);
    if (!ok) return false;
    chg_set_board(m, board);

    if (lowest < CHG_APP_START) {
        /* a whole-flash image brings its own bootloader, or is not an
           application at all: it runs from its own reset vector */
        m->entry = 0;
    } else if (chg_use_bootloader) {
        install_bootloader(m, length);
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
