#ifndef CHG_AUDIO_H
#define CHG_AUDIO_H

#include "machine.h"

typedef struct {
    int    rate;
    double cycles_per_sample;
    double pos;             /* the cycle the next sample starts at */
    ChgBuzzState state;     /* what drove the pin at 'pos' */
    float  dc;
    float  volume;
    bool   piezo;           /* the piezo's sound (piezo_filter.c), or the bare pin */
    float  fir[256];        /* the last samples, for the piezo filter */
    int    fir_pos;
} ChgAudio;

/* the piezo's response at 48 kHz, fitted to a recording (tools/fit_piezo.py) */
extern const int   chg_piezo_taps;
extern const float chg_piezo_fir[];

void chg_audio_init(ChgAudio *a, int rate, uint64_t now);
/* samples for the cycles run since the last call, at most max */
int  chg_audio_render(ChgAudio *a, ChgMachine *m, float *out, int max);
void chg_audio_skip(ChgAudio *a, ChgMachine *m);

#endif
