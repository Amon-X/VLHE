/*
 * effects.h - the effects stage: reverb (and, step 3, chorus).
 * design/21-effects.md. C89, gcc 2.95.2 clean, integer only.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * THE REVERB IS A SCHROEDER-MOORER REVERBERATOR - the Freeverb
 * topology (Jezar at Dreampoint, 2000, public domain; Smith,
 * "Physical Audio Signal Processing", the Freeverb page): per channel
 * eight parallel lowpass-feedback combs summed, then four allpasses in
 * series, the right channel's delays 23 samples longer than the
 * left's for width. What fluidsynth shipped for a decade; its current
 * FDN is a taste question for later (design/21 section 5).
 *
 * PARAMETERS ARE fluidsynth 2.5.7's DEFAULTS, through Freeverb's own
 * scalings, so a level comparison against it is like for like:
 *
 *     room size 0.7  ->  feedback  = 0.7 * 0.28 + 0.7 = 0.896
 *                        (fluidsynth's default is 0.5 -> 0.84; 0.7 is
 *                         the user's choice, 2026-09-08, for a tail
 *                         nearer fluidsynth's FDN's. Its steady comb
 *                         gain is 1/(1 - 0.896) = 9.6, still inside
 *                         the widths below, and the input gain's trim
 *                         was re-measured for it - effects.c.)
 *     damping   0.2  ->  damp1     = 0.2 * 0.4        = 0.08
 *     width     0.8  ->  wet1/wet2 = 0.9 / 0.1 of the wet level
 *     level     0.7  ->  wet       = 0.7 * 3 (scalewet) = 2.1
 *     input gain (fixedgain)         0.015, plus a measured trim
 *                                    onto fluidsynth 2.5.7's level
 *
 * THE FIXED POINT, worked out so that no product leaves 31 bits on
 * the target, with the bus's realistic peak taken as 2^16 and clamped
 * there on entry (the one guard that makes the rest exact):
 *
 *     stage             value bits   coefficient   product bits
 *     input             16           REV_IN  Q15   31   (bus * gain*wet*4)
 *     comb store/ring   <= 16        Q13           29
 *     sum of 8 combs    <= 19        -             -
 *     allpass out       <= 20        (0.5 = >> 1)  -
 *     width mix         <= 20        Q10           30
 *
 * The internal signal carries two extra fractional bits (the "* 4" in
 * REV_IN, taken back out with the width mix's shift) so the tail's
 * quantisation floor sits below the output's, not above it.
 *
 * MEMORY, all `long' (the user's decision 2026-09-08: fewer
 * operations and no per-stage clamp to get wrong, for 50 KB nobody
 * misses beside a 10 MB sample pool): 8 combs x 2 channels x up to
 * 1640 samples + 4 allpasses x 2 x up to 579, ~123 KB.
 */
/*
 * THE CHORUS IS fluidsynth 2.5.7's SHAPE (rvoice/fluid_chorus.c),
 * read not copied: one 2048-sample delay line, N = 3 taps read at a
 * delay modulated by a sine LFO per tap, phases 120 degrees apart,
 * the LFO updated every 5 frames (its mod_rate for this depth); the
 * three taps into a two-bus "stereo unit" (taps 0 and 2 left, 1 and 2
 * right) mixed by wet1/wet2. Its defaults through its own scalings:
 *
 *     depth 4.25 ms -> 187 samples peak to peak, 93 either side of a
 *                      94-sample centre: the delay swings 1..187
 *     speed 0.2 Hz
 *     level 0.6, width fixed at 10 ->
 *                      wet  = 0.6 / (1 + 10 * 0.2)   = 0.2
 *                      wet1 = wet * (10 / 2 + 0.5)   = 1.1
 *                      wet2 = wet * (1 - 10) / 2     = -0.9
 *
 * Ours reads the fractional delay with LINEAR interpolation where
 * fluidsynth uses a first-order allpass (design/21 section 5): the
 * same two reads and one product, no state, and so no feedback path
 * to hold a limit cycle. Nothing here recirculates - the line is
 * written once and read three times - so unlike the reverb the
 * shifts can floor and nothing sticks.
 *
 * Fixed point: the bus is clamped to +/- 65535 on entry as the
 * reverb's is; a tap difference is 18 bits, times a Q12 fraction is
 * 30; a bus of two taps is 18 bits, times a Q10 wet coefficient is
 * 29. The LFO phase is 32 bits per turn (masked, so the 64-bit host
 * build wraps where the target does), 0.2 Hz * 5 / rate * 2^32 per
 * update; a 64-entry Q14 sine table with linear interpolation.
 */
