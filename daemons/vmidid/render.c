/*
 * render.c - the wavetable renderer. See render.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Integer only, C89, gcc 2.95.2 clean.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <limits.h>       /* LONG_MAX, for filter_out's word-size guard */

#include "render.h"
#include "centtab.h"
#include "modtab.h"

#define ENV_BITS   16
#define ENV_ONE    (1L << ENV_BITS)

enum { ST_DELAY, ST_ATTACK, ST_HOLD, ST_DECAY, ST_SUSTAIN, ST_RELEASE,
       ST_DONE };

/*
 * SF2 UNITS ARE LOGARITHMIC, AND THIS IS WHERE A RENDERER GOES WRONG
 * QUIETLY. Each conversion below is one line of arithmetic and one
 * paragraph of why, because getting any of them backwards produces
 * sound rather than silence - notes that play at the wrong pitch, or
 * at the wrong volume, in a way that is heard as "it sounds bad".
 */

/*
 * TIMECENTS TO FRAMES. t = 1200 * log2(seconds), so
 * seconds = 2^(t/1200). -12000 is 1 ms, 0 is 1 second.
 *
 * No powf: a 12-entry table for the octaves plus a linear interpolation
 * inside one, which is accurate to well under a millisecond at the
 * short end where it matters.
 */
static long
timecents_to_frames(int tc, int rate)
{
    long whole, frac, r;
    int  idx;

    if (tc <= -12000)
        return 0;                       /* the spec's "instant" */
    if (tc > 8000)
        tc = 8000;                      /* ~4.5 minutes; a sane ceiling */

    /*
     * INDEXED THROUGH cent_tab, NOT INTERPOLATED - the same fix as
     * cents_to_ratio got in 425ea9c, and for the same reason.
     *
     * This interpolated linearly across a WHOLE OCTAVE, which put the
     * error at 5% mid-interval, and then shifted down before scaling
     * by the rate so small values vanished entirely:
     *
     *     tc -10000 returned 0 where 137 frames is right
     *     tc  -8000 returned 344 where 434 is right, 21% low
     *
     * Envelope times are not pitches, so a few percent would be
     * tolerable - but returning ZERO for a 3 ms attack is not, and it
     * is the same shape of bug as the 4-cent pitch error: a straight
     * line under an exponential, always low.
     *
     * cent_tab is 2^(i/2048) in 12-bit fixed point. Timecents want
     * 2^(tc/1200), which is the same curve with a different
     * denominator, so the octave split is identical.
     *
     * MULTIPLY BY THE RATE BEFORE SHIFTING DOWN, so a small value is
     * not rounded to zero before it has been scaled. That ordering is
     * what the code before 425ea9c got wrong.
     *
     * BUT THE OCTAVE SHIFT MUST COME AFTER THE FRACBITS SHIFT, and
     * getting THAT wrong overflowed a 32-bit long. `r <<= whole' on
     * the full-precision product needs up to 29 + 6 = 35 bits at
     * tc 8000; a signed long has 31. An earlier comment here claimed
     * 29 bits and was counting only the multiply, not the shift that
     * follows it.
     *
     * IT ONLY BROKE ON THE TARGET. Modern gcc's long is 64-bit, so
     * the workstation renders were correct and every test passed;
     * gcc 2.95.2's is 32-bit, where timecents_to_frames(4985) - the
     * SC-55 piano's 17.8 s decay - returned -263390 instead of
     * 785186. A negative frame count makes env_decay_drop return its
     * "immediate" case, so on the real target every envelope longer
     * than about 12 s was wrong.
     *
     * Found by building through the Makefile with corelcc rather than
     * by hand with gcc, which is why mklistentest.sh does exactly
     * that. Two renders of the same commit differed by 10 dB.
     */
    whole = tc / 1200;
    frac  = tc % 1200;
    if (frac < 0) { frac += 1200; whole--; }

    idx = (int) (((frac * CENT_TAB_N) + 600) / 1200);
    if (idx >= CENT_TAB_N)
        idx = CENT_TAB_N - 1;

    r = (long) cent_tab[idx] * (long) rate;   /* still 12-bit scaled */

    if (whole > 15)  whole = 15;
    if (whole < -24) return 0;

    /*
     * Shift DOWN first, then up. RENDER_FRACBITS is 12 and `whole' is
     * at most 15, so the intermediate never exceeds the 29-bit
     * product and the result is identical to shifting the other way -
     * minus the overflow.
     */
    r >>= RENDER_FRACBITS;

    if (whole >= 0)
        r <<= whole;
    else
        r >>= -whole;

    return r;
}

/*
 * CENTIBELS OF ATTENUATION TO A LINEAR GAIN in 0..GAIN_ONE.
 *
 * cB is ATTENUATION, so larger means QUIETER - 0 cB is full volume,
 * not silence. Treating it as a level rather than an attenuation
 * inverts every instrument's balance.
 *
 * gain = 10^(-cb/200). A 41-entry table at 25 cB steps, interpolated,
 * covers 0 to 1000 cB (100 dB) which is beyond audibility.
 */
/*
 * CENTIBELS OF ATTENUATION TO A LINEAR GAIN in 0..GAIN_ONE.
 *
 * gain = 10^(-cb/200), so 200 cB is a tenth of the amplitude and
 * 0 cB is unity. cB is ATTENUATION - larger is QUIETER.
 *
 * THIS WAS WRONG IN TWO COMPOUNDING WAYS, fixed 2026-09-04 after the
 * user asked why our output was quieter than fluidsynth's:
 *
 *   - the table's step is 2.5 cB, spanning 0..100 cB, but it was
 *     indexed as though the step were 25 cB, i.e. ten times too
 *     coarse;
 *   - and above 200 cB a "decade" was divided out on top, even though
 *     the table already covered that range.
 *
 * The result was an EIGHTFOLD CLIFF between 180 cB (gain 832) and
 * 200 cB (gain 102) for a 2 dB step. Every instrument attenuated
 * above 200 cB - the hi-hats, ride, bass and both guitars - played at
 * roughly a quarter of its correct level, while the kick, snare and
 * crash, which sit below that line, were unaffected. That is why the
 * difference against fluidsynth was NOT a uniform gain: three sounds
 * were louder and the rest were quiet.
 *
 * Rewritten as an explicit decade split with a 10-entry table over
 * one decade, which has no cliff and no recursion.
 */

/* 10^(-n/20) for n = 0..10 dB, in GAIN_ONE units (32768ths). One
 * decade, 1 dB steps. Was 1024ths until 2026-09-08 - see GAIN_BITS in
 * render.h for why the width moved. */
static const long db_table[11] = {
    32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362
};

static int
centibels_to_gain(int cb)
{
    int  decades, rem_cb, whole_db, frac;
    long a, b, gain;

    if (cb <= 0)
        return (int) GAIN_ONE;
    if (cb >= 1000)
        return 0;                       /* 100 dB down: silent */

    /* Whole decades of 200 cB (20 dB), then the remainder inside one. */
    decades = cb / 200;
    rem_cb  = cb % 200;

    whole_db = rem_cb / 10;             /* 0..19 dB */
    frac     = rem_cb % 10;             /* tenths of a dB */

    if (whole_db >= 10) {
        /* Second half of the decade: another factor of 10^(-1/2)...
         * handled by taking 10 dB out and scaling by 324/1024. */
        int hi = whole_db - 10;
        a = db_table[hi];
        b = db_table[hi + 1];
        gain = a + ((b - a) * frac) / 10;
        gain = (gain * db_table[10]) / GAIN_ONE;   /* the 10 dB already removed */
    } else {
        a = db_table[whole_db];
        b = db_table[whole_db + 1];
        gain = a + ((b - a) * frac) / 10;
    }

    /*
     * ONE ROUNDED DIVISION, NOT A DIVISION PER DECADE. `gain /= 10'
     * repeated truncates at every step: at 847 cB the chain ran
     * 19101 -> 1910 -> 191 -> 19 -> 1, a factor of two low, and the
     * spec test's 13D read silent at velocity 15 where fluidsynth
     * reads -83 dB (2026-09-08). cb < 1000 here, so at most four.
     */
    {
        static const long pow10[5] = { 1L, 10L, 100L, 1000L, 10000L };
        long d = pow10[decades];
        gain = (gain + d / 2) / d;
    }
    return (int) gain;
}

/*
 * SCALE A SAMPLE BY A GAIN WITHOUT LEAVING 31 BITS. `x' can be 17
 * bits after the filter and `g' is GAIN_BITS wide, so the plain
 * product does not fit the target's long.
 *
 * ONE PRODUCT WHEN THE SAMPLE FITS 16 BITS, which is every unfiltered
 * voice and nearly every filtered frame - 16 + 15 = 31 fits - and the
 * envelope's bit-8 split only when it does not. The branch is one
 * compare per channel per frame and is almost never taken.
 *
 * TWO OTHER FORMS WERE MEASURED AND REJECTED, 2026-09-08, on MT32
 * gmstriving: the split on EVERY frame costs +12% (4.02 -> 4.51 s) in
 * the loop design/20 spent an audit on, and it is not exact either -
 * its two floors truncate the same way on every voice, and summed
 * over a dense mix that bias reached a 29 LSB peak, -52 dB RMS,
 * against this form's single product; dropping two low bits of the
 * sample first costs +1% and has the same kind of bias (31 LSB peak).
 * This form costs +3% and the product is exact wherever the plain
 * branch is taken, i.e. wherever the sample fits 16 bits - and
 * ROUNDED there, half added before the shift, so the floor's own
 * half-LSB bias is gone too. 32767 * 32768 + 16384 still fits.
 */
#define GAIN_SCALE(x, g) \
    (((x) >= -32768L && (x) <= 32767L) \
        ? (((x) * (g) + (1L << (GAIN_BITS - 1))) >> GAIN_BITS) \
        : (((((x) >> 8) * (g)) >> (GAIN_BITS - 8)) + ((((x) & 0xff) * (g)) >> GAIN_BITS)))


/*
 * CENTS TO A PITCH RATIO in RENDER_FRACBITS fixed point.
 *
 * ratio = 2^(cents/1200). Split into octaves (a shift) and the
 * remainder from a 25-entry table at 50-cent steps, interpolated -
 * so the worst case is a fraction of a cent, well under the 0.59
 * cents the pointer's own precision costs.
 */
/*
 * CENTS TO A PITCH RATIO in RENDER_FRACBITS fixed point.
 *
 * INDEXED, NOT INTERPOLATED - which is the whole change here.
 *
 * This used a 25-entry table at 50-CENT steps with linear
 * interpolation between them, and a straight line under an
 * exponential always falls BELOW it: the error was a consistent flat
 * bias, up to 4 cents mid-interval. Measured on jazz.mid, key 81 of
 * program 56 played at 1755.0 Hz where fluidsynth played it at
 * 1759.0 - 3.94 cents, and the step confirmed it exactly (2445 where
 * 2450.67 is right).
 *
 * A listener who knows the piece heard it as "something different
 * around 45 to 50 seconds". Every counter said the note was fine,
 * because it WAS fine - just slightly flat, on a sample with a
 * 30-frame loop that repeats 900 times a second, so the mistuning
 * beats against itself rather than averaging away.
 *
 * NEITHER REFERENCE INTERPOLATES. fluidsynth tabulates one entry per
 * cent and indexes by cents % 1200 (fluid_conv.c:100); TiMidity uses
 * bend_coarse[128] x bend_fine[256] (tables.c:177, :244). Both make
 * the table fine enough to index directly. centtab.h now does the
 * same at 0.59 cents.
 */
/*
 * __inline__ ON THE PER-FRAME HELPERS, and why the keyword is there.
 *
 * GCC 2.95.2 AT -O2 INLINES NOTHING. Automatic inlining of static
 * functions is -finline-functions, which 2.95 turns on only at -O3 -
 * so with the Makefile's flags every one of these was a real call
 * per voice per frame: addl/pushl/call/addl around a callee with its
 * own prologue and epilogue. env_step and menv_step are too large for
 * even -O3's heuristic and stay calls there too.
 *
 * design/20-synth-audit.md section 1a counted them in the target's
 * own listing and measured the fix: the keyword on these six renders
 * 24% faster with BIT-IDENTICAL output, and beats -O3, whose loop
 * unrolling makes render_mix slower. __inline__ (not inline) because
 * -ansi reserves the plain spelling.
 *
 * Marked: cents_to_ratio, lfo_value, filter_run, menv_step,
 * env_decay_step, env_step.
 */
static __inline__ long
cents_to_ratio(int cents)
{
    int  oct, rem, idx;
    long r;

    oct = cents / 1200;
    rem = cents % 1200;
    if (rem < 0) { rem += 1200; oct--; }

    /*
     * rem is 0..1199; the table spans an octave in CENT_TAB_N steps.
     * Worst case here is 1199 * 2048 = 22 bits - nowhere near an
     * overflow, unlike three other places in this file.
     *
     * ROUNDED, NOT TRUNCATED. Truncating always picks the entry at or
     * below the wanted pitch, which is a SYSTEMATIC flat bias of half
     * a step - about 0.29 cents on average, and it showed as every
     * single comparison against fluidsynth's curve coming out
     * negative. Adding half a step first makes the error symmetric
     * and halves its worst case, 0.55 cents to 0.26.
     *
     * A bias that always points one way is worth more attention than
     * its size suggests: it is the same shape of mistake as the
     * interpolation this table replaced.
     */
    idx = (int) ((((long) rem * CENT_TAB_N) + 600) / 1200);
    if (idx >= CENT_TAB_N)
        idx = CENT_TAB_N - 1;
    r = (long) cent_tab[idx];

    if (oct > 0) {
        if (oct > 12) oct = 12;
        r <<= oct;
    } else if (oct < 0) {
        if (oct < -12) return 1;
        r >>= -oct;
    }
    return r > 0 ? r : 1;
}

/*
 * ABSOLUTE CENTS TO AN LFO PHASE STEP.
 *
 * freqModLFO and freqVibLFO are cents above 8.176 Hz, so
 * Hz = 8.176 * 2^(c/1200), and the default of 0 is 8.176 Hz - NOT
 * zero. A renderer that treats 0 as "no LFO" silences the vibrato on
 * every instrument that relies on the default, which is most of them.
 *
 * The step is how much phase to advance per frame, in LFO_PHASEBITS
 * fixed point: step = Hz * LFO_PHASE_ONE / rate.
 */
static long
abscents_to_lfo_step(int cents, int rate)
{
    long ratio, base, step, ceiling;

    /*
     * step = 8.176 Hz * 2^(cents/1200) * LFO_PHASE_ONE / rate. The
     * 8.176 * LFO_PHASE_ONE / rate part is one division per note
     * (137169477 / rate, 3110 at 44.1 kHz), then times the pitch
     * ratio from the same table the pitch path uses, in
     * RENDER_FRACBITS. No 1/256 Hz intermediate any more: that
     * quantised 0.1 Hz to 0.098 and, with the 16-bit phase, every
     * rate under 0.67 Hz to 0.67 - render.h on the widening. The
     * old form's hz_x256 * LFO_PHASE_ONE would also overflow 31 bits
     * at 24 phase bits for any rate above 1.6 Hz.
     */
    /*
     * CLAMPED BEFORE THE MULTIPLY - design/24 P3, design/54 D29
     * (2026-10-04). sf2gen.c does not range-check generators and the
     * modulator layer adds unclamped, so a broken font could ask for
     * 9600 cents (8400 at 22050), where base * ratio wrapped past 31
     * bits and the LFO STOPPED instead of hitting the 100 Hz ceiling
     * below. 4500 cents is 8.176 * 2^3.75 = 110 Hz, already over that
     * ceiling, so every value clamped here was capped to the ceiling
     * anyway - nothing that did not wrap sounds any different - and
     * at 11025 it is 12440 * 55141, well inside 31 bits.
     */
    if (cents > 4500)
        cents = 4500;
    ratio = cents_to_ratio(cents);
    base  = 137169477L / (long) rate;       /* 8.176 * 2^24 */
    step  = (base * ratio) >> RENDER_FRACBITS;

    /* A CEILING. 100 Hz is far above any musical LFO and stops a
     * malformed file spinning the phase fast enough to alias. */
    ceiling = (100L * LFO_PHASE_ONE) / (long) rate;
    if (step > ceiling)
        step = ceiling;
    return step > 0 ? step : 1;
}

/*
 * A TRIANGLE LFO, -32768..32767 from a 16-bit phase.
 *
 * The SF2 spec's LFOs are triangles. Rising for the first half of the
 * cycle, falling for the second; the arithmetic below is that with no
 * branch on the common path.
 */
static __inline__ long             /* see cents_to_ratio */
lfo_value(long phase)
{
    /* The top 16 bits of the phase: the triangle below is 16-bit
     * arithmetic and returns +/- 2^16 whatever the phase's width. */
    long p = (phase >> (LFO_PHASEBITS - LFO_VALBITS)) & ((1L << LFO_VALBITS) - 1);
    long q = (1L << LFO_VALBITS) / 4;

    /*
     * IT STARTS AT ZERO AND RISES, which is the spec's shape and not
     * what this first did.
     *
     * The obvious form - (p*4) - PHASE_ONE rising, mirrored falling -
     * is a triangle that begins at FULL NEGATIVE DEFLECTION. Every
     * note then starts maximally detuned (or attenuated) and swings
     * up from there, which on any instrument with vibrato is audibly
     * wrong from the first frame. The delay generators exist to let
     * modulation swell IN; a wave starting at its extreme defeats
     * them.
     *
     * So the phase is shifted a quarter cycle: 0 at phase 0, +1 at a
     * quarter, 0 at a half, -1 at three quarters.
     */
    if (p < q)                       /*  0 .. +1  */
        return p * 4;
    if (p < 3 * q)                   /* +1 .. -1  */
        return (2L << LFO_VALBITS) - (p * 4);
    return (p * 4) - (4L << LFO_VALBITS);   /* -1 .. 0 */
}

/*
 * SINE IN Q14, FOR THE PAN LAW.
 *
 * This served the biquad's coefficients too until the filter went
 * float; the cosine half went with it. What remains is the
 * constant-power pan, which is once per note-on and needs a
 * quarter-turn of sine at modest accuracy.
 *
 * omega = 2*pi*fc/rate, and it is bounded to (0, pi) - above Nyquist
 * the bilinear transform's warping makes the coefficients
 * meaningless, which is why fluidsynth clamps fres to the sample rate
 * too (fluid_voice.c:851).
 *
 * A TABLE OVER omega, NOT OVER CUTOFF, so the sample rate stays a
 * runtime parameter. Tabulating cents directly - as fluidsynth does -
 * bakes in 44100 and this renderer takes a rate.
 *
 * 33 entries over 0..pi is a step of about 5.6 degrees; linear
 * interpolation between them is accurate to better than 0.2% of full
 * scale, which for a filter coefficient is far below audibility.
 */
#define TRIG_N 33
static const short sin_tab[TRIG_N] = {
        0,  1606,  3196,  4756,  6270,  7723,  9102, 10394,
    11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305,
    16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665,
    11585, 10394,  9102,  7723,  6270,  4756,  3196,  1606,
        0
};

/* Q14 sine of a phase 0..TRIG_ONE mapping to 0..pi. */
#define TRIG_ONE 32768L

static long
q14_sin(long phase)
{
    long idx, frac, a, b;

    if (phase < 0)         phase = 0;
    if (phase > TRIG_ONE)  phase = TRIG_ONE;

    idx  = (phase * (TRIG_N - 1)) / TRIG_ONE;
    if (idx >= TRIG_N - 1)
        return sin_tab[TRIG_N - 1];
    frac = phase * (TRIG_N - 1) - idx * TRIG_ONE;

    a = sin_tab[idx];
    b = sin_tab[idx + 1];
    return a + ((b - a) * frac) / TRIG_ONE;
}

/*
 * COMPUTE THE BIQUAD COEFFICIENTS for a cutoff in absolute cents.
 *
 * A standard RBJ low-pass, the same form fluidsynth uses
 * (fluid_iir_filter_impl.cpp:84-115):
 *
 *     alpha = sin(w) / (2Q)
 *     a0 = 1 + alpha        a1 = -2cos(w)     a2 = 1 - alpha
 *     b1 = 1 - cos(w)       b0 = b2 = b1/2
 *
 * all divided by a0, and the b terms scaled by the gain compensation
 * below.
 *
 * THIS RUNS AT NOTE-ON AND ON A CUTOFF CHANGE, NOT PER SAMPLE, so
 * sin/cos/sqrt out of libm are affordable here in a way they would
 * never be in filter_run. That is also why the Q14 trig table no
 * longer serves this function - it still serves the pan law, which is
 * likewise once per note.
 */
