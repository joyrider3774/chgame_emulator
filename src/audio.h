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
} ChgAudio;

void chg_audio_init(ChgAudio *a, int rate, uint64_t now);
/* samples for the cycles run since the last call, at most max */
int  chg_audio_render(ChgAudio *a, ChgMachine *m, float *out, int max);
void chg_audio_skip(ChgAudio *a, ChgMachine *m);

#endif
