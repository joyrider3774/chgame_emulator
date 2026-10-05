/*
 * Animated GIF (89a) encoding for the screen recorder.
 *
 * Every frame is a full image with its own colour table: the exact colours
 * when it has at most 256 (nearly every CHGame game), otherwise a fixed
 * 3-3-2 palette. A frame equal to the previous one is not written again:
 * the previous one is shown longer. Delays are in hundredths of a second,
 * with the rounding carried over so the total stays right; GIF viewers show
 * delays under 2 as 10, so a frame shown shorter waits for the next one.
 * The image data is LZW coded with 8-bit roots and a dictionary that is
 * cleared when it reaches 4096 codes.
 */
#include <stdlib.h>
#include <string.h>
#include "gif.h"

static void put(Gif *g, const void *p, size_t n)
{
    if (g->failed) return;
    if (g->size + n > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 65536;
        while (cap < g->size + n) cap *= 2;
        uint8_t *d = realloc(g->data, cap);
        if (!d) { g->failed = true; return; }
        g->data = d;
        g->cap = cap;
    }
    memcpy(g->data + g->size, p, n);
    g->size += n;
}

static void put8(Gif *g, unsigned v) { uint8_t b = (uint8_t)v; put(g, &b, 1); }
static void put16(Gif *g, unsigned v) { put8(g, v & 255); put8(g, v >> 8); }

bool gif_begin(Gif *g, int w, int h)
{
    memset(g, 0, sizeof(*g));
    g->w = w;
    g->h = h;
    g->pending = malloc((size_t)w * h * sizeof(uint32_t));
    if (!g->pending) return false;
    put(g, "GIF89a", 6);
    put16(g, (unsigned)w);
    put16(g, (unsigned)h);
    put8(g, 0x70);          /* no global colour table, 8 bits of colour resolution */
    put8(g, 0);
    put8(g, 0);
    /* NETSCAPE2.0: loop forever */
    put8(g, 0x21); put8(g, 0xFF); put8(g, 11);
    put(g, "NETSCAPE2.0", 11);
    put8(g, 3); put8(g, 1); put16(g, 0); put8(g, 0);
    return !g->failed;
}

/* ------------------------------------------------------------------------ */
/* LZW                                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    Gif *g;
    uint8_t block[255];
    int nblock;
    uint32_t bits;
    int nbits;
} Out;

static void out_byte(Out *o, uint8_t b)
{
    o->block[o->nblock++] = b;
    if (o->nblock == 255) {
        put8(o->g, 255);
        put(o->g, o->block, 255);
        o->nblock = 0;
    }
}

static void out_code(Out *o, unsigned code, int size)
{
    o->bits |= (uint32_t)code << o->nbits;
    o->nbits += size;
    while (o->nbits >= 8) {
        out_byte(o, (uint8_t)o->bits);
        o->bits >>= 8;
        o->nbits -= 8;
    }
}

#define HASH_SIZE 5003

static void lzw(Gif *g, const uint8_t *idx, int n)
{
    const unsigned clear = 256, eoi = 257;
    static int32_t hkey[HASH_SIZE];
    static uint16_t hcode[HASH_SIZE];
    Out o = { g, { 0 }, 0, 0, 0 };
    put8(g, 8);             /* minimum code size */
    unsigned next = 258;
    int size = 9;
    memset(hkey, 0xff, sizeof hkey);
    out_code(&o, clear, size);
    unsigned prefix = idx[0];
    for (int i = 1; i < n; i++) {
        const unsigned c = idx[i];
        const int32_t key = (int32_t)(prefix << 8 | c);
        unsigned h = ((unsigned)key * 2654435761u) % HASH_SIZE;
        while (hkey[h] != -1 && hkey[h] != key) h = (h + 1) % HASH_SIZE;
        if (hkey[h] == key) { prefix = hcode[h]; continue; }
        out_code(&o, prefix, size);
        if (next < 4095) {  /* cleared one early, as most encoders do: some decoders need it */
            hkey[h] = key;
            hcode[h] = (uint16_t)next;
            /* the decoder widens its codes when the dictionary reaches 2^size */
            if (next == (1u << size) && size < 12) size++;
            next++;
        } else {
            out_code(&o, clear, size);
            memset(hkey, 0xff, sizeof hkey);
            next = 258;
            size = 9;
        }
        prefix = c;
    }
    out_code(&o, prefix, size);
    out_code(&o, eoi, size);
    if (o.nbits) out_byte(&o, (uint8_t)o.bits);
    if (o.nblock) { put8(g, (unsigned)o.nblock); put(g, o.block, (size_t)o.nblock); }
    put8(g, 0);             /* end of the image data */
}