static void
filter_coeffs(render_voice *v, long fc_cents, int rate)
{
    double hz, w, sw, cw, alpha, a0, gain, q;

    if (fc_cents < FILTER_FC_MIN) fc_cents = FILTER_FC_MIN;
    if (fc_cents > FILTER_FC_MAX) fc_cents = FILTER_FC_MAX;

    /*
     * fc in Hz: 8.176 * 2^(cents/1200), reusing the pitch table.
     *
     * THE MULTIPLY IS DONE IN DOUBLE, NOT IN A LONG, because
     * `2093L * cents_to_ratio(fc)' needs up to 35 bits and a signed
     * long has 31 on the target. At fc 13500 - the spec's default,
     * i.e. every zone that does not set a cutoff - it overflowed to
     * a NEGATIVE frequency, which produced coefficients around 5e-9
     * and a filter that output silence.
     *
     * Modern gcc's 64-bit long hid it completely: the workstation
     * renders were correct and the target's were not. Found by
     * rendering the same commit through both compilers.
     *
     * This function is already float and runs once per note-on, so
     * there is nothing to gain by keeping the product integral.
     */
    hz = 2093.0 * (double) cents_to_ratio((int) fc_cents)
         / (double) (1L << (RENDER_FRACBITS + 8));
    if (hz < 1.0)
        hz = 1.0;

    /*
     * w = 2*pi*fc/rate, clamped just below Nyquist: at or above it the
     * bilinear transform is degenerate, which is why fluidsynth clamps
     * fres to the sample rate too (fluid_voice.c:851).
     */
    w = 2.0 * 3.14159265358979323846 * hz / (double) rate;
    if (w > 0.9 * 3.14159265358979323846)
        w = 0.9 * 3.14159265358979323846;

    sw = sin(w);
    cw = cos(w);

    /* Q comes in as FILTER_QBITS fixed point; the filter is float. */
    q = (double) v->filter_q / (double) FILTER_QONE;
    if (q < 0.001)
        q = 0.001;

    alpha = sw / (2.0 * q);
    a0 = 1.0 + alpha;

    /*
     * THE GAIN COMPENSATION IS THE SPEC'S, NOT AN INVENTION.
     * SF2.01 page 59 asks for a reduction of half the resonance peak
     * height, i.e. multiply by sqrt(1/Q) - fluidsynth does exactly
     * this at fluid_iir_filter_impl.cpp:101. Without it a resonant
     * instrument is louder than a non-resonant one by the height of
     * its peak, which is the opposite of what the soundfont author
     * intended.
     */
    gain = sqrt(1.0 / q);
    if (gain > 1.0)
        gain = 1.0;

    v->f_a1 = (float) (-2.0 * cw / a0);
    v->f_a2 = (float) ((1.0 - alpha) / a0);
    v->f_b1 = (float) (((1.0 - cw) / a0) * gain);
    v->f_b0 = (float) (v->f_b1 / 2.0);

    v->f_last_fc = fc_cents;
}

/*
 * ONE SAMPLE THROUGH THE FILTER.
 *
 * Direct form I - it keeps the input history separately, so a
 * coefficient change mid-note does not produce the transient that
 * form II's shared state gives. The cutoff moves every frame under
 * modulation, so that matters here rather than being a nicety.
 *
 * FLOAT, AND THE ONE PLACE IN THE RENDERER THAT IS. See render.h for
 * the measurement behind it: fixed point cannot hold these
 * coefficients across the SF2 cutoff range at all, and float costs
 * 1.3x on the Acer for an operation that runs on a minority of
 * voices. No clamp is needed any more - a float biquad's state cannot
 * wrap the way an integer one does, and the ringing it clamped was an
 * artefact of the fixed-point scaling rather than of the filter.
 */
static __inline__ float            /* see cents_to_ratio */
filter_run(render_voice *v, float x)
{
    float y;

    y = v->f_b0 * x + v->f_b1 * v->f_x1 + v->f_b0 * v->f_x2
        - v->f_a1 * v->f_y1 - v->f_a2 * v->f_y2;

    v->f_x2 = v->f_x1; v->f_x1 = x;
    v->f_y2 = v->f_y1; v->f_y1 = y;
    return y;
}

/*
 * THE WAY BACK TO AN INTEGER - render.h has the decision and the
 * numbers. Adding 1.5 * 2^52 to a double puts the integer part of the
 * value in the low 32 bits of the mantissa, rounded to nearest by the
 * store; for |y| < 2^31 the low word read back as a signed long IS
 * the value (the 2^51 above it is a multiple of 2^32). One FADD and
 * a store, against the cast's FLDCW / FIST / FLDCW - two control-word
 * loads that drain a P6's floating-point pipeline on every filtered
 * frame. i386 is little-endian, so l[0] is the low word; the x87's
 * extended precision does not matter because the store to double is
 * where the rounding happens. Under RENDER_CONVERSION_CAST the cast
 * is used instead, to reproduce pre-2026-09-09 output.
 *
 * AND IT IS 32-BIT-ONLY - design/24 B3, fixed 2026-09-15. The trick
 * reads `l[0]', the LOW word of the double. On the target `long' is
 * 32 bits and that is the integer; on a 64-bit host `l[0]' is the
 * WHOLE double, so every filtered voice came back as the raw bit
 * pattern - D_INTRO through the SC-55 gave 654,300 clipped samples
 * and a peak of 32768, against 0 and 10609 from the corelcc build.
 *
 * That is a TOOLING bug, not a target one, and it is the worse kind:
 * the 64-bit build is the OVERFLOW AUDIT's instrument (its recipe is
 * in tests/compare/OVERFLOW-AUDIT.md, and it predates this function),
 * so the one tool for finding the seventh 33-bit bug before it is
 * heard was itself producing nonsense.
 *
 * The guard is on the WORD SIZE, not on a -D the recipe must
 * remember: sizeof(long) is not available to the preprocessor, so
 * LONG_MAX stands in for it. A build where long is not 32 bits takes
 * the cast, which is correct everywhere and merely slower - and
 * slowness on a workstation is not a cost.
 */
#if LONG_MAX > 2147483647L
#define RENDER_LONG_IS_64 1
#endif

static __inline__ long
filter_out(float y)
{
#if defined(RENDER_CONVERSION_CAST) || defined(RENDER_LONG_IS_64)
    return (long) y;
#else
    union { double d; long l[2]; } u;
    u.d = (double) y + 6755399441055744.0;
    return u.l[0];
#endif
}

/*
 * A CHANNEL'S POWER-ON STATE - what render_init gives every channel
 * and what a system reset (GM On, GS reset, XG reset) restores:
 * fluidsynth's fluid_channel_reset = init + init_ctrl(0). Channel 10
 * is the drum channel on bank 128; everything else program 0 on bank
 * 0; volume 100, pan 64, expression 127, the sound controllers
 * (CC 70-79) at 64 meaning "no change", bend centred with a 2
 * semitone range, no RPN or NRPN selected, no pressure.
 */
static void
channel_power_on(render_state *r, int i)
{
    render_channel *c = &r->chan[i];
    int k;

    memset(c, 0, sizeof *c);
    c->program    = 0;
    c->drum       = (i == RENDER_DRUM_CHAN);
    /* CHANNEL 9 IS PERCUSSION, and in a SoundFont that is BANK
     * 128, not a program on bank 0. The SC-55 has 8 kits there. */
    c->bank       = c->drum ? 128 : 0;
    c->volume     = 100;
    c->expression = 127;
    c->pan        = 64;
    c->modwheel   = 0;
    c->sustain    = 0;
    c->bend       = 0;
    c->bend_range = 2;
    c->rpn        = -1;
    c->nrpn_gen   = -1;
    c->log_font   = -1;
    c->log_preset = -1;
    c->cc[7]  = 100;
    c->cc[10] = 64;
    c->cc[11] = 127;
    for (k = 70; k <= 79; k++)
        c->cc[k] = 64;
    c->pressure = 0;
}

/*
 * MAKE ROOM FOR n VOICES - design/21 section 17. Only ever grows; the
 * new tail is zeroed, which is an inactive voice. Returns how many
 * there are room for, which is less than n only if realloc failed -
 * and then the old array is untouched and still valid.
 */
static int
voices_reserve(render_state *r, int n)
{
    render_voice *p;

    if (n <= r->nalloc)
        return n;
    p = (render_voice *) realloc(r->voices, (size_t) n * sizeof(*p));
    if (p == NULL)
        return r->nalloc;
    memset(p + r->nalloc, 0, (size_t) (n - r->nalloc) * sizeof(*p));
    r->voices = p;
    r->nalloc = n;
    return n;
}

void
render_free(render_state *r)
{
    if (r == NULL)
        return;
    free(r->voices);
    r->voices = NULL;
    r->nalloc = 0;
    r->max_voices = 0;
}

void
render_init(render_state *r, const sf2_file *sf, const short *pool,
            long pool_frames, int rate)
{
    int i;

    memset(r, 0, sizeof(*r));
    r->nfonts      = 0;
    render_add_font(r, sf, pool, pool_frames, 0);
    r->rate        = rate;
    /*
     * RENDER_DEFAULT_VOICES UNLESS A CALLER ASKS FOR MORE OR FEWER.
     * memset above has already zeroed the field, and a zero ceiling
     * would render silence - so this is what makes the default safe
     * rather than merely conventional. It is 64, which is what every
     * caller that never touched the ceiling got from the fixed array.
     */
    r->max_voices  = voices_reserve(r, RENDER_DEFAULT_VOICES);
    /* The AWE velocity-filter default is SF2_MOD_VF_AWE == 0, so the
     * memset already chose it; written out so the choice is visible. */
    r->modflags    = SF2_MOD_VF_AWE;
    r->effects     = 1;
    r->fx_reverb   = 1;
    r->fx_chorus   = 1;
    r->bank_style  = RENDER_BANK_GS;
    r->master      = RENDER_MASTER;
    reverb_init(&r->reverb, rate);
    chorus_init(&r->chorus, rate);

    for (i = 0; i < RENDER_CHANNELS; i++)
        channel_power_on(r, i);

}

int
render_add_font(render_state *r, const sf2_file *sf, const short *pool,
                long pool_frames, int bank_offset)
{
    render_font *f;

    if (r == NULL || sf == NULL || r->nfonts >= RENDER_MAX_FONTS)
        return -1;
    if (bank_offset < 0)   bank_offset = 0;
    if (bank_offset > 127) bank_offset = 127;
    f = &r->fonts[r->nfonts];
    f->sf          = sf;
    f->pool        = pool;
    f->pool_frames = pool_frames;
    f->bank_offset = bank_offset;
    return r->nfonts++;
}

/*
 * FIND A PRESET ACROSS THE STACKED FONTS - render.h on the shape.
 * Later fonts first; each is asked for (bank - its offset, program).
 * An exact hit returns at once. Otherwise the fallback is what it was
 * with one font: the SAME PROGRAM IN THE WRONG BANK, preferring the
 * font's bank 0 - better than silence, a font without the requested
 * variation should still make a sound - and now preferring the
 * later font too. Returns the preset index and the font in *font_out,
 * or -1.
 */
static int
find_preset_exact(const render_state *r, int bank, int program, int *font_out)
{
    int fi, i;

    for (fi = r->nfonts - 1; fi >= 0; fi--) {
        const sf2_file *sf = r->fonts[fi].sf;
        int want = bank - r->fonts[fi].bank_offset;
        for (i = 0; i < sf->npresets; i++)
            if ((int) sf->presets[i].preset == program &&
                (int) sf->presets[i].bank == want) {
                *font_out = fi;
                return i;
            }
    }
    return -1;
}

static int
find_preset(const render_state *r, int bank, int program, int *font_out)
{
    int fi, i, fallback = -1, fallback_font = -1, fallback_is_zero = 0;

    /*
     * A DRUM CHANNEL FALLS BACK WITHIN THE KITS, never to a melodic
     * program: fluidsynth's substitution order (fluid_synth_program_
     * change) - the kit asked for, then that kit number in bank 128,
     * then kit 0 in bank 128. A missing variation kit plays the
     * standard kit, which is what every GS device does.
     */
    if (bank >= 128) {
        i = find_preset_exact(r, bank, program, font_out);
        if (i < 0) i = find_preset_exact(r, 128, program, font_out);
        if (i < 0) i = find_preset_exact(r, 128, 0, font_out);
        return i;
    }

    for (fi = r->nfonts - 1; fi >= 0; fi--) {
        const sf2_file *sf = r->fonts[fi].sf;
        int want = bank - r->fonts[fi].bank_offset;
        for (i = 0; i < sf->npresets; i++) {
            if ((int) sf->presets[i].preset != program)
                continue;
            if ((int) sf->presets[i].bank == want) {
                *font_out = fi;
                return i;
            }
            if (fallback < 0 || (!fallback_is_zero && sf->presets[i].bank == 0)) {
                fallback = i; fallback_font = fi;
                fallback_is_zero = (sf->presets[i].bank == 0);
            }
        }
    }
    *font_out = fallback_font;
    return fallback;
}

/*
 * PICK A VOICE. A free one, else the OLDEST - not the first found.
 *
 * Stealing the first slot means a held pad is cut every time a busy
 * passage needs a voice, while a note that started a moment ago
 * survives. Oldest-first is what a listener expects and is one
 * comparison per voice.
 */
static int
alloc_voice(render_state *r)
{
    int  i, best = -1;
    long best_prio = 0;

    for (i = 0; i < r->max_voices; i++)
        if (!r->voices[i].active)
            return i;

    /*
     * NOTHING FREE - GROW RATHER THAN STEAL, if asked.
     *
     * OFFLINE ONLY (render_set_grow). There is no deadline in a file
     * render, so the only reason to steal is the ceiling, and raising
     * it ends the render at the file's TRUE demand. render_set_grow()
     * reserved the whole array up front, so this is a bounds change
     * and allocates nothing - alloc_voice() never may, since a caller
     * can hold a voice pointer across it.
     *
     * The slot returned is the one just uncovered, which is inactive
     * by definition - every slot below max_voices was checked above.
     */
    if (r->grow && r->max_voices < r->nalloc) {
        i = r->max_voices;
        r->max_voices++;
        if (r->max_voices > r->grew_to)
            r->grew_to = r->max_voices;
        return i;
    }

    /*
     * SCORE THE CANDIDATES; DO NOT JUST TAKE THE OLDEST.
     *
     * Oldest-first ignores whether a voice is still SOUNDING. A note
     * in its release tail at -50 dB is a far better victim than one
     * that started three seconds ago and is still at full volume
     * under the sustain pedal - and on gmstriving.mid, which has 227
     * sustain events, oldest-first was killing exactly the wrong
     * ones.
     *
     * fluidsynth scores instead
     * (fluid_voice_get_overflow_prio, and the weights registered at
     * fluid_synth.c:270-274). Lowest score is killed. Its weights,
     * used here unchanged:
     *
     *     percussion  +4000   a drum hit is short and unmissable
     *     released    -2000   already in its tail: cheapest to lose
     *     sustained   -1000   only alive because the pedal is down
     *     age         +1000   scaled by how RECENTLY it started
     *     volume       +500   quiet voices are cheaper to lose
     *
     * THE AGE TERM IS INVERTED FROM WHAT ONE MIGHT EXPECT, and
     * fluidsynth's comment says why: newer voices score HIGHER, so
     * that "hitting a chord may result in killing notes belonging to
     * that very same chord" does not happen. Age protects the new,
     * not the old.
     */
    for (i = 0; i < r->max_voices; i++) {
        render_voice *v = &r->voices[i];
        long prio = 0;
        unsigned long age;

        if (r->chan[v->channel].drum)
            prio += 4000;
        else if (v->stage >= ST_RELEASE)
            prio -= 2000;
        else if (v->held)
            prio -= 1000;

        /*
         * age: how many note-ons ago this started. r->order only ever
         * increases, so this cannot wrap in any realistic run.
         * Scaled so a just-started voice scores near +1000 and an old
         * one near 0.
         */
        age = r->order - v->start_order;
        prio += 1000 / (long) (age + 1);

        /* volume: 0..500 by envelope level. */
        prio += (long) ((v->env_level * 500) >> ENV_BITS);

        if (best < 0 || prio < best_prio) {
            best_prio = prio;
            best = i;
        }
    }

    if (best < 0)
        best = 0;
    r->notes_stolen++;
    return best;
}

/*
 * THE PER-FRAME DECAY FACTOR for an exponential stage of n frames.
 *
 * A factor f with f^n = 2^-16, computed without pow: f = 1 - k/n
 * where k = ln(2^16) ~ 11.09. Returned as `drop', the amount BELOW
 * ENV_ONE, so the caller scales by (ENV_ONE - drop)/ENV_ONE each
 * frame.
 *
 * THE OLD FORM UNDERFLOWED TO ZERO ON LONG ENVELOPES.
 * `11 * ENV_ONE / n' loses its last bit once n passes 11*65536 =
 * 720896 frames - about 16.3 s at 44100 - and the guard then clamped
 * it to 1, which is not a rescue: 1/65536 per frame is a FIXED decay
 * speed with no relation to the time requested. Every envelope longer
 * than 16 s decayed at the same wrong rate.
 *
 * The SC-55's acoustic piano has decayVolEnv 4985 timecents = 17.8 s
 * and hit this exactly. Releases are shorter and did not, which is
 * why it lived in the decay where it was harder to hear.
 *
 * THE FIX KEEPS 32-BIT ARITHMETIC by returning a FINER-GRAINED drop
 * and letting the caller apply it, rather than by widening the type.
 * env_scale already multiplies in 16.16 with a split high/low
 * product, so the extra resolution costs nothing there: `drop' is in
 * ENV_SUBBITS extra bits, giving 11 * 2^24 / n, which stays exact out
 * to n = 184 million frames - 70 minutes, longer than any envelope a
 * soundfont can express.
 *
 * No long long, so the file stays C89 and check-gcc295 stays clean.
 *
 * Third integer-resolution bug in this renderer, after env_scale's
 * overflow and timecents_to_frames' downshift. All three silenced
 * notes rather than distorting them.
 */
#define ENV_SUBBITS 8
#define ENV_SUBONE  (1L << ENV_SUBBITS)

static long
env_decay_drop(long frames)
{
    long drop;

    if (frames <= 0)
        return ENV_ONE * ENV_SUBONE;    /* immediate */

    /* 11 * ENV_ONE * ENV_SUBONE / n, in a long: 11 * 2^24 = 2^27.5,
     * so the numerator fits with four bits to spare. */
    drop = (11L * ENV_ONE * ENV_SUBONE) / (frames + 1);
    if (drop < 1)
        drop = 1;
    if (drop > ENV_ONE * ENV_SUBONE)
        drop = ENV_ONE * ENV_SUBONE;
    return drop;
}

/* THE MODULATION ENVELOPE ENTERS RELEASE from wherever it is - split
 * out of voice_release() so the FAST mode's catch-up applies it at the
 * same point in the replay (design/54 D28). */
static void
menv_release(render_voice *v)
{
    if (v->menv_stage < ST_RELEASE) {
        v->menv_stage = ST_RELEASE;
        v->menv_count = v->menv_release_frames;
        v->menv_sub   = v->menv_level << ENV_SUBBITS;
        v->menv_rate  = v->menv_release_frames > 0
                      ? -(v->menv_sub / (v->menv_release_frames + 1))
                      : -(ENV_ONE << ENV_SUBBITS);
        if (v->menv_rate == 0)
            v->menv_rate = -1;
    }
}

static void
voice_release(render_voice *v)
{
    if (!v->active || v->stage >= ST_RELEASE)
        return;
    v->stage = ST_RELEASE;
    v->env_count = v->release_frames;

    /*
     * THE RELEASE IS EXPONENTIAL, NOT LINEAR, and the difference is
     * both audible and expensive.
     *
     * SF2 envelope times are the time to fall 100 dB, so the decay is
     * multiplicative. A LINEAR ramp over the same period stays
     * audible for its whole length - an exponential one is inaudible
     * (-40 dB) after about a THIRD of it. With the SC-55's 2174
     * timecent releases (3.6 s) that is the difference between a
     * voice occupying a slot for 1.2 s and for 3.6 s, and it showed
     * up as 182 stolen voices on gmstriving.mid that more polyphony
     * did not fix.
     *
     * Implemented as a per-frame multiply in 16.16. `env_rate' holds
     * the numerator of a fraction just below 1: the level is scaled
     * by rate/ENV_ONE each frame, so it approaches zero
     * geometrically. env_count still bounds it so a voice always
     * terminates.
     *
     * The starting level is wherever the envelope ACTUALLY is - a
     * note released during its attack falls from the level it
     * reached, rather than jumping to full volume first and clicking.
     */
    /* The modulation envelope releases with the note - unless FAST has
     * it frozen, when the release is only RECORDED, after how many
     * skipped frames, for menv_catch_up() to apply at that point
     * (design/54 D28). */
    if (v->menv_skipped > 0) {
        if (v->menv_rel_at < 0)
            v->menv_rel_at = v->menv_skipped;
    } else {
        menv_release(v);
    }

    v->env_rate = env_decay_drop(v->release_frames);
    v->env_sub  = v->env_level << ENV_SUBBITS;
}

static void
note_off(render_state *r, int chan, int key)
{
    int i;

    for (i = 0; i < r->nalloc; i++) {
        render_voice *v = &r->voices[i];
        if (!v->active || v->channel != chan || v->key != key)
            continue;
        if (v->stage >= ST_RELEASE)
            continue;
        if (r->chan[chan].sustain) {
            v->held = 1;                /* the pedal is down */
            continue;
        }
        voice_release(v);
    }
}

/*
 * START ONE VOICE for one already-resolved zone.
 *
 * Split out of note_on on 2026-09-04 so that a note matching several
 * zones starts several voices - see note_on below, and the header
 * comment on sf2_zone_find_all.
 */
