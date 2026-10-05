/*
 * An animated GIF being recorded in memory, frame by frame (gif.c).
 */
#ifndef CHG_GIF_H
#define CHG_GIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int       w, h;
    uint8_t  *data;         /* the file so far */
    size_t    size, cap;
    uint32_t *pending;      /* the last frame, written once its length is known */
    double    pending_s;    /* how long it has been shown */
    bool      has_pending;
    bool      failed;       /* out of memory */
    double    carry_cs;     /* rounding left over from earlier delays */
    int       frames;
} Gif;

/* starts a recording of w x h frames; false when out of memory */
bool     gif_begin(Gif *g, int w, int h);
/* the screen (XRGB8888, top left first) as it is now, shown for 'seconds'
   since the previous call; a frame equal to the last one lengthens it */
void     gif_frame(Gif *g, const uint32_t *pixels, double seconds);
/* finishes the file: *size bytes at the returned pointer, which the caller
   frees (free()); NULL when the recording failed */
uint8_t *gif_end(Gif *g, size_t *size);
/* drops a recording */
void     gif_abort(Gif *g);

#endif
