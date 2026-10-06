/*
 * vsound_conv.c - normalise a channel's audio to the mixing format.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 * The FreeBSD-derived parts are BSD-2-Clause; their notice is below.
 *
 * Step 7 of design/06-softoss-design.md, and step 5 quietly depended on
 * it: the mixer adds raw bytes, which is only correct if every channel
 * is already in the same format. FreeBSD guarantees that by having
 * every chain stage call feeder_build_formatne() first
 * (feeder_chain.c:195, 215, 293, 350, 418) - feed_mixer_feed() can add
 * bytes precisely BECAUSE conversion happened upstream. This is that
 * guarantee.
 *
 * Shapes taken from FreeBSD's sound/pcm, BSD-2-Clause:
 *
 *     Copyright (c) 2008-2009 Ariff Abdullah <ariff@FreeBSD.org>
 *     Copyright (c) 1999 Cameron Grant <cg@FreeBSD.org>
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * The stages, in the order FreeBSD applies them:
 *
 *     format  -> intpcm.h:14-52, feeder_format.c:250-255
 *     matrix  -> feeder_matrix.c:124-178 (mono -> stereo)
 *     rate    -> feeder_rate.c, linear or zero-order hold
 *
 * The working format is S16_LE stereo, which is FEEDER_CHAIN_LEAN's
 * choice (feeder_chain.c:76, 84-88) and what the card is opened at.
 */

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#include <linux/soundcard.h>
#else
#include <string.h>
#include <stddef.h>
#ifndef AFMT_S16_LE
#include <sys/soundcard.h>
#endif
#endif

#include "vsound_chan.h"

/*
 * READ ONE SAMPLE, NORMALISED.
 *
 * intpcm.h's discipline: every format becomes a common intermediate on
 * read, so each later operation is written once rather than once per
 * format pair. FreeBSD normalises to 32 bits (8-bit << 24, 16-bit
 * << 16); we normalise to 16, because S16 is the working format and a
 * 32-bit intermediate would only be shifted straight back down.
 *
 * Unsigned becomes signed by XORing the sign bit, which is exactly
 * pcm.h:136-139 - `INTPCM_T((int16_t)(*((uint16_t *)(b8)) ^ 0x8000))'.
 * Nothing downstream ever sees an unsigned sample.
 */
static int
vsound_conv_read(const unsigned char *p, int format)
{
    int v;

    switch (format) {
    case AFMT_S16_LE:
        v = p[0] | (p[1] << 8);
        return v >= 32768 ? v - 65536 : v;
    case AFMT_U16_LE:
        v = (p[0] | (p[1] << 8)) ^ 0x8000;
        return v >= 32768 ? v - 65536 : v;
    case AFMT_S8:
        v = p[0];
        return (v >= 128 ? v - 256 : v) << 8;
    case AFMT_U8:
    default:
        /* 0x80 is silence for unsigned 8-bit, so the XOR maps it to 0. */
        v = p[0] ^ 0x80;
        return (v >= 128 ? v - 256 : v) << 8;
    }
}

/*
 * NOT static: vsound_mix.c calls this to round `count' to a whole
 * output frame (feeder_mixer.c:300-303, design 07's section 7.2).
 *
 * It was static, and the host tests did not notice because each one
 * #includes the .c file it is testing - so the call resolved within one
 * translation unit. Only the module link found it, as an unresolved
 * vsound_conv_bps in vsound.o. Another thing that is invisible until
 * the real build runs.
 */
int
vsound_conv_bps(int format)
{
    switch (format) {
    case AFMT_S16_LE:
    case AFMT_U16_LE:
        return 2;
    default:
        return 1;
    }
}

static void
vsound_conv_write(unsigned char *p, int v)
{
    if (v > 32767)       v = 32767;
    else if (v < -32768) v = -32768;
    p[0] = (unsigned char) (v & 0xff);
    p[1] = (unsigned char) ((v >> 8) & 0xff);
}

/*
 * THE RESAMPLER CONSTANTS, from FreeBSD's generator rather than from
 * inference.
 *
 * feeder_rate.c uses these and never defines them: they come from
 * feeder_rate_gen.h, which is produced at build time by
 * sys/tools/sound/feeder_rate_mkfilter.awk and is not in the FreeBSD
 * repository at all. The generator is saved as
 * contrib/refs/feeder_rate_mkfilter.awk.ref; these are its values
 * (mkfilter.awk:625, 638-641).
 *
 * NOTES-freebsd-rate.md has every rate pair we can meet checked for
 * overflow - seven source rates against both card rates, all 32-bit
 * safe with a factor of four in hand.
 */
#define VSOUND_Z_LINEAR_FULL_SHIFT  30
#define VSOUND_Z_LINEAR_FULL_ONE    (1L << VSOUND_Z_LINEAR_FULL_SHIFT)
#define VSOUND_Z_LINEAR_SHIFT       8
#define VSOUND_Z_LINEAR_UNSHIFT     (VSOUND_Z_LINEAR_FULL_SHIFT \
                                     - VSOUND_Z_LINEAR_SHIFT)
