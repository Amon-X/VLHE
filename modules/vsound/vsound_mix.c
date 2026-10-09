/*
 * vsound_mix.c - combine every running channel into one stream.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 * The FreeBSD-derived parts are BSD-2-Clause; their notice is below.
 *
 * Step 5 of design/06-softoss-design.md. The shape is FreeBSD's
 * feed_mixer_feed() (feeder_mixer.c:282-390), which is in
 * freebsd-pcm-reference/ under BSD-2-Clause:
 *
 *     Copyright (c) 2008-2009 Ariff Abdullah <ariff@FreeBSD.org>
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
 * THREE DECISIONS ARE TAKEN FROM THE REFERENCE AND EACH ONE MATTERS:
 *
 * 1. The FIRST channel with data writes STRAIGHT INTO the destination;
 *    every later one is mixed in. There is no clear-then-add pass, so
 *    a single running channel costs one copy and no arithmetic at all.
 *
 * 2. A SHORT first channel leaves a GAP that is remembered, not filled.
 *    It is zeroed only if a later channel has audio for that region;
 *    if none does, those bytes are never sent. Filling eagerly would
 *    write silence over a region another channel was about to supply,
 *    and sending them regardless is what replays a stale buffer tail -
 *    which is what "a continuous loud tone until I quit quake" sounds
 *    like.
 *
 * 3. A MAPPED channel's whole free region is declared ready before
 *    reading, without inspecting anything. FreeBSD does this at
 *    feeder_mixer.c:330-332 and channel.c:400-401. An mmap client
 *    writes into the buffer and issues no syscall, so there is nothing
 *    to observe; any attempt to work out what it wrote is a guess. The
 *    old tree's harvest guessed, and design/06-softoss-design.md
 *    section 4c records the 44-byte-per-tick drift that produced.
 *    There is no harvest here.
 */

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#include <linux/soundcard.h>
#include <linux/sched.h>        /* jiffies, for the trace timestamp */
#include <linux/kernel.h>       /* printk - this file had none until
                                 * 2026-08-27, which is exactly why the
                                 * mixer had no visibility at all */
#else
#include <string.h>
#include <stddef.h>
#ifndef AFMT_S16_LE
#include <sys/soundcard.h>
#endif
#endif

#include "vsound_chan.h"

/* Its own rate-limit counter: a busy site must not silence a quiet one.
 * See VSOUND_RL in vsound_chan.h. */
unsigned long vsound_rl_chan;
extern int vsound_rate_mix;


/* vsound_conv_run() - the normalisation the mixer depends on. */
extern int vsound_conv_run(struct vsound_chan *, unsigned char *, int,
                           const unsigned char *, int, int, int);
/* vsound_conv_bps() - bytes per sample for a format; the mixer needs it
 * to round `count' to a whole output frame (feeder_mixer.c:300-303). */
extern int vsound_conv_bps(int);

/*
 * Add src into dst, saturating.
 *
 * feeder_mixer.c:49-70, and vcd/mix.c:34-56 already does the same
 * thing for the userspace mixer - widen, add, clamp. Kept separate
 * rather than shared because that one takes a different argument
 * shape and lives in a program that is not this module.
 *
 * Saturation rather than wraparound: two loud samples summing past
 * full scale wrap into the opposite sign, heard as a click. Clamping
 * flattens the peak instead, which is what every mixer does.
 *
 * Samples are assembled from bytes rather than read through a short
 * pointer: a channel's buffer is vmalloc'd and a fragment boundary
 * carries no alignment guarantee. Endianness is explicit for the same
 * reason - AFMT_S16_LE is little-endian whatever the host is.
 */
/*
 * SAMPLES THAT SATURATED IN THIS PASS, and the total number of passes
 * in which any did.
 *
 * WHY IT IS COUNTED AT ALL. The clamp below is silent: four streams at
 * full scale sound identical to four streams that are clipping a
 * little, and "it sounded fine" cannot tell them apart. `design/06'
 * section 4g asks for the headroom to be measured and it never has
 * been; the per-client BOOST option in `design/09' waits on that
 * answer. This is how it gets one.
 *
 * TWO COUNTERS, because they answer different questions:
 *
 *   vsound_mix_clipped        SAMPLES. Grows fast - 44100 a second per
 *                             channel - so it is a rate, not a score.
 *   vsound_mix_clip_passes    PASSES in which at least one sample
 *                             clipped. This is the legible one: two
 *                             runs are comparable by it, and a handful
 *                             of passes is inaudible where a steady
 *                             count is the thing to act on.
 *
 * NOT TRACING, and the distinction is `design/09''s. Tracing is a
 * printk per event, floods the log and does not ship. This is an
 * increment inside a branch that already exists, reported through
 * VSOUND_IOC_STAT beside `underruns' - which is the precedent: a
 * counter that ships, that nobody calls debugging, and that a control
 * tool can read.
 */