/*
 * COMPUTE A VOICE'S LEFT AND RIGHT GAINS from its zone pan and the
 * channel's current CC 10.
 *
 * SPLIT OUT SO A SOUNDING VOICE CAN BE RE-PANNED. CC 10 used to be
 * stored and applied only at note-on, so a voice already playing kept
 * the pan it was born with: during a sweep the held notes stayed
 * where they started while new notes moved, and the image tore.
 * gmstriving sweeps channel 10 from hard left to centre over about a
 * second from 2.6 s, which is where the user heard it.
 *
 * fluidsynth re-runs every modulator on every sounding voice for any
 * CC (fluid_synth_modulate_voices_LOCAL, called from its CC handler
 * at fluid_synth.c:2205), so a live pan change reaches playing notes.
 * This is the same thing for the one modulator we implement.
 *
 * SF2.01 8.4.6 makes CC 10 a BIPOLAR LINEAR modulator onto the pan
 * generator with amount 500: 64 contributes nothing, the ends
 * contribute +/-500, and it ADDS to the zone's own pan so that a zone
 * panned hard one way and a channel panned hard the other cancel.
 *
 * The law itself is constant-power - a quarter-turn of sine - and was
 * checked against both references by sweeping CC 10 across its range:
 * our left-minus-right matches fluidsynth within 0.1 dB at every
 * position, with total power flat.
 */
static void
voice_set_pan(render_voice *v)
{
    /* zone_pan already carries CC10: default 8.4.6 is a modulator
     * onto the pan generator, summed at note-on and re-summed on the
     * event (step 3). The channel term this used to add was that
     * default hardwired; the suite's test 22 F exists to catch it. */
    long pan = (long) v->zone_pan;
    long ph_r, ph_l;

    if (pan < -500) pan = -500;
    if (pan >  500) pan =  500;

    ph_r = ((pan + 500) * (TRIG_ONE / 2)) / 1000;
    ph_l = (TRIG_ONE / 2) - ph_r;

    v->left_vol  = (int) (((long) v->gain * q14_sin(ph_l)) >> 14);
    v->right_vol = (int) (((long) v->gain * q14_sin(ph_r)) >> 14);

    if (v->left_vol  > GAIN_ONE) v->left_vol  = (int) GAIN_ONE;
    if (v->right_vol > GAIN_ONE) v->right_vol = (int) GAIN_ONE;
}

/*
 * HOLD AND DECAY TRACK THE KEYBOARD, SF2.01 8.1.3 gens 31, 32, 39, 40.
 *
 * "the degree, in timecents per KeyNumber units, to which the hold
 * time is DECREASED by increasing MIDI key number. The hold time at
 * key number 60 is always unchanged." So key 60 is the pivot and the
 * adjustment is (60 - key) * amount, added to the base time.
 *
 * An amount of 100 makes the time track the keyboard exactly: an
 * octave up halves it, because 12 semitones * 100 = 1200 timecents =
 * one halving. The spec's own worked example - hold -7973 (10 ms),
 * amount 50, key 36 - gives -7973 + 24*50 = -6773, which is 20 ms.
 * That example is the test below.
 *
 * A piano is why this exists: high notes decay fast and low notes ring
 * on, and without it every key holds for the same time, which sounds
 * mechanical in exactly the way a real instrument does not.
 *
 * The clamps are the spec's own (8.1.2): decay tops out at 8000
 * timecents and hold at 5000, and a hold at or below -32768 means no
 * hold at all. fluidsynth applies the same three at
 * fluid_voice.c:698-720.
 */
static int
keynum_scaled_tc(int base_tc, int per_key, int key, int is_decay)
{
    long tc;

    if (per_key == 0)
        return base_tc;

    tc = (long) base_tc + (long) (60 - key) * (long) per_key;

    if (is_decay) {
        if (tc > 8000L) tc = 8000L;
    } else {
        if (tc > 5000L) tc = 5000L;
        if (tc <= -32768L) return -32768;   /* no hold at all */
    }
    if (tc < -32768L) tc = -32768L;
    return (int) tc;
}

/* --- the modulator layer: sources, curves, and the note-on sum ----- */

/*
 * SF2.01 8.2: the source enumerator is index (bits 0-6), C (bit 7),
 * D (bit 8), P (bit 9) and type (bits 10-15). sf2mod.c validated the
 * indices and types at merge time, so nothing here has to.
 */
#define MSRC_INDEX(e)  ((int) ((e) & 0x7f))
#define MSRC_CC(e)     ((int) (((e) >> 7) & 1))
#define MSRC_D(e)      ((int) (((e) >> 8) & 1))
#define MSRC_P(e)      ((int) (((e) >> 9) & 1))
#define MSRC_TYPE(e)   ((int) (((e) >> 10) & 0x3f))

/*
 * A SOURCE'S CURRENT VALUE, 0..127. The note's own velocity and key
 * (after the keynum/velocity generator overrides), or the channel's
 * controller store; the 14-bit pitch wheel is scaled to seven bits
 * as modtab.h says a caller must.
 *
 * Pressure is tracked since step 3 - channel pressure per channel,
 * poly pressure per key, both in render_channel.
 */
static int
mod_source(const render_state *r, int chan, int key, int vel, unsigned e)
{
    const render_channel *c = &r->chan[chan];

    if (MSRC_CC(e))
        return c->cc[MSRC_INDEX(e)];
    switch (MSRC_INDEX(e)) {
    case 2:  return vel;
    case 3:  return key;
    case 10: return c->poly[key & 0x7f];
    case 13: return c->pressure;
    case 14: return (c->bend + 8192) >> 7;
    case 16: return c->bend_range;
    default: return 0;
    }
}

/*
 * DOES THIS MODULATOR LISTEN TO THAT SOURCE? Either of its two
 * sources, since the amount source moves the result just as the
 * primary does. `cc' says which palette the event is in; `index'
 * is the controller number, or the general-palette index (10 poly
 * pressure, 13 channel pressure, 14 pitch wheel, 16 sensitivity).
 */
static int
mod_listens(const sf2_mod *m, int cc, int index)
{
    return (MSRC_CC(m->src)     == cc && MSRC_INDEX(m->src)     == index)
        || (MSRC_CC(m->amt_src) == cc && MSRC_INDEX(m->amt_src) == index);
}

/*
 * MAP A SOURCE VALUE INTO -1..1, in MODTAB_ONE units - SF2.01 8.2.2
 * to 8.2.4 and the figures of 9.5.2.
 *
 * Unipolar: the curve of the type, and D inverts the OUTPUT -
 * MODTAB_ONE minus the curve, not the curve of 127 minus v. modtab.h
 * records the three times that was written the other way and the
 * 60 dB it cost each time; 8.4.1's 2.34 dB is the check.
 *
 * Bipolar: -1 at 0, 0 at 64, +1 at 127. Linear is (v - 64) / 64
 * exactly - fluidsynth's -1 + 2 * v / 128, which is what makes CC10
 * = 127 the suite's "49.22%" (500 x 63 / 64) rather than 50. The
 * curved types are applied to each half from the centre outward
 * (the spec's figures 6, 8, 10: mirrored about the midpoint, not
 * stretched). A bipolar switch is +1 or -1. D negates the whole.
 */
static long
mod_map(unsigned e, int v)
{
    long c;

    if (v < 0)   v = 0;
    if (v > 127) v = 127;

    if (!MSRC_P(e)) {
        switch (MSRC_TYPE(e)) {
        case 1:  c = modtab_concave[v]; break;
        case 2:  c = modtab_convex[v];  break;
        case 3:  c = modtab_switch(v);  break;
        default: c = modtab_linear[v];  break;
        }
        return MSRC_D(e) ? MODTAB_ONE - c : c;
    }
    {
        int up = v >= 64;
        int t  = up ? (v - 64) * 2 : (64 - v) * 2;      /* 0..128 outward */

        if (t > 127) t = 127;
        switch (MSRC_TYPE(e)) {
        case 1:  c = modtab_concave[t]; break;
        case 2:  c = modtab_convex[t];  break;
        case 3:  c = MODTAB_ONE;        break;
        default: c = ((long) (up ? v - 64 : 64 - v) * MODTAB_ONE) / 64; break;
        }
        if (!up)
            c = -c;
        return MSRC_D(e) ? -c : c;
    }
}

/*
 * ONE MODULATOR'S CONTRIBUTION, in the destination's units - the
 * spec's 9.5.1: Transform(amount * Map(source) * Map(amount source)).
 * Index 0 with C=0 is "no controller" and maps to 1, whatever its
 * other bits (8.2.1). Transform 2 is 2.04's absolute value; 0 is
 * linear; nothing else reaches here.
 *
 * amount is at most +/-32767 and a mapped value at most MODTAB_ONE,
 * so each product fits a 32-bit long with a bit to spare - checked
 * rather than assumed: 32767 * 65536 = 2147418112.
 */
static long
mod_eval(const render_state *r, int chan, int key, int vel, const sf2_mod *m)
{
    long v1, v2, x;

    v1 = (m->src & 0xff) == 0 ? MODTAB_ONE
       : mod_map(m->src, mod_source(r, chan, key, vel, m->src));
    v2 = (m->amt_src & 0xff) == 0 ? MODTAB_ONE
       : mod_map(m->amt_src, mod_source(r, chan, key, vel, m->amt_src));

    x = ((long) m->amount * v1) / MODTAB_ONE;
    x = (x * v2) / MODTAB_ONE;
    if (m->transform == 2 && x < 0)
        x = -x;
    return x;
}

/*
 * EVERY GENERATOR IS A REAL PARAMETER NOW, and every modulator onto
 * one is evaluated - the ten defaults included, since design/18 step
 * 3, and the two effect sends since design/21 step 1. Nothing is
 * special-cased in the mix loop; the suite's tests 20 B/C and 22 F
 * exist to catch a synth that hardwires a default.
 */

/*
 * THE DRUM GATE, `awe' ONLY - design/18 10k item 5, the user's
 * decision 2026-09-08.
 *
 * The AWE32 applied its velocity -> cutoff sweep only when
 * `bank != DRUM' (awe_wave.c:2083); the 2.01 spec's modulator model
 * cannot say that, so a default that copies the hardware's ramp
 * would still darken every drum hit on a kit that says nothing -
 * measured: 626 of 626 channel-9 voices on SC-88 DoomHangar. Under
 * `awe' the DEFAULT 8.4.2 row is skipped for a voice on bank 128. A
 * font's OWN velocity filter on a drum zone still applies, as it did
 * on the hardware; and `2.01' and `2.04' are the spec as printed,
 * which applies to every channel, so they are not gated. `none' has
 * no default to gate - fluidsynth's position.
 */
static int
mod_drum_gated(const render_state *r, int chan, const sf2_mod *m, int from)
{
    return (r->modflags & SF2_MOD_VF_MASK) == SF2_MOD_VF_AWE
        && from == SF2_MOD_FROM_DEFAULT
        && r->chan[chan].bank == 128
        && sf2_mod_is_velfilter(m);
}

/*
 * SUM THE ZONE'S MODULATORS INTO ITS GENERATOR VALUES. Returns the
 * initialAttenuation sum separately, unscaled, for the caller to add
 * after the generator's own x0.4. Everything else lands in z->gen[]
 * in the destination's units, clamped to the short it lives in.
 */
static long
mod_sum_zone(render_state *r, int chan, int key, int vel, sf2_zone *z)
{
    long sum[SF2_GEN_COUNT];
    long att = 0;
    int  i, d;

    memset(sum, 0, sizeof sum);
    for (i = 0; i < z->mods.n; i++) {
        const sf2_mod *m = &z->mods.mod[i];

        d = m->dest;
        if (d < 0 || d >= SF2_GEN_COUNT)
            continue;
        if (mod_drum_gated(r, chan, m, z->mods.from[i]))
            continue;
        r->mods_evaluated++;
        if (d == SF2_initialAttenuation)
            att += mod_eval(r, chan, key, vel, m);
        else
            sum[d] += mod_eval(r, chan, key, vel, m);
    }
    for (d = 0; d < SF2_GEN_COUNT; d++) {
        long v;

        if (sum[d] == 0)
            continue;
        v = (long) z->gen[d] + sum[d];
        if (v >  32767L) v =  32767L;
        if (v < -32768L) v = -32768L;
        z->gen[d] = (short) v;
    }
    return att;
}

/* --- the setters shared by note-on and the live path ---------------- */

/* The voice's gain from a total attenuation in centibels, then the
 * pan that derives left/right from it. */
static void
voice_set_gain(render_voice *v, long cb)
{
    if (cb > 32767L) cb = 32767L;
    if (cb < 0)      cb = 0;
    v->gain = centibels_to_gain((int) cb);
    voice_set_pan(v);
    /* The send products carry the gain; keep them in step. */
    v->send_reverb_gain = ((long) v->gain * v->send_reverb) >> GAIN_BITS;
    v->send_chorus_gain = ((long) v->gain * v->send_chorus) >> GAIN_BITS;
}

/*
 * The step from a pitch in cents. BOTH OPERANDS ARE SPLIT, and the
 * second split is design/24 B2 - fixed 2026-09-15.
 *
 * Dividing the ratio first was the 2026-09-05 audit's fix and it is
 * still right: `q * sample_rate' cannot overflow, because q is the
 * whole part of ratio/rate. The SECOND term was called safe and was
 * not. `rem' runs up to rate-1, so `rem * sample_rate' wraps a signed
 * 32-bit long once sample_rate passes 48695 at rate 44100, or 44739
 * at 48000 - and the wrapped product is a large negative, so `step'
 * clamps to 1 and the note plays at 1/4096 speed: a near-DC drone
 * where a note should be.
 *
 * MEASURED 2026-09-15, and the audit UNDERSTATED the trigger. It put
 * the first bad note 2.57 octaves above the sample's root, reasoning
 * from `rem' near its maximum; in fact `rem' is large at almost every
 * pitch, and for an 88200 Hz sample at 44100 the FIRST overflowing
 * note is ONE SEMITONE above the root. Such samples are real: the
 * 16.5mg GS/GM/MT32 bank has thirteen between 59516 and 89121 Hz
 * (Halo Pad, Atmosphere, Goblin, Ghost Theme, Organ 1, Reverse
 * Cymbal, three 808 hits, Pop Slap, Soft Crash) and Gus1live two, one
 * at 57190. `shdr' is not clamped when it is read (sf2.c:192), so the
 * font decides this number.
 *
 * The split: sq = sample_rate / rate, sr = sample_rate % rate, and
 *
 *     step = q * sample_rate + rem * sq + (rem * sr) / rate
 *
 * The only new product is `rem * sr', bounded by rate * rate, which
 * fits a signed 32-bit long while rate <= 46340. vmidid's -r ceiling
 * is lowered to that (vmidid.c), which costs nothing: no card here
 * runs above 48000 and the synth's own default is 44100. Checked
 * across 241 pitches x five rate pairs including 89121 Hz at both
 * 22050 and 44100: no term overflows.
 */
static void
voice_set_pitch(render_voice *v, long cents, int rate)
{
    long ratio, q, rem, sq, sr;

    if (cents < -32000L) cents = -32000L;
    if (cents >  32000L) cents =  32000L;
    ratio = cents_to_ratio((int) cents);
    q     = ratio / rate;
    rem   = ratio % rate;
    sq    = v->sample_rate / rate;
    sr    = v->sample_rate % rate;
    v->step = q * v->sample_rate + rem * sq + (rem * sr) / rate;
    if (v->step < 1)
        v->step = 1;
}

/*
 * initialFilterQ, centibels, to the filter's own fixed point.
 * fluidsynth's conversion (fluid_iir_filter.c:62-91): clamp to
 * 0..960 cB, subtract 3.01 dB, then 10^(dB/20). THE 3.01 IS NOT A
 * FUDGE: SF2.01 page 39 item 9 says Q of 0 dB should give NO
 * resonance hump - q_lin = 1/sqrt(2) - and 3.01 dB is exactly that.
 */
static void
voice_set_filter_q(render_voice *v, int qcb)
{
    int adj;

    if (qcb < 0)   qcb = 0;
    if (qcb > 960) qcb = 960;
    adj = qcb - 30;
    if (adj >= 0)
        v->filter_q = (FILTER_QONE * GAIN_ONE)
                    / (centibels_to_gain(adj) > 0
                       ? centibels_to_gain(adj) : 1);
    else
        v->filter_q = (FILTER_QONE * centibels_to_gain(-adj)) / GAIN_ONE;
    if (v->filter_q < 1)
        v->filter_q = 1;
}

/*
 * THE SENDS, from generator units (tenths of a percent, 0..1000 -
 * fluidsynth clips to the same range, fluid_voice.c:813-824) to
 * GAIN_ONE, and the gain-times-send products the mix loop reads.
 * Called with the voice's gain already set; voice_set_gain calls it
 * again when the gain moves, so the products never go stale.
 */
static void
voice_set_sends(render_voice *v, long rev, long cho)
{
    if (rev < 0)    rev = 0;
    if (rev > 1000) rev = 1000;
    if (cho < 0)    cho = 0;
    if (cho > 1000) cho = 1000;
    v->send_reverb = (int) ((rev * GAIN_ONE) / 1000L);
    v->send_chorus = (int) ((cho * GAIN_ONE) / 1000L);
    v->send_reverb_gain = ((long) v->gain * v->send_reverb) >> GAIN_BITS;
    v->send_chorus_gain = ((long) v->gain * v->send_chorus) >> GAIN_BITS;
}

/* Everything the voice's modulators contribute to one destination,
 * with the voice's own key and velocity and its channel as it stands. */
static long
voice_mod_sum(render_state *r, const render_voice *v, int dest)
{
    long sum = 0;
    int  i;

    for (i = 0; i < v->mods.n; i++)
        if (v->mods.mod[i].dest == dest) {
            if (mod_drum_gated(r, v->channel, &v->mods.mod[i], v->mods.from[i]))
                continue;
            sum += mod_eval(r, v->channel, v->key, v->velocity,
                            &v->mods.mod[i]);
            r->mods_evaluated++;
        }
    return sum;
}

/*
 * RE-APPLY ONE DESTINATION: base + the modulators' sum, through the
 * same setter note-on used. This is the apply table; a destination
 * not listed is note-on only (envelope times, sample offsets - a
 * stage already underway has no meaningful new length), and the two
 * sends join it when the effects stage exists.
 */
static void
voice_apply_dest(render_state *r, render_voice *v, int dest)
{
    long sum = voice_mod_sum(r, v, dest);
    long val = (long) v->gen_base[dest] + sum;

    switch (dest) {
    case SF2_initialAttenuation:
        voice_set_gain(v, v->att_base + sum);
        break;
    case SF2_pan:
        if (val < -500) val = -500;
        if (val >  500) val =  500;
        v->zone_pan = (int) val;
        voice_set_pan(v);
        break;
    case SF2_coarseTune:
    case SF2_fineTune:
        voice_set_pitch(v, v->pitch_base
                           + voice_mod_sum(r, v, SF2_coarseTune) * 100L
                           + voice_mod_sum(r, v, SF2_fineTune), r->rate);
        break;
    case SF2_initialFilterFc:
        if (val < FILTER_FC_MIN) val = FILTER_FC_MIN;
        if (val > FILTER_FC_MAX) val = FILTER_FC_MAX;
        v->fc_cents = val;
        /* A filter that was off because nothing had moved the cutoff
         * turns on the moment something does - test 15's CC1 sweep on
         * a zone that sets no cutoff of its own. */
        if (!v->filter_on && val < FILTER_FC_MAX) {
            v->filter_on = 1;
            voice_set_filter_q(v, v->gen_base[SF2_initialFilterQ]
                                  + (int) voice_mod_sum(r, v, SF2_initialFilterQ));
            v->f_x1 = v->f_x2 = v->f_y1 = v->f_y2 = 0;
            v->f_last_fc = -1;
        }
        break;
    case SF2_initialFilterQ:
        if (v->filter_on) {
            voice_set_filter_q(v, (int) val);
            v->f_last_fc = -1;          /* coefficients depend on Q */
        }
        break;
    case SF2_reverbEffectsSend:
    case SF2_chorusEffectsSend:
        /* The two rows design/18 reserved. One re-sum covers both:
         * CC91 and CC93 arrive separately but a send is cheap. */
        voice_set_sends(v, (long) v->gen_base[SF2_reverbEffectsSend]
                             + voice_mod_sum(r, v, SF2_reverbEffectsSend),
                           (long) v->gen_base[SF2_chorusEffectsSend]
                             + voice_mod_sum(r, v, SF2_chorusEffectsSend));
        break;
    case SF2_freqVibLFO:      v->vib_step = abscents_to_lfo_step((int) val, r->rate); break;
    case SF2_freqModLFO:      v->mod_step = abscents_to_lfo_step((int) val, r->rate); break;
    case SF2_vibLfoToPitch:   v->vib_to_pitch  = (int) val; break;
    case SF2_modLfoToPitch:   v->mod_to_pitch  = (int) val; break;
    case SF2_modLfoToVolume:  v->mod_to_volume = (int) val; break;
    case SF2_modLfoToFilterFc: v->mod_to_fc    = (int) val; break;
    case SF2_modEnvToFilterFc: v->menv_to_fc   = (int) val; break;
    case SF2_modEnvToPitch:   v->menv_to_pitch = (int) val; break;
    default:
        break;
    }
}

/*
 * A SOURCE MOVED: re-apply every destination this voice has a
 * modulator listening to it on - each destination once, however many
 * modulators share it (fluid_voice_modulate's updated-generator
 * bits, as a small array). `cc' and `index' as in mod_listens().
 */
