/*
 * The buzzer, turned into samples.
 *
 * bus.c logs every change to what drives PB10 with the cycle it happened
 * on: a steady level, or a PWM wave from TIM1 described by its period, its
 * high time and where a period started. Each output sample is the average
 * of the pin over exactly the cycles that sample covers, worked out in
 * closed form, so a 20 kHz tone and a tone at a frequency that does not
 * divide the sample rate come out without aliasing and without stepping the
 * CPU a cycle at a time. A DC blocker then does what the piezo's AC coupling
 * does.
 */
#include <math.h>
#include <string.h>
#include "machine.h"
#include "audio.h"

void chg_audio_init(ChgAudio *a, int rate, uint64_t now)
{
    memset(a, 0, sizeof(*a));
    a->rate = rate;
    a->cycles_per_sample = (double)CHG_HCLK / rate;
    a->pos = (double)now;
    a->volume = 0.5f;
    a->state.cycle = now;
}

/* how long the pin spends at level 1 in [t0, t1) under state s */
static double high_time(const ChgBuzzState *s, double t0, double t1)
{
    if (!s->pwm)
        return s->level ? t1 - t0 : 0.0;
    const double P = s->period, H = s->high, o = (double)s->origin;
#define F(t) (floor(((t) - o) / P) * H + fmin(((t) - o) - P * floor(((t) - o) / P), H))
    const double activeTime = F(t1) - F(t0);
#undef F
    return s->level ? activeTime : (t1 - t0) - activeTime;
}

int chg_audio_render(ChgAudio *a, ChgMachine *m, float *out, int max)
{
    ChgBuzzer *b = &m->buzzer;
    int n = 0;
    const double end = (double)m->cycles;
    while (n < max && a->pos + a->cycles_per_sample <= end) {
        const double t0 = a->pos, t1 = t0 + a->cycles_per_sample;
        double high = 0.0, t = t0;
        /* every logged change that falls inside this sample splits it */
        while (b->tail != b->head && (double)b->log[b->tail].cycle < t1) {
            const ChgBuzzState *next = &b->log[b->tail];
            const double at = (double)next->cycle > t ? (double)next->cycle : t;
            high += high_time(&a->state, t, at);
            t = at;
            a->state = *next;
            b->tail = (b->tail + 1) % CHG_BUZZ_LOG;
        }
        high += high_time(&a->state, t, t1);
        const float level = (float)(high / a->cycles_per_sample);

        /* AC coupling: a first order high pass at about 20 Hz */
        a->dc += (level - a->dc) * (float)(2.0 * 3.14159265 * 20.0 / a->rate);
        out[n++] = (level - a->dc) * a->volume;
        a->pos = t1;
    }
    return n;
}

void chg_audio_skip(ChgAudio *a, ChgMachine *m)
{
    /* throws away what was logged without playing it, e.g. while fast forwarding */
    ChgBuzzer *b = &m->buzzer;
    while (b->tail != b->head) {
        a->state = b->log[b->tail];
        b->tail = (b->tail + 1) % CHG_BUZZ_LOG;
    }
    a->pos = (double)m->cycles;
}