#ifndef EFFECTS_H
#define EFFECTS_H

#define REV_COMBS       8
#define REV_ALLPASSES   4
#define REV_SPREAD      23              /* right channel's extra delay */
#define REV_COMB_MAX    (1617 + REV_SPREAD)
#define REV_AP_MAX      (556 + REV_SPREAD)

typedef struct {
    long buf[REV_COMB_MAX];
    int  len, idx;
    long store;                         /* the lowpass's memory */
} rev_comb;

typedef struct {
    long buf[REV_AP_MAX];
    int  len, idx;
} rev_allpass;

/*
 * The longest block reverb_block() will be given. render.h's
 * RENDER_MIX_FRAMES is 1024 and the daemon uses 256; the scratch
 * below is sized for the larger and lives in the state rather than
 * on the stack, which a daemon this close to the kernel should not
 * be growing by 8 KB a call.
 */
#define REV_BLOCK_MAX   1024

typedef struct {
    rev_comb    comb[2][REV_COMBS];
    rev_allpass ap[2][REV_ALLPASSES];
    long        feedback, damp1, damp2; /* Q13 */
    long        wet1, wet2;             /* Q10 */
    long        in_gain;                /* Q15, includes wet and the x4 */
    /* reverb_block()'s scratch: the combs' running sums per channel
     * and the gained input, both per frame of the block. */
    long        sum[2][REV_BLOCK_MAX];
    long        x[REV_BLOCK_MAX];
} reverb_state;

#define CHO_LINE        2048            /* samples, a power of two */
#define CHO_MASK        (CHO_LINE - 1)
#define CHO_TAPS        3
#define CHO_MOD_RATE    5               /* frames per LFO update */

typedef struct {
    long          line[CHO_LINE];
    int           in;                   /* next write index */
    int           tick;                 /* frames since the LFO moved */
    unsigned long phase[CHO_TAPS];      /* 32-bit turns */
    unsigned long step;                 /* per update */
    long          depth;                /* samples either side, 93 */
    long          centre;               /* Q12 delay at LFO zero, 94 */
    long          dq[CHO_TAPS];         /* each tap's delay, Q12 */
    long          wet1, wet2;           /* Q10 */
} chorus_state;

/* Set the delays for the sample rate and the defaults above. */
void reverb_init(reverb_state *st, int rate);
void chorus_init(chorus_state *st, int rate);

/*
 * One frame: the mono send bus in, the wet pair out. `in' is clamped
 * to +/- 65535 on entry; the outputs are in the same scale as the
 * mix accumulator before the master.
 */
void reverb_frame(reverb_state *st, long in, long *out_l, long *out_r);
void chorus_frame(chorus_state *st, long in, long *out_l, long *out_r);

/*
 * A WHOLE BLOCK, and this is what the mix loop calls - design/20
 * 10b, 2026-09-09. `in' holds `n' bus samples; `out_l' and `out_r'
 * are ADDED to, not written, so the two effects sum into the same
 * pair without a temporary.
 *
 * WHY A BLOCK FORM AT ALL. Called per frame, the reverb re-reads its
 * eight combs' state and indices from memory 256 times a block and
 * the compiler can keep nothing live across a call boundary; per
 * block, one comb's state and ring pointer stay in registers while
 * its whole 256 samples go by, and the ring is walked linearly
 * instead of touched once per frame. The arithmetic is UNCHANGED and
 * in the same order - each comb is strictly sequential in time and
 * nothing outside the effects touches its state mid-block - so the
 * output is BIT-IDENTICAL to the per-frame form, which is the
 * acceptance for the change.
 *
 * It is also the prerequisite for any MMX in the effects: MMX shares
 * the x87 register file, so the machine must enter and leave it once
 * per block rather than once per frame beside the voice filter.
 *
 * The per-frame functions stay: they are the reference these are
 * checked against, and the block forms are written to be obviously
 * the same operations.
 */
void reverb_block(reverb_state *st, const long *in, long *out_l,
                  long *out_r, int n);
void chorus_block(chorus_state *st, const long *in, long *out_l,
                  long *out_r, int n);

#endif /* EFFECTS_H */