static void
voice_modulate(render_state *r, render_voice *v, int cc, int index)
{
    unsigned char done[SF2_GEN_COUNT];
    int i, d;

    memset(done, 0, sizeof done);
    for (i = 0; i < v->mods.n; i++) {
        const sf2_mod *m = &v->mods.mod[i];

        if (!mod_listens(m, cc, index))
            continue;
        d = m->dest;
        if (d < 0 || d >= SF2_GEN_COUNT)
            continue;
        /* coarse and fine tune are one pitch: mark both */
        if (d == SF2_coarseTune || d == SF2_fineTune) {
            if (done[SF2_fineTune])
                continue;
            done[SF2_coarseTune] = done[SF2_fineTune] = 1;
        } else {
            if (done[d])
                continue;
            done[d] = 1;
        }
        voice_apply_dest(r, v, d);
    }
}

/* Every sounding voice on a channel, for a channel-wide source;
 * `key' >= 0 narrows it to the voices on that key (poly pressure). */
static void
channel_modulate(render_state *r, int chan, int cc, int index, int key)
{
    int i;

    for (i = 0; i < r->nalloc; i++) {
        render_voice *v = &r->voices[i];

        if (!v->active || v->channel != chan)
            continue;
        if (key >= 0 && v->key != key)
            continue;
        voice_modulate(r, v, cc, index);
    }
}

static void
note_on_zone(render_state *r, const render_font *f, int chan, int key,
             int vel, const sf2_zone *zp)
{
    sf2_zone      z;
    int           vi, i;
    render_voice *v;
    const sf2_sample *s;
    int           root, cents, atten;


    long          att_mod;          /* the modulators' centibels, kept
                                     * apart from the generator's -
                                     * see the x0.4 below */
    short         gen_base[SF2_GEN_COUNT];  /* the generators before
                                             * the modulators, for the
                                             * live path */

    z = *zp;

    /*
     * THE CHANNEL'S NRPN STATE, design/22 step 3: an AWE32 value
     * replaces the zone's generator, the spec's NRPN offset adds to
     * it. Onto the zone copy, before anything reads it, so gen_base
     * and everything downstream see the adjusted values.
     */
    {
        const render_channel *c = &r->chan[chan];
        for (i = 0; i < SF2_GEN_COUNT; i++) {
            long g = z.gen[i];
            if (c->awe_set[i])
                g = c->awe_val[i];
            g += c->nrpn_add[i];
            if (g < -32768L) g = -32768L;
            if (g >  32767L) g =  32767L;
            z.gen[i] = (short) g;
        }
    }

    /*
     * keynum AND velocity OVERRIDE THE NOTE, SF2.01 8.1.2 gens 46-47.
     *
     * "This enumerator forces the MIDI key number to effectively be
     * interpreted as the value given", and the same for velocity.
     * Both are instrument-level only and default to -1, which is the
     * spec's "not set" - key 0 and velocity 0 are legitimate values,
     * so a sentinel is needed rather than a zero test.
     *
     * APPLIED HERE, AFTER ZONE SELECTION, WHICH IS THE WHOLE POINT.
     * The zone was chosen by the key and velocity the player actually
     * sent, against keyRange and velRange; the override then replaces
     * them for everything downstream - pitch, the velocity-to-gain
     * curve, velocity-to-filter, and the voice's own record of what
     * it is playing. Overriding before selection would pick a
     * different zone and defeat the generator.
     *
     * A drum kit is the usual user: every key mapped to one sample
     * that must always sound at its own pitch, whatever key was
     * struck. The SC-55 uses keynum twice.
     *
     * THESE WERE LISTED AS IMPLEMENTED AND NEVER READ - the exact
     * failure the filter generators caused before, where the
     * load-time warning stays silent about something ignored. Found
     * 2026-09-04 by enumerating all 60 generator slots rather than
     * trusting that table.
     */
    if (z.gen[SF2_keynum] >= 0 && z.gen[SF2_keynum] <= 127)
        key = z.gen[SF2_keynum];
    if (z.gen[SF2_velocity] >= 0 && z.gen[SF2_velocity] <= 127)
        vel = z.gen[SF2_velocity];

    if (z.sample_index < 0 || z.sample_index >= f->sf->nsamples) {
        r->notes_unmapped++;
        return;
    }
    s = &f->sf->samples[z.sample_index];

    /*
     * THE MODULATOR LAYER - design/18. Every modulator the merge left
     * on this zone is evaluated ONCE, here, with the note's own
     * velocity and key (after the overrides above, which is why it
     * is here and not earlier) and the channel's controllers as they
     * stand, and its contribution is added to the zone's generator
     * value in the destination's own units. Everything derived below
     * - pitch, envelopes, cutoff, LFO depths - then reads the summed
     * value and knows nothing about modulators.
     *
     * Three destinations are NOT summed here:
     *
     *   initialAttenuation - kept in att_mod and added AFTER the x0.4
     *     the generator alone receives (the comment on that scaling
     *     is explicit that modulators are not scaled). This is the
     *     spec's 8.4.1 velocity curve and its 13A target.
     *   CC7, CC11, pitch bend, CC10 pan - "live": the per-frame
     *     channel code already applies them and a font altering the
     *     default's amount is not yet honoured (step 3 of design/18
     *     section 9 re-evaluates on controller change). Counted.
     *   reverb, chorus - no such destination exists. Counted.
     */
    memcpy(gen_base, z.gen, sizeof gen_base);   /* before the sum */
    att_mod = mod_sum_zone(r, chan, key, vel, &z);

    /*
     * EXCLUSIVE CLASS: a new note in the same class stops the others
     * on that channel. This is what keeps a closed hi-hat from
     * ringing under an open one - without it a drum kit smears.
     *
     * "RAPIDLY TERMINATED", NOT CUT DEAD. SF2.01 8.1.3 gen 57 says a
     * matching note "should be rapidly terminated", and this used to
     * set active = 0 - a sample-accurate hard cut with no tail at
     * all.
     *
     * THE USER HEARD IT AS MISSING DRUMS AND CYMBALS. bohemian's
     * closing section fires the closed hi-hat 11 times in 14 seconds,
     * and each one instantly erased whatever shared its class: the
     * open hi-hat on class 1, and the two crashes on class 4. The
     * cymbals were being started and then deleted before they could
     * sound.
     *
     * fluidsynth does not kill the voice either. It sets
     * GEN_VOLENVRELEASE to -2000 timecents - 315 ms - and lets the
     * envelope run (fluid_voice_kill_excl, fluid_voice.c:1385-1405),
     * and its comment records that -200 was tried first and found too
     * long "through listening tests with hi-hat samples". We use the
     * same figure for the same reason.
     *
     * The voice keeps its own release if that is already faster: a
     * short percussive tail should not be lengthened by being cut.
     */
    if (z.gen[SF2_exclusiveClass] != 0)
        for (i = 0; i < r->nalloc; i++)
            if (r->voices[i].active &&
                r->voices[i].channel == chan &&
                r->voices[i].exclusive_class == z.gen[SF2_exclusiveClass]) {
                long fast = timecents_to_frames(-2000, r->rate);

                r->voices[i].exclusive_class = 0;   /* not killed twice */
                if (r->voices[i].release_frames > fast)
                    r->voices[i].release_frames = fast;
                voice_release(&r->voices[i]);
            }

    vi = alloc_voice(r);
    v  = &r->voices[vi];
    memset(v, 0, sizeof(*v));

    v->channel  = chan;
    v->key      = key;
    v->velocity = vel;
    v->exclusive_class = z.gen[SF2_exclusiveClass];

    /* The live path's data: the list, and the generators before it. */
    v->mods = zp->mods;
    memcpy(v->gen_base, gen_base, sizeof v->gen_base);
    v->sample_rate = (long) s->sample_rate;

    /* THE OFFSETS ARE IN FRAMES, and the coarse ones are x 32768. */
    {
        long start = (long) s->start
                   + z.gen[SF2_startAddrsOffset]
                   + (long) z.gen[SF2_startAddrsCoarseOffset] * 32768L;
        long end   = (long) s->end
                   + z.gen[SF2_endAddrsOffset]
                   + (long) z.gen[SF2_endAddrsCoarseOffset] * 32768L;
        long ls    = (long) s->loop_start
                   + z.gen[SF2_startloopAddrsOffset]
                   + (long) z.gen[SF2_startloopAddrsCoarse] * 32768L;
        long le    = (long) s->loop_end
                   + z.gen[SF2_endloopAddrsOffset]
                   + (long) z.gen[SF2_endloopAddrsCoarse] * 32768L;

        if (start < 0) start = 0;
        if (end > f->pool_frames) end = f->pool_frames;
        if (end <= start) { r->notes_unmapped++; return; }

        v->data       = f->pool + start;
        v->len        = end - start;
        v->loop_start = ls - start;
        v->loop_end   = le - start;
        if (v->loop_start < 0) v->loop_start = 0;
        if (v->loop_end > v->len) v->loop_end = v->len;
        if (v->loop_end <= v->loop_start) {
            v->loop_start = 0;
            v->loop_end   = v->len;
        }
    }

    v->looping     = (z.gen[SF2_sampleModes] & 1) ? 1 : 0;
    v->loop_to_end = (z.gen[SF2_sampleModes] == 3) ? 1 : 0;

    /*
     * PITCH. overridingRootKey wins over the sample's own root; -1
     * means "not set", which is why it defaults to -1 rather than 0 -
     * key 0 is a legitimate root.
     */
    root = (z.gen[SF2_overridingRootKey] >= 0)
         ? z.gen[SF2_overridingRootKey]
         : (int) s->original_key;
    v->root_key = root;

    /*
     * scaleTuning is PERCENT PER SEMITONE: 100 is normal, 0 makes
     * every key the same pitch (which drum kits use deliberately).
     */
    /*
     * THE PITCH, from the generators alone into pitch_base, and with
     * the modulators - pitch bend is one, 8.4.10 onto fineTune - into
     * the step. A bend or sensitivity event re-applies from the base
     * (voice_apply_dest); nothing per frame. The divide-before-
     * multiply that a five-octaves-up note needs (the Timpani clunk
     * of gmstriving on 32-bit) lives in voice_set_pitch now.
     */
    v->pitch_base = (long) (key - root) * gen_base[SF2_scaleTuning]
                  + (long) gen_base[SF2_coarseTune] * 100L
                  + gen_base[SF2_fineTune]
                  + (int) s->correction;
    cents = (key - root) * z.gen[SF2_scaleTuning]
          + z.gen[SF2_coarseTune] * 100
          + z.gen[SF2_fineTune]
          + (int) s->correction;
    voice_set_pitch(v, cents, r->rate);
    v->pos  = 0;

    /*
     * INITIALATTENUATION IS SCALED BY 0.4, AND THE SPEC DOES NOT SAY SO.
     *
     * The specification is unambiguous (SFSPEC21.txt, generator 48):
     * "the attenuation, in centibels... a value of 60 indicates the
     * note will be played at 6 dB below full scale", i.e.
     * gain = 10^(-cb/200). This code implemented exactly that, and it
     * was measurably WRONG against every real synth.
     *
     * From the FluidSynth maintainer, discussion #1708: the generator
     * "has to be multiplied by 0.04 to get decibels instead of 0.1...
     * All soundfont synths do this because the original Sound
     * Blasters did that for some reason and it stayed." SoundFonts
     * are authored against that behaviour, so a renderer following
     * the document sounds progressively over-attenuated as the
     * generator's value rises.
     *
     * FOUR SOURCES AGREE, three of them derived independently -
     * see vmidi/refs/soundfont/README.md for the measurements. The
     * AWE driver's own calc_attenuation_adip (awe_parm.c) works out
     * to 0.0417 dB/unit, within 4% of the maintainer's 0.04, and
     * TiMidity reaches the same span by a different route.
     *
     * x 2 / 5 rather than a float: 0.4 exactly, in integers.
     *
     * THE SCALING BELONGS HERE AND NOWHERE ELSE. The maintainer is
     * explicit that it applies to this generator and NOT to
     * modulators, so centibels_to_gain stays spec-correct for
     * sustainVolEnv and modLfoToVolume. Moving this inside it would
     * be a new bug wearing a fix's clothes.
     */
    atten = z.gen[SF2_initialAttenuation] * 2 / 5;
    {
        int gain;
        /*
         * VELOCITY THROUGH THE MODULATOR LAYER NOW - 8.4.1, concave,
         * 960 cB, negative: att = 960 * (1 - concave[vel]), which is
         * the same curve as the `gain * vel * vel / 127^2' this used
         * to apply in the gain domain (modtab.h derives both from
         * 200 * log10(127^2 / vel^2) and checks them against the
         * suite's 2.34 dB). The difference is that a font can now
         * supersede it - 13B/13C/13D/13E of the spec test - and that
         * the layer's cB are added AFTER the x0.4 above, which the
         * comment on that line insists applies to the generator only.
         * Expect +/-1 LSB of gain against the pre-layer baseline from
         * the two roundings; nothing more.
         */
        v->att_base = atten;
        gain = 0;                       /* set by voice_set_gain below */

        /*
         * PAN: -500..500, 0 centred. THE DIVISOR IS 500, NOT 1000.
         *
         * Dividing by 1000 gave a CENTRED note half amplitude on each
         * channel - a 6 dB loss on every sound that is not hard
         * panned, which is most of them. Centred must be full on both
         * sides; hard over must be full on one and silent on the
         * other, which is what this gives after the clamp.
         */
        /*
         * A CONSTANT-POWER PAN LAW, sin() not linear.
         *
         * The linear form - (500-pan)/500 - puts BOTH channels at
         * unity when centred, so a centred note is 3 dB louder than
         * the same note panned hard over. Every soundfont is mostly
         * centred, so that is a broad 1.41x lift on the whole mix
         * against a reference that does it properly.
         *
         * fluidsynth tabulates sin(i * (pi/2) / 1000) over 1001
         * entries and indexes it at pan+500
         * (gentables/fluid_pan.cpp, fluid_conv.c:315-332), so centre
         * is sin(pi/4) = 0.707 per channel and the total POWER is
         * constant as a source moves across the image. That is the
         * standard law and the one a soundfont author mixes against.
         *
         * q14_sin is already here for the filter; its phase runs
         * 0..TRIG_ONE over 0..pi, so a quarter-turn is TRIG_ONE/4 and
         * the pan maps onto 0..TRIG_ONE/2.
         */
        /*
         * CC 10 ADDS TO THE ZONE'S PAN. It was stored and never used,
         * so every note played at whatever the SoundFont said and the
         * song's own stereo image was discarded - measured as a
         * side/mid ratio of 0.037 against fluidsynth's 0.265 on
         * jazz.mid, which pans channels from 11 to 120. Nearly mono
         * where the file is wide.
         *
         * SF2.01 section 8.4.6 makes CC 10 a BIPOLAR LINEAR modulator
         * onto the pan generator with amount 500, which fluidsynth
         * sets up verbatim at fluid_synth.c:443-456. Bipolar means 64
         * contributes nothing and the ends contribute +/-500, so:
         *
         *     (cc10 - 64) * 500 / 64
         *
         * added to the zone's own pan, then clamped. A zone that pans
         * hard and a channel that pans hard the other way cancel,
         * which is what "additive" means and why this is not simply
         * an override.
         */
        (void) gain;
        {
            int p = z.gen[SF2_pan];     /* the CC10 default is in it */
            if (p < -500) p = -500;
            if (p >  500) p =  500;
            v->zone_pan = p;
        }
        voice_set_gain(v, v->att_base + att_mod);
        /* The effect sends, generators 16 and 15 with their
         * modulators already summed in - design/21. */
        voice_set_sends(v, z.gen[SF2_reverbEffectsSend],
                           z.gen[SF2_chorusEffectsSend]);
    }

    v->delay_frames   = timecents_to_frames(z.gen[SF2_delayVolEnv],  r->rate);
    v->attack_frames  = timecents_to_frames(z.gen[SF2_attackVolEnv], r->rate);
    v->hold_frames    = timecents_to_frames(
        keynum_scaled_tc(z.gen[SF2_holdVolEnv],
                         z.gen[SF2_keynumToVolEnvHold], key, 0), r->rate);
    v->decay_frames   = timecents_to_frames(
        keynum_scaled_tc(z.gen[SF2_decayVolEnv],
                         z.gen[SF2_keynumToVolEnvDecay], key, 1), r->rate);
    /*
     * THE RELEASE HAS A FLOOR OF -7200 TIMECENTS, ABOUT 16 ms -
     * fluidsynth's FLUID_MIN_VOLENVRELEASE, applied before the rate
     * is computed (fluid_voice.c, GEN_VOLENVRELEASE). The spec's
     * default release is -12000, one millisecond, and a zone that
     * sets none gets it: a note-off then cut the voice dead in a
     * millisecond and a song that retriggers the same key with a
     * 5 ms gap - Relentless's channel 4, every note of the passage -
     * played a click and a hole at every retrigger, heard by the
     * user 2026-09-08 as "cutting off and coming back on". With the
     * floor the old note is still fading when the new one starts,
     * as it is in fluidsynth and was on the AWE32's hardware
     * envelopes. The modulation envelope keeps the spec's range, as
     * fluidsynth's does. design/22 section 12.
     */
    {
        int rel = z.gen[SF2_releaseVolEnv];
        if (rel < -7200)
            rel = -7200;
        v->release_frames = timecents_to_frames(rel, r->rate);
    }

    /*
     * sustainVolEnv IS LINEAR IN THE ENVELOPE, AND THE ENVELOPE IS AN
     * ATTENUATION. Both halves are needed and this got it wrong twice
     * in opposite directions.
     *
     *   - Originally: centibels_to_gain(cB), i.e. 10^(-cB/200). Gave
     *     -22.4 dB for cB 224, which happened to be close to right
     *     for the wrong reason.
     *   - Then (f86e3bb): a linear amplitude 1 - cB/1000, i.e.
     *     -2.2 dB. Nineteen dB too loud, and the reasoning was that
     *     fluid_voice.c:1083 does exactly that subtraction.
     *
     * IT DOES - but the value it produces is an ENVELOPE value, not
     * an amplitude, and fluidsynth then spends it as attenuation at
     * fluid_rvoice.c:55:
     *
     *     fluid_cb2amp(FLUID_PEAK_ATTENUATION * (1.0f - volenv_val))
     *
     * with FLUID_PEAK_ATTENUATION = 960 cB (fluid_conv_tables.h:34).
     * Its own comment at fluid_rvoice.c:610 says so plainly: "The
     * attack section ramps up linearly with amplitude. The other
     * sections use logarithmic scaling."
     *
     * So the conversion is BOTH steps:
     *
     *     v    = 1 - cB/1000            (linear, the spec's scale)
     *     att  = 960 * (1 - v)          (centibels of attenuation)
     *     gain = 10^(-att/200)
     *
     * which for cB 224 is 960 * 0.224 = 215 cB = -21.5 dB.
     *
     * MEASURED, not just derived: rendering the isolated slap bass
     * note through fluidsynth and reading its sustain plateau gives
     * -21.2 dB, against -21.5 predicted. The old centibel reading was
     * -22.4, within a dB of correct by coincidence - the two curves
     * cross near this value, which is why the error hid.
     *
     * TIMIDITY AGREES ON THE STRUCTURE and differs only in range.
     * sndfont.c:469 maps the centibel value linearly onto its 0..255
     * envelope scale, TO_VOLUME(level) = 255 - level*255/1000, and
     * that scale is logarithmic - tables.c:89, 2^((x/127-1)*6). Same
     * two steps; its span is 36 dB where fluidsynth's is 96, so it
     * reads cB 224 as -8.0 dB. fluidsynth's 960 cB is the spec's own
     * full-scale attenuation and is the one to follow.
     */
    {
        long pct = z.gen[SF2_sustainVolEnv];
        long att;
        if (pct < 0)    pct = 0;
        if (pct > 1000) pct = 1000;
        /* 960 cB of attenuation at full scale, spent proportionally. */
        att = (960L * pct) / 1000L;
        /* ENV_ONE is 2^16 and GAIN_ONE 2^15: a shift, not a product
         * that would need 31 bits. */
        v->sustain_level = (long) centibels_to_gain((int) att)
                           << (ENV_BITS - GAIN_BITS);
    }

    v->stage     = ST_DELAY;
    v->env_level = 0;
    v->env_count = v->delay_frames;

    /*
     * THE LFOs. Both start at phase zero with their own delay, so a
     * note does not begin already wobbling - the delay is what makes
     * vibrato swell in after the attack rather than being present
     * from the first frame.
     */
    v->vib_step  = abscents_to_lfo_step(z.gen[SF2_freqVibLFO], r->rate);
    v->mod_step  = abscents_to_lfo_step(z.gen[SF2_freqModLFO], r->rate);
    v->vib_delay = timecents_to_frames(z.gen[SF2_delayVibLFO], r->rate);
    v->mod_delay = timecents_to_frames(z.gen[SF2_delayModLFO], r->rate);
    v->vib_phase = 0;
    v->mod_phase = 0;
    v->vib_to_pitch  = z.gen[SF2_vibLfoToPitch];
    v->mod_to_pitch  = z.gen[SF2_modLfoToPitch];
    v->mod_to_volume = z.gen[SF2_modLfoToVolume];

    /*
     * THE MODULATION ENVELOPE, same six stages as the volume one but
     * driving pitch. Its filter destination is resolved and ignored -
     * there is no filter, and sf2_zone_warn says so.
     */
    v->menv_delay_frames   = timecents_to_frames(z.gen[SF2_delayModEnv],  r->rate);
    v->menv_attack_frames  = timecents_to_frames(z.gen[SF2_attackModEnv], r->rate);
    v->menv_hold_frames    = timecents_to_frames(
        keynum_scaled_tc(z.gen[SF2_holdModEnv],
                         z.gen[SF2_keynumToModEnvHold], key, 0), r->rate);
    v->menv_decay_frames   = timecents_to_frames(
        keynum_scaled_tc(z.gen[SF2_decayModEnv],
                         z.gen[SF2_keynumToModEnvDecay], key, 1), r->rate);
    v->menv_release_frames = timecents_to_frames(z.gen[SF2_releaseModEnv],r->rate);
    /*
     * sustainModEnv is a PERCENTAGE in tenths (1000 = 100% = fully
     * attenuated to zero), NOT centibels like the volume envelope's.
     * Using the centibel conversion here gives a sustain level that
     * is wrong by orders of magnitude and a pitch envelope that never
     * settles.
     */
    {
        int pct = z.gen[SF2_sustainModEnv];
        if (pct < 0)    pct = 0;
        if (pct > 1000) pct = 1000;
        v->menv_sustain_level = (ENV_ONE * (1000 - pct)) / 1000;
    }
    v->menv_to_pitch = z.gen[SF2_modEnvToPitch];

    /*
     * THE FILTER, and it is OFF unless the zone asks for it.
     *
     * The spec's default for initialFilterFc is 13500 cents - the
     * filter wide open, 19912 Hz - so a zone that sets nothing wants
     * no filtering at all. Running a biquad at the top of its range
     * on every voice would cost a per-frame multiply-add chain for a
     * response that is flat, with 64 voices.
     *
     * So: on only when the zone SET a cutoff, or when something
     * modulates it. z.set[] is exactly the distinction sf2gen.c
     * records for this purpose - "explicitly at the default" and
     * "not mentioned" are different here.
     */
    v->mod_to_fc  = z.gen[SF2_modLfoToFilterFc];
    v->menv_to_fc = z.gen[SF2_modEnvToFilterFc];
    /*
     * SF2.01 8.4.2: VELOCITY DARKENS A SOFT NOTE.
     *
     * Amount -2400, unipolar NEGATIVE - the source is inverted, so
     * the offset is -2400 * (1 - vel/127):
     *
     *     vel 127 ->     0 cents, the zone's own cutoff
     *     vel 100 ->  -510
     *     vel  64 -> -1191
     *     vel   1 -> -2381, two octaves down
     *
     * A hard-struck note keeps its brightness and a soft one loses
     * it, which is most of what makes a sampled instrument sound
     * DYNAMIC rather than merely quieter.
     *
     * fluidsynth's comment at fluid_synth.c:381-387 records a genuine
     * spec ambiguity here: the second source's polarity is documented
     * as negative, but a respected soundfont overrides it as positive
     * and fluidsynth follows the soundfont. We have no modulator
     * layer for a soundfont to override, so only the primary source
     * applies and the ambiguity does not arise.
     */
    /*
     * ... AND SINCE THE MODULATOR LAYER, THAT AMBIGUITY IS THE FONT'S
     * TO SETTLE. The velocity term is no longer computed here: 8.4.2
     * is a row in sf2mod.c's defaults, in whichever of the four forms
     * -F selected, already summed into z.gen[SF2_initialFilterFc] by
     * mod_sum_zone() - and a font's cancel, negation or alteration of
     * it has already been honoured by the merge. What is left here
     * is the zone's cutoff as the layer resolved it.
     */
    v->fc_cents   = z.gen[SF2_initialFilterFc];
    if (v->fc_cents < FILTER_FC_MIN) v->fc_cents = FILTER_FC_MIN;
    /*
     * AND THE GATE HAS TO ACCOUNT FOR VELOCITY NOW.
     *
     * It used to be "on only if the zone set a cutoff or something
     * modulates it", because the default of 13500 cents is the filter
     * wide open and running a biquad for a flat response is pure
     * cost.
     *
     * The velocity modulator breaks that: at velocity 64 it subtracts
     * 1191 cents, taking an unset zone from 19912 Hz to 8.9 kHz -
     * audible filtering that the old gate would have silently
     * discarded. So the filter is on whenever the cutoff has actually
     * MOVED off wide-open, whatever moved it.
     */
    v->filter_on  = (z.set[SF2_initialFilterFc] ||
                     v->mod_to_fc || v->menv_to_fc ||
                     v->fc_cents < FILTER_FC_MAX) ? 1 : 0;

    if (v->filter_on) {
        /*
         * initialFilterQ is centibels, and fluidsynth's conversion is
         * the one to copy (fluid_iir_filter.c:62-91): clamp to
         * 0..960 cB, subtract 3.01 dB, then 10^(dB/20).
         *
         * THE 3.01 IS NOT A FUDGE. SF2.01 page 39 item 9 says the
         * gain at cutoff may be below zero when zero is specified -
         * i.e. Q of 0 dB should give NO resonance hump, which is
         * q_lin = 1/sqrt(2) = 0.707, not 1.0. Subtracting 3.01 dB is
         * exactly that. Omitting it puts a 3 dB hump under every
         * note whose zone mentions the filter at all.
         */
        voice_set_filter_q(v, z.gen[SF2_initialFilterQ]);
        v->f_x1 = v->f_x2 = v->f_y1 = v->f_y2 = 0;
        v->f_last_fc = -1;              /* force the first computation */
        filter_coeffs(v, v->fc_cents, r->rate);
    }
    /*
     * THE BYPASS RULE, design/20 section 3b, only when asked for.
     * Nothing modulates the cutoff (a modulated one moves, and the
     * realtime path would have to switch the filter back on mid-
     * note); the Q is at or below the no-hump default, 2910 in Q12 -
     * centibels_to_gain(30) is 726/1024 = 0.709, one table rounding
     * above 0.7071, and a threshold at 0.7072 bypassed nothing - so
     * there is no gain compensation to lose; and the cutoff in Hz is
     * at or above half the output rate times the playback ratio, the
     * highest frequency the resampled sample can carry, transposition
     * included. Then the biquad would only add phase shift, and it is
     * skipped for the life of the voice. A later cutoff change (CC 74,
     * an NRPN) still turns it on: the SF2_initialFilterFc case in
     * voice_apply_dest does that for any voice whose filter is off.
     */
    if (v->filter_on && r->filter_bypass
        && !v->mod_to_fc && !v->menv_to_fc
        && v->filter_q <= 2910L) {
        double fc_hz  = 8.176 * pow(2.0, (double) v->fc_cents / 1200.0);
        double top_hz = 0.5 * (double) r->rate
                      * (double) v->step / (double) RENDER_FRACONE;
        if (fc_hz >= top_hz) {
            v->filter_on = 0;
            r->voices_bypassed++;
        }
    }
    v->menv_stage    = ST_DELAY;
    v->menv_level    = 0;
    v->menv_count    = v->menv_delay_frames;
    v->menv_skipped  = 0;               /* design/54 D28 */
    v->menv_rel_at   = -1;

    v->start_order = ++r->order;
    v->active    = 1;
    r->notes_started++;

    /* Every generator is resolved by here, so a trace prints what the
     * voice will actually play rather than what was asked for. */
    if (r->trace != NULL)
        r->trace(v, r->trace_arg);
}