unsigned long vsound_mix_clipped;
unsigned long vsound_mix_clip_passes;

void
vsound_mix_s16le(unsigned char *dst, const unsigned char *src, int nbytes)
{
    int i;
    int clipped = 0;

    for (i = 0; i + 1 < nbytes; i += 2) {
        int a = dst[i] | (dst[i + 1] << 8);
        int b = src[i] | (src[i + 1] << 8);
        int v;

        if (a >= 32768) a -= 65536;
        if (b >= 32768) b -= 65536;

        v = a + b;
        if (v > 32767)       { v = 32767;  clipped++; }
        else if (v < -32768) { v = -32768; clipped++; }

        dst[i]     = (unsigned char) (v & 0xff);
        dst[i + 1] = (unsigned char) ((v >> 8) & 0xff);
    }

    /* Outside the loop: one add and one branch per call, not per
     * sample. The loop itself is unchanged apart from the increment,
     * which sits in a branch that was already being taken. */
    if (clipped > 0) {
        vsound_mix_clipped += (unsigned long) clipped;
        vsound_mix_clip_passes++;
    }
}

/*
 * Apply a channel's gain, in place, on S16_LE frames.
 *
 * The reference does this with feeder_volume as a chain stage per
 * channel (feeder_chain.c:355-373), which is the same position: after
 * conversion, before the mix. Doing it after conversion means one code
 * path regardless of the client's own format.
 *
 * UNITY IS SKIPPED ENTIRELY, not multiplied by 100/100. A client that
 * never touches its volume must be bit-identical to what it was before
 * this existed, and a multiply-then-divide would not be - it would
 * round.
 *
 * Integer throughout: (sample * vol) / 100 with a 16-bit sample and
 * vol <= 100 peaks at 3276700, well inside a long on i386, so no
 * fixed-point is needed at this scale.
 */
static void
vsound_mix_gain(unsigned char *buf, int nbytes, int vol)
{
    int i;

    if (vol >= VSOUND_VOL_UNITY || vol < 0)
        return;                         /* unity: touch nothing */

    for (i = 0; i + 1 < nbytes; i += 2) {
        int v = buf[i] | (buf[i + 1] << 8);

        if (v >= 32768)
            v -= 65536;
        v = (int) (((long) v * vol) / VSOUND_VOL_UNITY);
        buf[i]     = (unsigned char) (v & 0xff);
        buf[i + 1] = (unsigned char) ((v >> 8) & 0xff);
    }
}

/*
 * Declare a MAPPED channel's whole free region ready before it is read
 * (decision 3 above) - it is the equivalent of
 *
 *     sndbuf_acquire(ch->bufsoft, NULL, sndbuf_getfree(ch->bufsoft))
 *
 * and it is guarded on CLOSING for the same reason FreeBSD guards it:
 * during a drain the buffer must not manufacture audio out of whatever
 * the departing client left behind.
 *
 * ONE COPY, for both pulls. The direct pull consumes as it copies and
 * the converting pull peeks first (below); both must see the mapped
 * region declared the same way, and a second transcription of this
 * block is how the two would drift.
 */