/* ------------------------------------------------------------------------ */
/* Frames                                                                    */
/* ------------------------------------------------------------------------ */

static void write_frame(Gif *g, const uint32_t *px, unsigned delay_cs)
{
    const int n = g->w * g->h;
    uint8_t *idx = malloc((size_t)n);
    if (!idx) { g->failed = true; return; }
    /* the frame's colours: exact when there are at most 256 */
    uint32_t pal[256];
    int npal = 0;
    bool exact = true;
    for (int i = 0; i < n && exact; i++) {
        const uint32_t c = px[i] & 0xffffff;
        int k = 0;
        /* most frames have few colours: a short search from the last match */
        static int last;
        if (last < npal && pal[last] == c) k = last;
        else { for (k = 0; k < npal && pal[k] != c; k++) {} }
        if (k == npal) {
            if (npal == 256) { exact = false; break; }
            pal[npal++] = c;
        }
        last = k;
        idx[i] = (uint8_t)k;
    }
    if (!exact) {
        npal = 256;
        for (int k = 0; k < 256; k++)
            pal[k] = (uint32_t)((k >> 5) * 255 / 7) << 16 | (uint32_t)(((k >> 2) & 7) * 255 / 7) << 8 | (uint32_t)((k & 3) * 255 / 3);
        for (int i = 0; i < n; i++) {
            const uint32_t c = px[i];
            idx[i] = (uint8_t)(((c >> 21) & 7) << 5 | ((c >> 13) & 7) << 2 | ((c >> 6) & 3));
        }
    }
    int bits = 1;
    while ((1 << bits) < npal) bits++;
    /* graphic control: the delay, no transparency, no disposal */
    put8(g, 0x21); put8(g, 0xF9); put8(g, 4); put8(g, 0x04); put16(g, delay_cs); put8(g, 0); put8(g, 0);
    /* image descriptor with a local colour table */
    put8(g, 0x2C); put16(g, 0); put16(g, 0); put16(g, (unsigned)g->w); put16(g, (unsigned)g->h);
    put8(g, 0x80 | (unsigned)(bits - 1));
    for (int k = 0; k < (1 << bits); k++) {
        const uint32_t c = k < npal ? pal[k] : 0;
        put8(g, c >> 16 & 255); put8(g, c >> 8 & 255); put8(g, c & 255);
    }
    lzw(g, idx, n);
    free(idx);
    g->frames++;
}

/* the pending frame goes out once it has been shown at least 2/100 s */
static void flush_pending(Gif *g, bool last)
{
    if (!g->has_pending) return;
    double cs = g->pending_s * 100.0 + g->carry_cs;
    if (cs < 2.0 && !last) return;
    if (cs < 2.0) cs = 2.0;
    unsigned d = (unsigned)(cs + 0.5);
    if (d > 65535) d = 65535;
    g->carry_cs = cs - d;
    write_frame(g, g->pending, d);
    g->has_pending = false;
}

void gif_frame(Gif *g, const uint32_t *pixels, double seconds)
{
    if (g->failed) return;
    const size_t bytes = (size_t)g->w * g->h * sizeof(uint32_t);
    if (g->has_pending) {
        g->pending_s += seconds;
        if (!memcmp(g->pending, pixels, bytes)) return;     /* the same picture: shown longer */
        flush_pending(g, false);
        if (g->has_pending) {
            /* too short to be a frame of its own: the newer picture replaces it */
            memcpy(g->pending, pixels, bytes);
            return;
        }
    }
    memcpy(g->pending, pixels, bytes);
    g->pending_s = 0;
    g->has_pending = true;
}

uint8_t *gif_end(Gif *g, size_t *size)
{
    if (g->has_pending && g->pending_s <= 0) g->pending_s = 0.1;
    flush_pending(g, true);
    put8(g, 0x3B);
    free(g->pending);
    g->pending = NULL;
    if (g->failed || !g->frames) {
        free(g->data);
        g->data = NULL;
        return NULL;
    }
    *size = g->size;
    uint8_t *d = g->data;
    g->data = NULL;
    return d;
}

void gif_abort(Gif *g)
{
    free(g->pending);
    free(g->data);
    memset(g, 0, sizeof(*g));
}