/*
 * A NOTE CAN BE SEVERAL VOICES.
 *
 * SF2 stores a stereo sample as two instrument zones over the same
 * key range, one panned hard left and one hard right, and both sound.
 * This used to take the first zone only, which put every stereo
 * instrument in the left speaker - 38 of the SC-55's 142 sounding
 * presets are affected, measured at 64.5 dB of channel imbalance on
 * Choir Aahs where fluidsynth gives 1.7 and TiMidity 1.3.
 *
 * `notes_started' therefore counts VOICES and can exceed the number
 * of MIDI note-ons; `notes_unmapped' still counts notes that produced
 * nothing at all.
 */
static void
note_on(render_state *r, int chan, int key, int vel)
{
    sf2_zone zones[SF2_MAX_ZONES];
    int      pi, fi = -1, n, i;
    const render_font *f;

    pi = find_preset(r, r->chan[chan].bank, r->chan[chan].program, &fi);
    if (pi < 0 || fi < 0) { r->notes_unmapped++; return; }
    f = &r->fonts[fi];
    if (r->log != NULL &&
        (fi != r->chan[chan].log_font || pi != r->chan[chan].log_preset)) {
        r->chan[chan].log_font   = fi;
        r->chan[chan].log_preset = pi;
        fprintf(r->log, "render: ch%-2d bank %3d prog %3d -> font %d \"%s\" preset %d/%d \"%s\"\n",
                chan + 1, r->chan[chan].bank, r->chan[chan].program, fi,
                f->sf->name, (int) f->sf->presets[pi].bank,
                (int) f->sf->presets[pi].preset, f->sf->presets[pi].name);
    }

    n = sf2_zone_find_all_m(f->sf, pi, key, vel, zones, SF2_MAX_ZONES,
                            r->modflags);
    if (n <= 0) {
        r->notes_unmapped++;
        return;
    }
    for (i = 0; i < n; i++)
        note_on_zone(r, f, chan, key, vel, &zones[i]);
}

/*
 * CC 120 AND CC 123 DIFFER IN EXACTLY ONE THING: THE PEDAL.
 *
 * `hard' is CC 120, all SOUND off - it stops the voice whatever is
 * holding it. Everything else routes through the same path an
 * ordinary note-off takes, INCLUDING the sustain check, because CC
 * 123 means "act as though every key was released" and a held pedal
 * legitimately sustains a released key.
 *
 * This bypassed that check until 2026-09-07 and called voice_release
 * directly, so CC 123 cut notes the pedal should have kept ringing.
 * design/p3-midi-plan.md section 8 warned about getting this
 * distinction backwards and named the wrong direction: the worry was
 * that CC 123 would fail to cut sustained notes, and what the code
 * actually did was cut notes it should not have.
 *
 * Confirmed against fluidsynth before changing: its
 * fluid_synth_all_notes_off_LOCAL calls fluid_voice_noteoff, the same
 * function an ordinary note-off uses, and that marks a voice
 * SUSTAINED rather than releasing it when the pedal is down
 * (fluid_voice.c). Measured on fixtures/sustain.mid it keeps such a
 * note ringing where we cut it to -99 dB in 600 ms.
 *
 * The duplication is what allowed the divergence, so the fix is to
 * delete it rather than to add a second sustain check that could
 * drift from note_off's in the same way.
 */
static void
all_notes_off(render_state *r, int chan, int hard)
{
    int i;
    for (i = 0; i < r->nalloc; i++) {
        render_voice *v = &r->voices[i];
        if (!v->active || v->channel != chan)
            continue;
        if (hard) {
            v->active = 0;              /* all sound off: immediate */
            continue;
        }
        if (v->stage >= ST_RELEASE)
            continue;
        if (r->chan[chan].sustain) {
            v->held = 1;                /* the pedal is down */
            continue;
        }
        voice_release(v);
    }
}

/*
 * THE STREAM CAN STOP MID-PEDAL, AND NOTHING WILL EVER RELEASE THOSE
 * VOICES.
 *
 * design/p3-midi-plan.md section 8. A file renderer never meets this:
 * the events run out because the song ended. A LIVE stream ends for
 * reasons that have nothing to do with the music -
 *
 *   - lxdoom is killed, or musserv exits, while a score holds CC 64
 *   - the reader closes /dev/vmidi between songs
 *   - a sequencer client is ^C'd mid-phrase
 *
 * - and in every one of them the last pedal-up never arrives. A voice
 * held by v->held is then held by nothing that will ever release it.
 * With 64 voices and prio -= 1000 for held voices they are
 * PREFERENTIALLY kept, so they drone until something else finally
 * steals the slot.
 *
 * THE ORDER IS THE WHOLE OF IT: clear sustain FIRST, then release.
 * Releasing first means note_off's own sustain check absorbs it
 * exactly as it absorbs a real note-off, and the voice stays held -
 * which is the bug rather than the fix. That is not a hypothetical:
 * it is what fixtures/sustain.mid part C measures, -54.9 dB where
 * fluidsynth reads -93.1 at 5.8 s.
 *
 * SOFT, NOT HARD. This routes through all_notes_off(..., 0), the same
 * path an ordinary note-off takes, so voices decay through their own
 * release envelopes. A hard cut is available - CC 120 - and is wrong
 * here: the daemon calls this on close and on reset, where an abrupt
 * truncation of 64 voices clicks. With sustain already cleared the
 * soft path cannot be absorbed, so it releases everything.
 *
 * EVERY CHANNEL, not one - a reset is global by definition, and the
 * caller has no channel to name.
 */
void
render_panic(render_state *r)
{
    int c;

    for (c = 0; c < RENDER_CHANNELS; c++) {
        r->chan[c].sustain = 0;         /* FIRST. See above. */
        all_notes_off(r, c, 0);
    }
}

/*
 * NRPN DATA ENTRY - design/22 step 3, sections 3 and 4. Called on CC 6
 * (`msb' 1) and CC 38 (`msb' 0) while an NRPN is selected. What each
 * family does is fluidsynth 2.5.7's (synth/fluid_synth.c, DATA_ENTRY
 * and fluid_synth_process_awe32_nrpn_LOCAL), read not copied, with
 * the two departures noted where they are.
 */

/* The spec's family: the data entry, less 8192, times this per
 * generator - fluidsynth's nrpn_scale column. 0 marks a generator
 * that is not a real-time parameter (9.6.2's last paragraph). */
static const unsigned char nrpn_scale[SF2_GEN_COUNT] = {
    1, 1, 1, 1, 1,          /*  0-4  sample offsets                  */
    2, 2, 2,                /*  5-7  LFO/env -> pitch, cents         */
    2, 1, 2, 2,             /*  8-11 filter fc, Q, LFO/env -> fc     */
    1, 1, 0,                /* 12-14 end coarse, LFO -> vol, unused  */
    1, 1, 1,                /* 15-17 chorus, reverb, pan             */
    0, 0, 0,                /* 18-20 unused                          */
    2, 4, 2, 4,             /* 21-24 LFO delays and rates            */
    2, 2, 2, 2, 1, 2,       /* 25-30 mod env                         */
    1, 1,                   /* 31-32 keynum to mod env               */
    2, 2, 2, 2, 1, 2,       /* 33-38 vol env                         */
    1, 1,                   /* 39-40 keynum to vol env               */
    0, 0, 0, 0,             /* 41-44 instrument, ranges              */
    1, 0, 1, 1,             /* 45-48 loop coarse, keynum, vel, atten */
    0, 1, 1, 1,             /* 49-52 reserved, loop coarse, tunes    */
    0, 0, 0, 1,             /* 53-56 sample id, modes, res., scale   */
    0, 0, 0                 /* 57-59 exclusive, root key, unused     */
};

/* Seconds to timecents and hertz to absolute cents, for the AWE32
 * conversions. libm at control rate, as the filter coefficients are. */
static int
sec_to_tc(double sec)
{
    double tc;
    if (sec <= 0.0) return -12000;
    tc = 1200.0 * log(sec) / log(2.0);
    if (tc < -12000.0) tc = -12000.0;
    if (tc >   8000.0) tc =   8000.0;
    return (int) (tc + (tc >= 0 ? 0.5 : -0.5));
}

static int
hz_to_cents(double hz)
{
    double c;
    if (hz <= 0.0) return -16000;
    c = 1200.0 * log(hz / 8.176) / log(2.0);
    if (c < -16000.0) c = -16000.0;
    if (c >   4500.0) c =   4500.0;
    return (int) (c + (c >= 0 ? 0.5 : -0.5));
}

/* Set generator `g' on every sounding voice of the channel, as the
 * new base, and re-apply it through the apply table (a destination
 * not in the table takes effect on the next note only, as in
 * fluidsynth for the envelope times). `add' adds instead of sets. */
static void
nrpn_live(render_state *r, int chan, int g, long value, int add)
{
    int i;
    for (i = 0; i < r->nalloc; i++) {
        render_voice *v = &r->voices[i];
        long nb;
        if (!v->active || v->channel != chan)
            continue;
        nb = add ? (long) v->gen_base[g] + value : value;
        if (nb < -32768L) nb = -32768L;
        if (nb >  32767L) nb =  32767L;
        v->gen_base[g] = (short) nb;
        voice_apply_dest(r, v, g);
    }
}

static void
nrpn_data(render_state *r, int chan, int msb)
{
    render_channel *c = &r->chan[chan];
    int  family = c->cc[99];
    int  lsb    = c->cc[38];
    long data   = ((long) c->cc[6] << 7) | lsb;

    if (family == 120) {
        /*
         * THE SPEC'S. Applied on the data MSB only - fluidsynth
         * requires "DATA_LSB and DATA_MSB in that order", which is
         * 9.6.1's order. DEPARTURE: fluidsynth clears its selection
         * after one entry; 9.6.1 says the select follows running
         * status, so ours keeps it and a second entry moves the same
         * generator again.
         */
        int  g = c->nrpn_gen;
        long val, delta;
        if (!msb)
            return;
        if (g < 0 || g >= SF2_GEN_COUNT || nrpn_scale[g] == 0) {
            r->nrpns_ignored++;
            return;
        }
        val = (data - 8192L) * nrpn_scale[g];
        if (val < -32768L) val = -32768L;
        if (val >  32767L) val =  32767L;
        delta = val - c->nrpn_add[g];
        c->nrpn_add[g] = (short) val;
        nrpn_live(r, chan, g, delta, 1);
        r->nrpns++;
        return;
    }

    if (family == 127) {
        /*
         * THE AWE32's, parameter 0..26 in CC 98, APPLIED WHEN THE DATA
         * LSB ARRIVES and not on the MSB alone - the AWE32 kernel
         * driver's rule (usr/src/linux/drivers/sound/lowlevel/
         * awe_wave.c, midi_nrpn_event: "both MSB/LSB necessary"), the
         * period reference. fluidsynth applies on either byte and
         * zeroes both on every select, so a "select, CC 6 = 64" with
         * no CC 38 - every third cutoff pair in Relentless - reaches
         * it as LSB 0 and shuts the filter for two seconds; on the
         * driver, and here, a lone MSB does nothing. Altitude's
         * LSB-only entries still work, since the LSB is the trigger.
         * fluidsynth's table and conversions from the SB AWE32
         * Developer's Information Pack:
         * `data' less 8192 for the signed ones, the LSB alone for the
         * 0..127 ones (Uplift never sets the MSB). Two use the spec
         * suite's HARDWARE-MEASURED Audigy curves instead of
         * fluidsynth's constants - cutoff and Q - because sections
         * 2/4/6 of its filter test ARE those curves and are the
         * acceptance: cutoff 100 Hz plus 7646 cents over the range
         * (fluidsynth: 110 Hz plus 7493), Q 20 dB over the range
         * (fluidsynth: 22 dB).
         */
        static const signed char awe_gen[27] = {
            SF2_delayModLFO, SF2_freqModLFO, SF2_delayVibLFO, SF2_freqVibLFO,
            SF2_delayModEnv, SF2_attackModEnv, SF2_holdModEnv, SF2_decayModEnv,
            SF2_sustainModEnv, SF2_releaseModEnv,
            SF2_delayVolEnv, SF2_attackVolEnv, SF2_holdVolEnv, SF2_decayVolEnv,
            SF2_sustainVolEnv, SF2_releaseVolEnv,
            -1 /* 16: pitch, a bend */,
            SF2_modLfoToPitch, SF2_vibLfoToPitch, SF2_modEnvToPitch,
            SF2_modLfoToVolume, SF2_initialFilterFc, SF2_initialFilterQ,
            SF2_modLfoToFilterFc, SF2_modEnvToFilterFc,
            SF2_chorusEffectsSend, SF2_reverbEffectsSend
        };
        int  p = c->cc[98];
        long d = data - 8192L;
        long val;
        int  g, live = 0;

        if (msb)
            return;                             /* wait for the LSB */
        if (p > 26) { r->nrpns_ignored++; return; }
        g = awe_gen[p];
        switch (p) {
        case 0: case 2: case 4: case 10:            /* delays, 4 ms units */
            if (d < 0) d = 0; if (d > 5900) d = 5900;
            val = sec_to_tc(d * 0.004); break;
        case 1: case 3:                             /* LFO rate, 0.084 Hz units */
            val = hz_to_cents(lsb * 0.084); live = 1; break;
        case 5: case 11:                            /* attack, 1 ms units */
            if (d < 0) d = 0; if (d > 5940) d = 5940;
            val = sec_to_tc(d * 0.001); break;
        case 6: case 12:                            /* hold, 1 ms units */
            if (d < 0) d = 0; if (d > 8191) d = 8191;
            val = sec_to_tc(d * 0.001); break;
        case 7: case 9: case 13: case 15:           /* decay, release, 4 ms */
            if (d < 0) d = 0; if (d > 5940) d = 5940;
            val = sec_to_tc(d * 0.004); break;
        case 8: case 14:                            /* sustain, 0.75 dB units */
            val = (long) lsb * 75L / 10L; break;    /* centibels */
        case 16:                                    /* pitch: a bend */
            c->bend = (int) d;
            if (c->bend < -8192) c->bend = -8192;
            if (c->bend >  8191) c->bend =  8191;
            channel_modulate(r, chan, 0, 14, -1);
            r->nrpns++;
            return;
        case 17: case 18: case 19:                  /* LFO/env -> pitch */
            if (d < -127) d = -127; if (d > 127) d = 127;
            val = (d * 75L) / 8L;                   /* 9.375 cents */
            live = (p != 19); break;
        case 20:                                    /* LFO -> volume, 0.1875 dB */
            val = ((long) lsb * 1875L) / 1000L;     /* centibels */
            live = 1; break;
        case 21:                                    /* filter cutoff */
            val = 4337L + ((long) lsb * 7646L) / 127L;
            live = 1; break;
        case 22:                                    /* filter Q */
            val = ((long) lsb * 200L) / 127L;
            live = 1; break;
        case 23:                                    /* LFO -> fc, 56.25 cents */
            if (d < -64) d = -64; if (d > 63) d = 63;
            val = (d * 225L) / 4L; live = 1; break;
        case 24:                                    /* env -> fc */
            if (d < -127) d = -127; if (d > 127) d = 127;
            val = (d * 225L) / 4L; break;
        default:                                    /* 25, 26: sends 0..255 */
            if (data < 0) data = 0; if (data > 255) data = 255;
            val = (data * 200L) / 256L;             /* tenths of a percent */
            break;
        }
        c->awe_set[g] = 1;
        c->awe_val[g] = (short) val;
        if (live)
            nrpn_live(r, chan, g, val, 0);
        r->nrpns++;
        return;
    }

    if (family == 1 && r->bank_style == RENDER_BANK_GS && msb) {
        /* Roland GS: vibrato rate/depth/delay to the sound controllers
         * CC 76/77/78, as fluidsynth translates them. A font modulator
         * on those controllers sees the value; nothing else here reads
         * them. */
        int p = c->cc[98];
        if (p >= 8 && p <= 10) {
            render_event(r, (unsigned char) (0xb0 | chan),
                         (unsigned char) (76 + (p - 8)), c->cc[6]);
            r->nrpns++;
            return;
        }
    }
    r->nrpns_ignored++;
}

