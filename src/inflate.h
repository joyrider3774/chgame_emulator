/*
 * Raw deflate (RFC 1951) decoding, for the ZIP entries of a .chgame cart.
 * Small and self-contained, so the native and the web builds need no zlib.
 */
#ifndef CHG_INFLATE_H
#define CHG_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Decodes src into dst, which must hold the whole result: exactly dstlen
   bytes for a ZIP entry, whose size the directory gives. Returns 0 when the
   stream decoded to exactly dstlen bytes, -1 for a damaged stream or a size
   that does not match */
int chg_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

#endif