/*
 * ===================================================================
 * THE LIMITER, THE ATTENUATION AND THE WIDE SUM - 2026-10-02.
 * ===================================================================
 *
 * design/09 decided it 2026-09-11 ("a limiter is what we should go
 * with but also have an option for fixed attenuation"), leaving the
 * limiter's SHAPE open; the user settled that 2026-10-02 as a CHOICE -
 * "off, attack/release/soft knee as the options" - and the attenuation
 * as a plain opt-in at any channel count, both from the config.
 *
 * WHY THE SUM IS NOW WIDE. Channels were added into the 16-bit output
 * with a clamp on every add, so the true sum never existed anywhere
 * and nothing could turn it down before it clipped. Now each running
 * channel is added, WITH ITS GAIN, into an int accumulator; the
 * attenuation and the limiter act on that; ONE clamp at the end. A
 * boosted channel (VSOUND_VOL_BOOST) therefore reaches the limiter
 * whole instead of clipping on its own.
 *
 * THE MODES (vsound_limit, a module parameter from [Sound Settings]
 * Limiter):
 *   0  OFF          clamp at full scale, as before - and counted, as
 *                   before (vsound_mix_clipped, _clip_passes)
 *   1  ATTACK/RELEASE, THE DEFAULT. Any sample the sum would take past
 *                   full scale pulls the gain down AT ONCE to exactly
 *                   what fits (instant attack - so it never clips and
 *                   needs no lookahead), and the gain recovers over
 *                   ~0.1-0.2 s (VSOUND_LIM_RELEASE). BELOW FULL SCALE
 *                   IT DOES NOTHING: one program at unity is
 *                   bit-identical to what it was before this existed.
 *   2  SOFT KNEE    each sample above VSOUND_KNEE (about -1.2 dB) is
 *                   curved toward full scale, never reaching it. No
 *                   memory, cheapest - but it shapes loud peaks even
 *                   of a single program.
 *
 * vsound_atten, per cent 1..100 (100 = off), scales the whole sum
 * before the limiter: fixed headroom, for a user who wants it.
 *
 * INTEGER ONLY - kernel code, and the P1 has no time to spare in a
 * timer. Q15 gain; a division only on a sample that is being limited.
 *
 * HERE, NOT IN vsound_dev.c, so the host test sets the mode; the
 * module's MODULE_PARM lines refer to these.
 */
int vsound_limit = 1;
int vsound_atten = 100;
unsigned long vsound_mix_limit_passes;      /* passes the limiter acted */

#define VSOUND_LIM_ONE      32768L          /* Q15 unity gain */
#define VSOUND_LIM_RELEASE  13              /* recover 1/8192 per sample */
#define VSOUND_KNEE         28672L          /* soft knee starts here */

static long vsound_lim_gain = VSOUND_LIM_ONE;
/* THE LOWEST GAIN SINCE vsound_mix_limit_low() LAST ASKED - for the
 * trace's once-a-second limiter line (2026-10-02). */
static long vsound_lim_low = VSOUND_LIM_ONE;

/* The lowest attack/release gain since the last call, in per cent of
 * unity, and start again. 100 when it never acted. */
long
vsound_mix_limit_low(void)
{
    long pct = (vsound_lim_low * 100L + VSOUND_LIM_ONE / 2) / VSOUND_LIM_ONE;

    vsound_lim_low = VSOUND_LIM_ONE;
    return pct;
}

/* Back to unity gain - a fresh start for the limiter's memory. For the
 * host test, which runs passes far shorter than the release time. */
void
vsound_mix_limit_reset(void)
{
    vsound_lim_gain = VSOUND_LIM_ONE;
}

/* One pass's sum, in samples. The caller's scratch is 8192 bytes, so
 * 4096 16-bit samples; file scope because the timer's stack is small,
 * and static because a timer must not allocate. */
#define VSOUND_MIX_ACC      4096
static int vsound_mix_acc[VSOUND_MIX_ACC];

/*
 * Add (or, for the first channel, ASSIGN) a channel's samples into the
 * accumulator with its gain. `have' is how far the accumulator already
 * holds data; a longer channel zero-extends it first, so a short
 * channel and silence are the same thing.
 */
static void
vsound_mix_acc_add(const unsigned char *src, int nbytes, int vol, int *have)
{
    int i, n = nbytes / 2;

    if (n > VSOUND_MIX_ACC)
        n = VSOUND_MIX_ACC;
    for (i = *have; i < n; i++)
        vsound_mix_acc[i] = 0;
    if (n > *have)
        *have = n;
    if (vol <= 0)
        return;                     /* muted: consumed, contributes 0 */

    for (i = 0; i < n; i++) {
        int v = src[2 * i] | (src[2 * i + 1] << 8);

        if (v >= 32768)
            v -= 65536;
        if (vol != VSOUND_VOL_UNITY)
            v = (int) (((long) v * vol) / VSOUND_VOL_UNITY);
        vsound_mix_acc[i] += v;
    }
}