void
render_event(render_state *r, unsigned char status, unsigned char d1,
             unsigned char d2)
{
    int chan = status & 0x0f;
    int kind = status & 0xf0;

    switch (kind) {
    case 0x80:
        note_off(r, chan, d1 & 0x7f);
        break;

    case 0x90:
        /* A NOTE-ON WITH VELOCITY 0 IS A NOTE-OFF. Every sequencer
         * uses it for running status, so this is the common case, not
         * a corner one. */
        if (d2 == 0)
            note_off(r, chan, d1 & 0x7f);
        else
            note_on(r, chan, d1 & 0x7f, d2 & 0x7f);
        break;

    case 0xb0:
        /* The modulator layer's view of every controller; the named
         * fields below are what the per-frame paths read. */
        r->chan[chan].cc[d1 & 0x7f] = (unsigned char) (d2 & 0x7f);
        switch (d1 & 0x7f) {
        /*
         * BANK SELECT, AND THE DRUM CHANNEL IGNORES IT.
         *
         * In General MIDI channel 10 - 0-based 9 - is ALWAYS
         * percussion, whatever bank a file selects. render_init sets
         * that channel to bank 128 where the SC-55 keeps its kits;
         * honouring a later CC 0 or CC 32 there moves it off the kit
         * and every drum note plays a melodic instrument instead.
         *
         * THE USER HEARD THIS AS "MISSING DRUMS AND CYMBALS" and
         * separately as the piano sounding wrong - which it was,
         * because bohemian.mid sends CC 0 = 0 on channel 9 at 1.2 s
         * and we then loaded PIANO 1 for the whole drum kit. Every
         * hit played a piano sample pitched by the drum key, which
         * measures as 32 dB missing above 6 kHz and 8 dB too much in
         * the low mids: no cymbals, no stick attack, and a dull
         * thickening where the kit belongs.
         *
         * The kit is not a program on bank 0 - see render_init.
         *
         * CC 0 IS THE BANK; CC 32 IS IGNORED - the GS rule, which is
         * fluidsynth's default (`synth.midi-bank-select` = gs) and
         * what the SC-55 itself does. Until 2026-09-08 this combined
         * them as MSB << 7 | LSB, the MMA style, which turned the
         * AWE32-era "CC 0 = 1" into bank 128 and only landed on the
         * right sound by missing and falling back to bank 0 - found
         * the moment a second font sat in bank 1 (design/22 step 1's
         * acceptance). The style variable that the GM/GS/XG resets
         * set, and XG's LSB-is-the-bank rule, are design/22 step 2;
         * this is its default case brought forward because step 1
         * cannot pass without it.
         */
        case 0:
            switch (r->bank_style) {
            case RENDER_BANK_GM:
                /* THE AWE32's GM, NOT fluidsynth's: bank select stays
                 * honoured after a GM On (render.h's table, and the
                 * Acer run that found it). */
            case RENDER_BANK_GS:
                r->chan[chan].bank = r->chan[chan].drum ? 128 + d2 : d2;
                break;
            case RENDER_BANK_XG:
                /* 120/126/127 make the channel drums, anything else
                 * makes it melodic and is otherwise ignored - the
                 * LSB is XG's bank. fluid_chan.c, set_bank_msb. */
                r->chan[chan].drum = (d2 == 120 || d2 == 126 || d2 == 127);
                if (r->chan[chan].drum)
                    r->chan[chan].bank = 128;
                else if (r->chan[chan].bank >= 128)
                    r->chan[chan].bank = 0;
                break;
            default:
                break;
            }
            break;
        case 32:
            if (r->bank_style == RENDER_BANK_XG && !r->chan[chan].drum)
                r->chan[chan].bank = d2;
            break;                            /* gs, gm: ignored */
        case 7:   r->chan[chan].volume = d2;                   break;
        case 1:   r->chan[chan].modwheel = d2;                 break;
        case 10:  r->chan[chan].pan = d2;                      break;
        case 11:  r->chan[chan].expression = d2;               break;
        case 64:
            /*
             * SUSTAIN. Releasing the pedal must release every note
             * that was held while it was down - not just stop holding
             * new ones. p3-midi-plan section 8 names this as not
             * ignorable, and gmstriving.mid has 227 of them.
             */
            r->chan[chan].sustain = (d2 >= 64);
            if (!r->chan[chan].sustain) {
                int i;
                for (i = 0; i < r->nalloc; i++)
                    if (r->voices[i].active &&
                        r->voices[i].channel == chan &&
                        r->voices[i].held) {
                        r->voices[i].held = 0;
                        voice_release(&r->voices[i]);
                    }
            }
            break;

        /* RPN, so pitch-bend range is read rather than guessed. An
         * RPN select ends an NRPN in progress, as an NRPN select ends
         * an RPN: the data entry that follows belongs to the last
         * select of either kind. */
        case 101: r->chan[chan].rpn = (r->chan[chan].rpn & 0x7f)
                                    | (d2 << 7);
                  r->chan[chan].nrpn_active = 0;                 break;
        case 100: r->chan[chan].rpn = (r->chan[chan].rpn < 0 ? 0 : 
                                       (r->chan[chan].rpn & ~0x7f)) | d2;
                  r->chan[chan].nrpn_active = 0;                 break;

        /*
         * NRPN SELECT - design/22 step 3. CC 99 is the family (120 the
         * spec's, 127 the AWE32's, 1 Roland GS); CC 98 the parameter.
         * For the spec's family an LSB of 100/101/102 adds 100/1000/
         * 10000 to the NEXT LSB under 100 (9.6.2) - and only the next:
         * "running status does not include multiple sends of values
         * greater than 100".
         */
        case 99:  r->chan[chan].nrpn_active   = 1;
                  r->chan[chan].nrpn_hundreds = 0;
                  r->chan[chan].rpn           = -1;              break;
        case 98:
            r->chan[chan].nrpn_active = 1;
            r->chan[chan].rpn         = -1;
            if (d2 < 100) {
                r->chan[chan].nrpn_gen = r->chan[chan].nrpn_hundreds + d2;
                r->chan[chan].nrpn_hundreds = 0;
            } else if (d2 == 100) r->chan[chan].nrpn_hundreds += 100;
            else if   (d2 == 101) r->chan[chan].nrpn_hundreds += 1000;
            else if   (d2 == 102) r->chan[chan].nrpn_hundreds += 10000;
            break;

        case 6:
            if (r->chan[chan].rpn == 0)
                r->chan[chan].bend_range = d2;
            else if (r->chan[chan].nrpn_active)
                nrpn_data(r, chan, 1);
            break;
        case 38:
            if (r->chan[chan].nrpn_active)
                nrpn_data(r, chan, 0);
            break;

        /*
         * CHANNEL MODE MESSAGES, NOT ORDINARY CONTROLLERS. A synth
         * that ignores 123 hangs notes - the failure this project has
         * already chased at length.
         */
        case 120: all_notes_off(r, chan, 1); break;   /* all sound off */
        case 123:                                     /* all notes off */
        case 124: case 125: case 126: case 127:
                  all_notes_off(r, chan, 0); break;
        case 121: {                                   /* reset ctrls  */
            /*
             * RESET ALL CONTROLLERS IS NOT A SYSTEM RESET - MIDI
             * RP-015, and fluidsynth's init_ctrl(1). It clears the
             * PERFORMANCE state a player leaves behind mid-song -
             * bend, modulation, the pedals, pressure, expression
             * back to full, RPN/NRPN selection - and LEAVES the mix
             * alone: bank, program, volume, pan, balance, the effect
             * depths (CC 91-95) and the sound controllers (CC 70-79)
             * keep their values, and so does the bend range. Until
             * 2026-09-08 this also set volume 100 and pan 64, which
             * is a system reset's job (channel_power_on) and would
             * collapse a file's balance every time it sent 121
             * between sections. design/22 step 2.
             */
            render_channel *c = &r->chan[chan];
            int k, i;
            for (k = 0; k < 128; k++) {
                if (k == 0 || k == 32 || k == 7 || k == 39 || k == 8 ||
                    k == 40 || k == 10 || k == 42 || (k >= 70 && k <= 79) ||
                    (k >= 91 && k <= 95))
                    continue;
                c->cc[k] = 0;
            }
            c->cc[11]  = 127;
            c->cc[100] = c->cc[101] = c->cc[98] = c->cc[99] = 127;
            c->expression = 127;
            c->modwheel   = 0;
            c->bend       = 0;
            c->rpn        = -1;
            c->pressure   = 0;
            memset(c->poly, 0, sizeof c->poly);
            /* ..and the NRPN state, as fluidsynth's init_ctrl clears
             * chan->gen[] and the AWE32 overrides on the same message. */
            c->nrpn_active = 0;
            c->nrpn_gen    = -1;
            c->nrpn_hundreds = 0;
            memset(c->nrpn_add, 0, sizeof c->nrpn_add);
            memset(c->awe_set,  0, sizeof c->awe_set);
            /* The pedal is up: release what it was holding, as the
             * CC 64 path does. */
            c->sustain = 0;
            for (i = 0; i < r->nalloc; i++)
                if (r->voices[i].active && r->voices[i].channel == chan &&
                    r->voices[i].held) {
                    r->voices[i].held = 0;
                    voice_release(&r->voices[i]);
                }
            break;
        }
        default:
            break;
        }
        /*
         * THE LIVE PATH. Every sounding voice on the channel with a
         * modulator listening to this controller re-applies that
         * destination - CC7 and CC11 to the gain, CC10 to the pan, CC1
         * to the vibrato depth or a font's own filter sweep (test 15),
         * CC91/93 to nothing yet. A reset (121) moved several sources
         * at once, so it re-applies them all, as fluidsynth's
         * fluid_voice_modulate_all does after the same message.
         */
        if ((d1 & 0x7f) == 121) {
            static const int reset_src[] = { 7, 10, 11, 1, 91, 93, 33 };
            int k;
            for (k = 0; k < (int) (sizeof reset_src / sizeof reset_src[0]); k++)
                channel_modulate(r, chan, 1, reset_src[k], -1);
            channel_modulate(r, chan, 0, 13, -1);
            channel_modulate(r, chan, 0, 14, -1);
        } else {
            channel_modulate(r, chan, 1, d1 & 0x7f, -1);
            /* RPN 0 data entry moved the pitch-wheel sensitivity, the
             * amount source of 8.4.10 */
            if ((d1 & 0x7f) == 6 && r->chan[chan].rpn == 0)
                channel_modulate(r, chan, 0, 16, -1);
        }
        break;

    case 0xc0:
        r->chan[chan].program = d1 & 0x7f;
        break;

    case 0xe0:
        r->chan[chan].bend = ((int) (d2 & 0x7f) << 7) + (d1 & 0x7f) - 8192;
        channel_modulate(r, chan, 0, 14, -1);
        break;

    case 0xd0:                          /* channel pressure */
        r->chan[chan].pressure = d1 & 0x7f;
        channel_modulate(r, chan, 0, 13, -1);
        break;

    case 0xa0:                          /* poly pressure, per key */
        r->chan[chan].poly[d1 & 0x7f] = (unsigned char) (d2 & 0x7f);
        channel_modulate(r, chan, 0, 10, d1 & 0x7f);
        break;

    default:
        break;
    }
}

void
render_system_reset(render_state *r)
{
    int i;

    for (i = 0; i < RENDER_CHANNELS; i++) {
        all_notes_off(r, i, 1);             /* all sound off */
        channel_power_on(r, i);
    }
    /* The effects' memory is cleared too, as fluidsynth's mixer
     * reset does - a tail from the last song does not carry into
     * the next. */
    reverb_init(&r->reverb, r->rate);
    chorus_init(&r->chorus, r->rate);
}

/*
 * The GS "use for rhythm part" channel byte: 0 is channel 10, 1-9 are
 * channels 1-9, 0x0A-0x0F are channels 11-16 (SC-88Pro manual, and
 * fluid_synth_sysex_gs_dt1). Returns the 0-based channel.
 */
static int
gs_part_channel(int x)
{
    if (x == 0)     return 9;
    if (x <= 9)     return x - 1;
    return x;                                   /* 0x0a..0x0f -> 10..15 */
}

static void
sysex_log(render_state *r, const unsigned char *body, int len, const char *what)
{
    int i;
    if (r->log == NULL)
        return;
    fprintf(r->log, "render: sysex %d bytes:", len);
    for (i = 0; i < len && i < 12; i++)
        fprintf(r->log, " %02x", body[i]);
    fprintf(r->log, "%s - %s\n", len > 12 ? " .." : "", what);
}

void
render_sysex(render_state *r, const unsigned char *body, int len)
{
    int dev;

    if (r == NULL || body == NULL || len < 4)
        return;
    dev = body[1];
    if (dev != 0x10 && dev != 0x7f) {
        sysex_log(r, body, len, "ignored (device id)");
        return;
    }

    /* GM System On: 7E dev 09 01 (GM2 On, 09 03, is taken as GM). */
    if (body[0] == 0x7e && body[2] == 0x09 &&
        (body[3] == 0x01 || body[3] == 0x03)) {
        sysex_log(r, body, len, "GM System On: system reset, bank style gm");
        r->bank_style = RENDER_BANK_GM;
        render_system_reset(r);
        return;
    }

    /* GS DT1: 41 dev 42 12 addr(3) data... checksum */
    if (body[0] == 0x41 && body[2] == 0x42 && body[3] == 0x12) {
        long addr;
        int  sum = 0, i, ndata;

        if (len < 9)
            return;
        ndata = len - 8;
        addr = ((long) body[4] << 16) | ((long) body[5] << 8) | body[6];
        for (i = 4; i < len - 1; i++)
            sum += body[i];
        /* The Roland checksum: address plus data plus checksum is a
         * multiple of 128. fluidsynth: 0x80 - (sum & 0x7f), masked. */
        if (((0x80 - (sum & 0x7f)) & 0x7f) != body[len - 1]) {
            sysex_log(r, body, len, "GS DT1 ignored (bad checksum)");
            return;
        }

        if (addr == 0x40007fL) {                /* mode set: GS reset */
            if (ndata != 1 || (body[7] != 0 && body[7] != 0x7f))
                return;
            r->bank_style = (body[7] == 0) ? RENDER_BANK_GS : RENDER_BANK_GM;
            sysex_log(r, body, len, body[7] == 0 ? "GS reset: system reset, bank style gs"
                                                 : "GS mode set to GM: system reset, bank style gm");
            render_system_reset(r);
            return;
        }
        if (r->bank_style != RENDER_BANK_GS) {
            sysex_log(r, body, len, "GS parameter ignored (not in gs style)");
            return;                             /* GS-only parameters */
        }
        if ((addr & 0xfff0ffL) == 0x401015L) {  /* use for rhythm part */
            int chan, drum;
            if (ndata != 1 || body[7] > 2)
                return;
            chan = gs_part_channel((int) ((addr >> 8) & 0x0f));
            drum = (body[7] != 0);
            r->chan[chan].drum = drum;
            /* As fluidsynth: controllers reset on that part, the bank
             * to the kits or to 0, and program 0 - the standard kit or
             * the piano - until the file says otherwise. */
            render_event(r, (unsigned char) (0xb0 | chan), 121, 0);
            r->chan[chan].bank    = drum ? 128 : 0;
            r->chan[chan].program = 0;
            sysex_log(r, body, len, drum ? "GS rhythm part: channel to drums"
                                         : "GS rhythm part: channel to melodic");
            return;
        }
        sysex_log(r, body, len, "GS parameter ignored");
        return;
    }

    /* XG: 43 dev 4C 00 00 7E/7F 00 - the resets, nothing else. */
    if (body[0] == 0x43 && body[2] == 0x4c && len >= 7 &&
        body[3] == 0x00 && body[4] == 0x00 &&
        (body[5] == 0x7e || body[5] == 0x7f) && body[6] == 0x00) {
        sysex_log(r, body, len, "XG reset: system reset, bank style xg");
        r->bank_style = RENDER_BANK_XG;
        render_system_reset(r);
        return;
    }
    sysex_log(r, body, len, "ignored");
}

/*
 * THE MODULATION ENVELOPE, one frame.
 *
 * The same six stages as the volume envelope, and deliberately a
 * SEPARATE function rather than a shared one parameterised by a
 * struct: the two differ in their sustain units (centibels against
 * per-mille) and in their release shape, and folding them together
 * would hide exactly the difference that matters.
 *
 * Its release is LINEAR, unlike the volume envelope's. A pitch
 * envelope is not an amplitude and has no -60 dB below which it stops
 * mattering; it simply has to arrive at zero.
 */
static __inline__ long             /* see cents_to_ratio */
menv_step(render_voice *v)
{
    switch (v->menv_stage) {
    case ST_DELAY:
        if (--v->menv_count <= 0) {
            v->menv_stage = ST_ATTACK;
            v->menv_count = v->menv_attack_frames;
            v->menv_sub   = v->menv_level << ENV_SUBBITS;
            v->menv_rate  = v->menv_attack_frames > 0
                          ? (ENV_ONE << ENV_SUBBITS)
                            / (v->menv_attack_frames + 1)
                          : (ENV_ONE << ENV_SUBBITS);
            if (v->menv_rate == 0)
                v->menv_rate = 1;
        }
        return 0;

    case ST_ATTACK:
        v->menv_sub  += v->menv_rate;
        v->menv_level = v->menv_sub >> ENV_SUBBITS;
        if (v->menv_level >= ENV_ONE || --v->menv_count <= 0) {
            v->menv_level = ENV_ONE;
            v->menv_stage = ST_HOLD;
            v->menv_count = v->menv_hold_frames;
        }
        break;

    case ST_HOLD:
        if (--v->menv_count <= 0) {
            v->menv_stage = ST_DECAY;
            v->menv_count = v->menv_decay_frames;
            v->menv_sub   = v->menv_level << ENV_SUBBITS;
            v->menv_rate  = v->menv_decay_frames > 0
                          ? -(((ENV_ONE - v->menv_sustain_level)
                               << ENV_SUBBITS)
                              / (v->menv_decay_frames + 1))
                          : -(ENV_ONE << ENV_SUBBITS);
            if (v->menv_rate == 0)
                v->menv_rate = -1;
        }
        break;

    case ST_DECAY:
        v->menv_sub  += v->menv_rate;
        v->menv_level = v->menv_sub >> ENV_SUBBITS;
        if (v->menv_level <= v->menv_sustain_level ||
            --v->menv_count <= 0) {
            v->menv_level = v->menv_sustain_level;
            v->menv_stage = ST_SUSTAIN;
        }
        break;

    case ST_RELEASE:
        v->menv_sub  += v->menv_rate;
        v->menv_level = v->menv_sub >> ENV_SUBBITS;
        if (v->menv_level <= 0) {
            v->menv_level = 0;
            v->menv_sub   = 0;
            v->menv_stage = ST_DONE;
        }
        break;

    case ST_SUSTAIN:
    default:
        break;
    }
    if (v->menv_level < 0)       v->menv_level = 0;
    if (v->menv_level > ENV_ONE) v->menv_level = ENV_ONE;
    return v->menv_level;
}