#define VSOUND_Z_LINEAR_ONE         (1 << VSOUND_Z_LINEAR_SHIFT)

/*
 * genlerp() emits this for 16-bit, taking the cheap branch because
 * 16 + Z_LINEAR_SHIFT is 24 and that is <= 32 (mkfilter.awk:496-528).
 * All 32-bit: int64_t appears only at 24-bit and above.
 */
#define VSOUND_Z_LERP(z, x, y)  ((x) + ((((y) - (x)) * (z)) \
                                        >> VSOUND_Z_LINEAR_SHIFT))

/* Greatest common divisor, feeder_rate.c:399-411 verbatim in shape. */
static long
vsound_conv_gcd(long x, long y)
{
    long w;

    while (y != 0) {
        w = x % y;
        x = y;
        y = w;
    }
    return x;
}

/*
 * Set up a channel's conversion. Call whenever the negotiated format or
 * the mixing format changes; cheap enough to call on every open.
 *
 * The ratio is reduced by GCD first (feeder_rate.c:1105-1110), which
 * keeps the phase arithmetic in small integers and makes an exact ratio
 * cost nothing: 11025 -> 44100 becomes 1/4.
 */
void
vsound_conv_setup(struct vsound_chan *c, int mix_rate)
{
    long g;

    if (c == NULL || c->rate <= 0 || mix_rate <= 0)
        return;

    g = vsound_conv_gcd((long) c->rate, (long) mix_rate);
    c->z_gx = (int) ((long) c->rate / g);
    c->z_gy = (int) ((long) mix_rate / g);

    /*
     * feeder_rate.c:1235-1241, and its comment is the whole design:
     *
     *     "Don't put much effort if we're doing linear interpolation.
     *      Just center the interpolation distance within Z_LINEAR_ONE,
     *      and be happy about it."
     *
     * Direction-agnostic: gx/gy is a plain ratio, so downsampling
     * (48000 -> 44100 on an SB16, gx > gy) needs no special case.
     */
    c->z_dx    = (c->z_gy > 0)
               ? (unsigned long) (VSOUND_Z_LINEAR_FULL_ONE / c->z_gy) : 0;
    c->z_alpha = 0;
    c->z_pos   = 0;
    c->z_prev0 = 0;
    c->z_prev1 = 0;
    c->z_cur0  = 0;
    c->z_cur1  = 0;
    c->z_primed = 0;
}

/*
 * Convert from `in' - `inbytes' of the channel's format, PEEKED from the
 * ring and not yet disposed - into S16_LE stereo at `mix_rate', writing
 * at most `outmax' bytes.
 *
 * Returns bytes written, or a negative errno. REPORTS IN c->z_pos HOW
 * MANY WHOLE INPUT FRAMES OF `in' IT CONSUMED, and the caller disposes
 * exactly that many from the ring - never `inbytes'. That report is
 * the contract, and its absence was design/25 section 4.1.
 *
 * THE CONTRACT IN FULL:
 *
 *   - The interpolator holds TWO frames across calls: z_prev, the
 *     frame behind the phase, and z_cur, the frame ahead of it. Every
 *     output sample is computed from those two and z_alpha alone; `in'
 *     is read only when the phase moves on.
 *   - A frame of `in' is consumed when the phase crosses INTO it: it
 *     becomes z_cur and the old z_cur becomes z_prev. A frame the phase
 *     has not reached is not consumed, is not reported, and stays in
 *     the ring for the next call.
 *   - The FIRST call primes both: frames 0 and 1 of `in' become z_prev
 *     and z_cur and count as consumed. With fewer than two frames it
 *     produces nothing, consumes nothing, and waits.
 *   - When `in' holds fewer frames than `outmax' samples would cross,
 *     it produces every sample whose two frames are in hand and
 *     returns short. The crossing the last sample owes is left in
 *     z_alpha - at or above z_gy - and paid at the start of the next
 *     call, from the next call's frames. Nothing is skipped and
 *     nothing repeats. A short return is a short pull to the mixer,
 *     which it handles.
 *
 * WHY IT IS SHAPED THIS WAY. Before 2026-09-14 the caller disposed
 * everything it handed over and this function read the frame AHEAD of
 * the phase straight out of `in' - so at the end of a call that frame,
 * read but not yet crossed into, was already gone from the ring, and
 * the next call's frame 0 took its place. One input frame lost per
 * pass whenever the phase did not happen to cross on the last sample:
 * every pass at 1:1, three in four at 1:4, one in two at 1:2 - measured
 * on the host, design/25 section 4.1, heard as a 100 Hz comb and a
 * pitch 0.68 % sharp at 11025. The copy path never comes here and was
 * clean, which is why every listening test on this project missed it.
 * Carrying the ahead frame in the channel is what makes "consumed" a
 * number this function can report.
 *
 * THE PHASE IS CARRIED IN THE CHANNEL, not recomputed per call. That is
 * the difference between this and vcd/mix.c:249, which derives the
 * source index from the output index and therefore restarts phase at
 * every fragment boundary - inaudible at an exact ratio like 1:4, and a
 * dropped or duplicated fraction of a frame at every boundary
 * otherwise. FINDINGS.md item 9.
 */
