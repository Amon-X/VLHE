/*
 * effects.c - the reverb. See effects.h for the topology, the
 * parameters and the fixed-point table; this file is the arithmetic.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 */

#include <string.h>

#include "effects.h"

/* Freeverb's tunings at 44.1 kHz, left channel; right is +REV_SPREAD.
 * Scaled to the sample rate at init, as Freeverb's own derivations
 * do not (it was written for 44.1 kHz only); the ratios matter, the
 * absolute lengths do not. */
static const int comb_len[REV_COMBS] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
static const int ap_len[REV_ALLPASSES] = { 556, 441, 341, 225 };

#define Q13   8192L
#define Q10   1024L
#define Q15   32768L

/*
 * SHIFTS INSIDE A FEEDBACK LOOP MUST TRUNCATE TOWARD ZERO. An
 * arithmetic right shift floors, so a negative value in a loop with
 * gain 0.84 never reaches zero: -1 >> 13 is -1, and the loop settles
 * at whatever negative constant balances the bias - measured
 * 2026-09-08 as a -7 DC offset on the output that lasted for the
 * rest of the file, and read as a reverb tail that never ended.
 * Truncating toward zero makes |0.84 v| < |v| for every v, so the
 * only fixed point is zero and a tail decays to true silence, as
 * fluidsynth's does. The form below is what gcc 2.95.2 turns into
 * four branchless instructions (cltd; and; add; sar) - measured, not
 * assumed; `v / Q13' compiles to the same with a branch, and C89
 * leaves a negative quotient's direction implementation-defined
 * anyway. sizeof keeps it right for a 64-bit host long.
 */
#define SIGN_SHIFT  ((int) (sizeof(long) * 8 - 1))
#define TZ(v, bits) (((v) + (((v) >> SIGN_SHIFT) & ((1L << (bits)) - 1))) >> (bits))

void
reverb_init(reverb_state *st, int rate)
{
    int c, i;

    memset(st, 0, sizeof *st);
    for (c = 0; c < 2; c++) {
        for (i = 0; i < REV_COMBS; i++) {
            long n = ((long) comb_len[i] * rate) / 44100L + (c ? REV_SPREAD : 0);
            if (n > REV_COMB_MAX) n = REV_COMB_MAX;
            if (n < 1) n = 1;
            st->comb[c][i].len = (int) n;
        }
        for (i = 0; i < REV_ALLPASSES; i++) {
            long n = ((long) ap_len[i] * rate) / 44100L + (c ? REV_SPREAD : 0);
            if (n > REV_AP_MAX) n = REV_AP_MAX;
            if (n < 1) n = 1;
            st->ap[c][i].len = (int) n;
        }
    }
    /* fluidsynth's defaults through Freeverb's scalings - effects.h */
    st->feedback = (long) (0.896 * Q13 + 0.5);      /* 7340: room 0.7 */
    st->damp1    = (long) (0.08 * Q13 + 0.5);       /*  655 */
    st->damp2    = Q13 - st->damp1;                 /* 7537 */
    st->wet1     = (long) (0.9 * Q10 + 0.5);        /*  922 */
    st->wet2     = (long) (0.1 * Q10 + 0.5);        /*  102 */
    /*
     * The input gain carries fixedgain, the wet level and the x4 of
     * the internal scale - and a trim, MEASURED on spec test 17A
     * (instrument sends 333/666/1000) against fluidsynth 2.5.7, so
     * that "reverb level 0.7" means the same wetness here as there
     * and a font voiced against fluidsynth sounds the same. Two
     * measurements, 2026-09-08: at room 0.5 (feedback 0.84)
     * Freeverb's own scaling landed 1.4 dB UNDER fluidsynth, so the
     * trim was x1.175; at room 0.7 (0.896, the user's choice for a
     * longer tail, listen tests 19 and 20) the combs ring 3.8 dB
     * louder - a comb's steady gain is 1/(1 - feedback) - so the trim
     * is x1.175 / 1.55 = x0.759. Change the room and this number
     * moves; design/21 section 13 has the scores either way, and
     * tests/compare/wet.py re-measures it in a minute.
     */
    st->in_gain  = (long) (0.015 * 2.1 * 4 * 0.759 * Q15 + 0.5); /* 3134 */
}