/*
 * CATCH A FROZEN ENVELOPE UP - design/54 D28, 2026-10-05.
 *
 * RENDER_MENV_FAST skips the envelope of a voice that modulates nothing
 * (design/20's 7%). The skip used to be the whole story, and it was
 * wrong once a depth turned non-zero mid-note - a font modulator on a
 * controller, or the SoundFont NRPN (family 120): the envelope then
 * STARTED at that moment instead of continuing from where it would have
 * been. design/22 section 18.3 measured it, an octave arriving 2 s late.
 *
 * So the skipped frames are replayed here, through menv_step() itself,
 * with the release applied after exactly as many of them as it came -
 * the same steps in the same order as RENDER_MENV_REFERENCE takes, so
 * the two render byte-identically. Bounded: once the envelope sits in
 * SUSTAIN nothing changes until the release, so the replay jumps to it
 * (or stops), and it stops at DONE.
 */
static void
menv_catch_up(render_state *r, render_voice *v)
{
    long n = v->menv_skipped, rel = v->menv_rel_at, i;

    v->menv_skipped = 0;
    v->menv_rel_at  = -1;
    if (n <= 0)
        return;
    r->menv_catchups++;
    for (i = 0; i < n; i++) {
        if (i == rel)
            menv_release(v);
        if (v->menv_stage == ST_SUSTAIN) {
            if (rel > i) {
                i = rel - 1;            /* nothing moves until then */
                continue;
            }
            break;                      /* sustained, and no release */
        }
        if (v->menv_stage >= ST_DONE)
            break;
        (void) menv_step(v);
    }
    if (rel >= n)
        menv_release(v);                /* released after the last skip */
}

/*
 * ONE FRAME OF EXPONENTIAL DECAY, IN A FINER UNIT THAN THE LEVEL.
 *
 * `sub' is the voice's level scaled up by ENV_SUBBITS, and it is the
 * value that actually decays; the caller reads env_level back out of
 * it. Keeping the state fine rather than the step fine is what makes
 * a long envelope work, and two earlier attempts got this wrong in
 * instructive ways:
 *
 *   - `(level * drop) >> 24' rounds a fractional step to zero and the
 *     envelope freezes.
 *   - forcing that step up to a minimum of 1 gives a FIXED decay of
 *     one unit per frame, which empties a full envelope in 1.48 s
 *     whatever time was asked for. That is the same bug as the
 *     original 16-second underflow, moved one function along: a clamp
 *     standing in for missing resolution.
 *
 * With the state carrying 8 extra bits there is no fraction to lose:
 * the SC-55 piano's 17.8 s decay subtracts 235 sub-units from
 * 16777216 on its first frame, and every frame thereafter subtracts a
 * proportional amount that stays representable all the way down.
 *
 * THE PRODUCT IS SPLIT TO AVOID OVERFLOW, inherited from the
 * env_scale this replaced: `sub * drop' would need 24+17 = 41 bits,
 * and ANSI C guarantees long is 32. Splitting the multiplicand keeps
 * every partial product in range. The original overflow silenced
 * cymbals outright - they are pure decay with sustainVolEnv 1000, so
 * the decay is the whole sound.
 */
static __inline__ long             /* see cents_to_ratio */
env_decay_step(long sub, long drop)
{
    long d;

    if (sub <= 0)
        return 0;
    if (drop <= 0)
        return sub;

    /*
     * A FULL-SCALE DROP MEANS "IMMEDIATELY", AND MUST BE TAKEN FIRST.
     *
     * env_decay_drop returns ENV_ONE * ENV_SUBONE for a zero-length
     * stage. Feeding that to the split below overflows: hi is then
     * sub>>12 = 4096 and hi*drop needs 37 bits against a signed
     * long's 31. It wraps, d collapses to the `d < 1' floor of 1, and
     * an envelope that should end instantly instead creeps down one
     * unit per frame - 6.3 minutes to fall from full scale.
     *
     * ON 32-BIT ONLY. A 64-bit long absorbs the 37 bits, so every
     * workstation render was correct while the target's was not: a
     * note with sustainVolEnv 0 and an instant decay sustained
     * forever instead of stopping, and the SoundFont spec test failed
     * with silent gaps where voices should have ended and freed their
     * slots. Two 32-bit compilers seven years apart - gcc 2.95.2 and
     * 3.2 - produced byte-identical wrong output, which is what
     * proved it was ours and not a codegen quirk.
     *
     * The comment that used to sit here said the split was chosen
     * "so neither partial product exceeds a signed long". That was
     * simply false for a large drop, and stating it stopped anyone
     * checking.
     */
    if (drop >= ENV_ONE * ENV_SUBONE)
        return 0;

    /*
     * d = sub * drop >> 24, WITH BOTH OPERANDS SPLIT.
     *
     * Splitting only `sub' is not enough: its high half is 12 bits
     * and `drop' is up to 24, so that partial product still needs 36.
     * Both are cut at 12 bits and the four cross terms recombined -
     * the schoolbook long multiplication - which keeps every partial
     * product to 25 bits at worst.
     *
     *   sub*drop = sh*dh<<24 + (sh*dl + sl*dh)<<12 + sl*dl
     *
     * so shifting right by 24 gives the three terms below. Checked
     * exact against 64-bit arithmetic over 20000 random pairs across
     * the whole range, worst error 1 unit from the final truncation.
     */
    {
        long sh = sub >> 12,  sl = sub & 0xfff;
        long dh = drop >> 12, dl = drop & 0xfff;

        d = (sh * dh)
          + (((sh * dl) + (sl * dh)) >> 12)
          + ((sl * dl) >> 24);
    }

    if (d < 1)
        d = 1;                          /* always makes progress */

    sub -= d;
    return sub > 0 ? sub : 0;
}

/* The envelope, one frame. Returns the current level, 0..ENV_ONE. */
static __inline__ long             /* see cents_to_ratio */
env_step(render_voice *v)
{
    switch (v->stage) {
    case ST_DELAY:
        if (--v->env_count <= 0) {
            v->stage = ST_ATTACK;
            v->env_count = v->attack_frames;
            /*
             * THE ATTACK RUNS IN SUB-BITS, LIKE THE DECAY - fixed
             * 2026-09-08 from the spec test's test 1. As an integer
             * `ENV_ONE / frames' truncates: a one-second attack is
             * 65536 / 44101 = 1 per frame, which reaches 44100 of
             * 65536 - 0.673, -3.4 dB - and was then SNAPPED to full
             * level when the count ran out. Every attack over 0.15 s
             * ended short and stepped, 0.3 s at -1.9 dB, 1 s at -3.4,
             * 2 s at -9.4; measured against fluidsynth as -3.7 vs
             * -0.2 dB at 95% of the ramp. The modulation envelope had
             * the identical bug (ae062e2). With ENV_SUBBITS the rate
             * has 256 steps per LSB and the ramp lands on ENV_ONE by
             * itself. The shape - linear in amplitude, which the
             * suite's README asks for - was always right.
             */
            v->env_sub  = 0;
            v->env_rate = v->attack_frames > 0
                        ? (ENV_ONE << ENV_SUBBITS) / (v->attack_frames + 1)
                        : (ENV_ONE << ENV_SUBBITS);
            if (v->env_rate < 1)
                v->env_rate = 1;
        }
        return 0;

    case ST_ATTACK:
        v->env_sub  += v->env_rate;
        v->env_level = v->env_sub >> ENV_SUBBITS;
        if (v->env_level >= ENV_ONE || --v->env_count <= 0) {
            v->env_level = ENV_ONE;
            v->stage = ST_HOLD;
            v->env_count = v->hold_frames;
        }
        break;

    case ST_HOLD:
        if (--v->env_count <= 0) {
            v->stage = ST_DECAY;
            v->env_count = v->decay_frames;
            /*
             * THE DECAY IS EXPONENTIAL, LIKE THE RELEASE.
             *
             * This was linear, and it is the same bug that was fixed
             * in the release on 2026-09-04 - left behind in the stage
             * next door. SF2 envelope times are the time to fall
             * 100 dB, so a decay is multiplicative.
             *
             * CYMBALS ARE WHERE IT SHOWS, because they are pure decay
             * with sustainVolEnv 1000 - 100 dB, i.e. to silence - and
             * nothing else. A linear ramp is still at HALF amplitude
             * when an exponential one is already 50 dB down, so a
             * crash became a sustained wash instead of a strike that
             * rings out. The user heard it as "the cymbals don't
             * sound right"; every pitched instrument disguised it
             * because they decay to a non-zero sustain and then hold.
             *
             * Same 11/n factor as voice_release: f^n = 2^-16 over n
             * frames without a pow().
             */
            v->env_rate = env_decay_drop(v->decay_frames);
            v->env_sub  = v->env_level << ENV_SUBBITS;
        }
        break;

    case ST_DECAY:
        /* MULTIPLICATIVE - see the transition above. */
        v->env_sub   = env_decay_step(v->env_sub, v->env_rate);
        v->env_level = v->env_sub >> ENV_SUBBITS;
        /*
         * SETTLE ONLY WHEN THE LEVEL HAS ACTUALLY ARRIVED.
         *
         * The obvious condition - `level <= sustain || --count <= 0'
         * - collapses the whole decay on its FIRST frame whenever
         * sustain is 0, because the assignment then forces the level
         * to 0 rather than letting it fall. A cymbal, whose
         * sustainVolEnv is 1000 cB (silence), went from full to
         * nothing in one sample: audible as no cymbal at all.
         *
         * So the count is checked separately, and a zero sustain is
         * ended by the -60 dB floor rather than by a comparison an
         * exponential satisfies immediately.
         */
        if (--v->env_count <= 0) {
            v->env_level = v->sustain_level;
            v->stage = ST_SUSTAIN;
        } else if (v->sustain_level > 0 &&
                   v->env_level <= v->sustain_level) {
            v->env_level = v->sustain_level;
            v->stage = ST_SUSTAIN;
        } else if (v->env_level <= (ENV_ONE >> 10)) {
            /* Below audibility with nothing to sustain at: done. */
            v->env_level = 0;
            v->stage = ST_SUSTAIN;
        }
        break;

    case ST_SUSTAIN:
        break;

    case ST_RELEASE:
        /* MULTIPLICATIVE, not additive - see voice_release. */
        v->env_sub   = env_decay_step(v->env_sub, v->env_rate);
        v->env_level = v->env_sub >> ENV_SUBBITS;
        /*
         * CUT AT -60 dB rather than waiting for zero. An exponential
         * never reaches it, and a voice held below audibility is a
         * slot another note cannot have.
         */
        if (v->env_level <= (ENV_ONE >> 10) || --v->env_count <= 0) {
            v->env_level = 0;
            v->stage = ST_DONE;
        }
        break;

    default:
        v->env_level = 0;
        break;
    }
    if (v->env_level < 0)       v->env_level = 0;
    if (v->env_level > ENV_ONE) v->env_level = ENV_ONE;
    return v->env_level;
}