int
vsound_conv_run(struct vsound_chan *c, unsigned char *out, int outmax,
                const unsigned char *in, int inbytes, int mix_rate,
                int zoh)
{
    int sbps, frame, inframes, consumed = 0, produced = 0, starved = 0;

    if (c == NULL || out == NULL || in == NULL)
        return -1;
    /* Every exit reports; a refusal consumed nothing. */
    c->z_pos = 0;
    if (c->rate <= 0 || mix_rate <= 0 || c->z_gy <= 0 || c->z_gx <= 0)
        return -1;

    sbps  = vsound_conv_bps(c->format);
    frame = sbps * (c->channels > 1 ? 2 : 1);
    if (frame <= 0)
        return -1;

    inframes = inbytes / frame;
    if (inframes <= 0)
        return 0;

    /*
     * Prime the interpolator ONCE per stream, from TWO frames.
     *
     * z_prev must hold the frame behind the phase and z_cur the one
     * ahead of it, and at the very start there is neither. Priming
     * both from frame 0 would make the first outputs interpolate from
     * a sample to ITSELF - four frames of silence at a 1:4 ratio,
     * which is what the first attempt did.
     *
     * So frame 0 becomes prev and frame 1 becomes cur, both consumed:
     * the first output then interpolates from frame 0 toward frame 1,
     * which is correct and costs one frame of latency rather than four
     * of silence. With only one frame in hand there is nothing to
     * interpolate toward, so wait for the second rather than guess.
     */
    if (!c->z_primed) {
        if (inframes < 2)
            return 0;
        c->z_prev0 = vsound_conv_read(in, c->format);
        c->z_prev1 = (c->channels > 1)
                   ? vsound_conv_read(in + sbps, c->format) : c->z_prev0;
        c->z_cur0  = vsound_conv_read(in + frame, c->format);
        c->z_cur1  = (c->channels > 1)
                   ? vsound_conv_read(in + frame + sbps, c->format)
                   : c->z_cur0;
        consumed   = 2;
        c->z_primed = 1;
    }

    while (produced + 4 <= outmax && !starved) {
        int l, r;

        /*
         * FIRST SETTLE THE PHASE, then produce.
         *
         * z_alpha counts up by z_gx per output sample and every whole
         * z_gy in it is a crossing into the next input frame: what was
         * ahead of the phase becomes the frame behind it, and the next
         * frame of `in' becomes the one ahead. Doing this at the TOP
         * of the iteration rather than after the write is what lets a
         * crossing wait: if the frame it needs is not in `in' yet, the
         * sample past it is not produced, z_alpha stays owing, and the
         * next call settles it first. Carrying the remainder is what
         * makes 147/640 exact over its whole period.
         */
        while (c->z_alpha >= c->z_gy) {
            const unsigned char *p;

            if (consumed >= inframes) {
                starved = 1;            /* need more input */
                break;
            }
            p = in + (long) consumed * frame;
            c->z_alpha -= c->z_gy;
            c->z_prev0 = c->z_cur0;
            c->z_prev1 = c->z_cur1;
            c->z_cur0  = vsound_conv_read(p, c->format);
            c->z_cur1  = (c->channels > 1)
                       ? vsound_conv_read(p + sbps, c->format) : c->z_cur0;
            consumed++;
        }
        if (starved)
            break;

        if (zoh) {
            /*
             * ZERO ORDER HOLD - feeder_rate.c:434-455. The nearest
             * input sample, repeated. Exact at an integer ratio and
             * a staircase otherwise; kept as a fallback because it
             * is what vcd shipped and it costs nothing to offer.
             */
            l = c->z_cur0;
            r = c->z_cur1;
        } else {
            /*
             * LINEAR - feeder_rate.c:464-490, using the macro
             * genlerp() emits for 16 bits. `z' is the interpolation
             * distance scaled into Z_LINEAR_ONE. From prev toward
             * cur, both in hand, z_alpha below z_gy.
             */
            int z = (int) (((unsigned long) c->z_alpha * c->z_dx)
                           >> VSOUND_Z_LINEAR_UNSHIFT);

            l = VSOUND_Z_LERP(z, c->z_prev0, c->z_cur0);
            r = VSOUND_Z_LERP(z, c->z_prev1, c->z_cur1);
        }

        vsound_conv_write(out + produced,     l);
        vsound_conv_write(out + produced + 2, r);
        produced += 4;

        /* Advance the phase by one output sample. Settled at the top
         * of the next iteration, or of the next call. */
        c->z_alpha += c->z_gx;
    }

    /* THE REPORT. Frames of `in' this call consumed - the caller
     * retires exactly these from the ring and no more. z_alpha, z_prev
     * and z_cur are NOT reset: they are the phase, and carrying them is
     * the point - including a z_alpha at or above z_gy, which is a
     * crossing owed to the next call. */
    c->z_pos = consumed;
    return produced;
}