/*
 * A lowpass-feedback comb, Freeverb's: the delayed sample is the
 * output; it is lowpassed into `store' and fed back scaled. All
 * values are <= 16 bits by the input bound, so Q13 products fit.
 */
static __inline__ long   /* per frame x 24: the calls were 8 % of the renderer on the Acer (design/20 10b) */
comb_step(rev_comb *cb, long in, long feedback, long damp1, long damp2)
{
    long out = cb->buf[cb->idx];

    cb->store = TZ(out * damp2 + cb->store * damp1, 13);
    cb->buf[cb->idx] = in + TZ(cb->store * feedback, 13);
    if (++cb->idx >= cb->len)
        cb->idx = 0;
    return out;
}

/* An allpass with feedback 0.5, which is a shift - toward zero, or
 * the allpass holds a -1 forever exactly as the comb would. */
static __inline__ long   /* per frame x 24: the calls were 8 % of the renderer on the Acer (design/20 10b) */
ap_step(rev_allpass *ap, long in)
{
    long bufout = ap->buf[ap->idx];
    long out    = bufout - in;

    ap->buf[ap->idx] = in + TZ(bufout, 1);
    if (++ap->idx >= ap->len)
        ap->idx = 0;
    return out;
}

void
reverb_frame(reverb_state *st, long in, long *out_l, long *out_r)
{
    long x, acc[2];
    int  c, i;

    /* THE ONE GUARD: the bus is bounded here, so every product below
     * is bounded by construction (effects.h's table). */
    if (in >  65535L) in =  65535L;
    if (in < -65535L) in = -65535L;
    x = (in * st->in_gain) >> 15;                   /* <= 2^13 */

    for (c = 0; c < 2; c++) {
        long sum = 0;
        for (i = 0; i < REV_COMBS; i++)
            sum += comb_step(&st->comb[c][i], x, st->feedback,
                             st->damp1, st->damp2);
        for (i = 0; i < REV_ALLPASSES; i++)
            sum = ap_step(&st->ap[c][i], sum);
        acc[c] = sum;
    }
    /* The width mix, Q10, and the two extra fractional bits out:
     * >> 10 for the coefficient, >> 2 for the x4 in the input gain. */
    *out_l = (acc[0] * st->wet1 + acc[1] * st->wet2) >> 12;
    *out_r = (acc[1] * st->wet1 + acc[0] * st->wet2) >> 12;
}

/*
 * THE CHORUS - effects.h has the shape and the widths.
 */

/* One turn of sine in 64 steps plus the wrap entry, Q14. */
#define CHO_SIN_N 64
static const short cho_sin[CHO_SIN_N + 1] = {
        0,   1606,   3196,   4756,   6270,   7723,   9102,  10394,
    11585,  12665,  13623,  14449,  15137,  15679,  16069,  16305,
    16384,  16305,  16069,  15679,  15137,  14449,  13623,  12665,
    11585,  10394,   9102,   7723,   6270,   4756,   3196,   1606,
        0,  -1606,  -3196,  -4756,  -6270,  -7723,  -9102, -10394,
   -11585, -12665, -13623, -14449, -15137, -15679, -16069, -16305,
   -16384, -16305, -16069, -15679, -15137, -14449, -13623, -12665,
   -11585, -10394,  -9102,  -7723,  -6270,  -4756,  -3196,  -1606,
        0
};

/* Q14 sine of a 32-bit phase: the top 6 bits index, the next 10
 * interpolate. */
static __inline__ long   /* per frame x 24: the calls were 8 % of the renderer on the Acer (design/20 10b) */
cho_sine(unsigned long phase)
{
    int  idx  = (int) ((phase >> 26) & 63);
    long frac = (long) ((phase >> 16) & 1023);
    long a = cho_sin[idx], b = cho_sin[idx + 1];

    return a + (((b - a) * frac) >> 10);
}