/*
 * THE SUM, OUT: attenuation, the limiter, one clamp, S16_LE into dst.
 */
static void
vsound_mix_finish(unsigned char *dst, int nsamp)
{
    int  i, clipped = 0, limited = 0;
    long g = vsound_lim_gain;
    long low = vsound_lim_low;

    for (i = 0; i < nsamp; i++) {
        long x = vsound_mix_acc[i];
        long ax;
        int  neg;

        if (vsound_atten > 0 && vsound_atten < 100)
            x = (x * vsound_atten) / 100;
        neg = x < 0;
        ax  = neg ? -x : x;

        if (vsound_limit == 1) {
            long target = VSOUND_LIM_ONE;

            if (ax > 32767)
                target = (32767L << 15) / ax;
            if (target < g)
                g = target;                     /* attack: at once */
            else if (target > g) {
                /*
                 * RELEASE, AND IT MUST ARRIVE - fixed 2026-10-03. This
                 * was `g += (target - g) >> 13', which adds NOTHING once
                 * the gap is under 8192 (a quarter of unity): the gain
                 * stalled below 1.0 for the rest of the load and every
                 * pass counted as limited (86Box, after a 200% boost:
                 * "acted in 100 mix passes, lowest gain 100%" each
                 * second, 14587 passes - tests/logs/2026-10-03-86box-
                 * timidity-autovoice-capture-seed). After a deep limit
                 * it would have sat up to 25% down. So at least one step
                 * per sample, and never past the target.
                 */
                long step = (target - g) >> VSOUND_LIM_RELEASE;

                g += step > 0 ? step : 1;
                if (g > target)
                    g = target;
            }
            if (g < low)
                low = g;
            if (g < VSOUND_LIM_ONE) {
                /* UNSIGNED, AND THE GAIN TO 12 BITS: ax can reach 2^19
                 * (five boosted channels), and 2^19 * 2^15 does not
                 * fit a long; 2^19 * 2^12 fits an unsigned one. */
                ax = (long) (((unsigned long) ax
                              * (unsigned long) (g >> 3)) >> 12);
                limited = 1;
            }
        } else if (vsound_limit == 2) {
            if (ax > VSOUND_KNEE) {
                /* K + R*d/(d+R), written as K + R - R*R/(d+R) so
                 * nothing overflows: R*R is 2^24. Approaches full
                 * scale and never reaches it. */
                long r = 32767L - VSOUND_KNEE;
                long d = ax - VSOUND_KNEE;

                ax = VSOUND_KNEE + r - (r * r) / (d + r);
                limited = 1;
            }
        }

        if (ax > 32767) {                       /* off, or a rounding */
            ax = neg ? 32768 : 32767;
            clipped++;
        }
        x = neg ? -ax : ax;
        dst[2 * i]     = (unsigned char) (x & 0xff);
        dst[2 * i + 1] = (unsigned char) ((x >> 8) & 0xff);
    }
    vsound_lim_gain = g;
    vsound_lim_low = low;

    if (clipped > 0) {
        vsound_mix_clipped += (unsigned long) clipped;
        vsound_mix_clip_passes++;
    }
    if (limited)
        vsound_mix_limit_passes++;
}

