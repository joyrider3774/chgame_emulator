/*
 * Raw deflate (RFC 1951): stored, fixed Huffman and dynamic Huffman blocks.
 * Codes are decoded a bit at a time against canonical code counts, which is
 * slow next to a table decoder but more than fast enough for a cart's few
 * hundred kilobytes, read once when it is loaded.
 */
#include <string.h>
#include "inflate.h"

#define MAXBITS   15        /* the longest code */
#define MAXLCODES 286       /* literal/length codes */
#define MAXDCODES 30        /* distance codes */
#define FIXLCODES 288       /* the fixed table's literal/length codes */

typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outlen, outpos;
    int err;
} State;

/* a canonical Huffman code: how many codes of each length, and the symbols
   in code order */
typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} Huffman;

static int bits(State *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static void stored(State *s)
{
    /* the rest of the current byte is dropped, then LEN and its complement */
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) { s->err = 1; return; }
    const unsigned len = s->in[s->inpos] | s->in[s->inpos + 1] << 8;
    const unsigned nlen = s->in[s->inpos + 2] | s->in[s->inpos + 3] << 8;
    s->inpos += 4;
    if (len != (~nlen & 0xffffu) || s->inpos + len > s->inlen || s->outpos + len > s->outlen) {
        s->err = 1;
        return;
    }
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len;
    s->outpos += len;
}

/* a symbol, read a bit at a time: codes of each length follow the shorter
   ones in order (canonical codes), so the code is in range once it is less
   than the first code of the next length */
static int decode(State *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        const int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

/* the code from each symbol's length; 0 for a complete code, negative for
   an oversubscribed one, positive for an incomplete one (allowed for a
   single distance code) */
static int construct(Huffman *h, const short *length, int n)
{
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(State *s, const Huffman *lencode, const Huffman *distcode)
{
    for (;;) {
        int sym = decode(s, lencode);
        if (s->err) return;
        if (sym < 256) {
            if (s->outpos >= s->outlen) { s->err = 1; return; }
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return; }
            const size_t len = (size_t)(lbase[sym] + bits(s, lext[sym]));
            const int dsym = decode(s, distcode);
            if (s->err || dsym < 0 || dsym >= 30) { s->err = 1; return; }
            const size_t dist = (size_t)(dbase[dsym] + bits(s, dext[dsym]));
            if (s->err || dist > s->outpos || s->outpos + len > s->outlen) { s->err = 1; return; }
            /* byte by byte: a match may overlap what it copies */
            for (size_t i = 0; i < len; i++, s->outpos++)
                s->out[s->outpos] = s->out[s->outpos - dist];
        }
    }
}

static void fixed(State *s)
{
    static Huffman lencode, distcode;
    static int built;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    codes(s, &lencode, &distcode);
}

static void dynamic(State *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[MAXLCODES + MAXDCODES];
    Huffman lencode, distcode;
    const int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) { s->err = 1; return; }
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err || construct(&lencode, lengths, 19) != 0) { s->err = 1; return; }

    for (index = 0; index < nlen + ndist; ) {
        int sym = decode(s, &lencode);
        if (s->err) return;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            short len = 0;
            if (sym == 16) {
                if (index == 0) { s->err = 1; return; }
                len = lengths[index - 1];
                sym = 3 + bits(s, 2);
            } else if (sym == 17) {
                sym = 3 + bits(s, 3);
            } else {
                sym = 11 + bits(s, 7);
            }
            if (s->err || index + sym > nlen + ndist) { s->err = 1; return; }
            while (sym--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) { s->err = 1; return; }      /* no end-of-block code */
    const int lerr = construct(&lencode, lengths, nlen);
    if (lerr < 0 || (lerr > 0 && nlen - lencode.count[0] != 1)) { s->err = 1; return; }
    const int derr = construct(&distcode, lengths + nlen, ndist);
    if (derr < 0 || (derr > 0 && ndist - distcode.count[0] != 1)) { s->err = 1; return; }
    codes(s, &lencode, &distcode);
}

int chg_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    State s;
    memset(&s, 0, sizeof s);
    s.in = src;
    s.inlen = srclen;
    s.out = dst;
    s.outlen = dstlen;
    int last;
    do {
        last = bits(&s, 1);
        const int type = bits(&s, 2);
        if (s.err) return -1;
        switch (type) {
        case 0: stored(&s); break;
        case 1: fixed(&s); break;
        case 2: dynamic(&s); break;
        default: return -1;
        }
        if (s.err) return -1;
    } while (!last);
    return s.outpos == dstlen ? 0 : -1;
}