void
chorus_init(chorus_state *st, int rate)
{
    int i;
    long pp;

    memset(st, 0, sizeof *st);
    /* depth in samples, peak to peak, truncated as fluidsynth does,
     * then halved; the centre sits one interpolation sample beyond
     * it so the shortest delay is one sample. */
    pp = (4250L * (long) rate) / 1000000L;          /* 4.25 ms: 187 */
    if (pp > CHO_LINE - 2) pp = CHO_LINE - 2;
    st->depth  = pp / 2;                            /* 93 */
    st->centre = (st->depth + 1) << 12;             /* 94, Q12 */
    /* 0.2 Hz, advanced every CHO_MOD_RATE frames, 2^32 per turn:
     * 0.2 * 5 * 2^32 / rate = 97391 at 44.1 kHz. Floating point at
     * init only, as the filter's coefficients are; the frame path
     * is integer. */
    st->step = (unsigned long) ((4294967296.0 * 0.2 * CHO_MOD_RATE) / rate);
    for (i = 0; i < CHO_TAPS; i++) {
        /* phases 360/N degrees apart, as fluidsynth's */
        st->phase[i] = (unsigned long) ((4294967296.0 / CHO_TAPS) * i);
        st->dq[i] = st->centre - ((st->depth * cho_sine(st->phase[i])) >> 2);
    }
    st->tick = CHO_MOD_RATE;        /* the first frame uses dq[] as set */
    st->wet1 = (long) ( 1.1 * Q10 + 0.5);           /* 1126 */
    st->wet2 = (long) (-0.9 * Q10 - 0.5);           /* -922 */
}

void
chorus_frame(chorus_state *st, long in, long *out_l, long *out_r)
{
    long tap[CHO_TAPS], d0, d1;
    int  i;

    if (++st->tick >= CHO_MOD_RATE) {
        st->tick = 0;
        for (i = 0; i < CHO_TAPS; i++) {
            st->phase[i] = (st->phase[i] + st->step) & 0xffffffffUL;
            /* delay = centre - depth * sin, Q14 sine into Q12 */
            st->dq[i] = st->centre - ((st->depth * cho_sine(st->phase[i])) >> 2);
        }
    }

    /* Read before write, so a delay of one sample is the previous
     * frame. The fraction reaches toward the OLDER sample. */
    for (i = 0; i < CHO_TAPS; i++) {
        long dq   = st->dq[i];
        int  ia   = (st->in - (int) (dq >> 12)) & CHO_MASK;
        long frac = dq & 4095;
        long xa   = st->line[ia];
        long xb   = st->line[(ia - 1) & CHO_MASK];
        tap[i] = xa + (((xb - xa) * frac) >> 12);
    }

    if (in >  65535L) in =  65535L;
    if (in < -65535L) in = -65535L;
    st->line[st->in] = in;
    st->in = (st->in + 1) & CHO_MASK;

    /* fluidsynth's stereo unit for N = 3: taps 0 and 2 on one bus,
     * 1 and 2 on the other (the odd tap feeds both so the buses
     * balance), then the width mix. */
    d0 = tap[0] + tap[2];
    d1 = tap[1] + tap[2];
    *out_l = (d0 * st->wet1 + d1 * st->wet2) >> 10;
    *out_r = (d1 * st->wet1 + d0 * st->wet2) >> 10;
}

/*
 * ONE BLOCK OF REVERB - effects.h has why. The operations and their
 * order are reverb_frame's, unrolled over `n' frames: the input gain
 * and clamp per frame, then each comb across the whole block into a
 * running sum, then the four allpasses in series over that sum, then
 * the width mix. A comb's state and ring index live in locals for
 * the length of the block instead of being reloaded per frame.
 *
 * THE SUM BUFFER is the one addition: comb outputs must be summed
 * before the allpasses see them, and processing a comb at a time
 * means holding those sums. It is `n' longs per channel on the
 * stack, 2 KB at the daemon's 256-frame block.
 */
static void reverb_chunk(reverb_state *st, const long *in, long *out_l,
                         long *out_r, int n);

/*
 * THE CHUNK, 2026-09-10. reverb_block() below processes the block in
 * pieces of this many frames rather than all at once: one comb's
 * state is loaded, REV_CHUNK frames go by, and the next comb starts.
 * The question it tests is whether the sweeps over sum[] and x[] -
 * 26 of them per block at 256 frames - are what cost the block form
 * its measured regression on the Acer (run of 2026-09-09 23:32: the
 * renderer improved 54.2 -> 52.7 % while the daemon went from 14
 * seconds short to 34).
 *
 * MEASURE BEFORE BELIEVING EITHER WAY. The disassembly says the
 * inner loop spills the three coefficients and the ring length to
 * the stack and reloads them every iteration - 2.95.2 has seven
 * usable registers and the loop wants eleven live values - and that
 * spill is per ITERATION, so a smaller chunk does not remove it. If
 * that reading is right this changes nothing and costs a little more
 * per-pass setup. It is here because the reading is a reading.
 */