static void
vsound_mix_mapped_ready(struct vsound_chan *c)
{
    if (c == NULL || c->b.buf == NULL)
        return;

    if ((c->flags & VSOUND_CHN_MMAP) && !(c->flags & VSOUND_CHN_CLOSING)) {
        /*
         * A MAPPED CHANNEL IS READ AT rp, LIKE EVERY OTHER.
         *
         * FreeBSD has NO separate read position - `harvest', `swptr'
         * or anything of the kind appears nowhere in buffer.c,
         * channel.c or feeder_mixer.c. A buffer carries rp, rl, hp and
         * total, and that is all. For a mapped channel chn_wrfeed()
         * does exactly two things (channel.c:400-409):
         *
         *     sndbuf_acquire(bs, NULL, sndbuf_getfree(bs));  -> rl full
         *     sndbuf_feed(bs, b, c, c->feeder, amt);         -> drains rp
         *
         * acquire fills the ready length, feed drains through rp. ONE
         * pointer, moved by the consumer.
         *
         * The old tree had two - c->harvest and a file-scope mirror -
         * carried forward from a design where a pointer walked a shared
         * buffer tracking what had been sent to the daemon. Keeping
         * them in step is a bug waiting to happen and did happen: they
         * held counts in DIFFERENT UNITS (input bytes and output bytes)
         * and the read position ran four times too fast at
         * 11025 -> 44100.
         *
         * WHAT STOPS rp RUNNING AWAY is the caller - and the citation
         * matters, because the old tree cited the wrong one.
         *
         * There are TWO mapped acquires in FreeBSD and they are nearly
         * identical text:
         *
         *   channel.c:400-401       in chn_wrfeed, on a HARDWARE
         *                           channel, bounded by bufhard's free
         *                           space
         *   feeder_mixer.c:330-332  on each VCHAN, inside the parent's
         *                           mix, bounded by `count'
         *
         * chn_wrfeed is reached only from chn_wrintr (channel.c:432,
         * :446 - the caller at :427 is inside #if 0), the interrupt
         * path. A vchan NEVER goes through it. Our per-client channels
         * are the vchans, so THIS acquire is feeder_mixer.c:330, and
         * its bound is the `count' argument - which never touches
         * bufhard.
         *
         * That count reaches the chain from sndbuf_feed
         * (buffer.c:711-712), whose own count is chn_wrfeed's `amt'. So
         * the hard buffer bounds the whole mix ONCE, at the top, as a
         * single scalar - not each child separately.
         *
         * Ours is the `count' this function is passed, and the caller
         * derives it from the hard buffer's free space
         * (vsound_hard_free), which the CARD's reported position
         * creates. Either way the consumer's rate bounds the drain and
         * rp simply follows it.
         *
         * An earlier attempt transcribed the acquire without that
         * relationship and let rp advance at whatever rate the mixer
         * happened to be called: the ring saturated on the first pass
         * and every fragment came from an arbitrary offset. The acquire
         * is only correct WITH a bounded caller.
         *
         * So: acquire, then dispose, and rp is the only position.
         */
        vsound_buf_acquire(&c->b, (const unsigned char *) 0,
                           vsound_buf_free(&c->b));
    }
}

/*
 * Pull up to `count' bytes from one channel into `dst', CONSUMING them.
 *
 * This is the direct path: the channel is already in the mixing format,
 * so every byte copied is a byte used and the ring can move as it
 * copies. A converting channel must not come through here - see
 * vsound_mix_pull_conv() for why.
 *
 * Returns bytes copied, which may be short or zero.
 */
static int
vsound_mix_pull(struct vsound_chan *c, unsigned char *dst, int count)
{
    if (c == NULL || c->b.buf == NULL || count <= 0)
        return 0;

    vsound_mix_mapped_ready(c);

    /*
     * A SHORT PULL IS NORMAL HERE, so it is asked for at its real size -
     * design/25 I2, design/54 D33 (2026-10-04). The mixer asks for a
     * whole pass and a client often has less ready; passing the whole
     * count let vsound_buf_dispose() clamp it and count it in
     * vsound_buf_clamped, the counter that exists to catch a COMPUTED
     * count that is wrong - so every ordinary short pull polluted it
     * (`dispose clamped 8192 -> 4096' in 2026-09-08-acer-lfofix-13).
     * Same bytes copied either way; only the counter changes.
     */
    {
        int ready = vsound_buf_ready(&c->b);

        if (count > ready)
            count = ready;
    }
    return vsound_buf_dispose(&c->b, dst, count);
}


/*
 * Mix every running channel into `dst'.
 *
 * `tmp' must be at least `count' bytes and is scratch: the caller owns
 * it so this function allocates nothing. FreeBSD reuses its source
 * buffer for the same purpose ("we are going to use our source as a
 * temporary buffer since it's got no other purpose",
 * feeder_mixer.c:305-310).
 *
 * `silence' is the byte to fill a gap with, which is FORMAT-DEPENDENT
 * (vsound_buf_silence): 0x00 signed, 0x80 unsigned. Filling an
 * unsigned buffer with zeroes writes full-scale DC.
 *
 * Returns bytes valid in dst. Zero means no channel had anything, and
 * the caller should send nothing rather than sending silence - an idle
 * device should not be writing to the card at all.
 */