static void
mix_block(render_state *r, short *out, int frames)
{
    int i, f;

    /*
     * ACCUMULATE IN LONGS, SATURATE ONCE AT THE END.
     *
     * This used to add each voice straight into the short output -
     * out[f*2] = (short)(out[f*2] + l) - which WRAPS past 32767 to
     * full-scale garbage of the opposite sign. And r->clipped, which
     * vmidid prints every second, was incremented nowhere: it read 0
     * because there was nothing to count. design/20-synth-audit.md
     * section 8. RENDER_MASTER at 512 kept this corpus in range; a
     * hotter font with 64 voices would not have been, and users bring
     * their own fonts.
     *
     * A long accumulator cannot wrap: a voice contributes at most
     * about 20000 after master gain, and 64 of them is 1.3M against a
     * 31-bit range. So the sum is exact and only the final store
     * clips - which also avoids the intermediate-saturation error
     * where one voice pushes past full scale and the next would have
     * brought it back. And it is CHEAPER than what it replaced: the
     * inner loop loses two short<->long conversions per channel and
     * gains no branch; the two compares move to one pass per block.
     */
    const long master = r->master;      /* one load per block */
    /* THE LARGEST GAINED SAMPLE THAT CAN BE MULTIPLIED BY master IN 31
     * BITS - design/24 P1, design/54 D27 (2026-10-04). The over-range
     * branch below clamps to it; see there. */
    const long glim = master > 0 ? 0x7fffffffL / master : 0x7fffffffL;

    memset(r->acc, 0, sizeof(long) * 2 * (size_t) frames);
    if (r->fx_reverb)
        memset(r->bus_reverb, 0, sizeof(long) * (size_t) frames);
    if (r->fx_chorus)
        memset(r->bus_chorus, 0, sizeof(long) * (size_t) frames);

    /*
     * EVERY ALLOCATED VOICE, NOT ONLY THOSE UNDER THE CEILING - fixed
     * 2026-10-02. The ceiling limits what alloc_voice() may START
     * (and what it may steal); a voice already sounding above a
     * lowered ceiling plays to its natural end. Every loop that acts
     * on sounding voices - this one, note-off, sustain, controllers,
     * exclusive class, all-notes-off, render_active() - runs to
     * `nalloc' for that reason.
     *
     * IT USED TO RUN TO max_voices, and lowering the ceiling FROZE the
     * voices above it: not mixed, not advanced, and unreachable by a
     * note-off, so they resumed mid-sample if the ceiling rose again.
     * render_set_max_voices() said they kept sounding; they did not.
     * Harmless while only a SIGUSR1 lowered it, live once automatic
     * voice reduction did (design/36 row 126).
     */
    for (i = 0; i < r->nalloc; i++) {
        render_voice   *v = &r->voices[i];
        const render_channel *c;

        if (!v->active)
            continue;
        c = &r->chan[v->channel];

        /*
         * CHANNEL VOLUME AND EXPRESSION ARE SEPARATE and both apply -
         * CC 7 is the fader, CC 11 the phrase-level swell. Using only
         * one loses the other's dynamics; gmstriving.mid uses both.
         *
         * BOTH ARE CONCAVE, NOT LINEAR, and this was applied linearly
         * until 2026-09-05. SF2.01 8.4.5 and 8.4.7 make CC 7 and CC 11
         * CONCAVE UNIPOLAR NEGATIVE modulators onto initial
         * attenuation with amount 960 cB, which fluidsynth sets up
         * verbatim at fluid_synth.c:433-441. The curve is the same
         * squared one already used for velocity a few lines above:
         *
         *     attenuation_cB = -200 * log10((cc/127)^2)
         *
         * so the gain is (cc/127)^2 rather than cc/127.
         *
         * THE ERROR GROWS AS THE FADER COMES DOWN, which is why it
         * survived: at CC 7 = 127 the two agree exactly, and only a
         * channel mixed below full shows it.
         *
         *     CC7 110 -> concave -2.50 dB, linear -1.25, err 1.25
         *     CC7  76 -> concave -8.92 dB, linear -4.46, err 4.46
         *     CC7  40 -> concave -20.1 dB, linear -10.0, err 10.0
         *
         * The user heard it as gmstriving's opening being too loud
         * after the pitch fault there was fixed. Its channel 10, the
         * timpani, sits at CC 7 = 76 and measured 4.71 dB above
         * fluidsynth - the 4.46 dB above plus the usual offset. Every
         * other channel in that passage was within a decibel.
         *
         * Squared in 14 bits: cc*cc reaches 16129, and the product
         * with the other controller's square stays inside a long.
         */
        {
            long step = v->step;
            int  looping_now;
            long limit;
            /* Hoisted per block: zero unless effects are on AND the
             * voice sends, so the frame loop's compare is one load. */
            long send_rev = r->fx_reverb ? v->send_reverb_gain : 0;
            long send_cho = r->fx_chorus ? v->send_chorus_gain : 0;
            /*
             * THE LOOP-WRAP CONDITION, ONCE PER BLOCK - NOT THREE
             * TIMES PER FRAME.
             *
             * v->looping and v->loop_to_end are set at note-on. And
             * the ONLY assignment of ST_RELEASE in this file is in
             * voice_release (:725), which runs between blocks, never
             * inside this loop: env_step moves a stage forward through
             * the earlier ones, or from RELEASE to DONE, and DONE is
             * above RELEASE in the enum. So "stage >= ST_RELEASE"
             * cannot change during a block, and hoisting it is EXACT,
             * not an approximation that holds until the next stage
             * change.
             *
             * It was evaluated at three sites per frame - the end
             * test, the second interpolation point and the mid-block
             * wrap - nine compare-and-branch pairs on a value that
             * changes once per note (design/20-synth-audit.md 1d).
             * f7badfe was a loop-wrap bug: fixtures/loop.mid is the
             * regression check and is in the bit-identity set.
             */
            looping_now = v->looping &&
                          !(v->loop_to_end && v->stage >= ST_RELEASE);
            limit       = looping_now ? v->loop_end : v->len - 1;

            for (f = 0; f < frames; f++) {
                long idx, fract, s0, s1, acc, env, menv;

                idx   = v->pos >> RENDER_FRACBITS;
                fract = v->pos & RENDER_FRACMASK;

                /*
                 * THE END TEST DEPENDS ON WHETHER WE ARE LOOPING.
                 *
                 * This used to test idx >= len-1 in both cases, so a
                 * looping voice ran past loop_end to the end of the
                 * SAMPLE before wrapping. On sample 226 - a 30-frame
                 * loop with 8 frames after it - that is a 27%
                 * overshoot on the first pass.
                 *
                 * fluidsynth sets end_index to loopend-1 when
                 * looping and end-1 when not
                 * (fluid_rvoice_dsp.cpp:235); this is that.
                 */
                {
                    if (idx >= limit) {
                        if (looping_now) {
                            long loop_len = v->loop_end - v->loop_start;
                            if (loop_len <= 0) { v->active = 0; break; }
                            while ((v->pos >> RENDER_FRACBITS) >= v->loop_end)
                                v->pos -= loop_len << RENDER_FRACBITS;
                            idx   = v->pos >> RENDER_FRACBITS;
                            fract = v->pos & RENDER_FRACMASK;
                            if (idx >= v->len - 1) { v->active = 0; break; }
                        } else {
                            v->active = 0;
                            break;
                        }
                    }
                }

                /* LINEAR INTERPOLATION, as softoss_rs.c:52-57 does at
                 * 9 bits. Without it a transposed sample is audibly
                 * grainy - this is the difference between a wavetable
                 * and a toy. */
                /*
                 * THE SECOND INTERPOLATION POINT WRAPS TO loop_start.
                 *
                 * At the last frame of a loop, data[idx+1] is whatever
                 * follows the loop in the sample - NOT the sample the
                 * loop continues into. Interpolating toward it puts a
                 * discontinuity at every loop crossing.
                 *
                 * On a long loop that is a once-per-cycle blemish. On
                 * sample 226 - THIRTY frames, crossed about 900 times
                 * a second - it is a 900 Hz buzz whose harmonics sit
                 * right on top of the note. That is the missing
                 * 1700-1800 Hz energy a listener heard as "something
                 * different around 45 to 50 seconds" in jazz.mid.
                 *
                 * fluidsynth fetches loopstart as the second point for
                 * exactly this reason (fluid_rvoice_dsp.cpp:240).
                 */
                s0 = v->data[idx];
                if (looping_now && idx + 1 >= v->loop_end)
                    s1 = v->data[v->loop_start];
                else
                    s1 = v->data[idx + 1];
                acc = s0 + (((s1 - s0) * fract) >> RENDER_FRACBITS);

                env = env_step(v);
                if (v->stage == ST_DONE) { v->active = 0; break; }

                /*
                 * MODULATION, PER FRAME.
                 *
                 * The LFOs advance every frame whether or not they
                 * have a destination, because their PHASE must stay
                 * correct - an LFO stepped only when it is audible
                 * restarts wrong the moment it becomes so.
                 *
                 * THE MODULATION ENVELOPE IS THE EXCEPTION, and it is
                 * safe for a reason the LFO argument does not cover:
                 * an envelope that modulates nothing has no phase
                 * anyone can observe. Its value is consumed only
                 * under the two tests below (menv_to_fc, and
                 * menv_to_pitch in the pitch block), and its stage is
                 * read elsewhere only by voice_release, which handles
                 * any stage. So a voice whose zone sets neither
                 * generator skips the whole state machine - one of
                 * two envelope machines per voice per frame, on most
                 * zones of most fonts.
                 *
                 * design/20-synth-audit.md section 4: 7% faster with
                 * output md5-IDENTICAL, which is the proof that
                 * nothing observed it. design/19 had rated this
                 * "medium risk" on the LFO reasoning; the measurement
                 * is what settled it.
                 */
                /*
                 * AND SINCE 2026-10-05 IT IS A MODE, AND EXACT IN BOTH
                 * (design/54 D28). REFERENCE steps every frame; FAST
                 * skips while neither depth is set and counts the
                 * skipped frames, and the first frame that needs the
                 * envelope - a depth set mid-note, or a switch to
                 * REFERENCE - replays them first (menv_catch_up()).
                 */
                if (r->menv_mode == RENDER_MENV_REFERENCE
                    || v->menv_to_pitch || v->menv_to_fc) {
                    if (v->menv_skipped > 0)
                        menv_catch_up(r, v);
                    menv = menv_step(v);
                } else {
                    v->menv_skipped++;
                    menv = 0;
                }

                /*
                 * THE FILTER, BEFORE THE ENVELOPE.
                 *
                 * Order matters and this is the SF2 signal path: the
                 * sample is filtered, then the volume envelope and
                 * the channel gains scale the result. Filtering after
                 * the envelope would make the filter's own resonance
                 * track the amplitude, which is not what a
                 * low-pass on an oscillator does.
                 *
                 * Coefficients are recomputed only when the cutoff
                 * has actually MOVED by a table step. Under
                 * modulation that is still often, but a static
                 * filtered note computes them once - and most
                 * filtered zones modulate nothing.
                 */
                if (v->filter_on) {
                    long fc = v->fc_cents;

                    if (v->mod_to_fc && v->mod_delay == 0)
                        fc += (v->mod_to_fc * lfo_value(v->mod_phase))
                              >> LFO_VALBITS;
                    if (v->menv_to_fc)
                        fc += (v->menv_to_fc * menv) >> ENV_BITS;

                    if (fc < FILTER_FC_MIN) fc = FILTER_FC_MIN;
                    if (fc > FILTER_FC_MAX) fc = FILTER_FC_MAX;

                    if (fc > v->f_last_fc + 20 || fc < v->f_last_fc - 20)
                        filter_coeffs(v, fc, r->rate);

                    /* Into float and straight back out: the filter is
                     * the only float stage, so the conversion is
                     * confined to this one line. */
                    acc = filter_out(filter_run(v, (float) acc));
                }

                if (v->vib_delay > 0) v->vib_delay--;
                else                  v->vib_phase += v->vib_step;
                if (v->mod_delay > 0) v->mod_delay--;
                else                  v->mod_phase += v->mod_step;

                /*
                 * modLfoToVolume is in CENTIBELS at full deflection,
                 * and it is bipolar - the LFO swings both ways, so
                 * tremolo makes a note alternately quieter and louder
                 * around its nominal level.
                 */
                if (v->mod_to_volume != 0 && v->mod_delay == 0) {
                    long lv = lfo_value(v->mod_phase);      /* +/-65536 */
                    int  cb = (int) ((v->mod_to_volume * lv)
                                     >> LFO_VALBITS);
                    long g  = (cb >= 0) ? centibels_to_gain(cb)
                                        : GAIN_ONE;
                    /* Boost is clamped at unity: without a headroom
                     * budget, amplifying here would clip the mix. */
                    acc = GAIN_SCALE(acc, g);
                }

                /*
                 * THE ENVELOPE MULTIPLY OVERFLOWS A 32-BIT LONG, and
                 * on the target that is not theoretical.
                 *
                 * `acc' here is the sample AFTER the filter, and a
                 * resonant low-pass rings above its input - so acc
                 * regularly exceeds 32767 even though the final
                 * output does not. env reaches ENV_ONE-1, so the
                 * product needs up to 17+16 = 33 bits. A signed long
                 * has 31 on the target and it wraps NEGATIVE: at acc
                 * 40000 the result is -25537 instead of 39999.
                 *
                 * IT ONLY BROKE ON THE TARGET, and it broke silently.
                 * Modern gcc's 64-bit long absorbs it, so every
                 * workstation render was correct while the corelcc
                 * build - the one that matters - came out 3 to 11 dB
                 * quiet with notes cancelling against each other.
                 * Found by rendering the same commit through both
                 * compilers and diffing the audio.
                 *
                 * Split the same way env_decay_step splits its own
                 * product: the high half is shifted before it is
                 * scaled, so no partial product leaves 31 bits.
                 *
                 * BUT ONLY WHEN IT HAS TO, since 2026-09-08. The split's
                 * two floors both round toward minus infinity, so every
                 * voice came out up to an LSB low on every frame, the
                 * same way - a bias that summed over a dense mix to
                 * 29 LSB peak and -52 dB RMS when the same split was
                 * tried for the gain (render.h, GAIN_BITS). A sample
                 * that fits 16 bits - every unfiltered voice, nearly
                 * every filtered frame - times a 16-bit envelope fits
                 * 31 bits with room for the rounding half:
                 * 32767 * 65535 + 32768 < 2^31 - 1. One rounded product
                 * there, the split only for the over-range ring.
                 */
                if (acc >= -32768L && acc <= 32767L) {
                    acc = (acc * env + (1L << (ENV_BITS - 1))) >> ENV_BITS;
                } else {
                    long hi = acc >> 8, lo = acc & 0xff;
                    acc = ((hi * env) >> (ENV_BITS - 8))
                        + ((lo * env) >> ENV_BITS);
                }

                /*
                 * THE VOICE'S SAMPLE, and that is all this loop does
                 * with it now - design/20 10b candidate 1, render.h.
                 * The pan and the two sends are constant for the
                 * block, so their products are formed in streaming
                 * passes after the loop rather than four scattered
                 * read-modify-writes per frame here.
                 */
                r->vbuf[f] = acc;

                /*
                 * PITCH MODULATION. Only computed when something
                 * actually drives it - most instruments modulate
                 * nothing, and this is the innermost loop of the
                 * whole synth.
                 */
                if (v->vib_to_pitch || v->mod_to_pitch || v->menv_to_pitch) {
                    long cents = 0;
                    if (v->vib_to_pitch && v->vib_delay == 0)
                        cents += (v->vib_to_pitch * lfo_value(v->vib_phase))
                                 >> LFO_VALBITS;
                    if (v->mod_to_pitch && v->mod_delay == 0)
                        cents += (v->mod_to_pitch * lfo_value(v->mod_phase))
                                 >> LFO_VALBITS;
                    if (v->menv_to_pitch)
                        cents += (v->menv_to_pitch * menv) >> ENV_BITS;

                    if (cents != 0) {
                        /*
                         * DIVIDED BEFORE MULTIPLYING - the same split
                         * the bend path makes at the top of this
                         * block, for the same reason: step and the
                         * ratio are both 12-bit fixed point and their
                         * product leaves 31 bits once step passes
                         * about 128 samples per frame, a note seven
                         * octaves above its root. gmstriving's timpani
                         * is at 28. This was the one product in the
                         * file the 2026-09-05 overflow audit missed
                         * (design/20-synth-audit.md section 8); the
                         * high half is exact and the low half cannot
                         * overflow, so the result is bit-identical.
                         */
                        long ratio = cents_to_ratio((int) cents);
                        long q     = step >> RENDER_FRACBITS;
                        long rem   = step & RENDER_FRACMASK;
                        long m     = q * ratio
                                   + ((rem * ratio) >> RENDER_FRACBITS);
                        v->pos += (m > 0) ? m : 1;
                    } else {
                        v->pos += step;
                    }
                } else {
                    v->pos += step;
                }

                /* A loop that ends mid-block still has to wrap. */
                if (looping_now &&
                    (v->pos >> RENDER_FRACBITS) >= v->loop_end) {
                    long loop_len = v->loop_end - v->loop_start;
                    if (loop_len > 0)
                        v->pos -= loop_len << RENDER_FRACBITS;
                }
            }
            /*
             * THE PASSES - one multiply per frame per destination,
             * sequential, over what the frame loop just wrote.
             * `f' is how many frames this voice actually produced:
             * a voice that ended mid-block wrote fewer, and the
             * passes must not read past it.
             *
             * The products and their order are the frame loop's:
             * GAIN_SCALE by the same block-constant coefficient, the
             * master shift on the dry pair only, and the sends
             * skipped when their gain is zero - which the compare
             * above the loop already decided for the whole block.
             */
            {
                const long  lv = (long) v->left_vol;
                const long  rv = (long) v->right_vol;
                const long *vb = r->vbuf;
                long       *ac = r->acc;
                long       *br = r->bus_reverb;
                long       *bc = r->bus_chorus;
                int         k;

                /*
                 * ONE RANGE TEST PER FRAME, not one per destination.
                 * GAIN_SCALE tests whether the sample fits 16 bits
                 * and takes the cheap single product when it does;
                 * four separate passes would test it four times and
                 * reload the sample each time, which measured SLOWER
                 * than the loop this replaced (workstation, 143 ->
                 * 150 us). So the four products share one test and
                 * one register-resident sample, exactly as the frame
                 * loop used to - what is hoisted is the coefficients,
                 * the master, the send compares and the pointers, and
                 * what is gained is a sequential walk of one buffer
                 * instead of a scattered one over four.
                 */
                for (k = 0; k < f; k++) {
                    long x = vb[k];

                    if (x >= -32768L && x <= 32767L) {
                        ac[k * 2]     += (((x * lv + (1L << (GAIN_BITS - 1)))
                                           >> GAIN_BITS) * master) >> 10;
                        ac[k * 2 + 1] += (((x * rv + (1L << (GAIN_BITS - 1)))
                                           >> GAIN_BITS) * master) >> 10;
                        if (send_rev)
                            br[k] += (x * send_rev + (1L << (GAIN_BITS - 1)))
                                     >> GAIN_BITS;
                        if (send_cho)
                            bc[k] += (x * send_cho + (1L << (GAIN_BITS - 1)))
                                     >> GAIN_BITS;
                    } else {
                        long hi = x >> 8, lo = x & 0xff;
                        long gl, gr;

                        /*
                         * CLAMPED BEFORE THE MASTER - design/24 P1. A
                         * resonant filter near the 960 cB ceiling lifts a
                         * full-scale sample to ~23 bits, the gain leaves it
                         * there, and `* master' then passed 31 bits (sooner
                         * at -g above 1.0): undefined, in practice a wrap to
                         * the opposite sign. Clamped to glim the sum stays
                         * defined and is still far past full scale, so the
                         * output clips and counts it as it should. A value
                         * that fitted is untouched, so nothing that did not
                         * overflow sounds any different.
                         */
                        gl = ((hi * lv) >> (GAIN_BITS - 8)) + ((lo * lv) >> GAIN_BITS);
                        gr = ((hi * rv) >> (GAIN_BITS - 8)) + ((lo * rv) >> GAIN_BITS);
                        if (gl > glim) gl = glim; else if (gl < -glim) gl = -glim;
                        if (gr > glim) gr = glim; else if (gr < -glim) gr = -glim;
                        ac[k * 2]     += (gl * master) >> 10;
                        ac[k * 2 + 1] += (gr * master) >> 10;
                        if (send_rev)
                            br[k] += ((hi * send_rev) >> (GAIN_BITS - 8))
                                     + ((lo * send_rev) >> GAIN_BITS);
                        if (send_cho)
                            bc[k] += ((hi * send_cho) >> (GAIN_BITS - 8))
                                     + ((lo * send_cho) >> GAIN_BITS);
                    }
                }
            }

            /* Per block, not per frame: f is what this voice rendered. */
            r->voice_frames += (unsigned long) f;
            if (v->filter_on)
                r->filtered_frames += (unsigned long) f;
        }
    }

    /*
     * THE EFFECTS STAGE - design/21 steps 2 and 3. Once per frame,
     * after every voice has fed the buses: the reverb's and the
     * chorus's wet pairs, summed and scaled by the master exactly as
     * a voice's dry pair is (the buses were summed at voice gain, see
     * the send comment above), into acc[]. It
     * lands BEFORE the saturating store on purpose, so a tail that
     * pushes the mix past full scale clips once with the dry signal
     * rather than the dry clipping and the wet being added on top.
     * The whole stage is skipped with -E off, and with it off the
     * output is what it was before the stage existed - the baseline
     * diff is the guard.
     */
    if (r->effects) {
        /*
         * A BLOCK AT A TIME since 2026-09-09 (design/20 10b): the
         * per-frame calls re-read eight combs' state 256 times a
         * block and let the compiler keep nothing live across them.
         * The two effects sum into one wet pair - reverb_block and
         * chorus_block ADD - and the master is applied once per
         * frame over that pair, exactly as the per-frame form did.
         * Bit-identical; effects.h has the argument.
         */
        int f;
        memset(r->wet_l, 0, (size_t) frames * sizeof r->wet_l[0]);
        memset(r->wet_r, 0, (size_t) frames * sizeof r->wet_r[0]);
        /* EACH ITS OWN SWITCH (2026-10-05) - an effect that is off is
         * neither fed above nor run here, so its delay lines keep what
         * they held, exactly as both did under -E off before. */
        if (r->fx_reverb)
            reverb_block(&r->reverb, r->bus_reverb, r->wet_l, r->wet_r,
                         frames);
        if (r->fx_chorus)
            chorus_block(&r->chorus, r->bus_chorus, r->wet_l, r->wet_r,
                         frames);
        for (f = 0; f < frames; f++) {
            r->acc[f * 2]     += (r->wet_l[f] * master) >> 10;
            r->acc[f * 2 + 1] += (r->wet_r[f] * master) >> 10;
        }
    }

    /* THE ONE SATURATING STORE - the only place the mix clips, and
     * where it is counted. Per sample, not per frame. */
    {
        int n = frames * 2;
        for (i = 0; i < n; i++) {
            long s = r->acc[i];
            if (s > 32767)       { s = 32767;  r->clipped++; }
            else if (s < -32768) { s = -32768; r->clipped++; }
            out[i] = (short) s;
        }
    }
}

void
render_mix(render_state *r, short *out, int frames)
{
    /* Any frame count, RENDER_MIX_FRAMES at a time - see render.h. */
    while (frames > 0) {
        int n = frames < RENDER_MIX_FRAMES ? frames : RENDER_MIX_FRAMES;
        mix_block(r, out, n);
        out    += 2 * n;
        frames -= n;
    }
}

/*
 * LOWER THE CEILING, and report what was actually taken.
 *
 * CLAMPED RATHER THAN REFUSED, because this is a measurement knob and
 * a sweep will be given values out of range - by a typo as readily as
 * on purpose. A zero would render silence and a negative would render
 * nothing while looking like it worked, which is the failure that
 * wastes a boot.
 *
 * Returning the value taken lets the caller PRINT it rather than
 * assume it: the block-size sweep was only readable because each run
 * stated its own configuration in its own log, and the same applies
 * here.
 *
 * NOT SAFE TO LOWER MID-RENDER, and deliberately not guarded against.
 * Voices above the new limit keep sounding until they end naturally -
 * every loop that would release or steal them now stops short of
 * them, so they cannot be reached. Call it once, after render_init,
 * before rendering. The daemon does exactly that.
 */
void
render_set_velfilter(render_state *r, unsigned modflags)
{
    if (r != NULL)
        r->modflags = modflags & (SF2_MOD_VF_MASK | SF2_MOD_LAW_MASK);
}

void
render_set_effects(render_state *r, int on)
{
    if (r != NULL) {
        r->fx_reverb = r->fx_chorus = on ? 1 : 0;
        r->effects   = r->fx_reverb;
    }
}

void
render_set_reverb(render_state *r, int on)
{
    if (r != NULL) {
        r->fx_reverb = on ? 1 : 0;
        r->effects   = r->fx_reverb || r->fx_chorus;
    }
}

void
render_set_chorus(render_state *r, int on)
{
    if (r != NULL) {
        r->fx_chorus = on ? 1 : 0;
        r->effects   = r->fx_reverb || r->fx_chorus;
    }
}

int
render_fx_parse(const char *word)
{
    if (word == NULL)
        return -1;
    if (strcmp(word, "on") == 0)     return RENDER_FX_BOTH;
    if (strcmp(word, "off") == 0)    return 0;
    if (strcmp(word, "reverb") == 0) return RENDER_FX_REVERB;
    if (strcmp(word, "chorus") == 0) return RENDER_FX_CHORUS;
    return -1;
}

const char *
render_fx_name(int mask)
{
    switch (mask & RENDER_FX_BOTH) {
    case RENDER_FX_BOTH:   return "on";
    case RENDER_FX_REVERB: return "reverb";
    case RENDER_FX_CHORUS: return "chorus";
    default:               return "off";
    }
}

void
render_set_fx(render_state *r, int mask)
{
    render_set_reverb(r, (mask & RENDER_FX_REVERB) != 0);
    render_set_chorus(r, (mask & RENDER_FX_CHORUS) != 0);
}

void
render_set_filter_bypass(render_state *r, int on)
{
    if (r != NULL)
        r->filter_bypass = on ? 1 : 0;
}

void
render_set_menv_mode(render_state *r, int mode)
{
    if (r != NULL)
        r->menv_mode = (mode == RENDER_MENV_FAST) ? RENDER_MENV_FAST
                                                  : RENDER_MENV_REFERENCE;
}

void
render_set_master(render_state *r, long gain_x1024)
{
    if (r == NULL)
        return;
    if (gain_x1024 < 1)                 gain_x1024 = 1;
    if (gain_x1024 > RENDER_MASTER_MAX) gain_x1024 = RENDER_MASTER_MAX;
    r->master = gain_x1024;
}

void
render_set_grow(render_state *r, int on)
{
    r->grow = on ? 1 : 0;
    /* THE WHOLE BOUND, NOW - alloc_voice() grows the ceiling one voice
     * at a time and must not allocate (see there). If this falls
     * short, growing simply stops at what was reserved. */
    if (r->grow)
        (void) voices_reserve(r, RENDER_MAX_VOICES);
}

int
render_set_max_voices(render_state *r, int n)
{
    if (n < 1)
        n = 1;
    if (n > RENDER_MAX_VOICES)
        n = RENDER_MAX_VOICES;
    /* GROWN IF NEEDED, never shrunk - a lowered ceiling may still
     * have voices sounding above it, and their storage must stay. */
    n = voices_reserve(r, n);
    if (n < 1)
        return r->max_voices;   /* no array at all: leave it alone */
    r->max_voices = n;
    return n;
}

/*
 * CHANGE THE OUTPUT RATE ON A LIVE RENDERER - design/43 section 5b.
 *
 * WHY THIS CAN EXIST AT ALL, and it is not obvious: `r->rate' is read
 * AT NOTE-ON, never cached in the state. Every rate-derived quantity
 * is computed per voice when it is allocated - the volume envelope at
 * `:1903', the pitch at `:1787', the LFO steps at `:2001' - so
 * setting the field makes every SUBSEQUENT note correct while
 * sounding voices keep what they resolved with. That is the same
 * contract render_set_velfilter() and render_set_filter_bypass()
 * already have.
 *
 * WHAT IT IS FOR: a daemon whose output device CHANGED. vmidid can
 * start on a real card, adopt whatever that card granted, and later
 * rebind to vsound - or the reverse, where an SB Pro clamps stereo to
 * 22050 and the renderer must follow or play an octave down
 * (design/42 section 4b). The alternative was render_init(), which
 * memsets the whole state and loses every channel's programs,
 * controllers and pitch bend.
 *
 * THE EFFECTS ARE RE-DERIVED AND THEIR TAILS ARE LOST. reverb_init()
 * and chorus_init() memset their delay lines, so whatever was ringing
 * is discarded. Acceptable because the only caller changes rate at a
 * point where nothing has sounded for RELEASE_MS - vmidid's 3000 ms
 * default, which its own comment says outlasts the tail.
 *
 * REFUSES OUT OF RANGE rather than clamping, and returns what is in
 * force either way - a caller that asked for something impossible
 * must be able to say so rather than silently transposing. The
 * ceiling is RENDER_RATE_MAX (46340, a 32-bit bound in
 * voice_set_pitch - design/24 B2), which is BELOW what a card may
 * grant: 48000 is legal for a card and not for this synth.
 */
int
render_set_rate(render_state *r, int rate)
{
    if (rate < RENDER_RATE_MIN || rate > RENDER_RATE_MAX)
        return r->rate;
    if (rate == r->rate)
        return r->rate;
    r->rate = rate;
    reverb_init(&r->reverb, rate);
    chorus_init(&r->chorus, rate);
    return r->rate;
}

int
render_active(const render_state *r)
{
    int i, n = 0;
    for (i = 0; i < r->nalloc; i++)
        if (r->voices[i].active)
            n++;
    return n;
}

/*
 * SHED RELEASE TAILS - see render.h. Built 2026-10-02 for automatic
 * voice reduction on TiMidity's model: its reduce_voice()
 * (playmidi.c:979) takes "the decaying note with the smallest volume"
 * and protects drum decays, because "truncating them early sounds bad,
 * especially on snares and cymbals". TiMidity frees the voice outright
 * and its own comment admits that "can still cause a click"; this
 * fades the tail over 10 ms instead - fast enough to free the slot
 * within the controller's settle time, slow enough not to click.
 *
 * NOT THE EXCLUSIVE-CLASS TIME, which a first version reused: that is
 * -2000 timecents, about 315 ms, and a scratch test showed six shed
 * tails all still sounding 35 ms later. A third of a second does not
 * relieve a draining buffer.
 */
int
render_shed_tails(render_state *r, int n)
{
    long fast = r->rate / 100 > 0 ? r->rate / 100 : 1;   /* 10 ms */
    int  done = 0;

    while (done < n) {
        int i, best = -1;

        for (i = 0; i < r->nalloc; i++) {
            render_voice *v = &r->voices[i];

            if (!v->active || v->stage != ST_RELEASE || v->shed
                || r->chan[v->channel].drum)
                continue;
            if (best < 0 || v->env_level < r->voices[best].env_level)
                best = i;
        }
        if (best < 0)
            break;
        {
            render_voice *v = &r->voices[best];

            /* THE SAME MULTIPLY voice_release() SET UP, at the fast
             * time, from wherever the level is now. */
            v->env_rate = env_decay_drop(fast);
            v->env_sub  = v->env_level << ENV_SUBBITS;
            if (v->env_count > fast)
                v->env_count = fast;
            v->shed = 1;
        }
        r->tails_shed++;
        done++;
    }
    return done;
}

void
render_voice_counts(const render_state *r, int *active, int *held)
{
    int i, a = 0, h = 0;

    for (i = 0; i < r->nalloc; i++) {
        if (!r->voices[i].active)
            continue;
        a++;
        if (r->voices[i].stage < ST_RELEASE || r->voices[i].held)
            h++;
    }
    if (active != NULL)
        *active = a;
    if (held != NULL)
        *held = h;
}