#define REV_CHUNK       32

void
reverb_block(reverb_state *st, const long *in, long *out_l, long *out_r, int n)
{
    int base;

    for (base = 0; base < n; base += REV_CHUNK) {
        int len = n - base;
        if (len > REV_CHUNK)
            len = REV_CHUNK;
        reverb_chunk(st, in + base, out_l + base, out_r + base, len);
    }
}

static void
reverb_chunk(reverb_state *st, const long *in, long *out_l, long *out_r, int n)
{
    long (*sum)[REV_BLOCK_MAX] = st->sum;
    long  *x = st->x;
    int    c, i, f;

    for (f = 0; f < n; f++) {
        long v = in[f];
        if (v >  65535L) v =  65535L;
        if (v < -65535L) v = -65535L;
        x[f] = (v * st->in_gain) >> 15;
        sum[0][f] = 0;
        sum[1][f] = 0;
    }

    for (c = 0; c < 2; c++) {
        for (i = 0; i < REV_COMBS; i++) {
            rev_comb *cb   = &st->comb[c][i];
            long     *buf  = cb->buf;
            long      store = cb->store;
            long      fb    = st->feedback;
            long      d1    = st->damp1, d2 = st->damp2;
            int       idx   = cb->idx, len = cb->len;

            for (f = 0; f < n; f++) {
                long out = buf[idx];
                store = TZ(out * d2 + store * d1, 13);
                buf[idx] = x[f] + TZ(store * fb, 13);
                if (++idx >= len)
                    idx = 0;
                sum[c][f] += out;
            }
            cb->store = store;
            cb->idx   = idx;
        }
        for (i = 0; i < REV_ALLPASSES; i++) {
            rev_allpass *ap  = &st->ap[c][i];
            long        *buf = ap->buf;
            int          idx = ap->idx, len = ap->len;

            for (f = 0; f < n; f++) {
                long bufout = buf[idx];
                long v      = sum[c][f];
                buf[idx] = v + TZ(bufout, 1);
                sum[c][f] = bufout - v;
                if (++idx >= len)
                    idx = 0;
            }
            ap->idx = idx;
        }
    }

    for (f = 0; f < n; f++) {
        out_l[f] += (sum[0][f] * st->wet1 + sum[1][f] * st->wet2) >> 12;
        out_r[f] += (sum[1][f] * st->wet1 + sum[0][f] * st->wet2) >> 12;
    }
}

/*
 * ONE BLOCK OF CHORUS - chorus_frame's operations in its order. The
 * LFO still moves every CHO_MOD_RATE frames, counted the same way,
 * and the line is still read before it is written, so a delay of one
 * sample is still the previous frame.
 */
void
chorus_block(chorus_state *st, const long *in, long *out_l, long *out_r, int n)
{
    long *line = st->line;
    int   pos  = st->in;
    int   f, i;

    for (f = 0; f < n; f++) {
        long tap[CHO_TAPS], d0, d1, v;

        if (++st->tick >= CHO_MOD_RATE) {
            st->tick = 0;
            for (i = 0; i < CHO_TAPS; i++) {
                st->phase[i] = (st->phase[i] + st->step) & 0xffffffffUL;
                st->dq[i] = st->centre
                          - ((st->depth * cho_sine(st->phase[i])) >> 2);
            }
        }

        for (i = 0; i < CHO_TAPS; i++) {
            long dq   = st->dq[i];
            int  ia   = (pos - (int) (dq >> 12)) & CHO_MASK;
            long frac = dq & 4095;
            long xa   = line[ia];
            long xb   = line[(ia - 1) & CHO_MASK];
            tap[i] = xa + (((xb - xa) * frac) >> 12);
        }

        v = in[f];
        if (v >  65535L) v =  65535L;
        if (v < -65535L) v = -65535L;
        line[pos] = v;
        pos = (pos + 1) & CHO_MASK;

        d0 = tap[0] + tap[2];
        d1 = tap[1] + tap[2];
        out_l[f] += (d0 * st->wet1 + d1 * st->wet2) >> 10;
        out_r[f] += (d1 * st->wet1 + d0 * st->wet2) >> 10;
    }
    st->in = pos;
}