/*
 * Pull from a channel and NORMALISE it to the mixing format.
 *
 * THE MIXER ADDS RAW BYTES, which is only correct if every channel is
 * already in the same format. FreeBSD guarantees that structurally -
 * every chain stage calls feeder_build_formatne() first
 * (feeder_chain.c:195, 215, 293, 350, 418), so feed_mixer_feed() adds
 * bytes precisely BECAUSE conversion happened upstream. Without it the
 * mixer combines quake's 11025 Hz stereo against the card's 44100 Hz
 * stereo as though they were the same thing. Their BYTE rates are both
 * 44100 - 11025 x 2 x 2 - which is a coincidence that has misled this
 * project more than once; the frame rates differ by four, so playing
 * one as the other runs it four times fast.
 *
 * `raw' is scratch for the channel's own bytes before conversion; the
 * caller owns it, as with `tmp'.
 *
 * COUNT IS IN OUTPUT BYTES. How many input bytes that needs depends on
 * the channel's format and rate, so the pull is sized backwards from
 * it - and deliberately UNDER-estimates, because producing less than
 * asked for is a short read the mixer already handles, while producing
 * more would overrun the destination.
 */
static int
vsound_mix_pull_conv(struct vsound_chan *c, unsigned char *dst,
                     unsigned char *raw, int rawsz, int count, int mix_rate,
                     int zoh)
{
    int inbytes, got, produced, sbps, frame;

    if (c == NULL || c->b.buf == NULL || count <= 0)
        return 0;

    /* Already the mixing format: no conversion, no copy through raw. */
    if (c->format == AFMT_S16_LE && c->channels == 2
        && c->rate == mix_rate) {
        return vsound_mix_pull(c, dst, count);
    }

    sbps  = (c->format == AFMT_S16_LE || c->format == AFMT_U16_LE) ? 2 : 1;
    frame = sbps * (c->channels > 1 ? 2 : 1);
    if (frame <= 0 || c->rate <= 0 || mix_rate <= 0)
        return 0;

    /*
     * count/4 output frames need count/4 * rate/mix_rate input frames,
     * rounded DOWN, plus TWO. On the first call the interpolator primes
     * itself from two frames before it produces anything; on every
     * later call the phase carried over from the last one can push the
     * number of frames crossed one above the floor. This is the MOST
     * the converter can use this pass, not what will be taken from the
     * ring - the ring gives up only what the converter reports using,
     * below - so asking for a frame more than needed costs a peeked
     * copy of that frame and nothing else.
     */
    inbytes = (int) (((long) (count / 4) * c->rate) / mix_rate + 2) * frame;
    if (inbytes <= 0)
        return 0;

    /*
     * BOUNDED BY THE SCRATCH IT IS WRITTEN INTO. Review finding 5.
     *
     * When c->rate > mix_rate this exceeds `count': a 48000 Hz client
     * against a 44100 Hz mix needs about 1.088x, so at count == 8192
     * inbytes is about 8912 - a 720-byte overrun of a file-scope array.
     * vsound_buf_peek copies at most what is READY, so it only overruns
     * when the channel actually holds that much, which a steadily-fed
     * fast client will.
     *
     * Not reachable with quake or doom, both at 11025, which is why it
     * survived a run. It is reachable with any client faster than the
     * mix rate.
     *
     * The bound lives HERE, with the code that computes the
     * requirement, rather than in the caller: a caller cannot know what
     * ratio this will apply, and making it guess is how the units bug
     * happened. Whole frames only, so a clamp cannot split a sample.
     */
    if (inbytes > rawsz) {
        inbytes = rawsz - (rawsz % frame);
        if (inbytes <= 0)
            return 0;
    }

    /*
     * PEEK, CONVERT, THEN DISPOSE WHAT WAS USED - in that order, and
     * the order is the fix for design/25 section 4.1.
     *
     * Until 2026-09-14 this pulled through vsound_mix_pull(), which
     * DISPOSES: the ring moved past every byte handed over. But the
     * converter uses a frame only when its phase crosses into it, and
     * the one frame it had read ahead of the phase and not yet crossed
     * into was gone from the ring by the next call. One input frame
     * lost per pass on every converting client - every pass at 1:1,
     * three in four at 1:4, one in two at 1:2 - heard as a 100 Hz comb
     * on a tone and a pitch 0.68 % sharp at 11025. The copy path above
     * never comes here, which is why tonetest and the CD sounded right.
     *
     * Now the ring is read WITHOUT moving, the converter reports in
     * z_pos how many whole frames it consumed, and exactly those are
     * retired. Whole frames only, so a partial frame a client's write
     * left at the tail waits in the ring for its other half.
     *
     * rp IS STILL THE ONLY READ POSITION, as in FreeBSD - it moves
     * after the conversion instead of before it, by the converter's
     * count instead of this function's estimate. This is where the
     * units bug lived: a second position advanced by the OUTPUT count
     * while rp had moved by the INPUT count, four times apart at
     * 11025 -> 44100. There is still no second position. `total' is
     * untouched, because only acquire moves it.
     */
    vsound_mix_mapped_ready(c);
    got = vsound_buf_peek(&c->b, raw, inbytes);
    got -= got % frame;
    if (got <= 0)
        return 0;

    produced = vsound_conv_run(c, dst, count, raw, got, mix_rate, zoh);
    if (produced < 0)
        return produced;            /* refused: it consumed nothing */

    if (c->z_pos > 0)
        vsound_buf_dispose(&c->b, (unsigned char *) 0, c->z_pos * frame);

    return produced;
}

int
vsound_mix_run(struct vsound_dev *d, unsigned char *dst, unsigned char *tmp,
               unsigned char *raw, int rawsz, int count, unsigned char silence,
               int zoh)
{
    int i, rcnt = 0, have = 0, sz;
#ifdef __KERNEL__
    int show = 0;
#endif

    if (d == NULL || dst == NULL || tmp == NULL || raw == NULL || count <= 0)
        return 0;

    /*
     * ROUND TO A WHOLE OUTPUT FRAME BEFORE MIXING ANYTHING.
     *
     * feeder_mixer.c:300-303, and the old tree did not have it:
     *
     *     sz = info->bps * FEEDMIXER_CHANNELS(f->data);
     *     count = SND_FXROUND(count, sz);
     *     if (count < sz)
     *             return (0);
     *
     * SND_FXROUND rounds DOWN to a multiple of the alignment - see the
     * skeleton every feeder shares, NOTES-freebsd-feeders.md section 3,
     * where the `count < align' guard immediately follows it.
     *
     * Why it matters here even though vsound_mix_pull_conv() aligns per
     * channel: the per-channel alignment stops a single stream being
     * corrupted, but `mcnt' - the remembered gap between a short first
     * channel and a longer later one - is computed in BYTES. A count
     * that is not a whole frame puts that memset boundary mid-frame,
     * which writes silence across half a sample.
     *
     * Design 07's section 7.2. Fixed here rather than carried; it is
     * still open on the old branch.
     */
    sz = vsound_conv_bps(d->mix_format) * (d->mix_channels > 1 ? 2 : 1);
    /* NO MORE THAN THE WIDE SUM HOLDS - the callers' scratch is the same
     * 8192 bytes, so this never bites; it is a bound, not a policy. */
    if (count > VSOUND_MIX_ACC * 2)
        count = VSOUND_MIX_ACC * 2;
    count -= count % sz;
    if (count < sz)
        return 0;
    (void) silence;                 /* the sum starts at 0 - silence */

    /*
     * S16_LE ONLY, AND SAID. The mix format is VSOUND_FORMAT, a
     * constant AFMT_S16_LE in vsound_chan.c, so nothing else reaches
     * here on a machine. The old 16-bit-only gain and add made a U8 mix
     * of two channels wrong already; the wide sum makes the restriction
     * explicit instead of half-true (2026-10-02).
     */
    if (d->mix_format != AFMT_S16_LE)
        return 0;

#ifdef __KERNEL__
    show = VSOUND_RL_AT(vsound_rl_chan, vsound_rate_mix);
#endif

    /* EVERY slot, including the reserved one - a reserved channel that
     * is open and running is mixed exactly like any other. */
    for (i = 0; i < VSOUND_SLOTS; i++) {
        struct vsound_chan *c = &d->chan[i];
        int cnt;

        /*
         * WHY EVERY SLOT IS REPORTED, not just the ones that mix.
         *
         * On the Acer 2026-08-27 a channel was counted in `nrunning'
         * (the tick said "running 3") while the mixer never visited
         * it - `mixed 0 bytes, 0 empty pulls' at release, which means
         * pull was never even CALLED on it. Six theories from reading
         * the code were all eliminated by the data, and none of them
         * explained a persistent miss.
         *
         * `nrunning' is a COUNT and this loop walks a TABLE, so the
         * two can disagree without anything in the trace saying so.
         * This line closes that gap: it names the slot, its flags and
         * its pid, so a channel that is BUSY-but-not-RUNNING - or
         * RUNNING with no buffer - is visible as itself rather than
         * as an absence.
         *
         * Rate-limited with its own counter, so a busy site cannot
         * silence it. See CLAUDE.md's `measure-dont-read'.
         */
#ifdef __KERNEL__
        /* RATE-LIMITED ONCE PER MIX, NOT PER SLOT.
         *
         * The first version tested VSOUND_RL inside this loop with a
         * shared counter, which advances by VSOUND_MAX_CHAN every mix.
         * With 4 slots and `-r 10' the counter strides 4 against a
         * modulus of 10, so it selected indices 0, 2, 0, 2 ... and
         * chan[1] and chan[3] were NEVER PRINTED AT ALL. That looked
         * exactly like a channel the mixer refused to visit, which is
         * the bug it was added to find - an instrument that fabricates
         * its own finding.
         *
         * `show' is decided ONCE before the loop, so a printed mix
         * shows all four slots and the snapshot is coherent.
         */
        /*
         * READY AND EMPTY PULLS ON THE LINE, added 2026-08-31.
         *
         * `-> MIXING' meant only that the channel was RUNNING and would
         * be pulled from - never whether anything came back. A channel
         * supplying silence and one supplying music printed the same
         * line, so a run where the CD went inaudible while the mixer
         * reported full ticks could not be told from a healthy one.
         * That ambiguity cost a whole session of misreadings on
         * 2026-08-31.
         *
         * `ready' is what is waiting in the channel right now;
         * `empty' is the running total of pulls that returned nothing.
         * A climbing `empty' beside a `ready' of 0 is a starved
         * channel, and it is visible AS IT HAPPENS rather than only in
         * the release line at the end.
         */
        if (show)
            vsound_vt_printf("  chan[%d] %p pid %d flags 0x%x"
                              " buf %s rate %d ready %d empty %lu -> %s\n",
                   i, (void *) c, c->pid, c->flags,
                   c->b.buf ? "yes" : "NULL", c->rate,
                   c->b.buf ? vsound_buf_ready(&c->b) : 0,
                   c->empty_pulls,
                   (c->flags & VSOUND_CHN_RUNNING) ? "MIXING" : "skipped");
#endif

        /* Only running channels participate. This is the equivalent of
         * FreeBSD's children.busy list (feeder_mixer.c:316): membership
         * IS participation, and a stopped channel is simply absent
         * rather than being skipped by a test inside the mix. */
        if (!(c->flags & VSOUND_CHN_RUNNING))
            continue;

        /*
         * EVERY RUNNING CHANNEL INTO THE WIDE SUM, with its gain - see
         * the limiter's account above. The pull runs whether or not the
         * channel is muted, so a muted client keeps advancing rather
         * than stalling at a full buffer; its gain is simply 0.
         */
        cnt = vsound_mix_pull_conv(c, tmp, raw, rawsz, count,
                                   d->mix_rate, zoh);
        /*
         * A NEGATIVE RETURN IS AN ERROR, NOT A BYTE COUNT -
         * vsound_conv_run() refuses with -1 (no setup, a bad format, a
         * zero rate). Stored as a count it poisoned the gap arithmetic
         * once (AddressSanitizer, a 33-byte write at dst - 1).
         */
        if (cnt < 0)
            cnt = 0;

        /* PER-CHANNEL ACCOUNTING - what this channel CONTRIBUTED, for
         * its lifetime (a RUNNING channel producing nothing is
         * otherwise invisible: Acer, 2026-08-27). A-8's wake condition:
         * what this pass took from it, which is what freed space. */
        c->mixed_bytes += (unsigned long) cnt;
        if (cnt == 0)
            c->empty_pulls++;
        c->freed_this_pass += cnt;

        if (cnt > 0) {
            vsound_mix_acc_add(tmp, cnt,
                               (c->flags & VSOUND_CHN_MUTED) ? 0 : c->vol,
                               &have);
            if (cnt > rcnt)
                rcnt = cnt;
        }
    }

    /* THE SUM OUT - attenuation, limiter, one clamp. */
    if (rcnt > 0)
        vsound_mix_finish(dst, rcnt / 2);

    return rcnt;
}
