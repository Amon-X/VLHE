/*
 * vsound_drain.c - the hard buffer, and the handoff to vsoundd.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/07-vsound.md sections 4, 4.1 and 6. THIS FILE HAS NO REFERENCE
 * COUNTERPART and that is why it is a file of its own.
 *
 * FreeBSD never needed one. Its bufhard IS the DMA buffer - the card
 * reads it directly - so there is no boundary to cross and nothing to
 * hand off. Ours is read by a userspace process, because a kernel
 * module cannot reach esssolo1 (section 2), and every line below exists
 * to bridge that gap.
 *
 * What IS taken from the reference is the discipline, not the code:
 *
 *   - the drain is bounded by MEASURED consumption, never by a clock.
 *     chn_dmaupdate() reads the DMA pointer and takes a delta
 *     (channel.c:356-388); we take the delta from the card's own
 *     GETOPTR, reported by the pump. Same shape, one process further
 *     out.
 *   - the delta is CLAMPED by what could plausibly have been consumed,
 *     exactly as `amt = min(delta, sndbuf_getready(b))' does
 *     (channel.c:366-376). A counter that resets cannot corrupt us.
 *   - free space bounds the mix, and free space exists only because
 *     something was genuinely consumed (chn_wrfeed's `wasfree',
 *     channel.c:403-407).
 *
 * THE OLD TREE'S PUMP WAS NEVER RIGHT. Its handoff passed unmixed
 * fragments with per-fragment metadata and a single in-flight slot, and
 * the position driving it was PREDICTED from a byte rate - which drifts
 * 44 bytes per tick (06-softoss-design.md section 4c). This replaces
 * all of that rather than repairing it.
 */

#ifndef __KERNEL__
#error "vsound_drain.c is kernel-only"
#endif

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/soundcard.h>
#include <asm/uaccess.h>

#include "vsound_chan.h"
#include "vsound.h"

extern int vsound_trace;
extern int vsound_ratelimit;
extern int vsound_depth;

static unsigned long vsound_rl_drain;
static unsigned long vsound_rl_ack;

/*
 * Set up the hard buffer.
 *
 * SIZED FOR THE TIGHTEST CLIENT, NOT THE LOOSEST - section 6. An
 * earlier draft of the design said to size this from GETOSPACE, i.e. to
 * the card's whole 16 x 4096. That would re-add 371 ms on top of the
 * card's own queue, which is exactly the latency the per-channel sizing
 * had just removed.
 *
 * The card's total is CAPACITY, not delay: solo1_write() starts the DAC
 * on the first bytes present, so what a listener hears is how much sits
 * ahead of the play pointer. This buffer only has to be big enough that
 * the pump is never starved between wakeups - a few fragments.
 */
int
vsound_drain_init(struct vsound_dev *d, unsigned char *buf, unsigned int size)
{
    if (d == NULL || buf == NULL || size == 0)
        return -1;

    memset(&d->hard, 0, sizeof d->hard);
    d->hard.b.buf     = buf;
    d->hard.b.bufsize = size;
    d->hard.b.rp      = 0;
    d->hard.b.rl      = 0;
    d->hard.b.total   = 0;
    return 0;
}

/*
 * How much the mixer may produce this tick.
 *
 * Two bounds, and both are measured - which is chn_wrfeed's shape
 * (channel.c:403-407):
 *
 *     wasfree = sndbuf_getfree(b);        physical room
 *     want    = ... - sndbuf_getready(b); how much is useful
 *     amt     = min(wasfree, want);
 *
 * Ours are the hard buffer's free space (physical room, reduced by what
 * is outstanding to the pump) and vsound_depth (how much we are willing
 * to have in flight, which is the latency cap).
 *
 * THE SECOND IS THE LATENCY KNOB. Capping outstanding caps the card's
 * occupancy, and occupancy is what a listener hears.
 */
int
vsound_drain_room(struct vsound_dev *d)
{
    int room, out, cap;

    if (d == NULL || d->hard.b.buf == NULL)
        return 0;

    room = vsound_hard_free(&d->hard);

    /* The depth cap, in bytes. 0 means "not configured yet": until the
     * pump reports the card's fragment size there is nothing sensible
     * to derive a default from, so the buffer's own size is the only
     * bound. That is generous by design - getting audible comes before
     * getting tight, and a too-small depth underruns, which sounds far
     * worse than latency. */
    cap = vsound_depth;
    if (cap <= 0)
        return room;

    /*
     * THE CAP IS ON OUTSTANDING, and `room' has already had
     * outstanding taken out - so the clamp is against `cap - out',
     * which excludes it once on each side. Correct as written.
     *
     * `design/12-reference-audit.md' B-2 claimed this subtracted
     * outstanding twice. **That claim was wrong** and is corrected
     * there: min(free - out, cap - out) removes it once from each
     * term, which is what was intended.
     */
    out = vsound_hard_outstanding(&d->hard);
    if (out >= cap)
        return 0;
    if (room > cap - out)
        room = cap - out;
    return room;
}

/*
 * The mixer's output goes here.
 *
 * Returns what was accepted, which may be short: the caller must not
 * assume the whole count fitted, and vsound_buf_acquire clamps rather
 * than overruns.
 */
int
vsound_drain_put(struct vsound_dev *d, const unsigned char *src, int count)
{
    if (d == NULL || d->hard.b.buf == NULL || src == NULL || count <= 0)
        return 0;

    return vsound_buf_acquire(&d->hard.b, src, count);
}

/*
 * The pump's read().
 *
 * Returns bytes handed out, 0 when there is nothing ready. `outstanding'
 * increments HERE, not at ack time - that is the whole point of section
 * 4.1: free space is known continuously, and only its RETIREMENT waits
 * for the card to report. A 40 ms ack gap therefore delays recovery of
 * space without ever delaying the kernel's knowledge of what is in
 * flight.
 *
 * Callers hold no lock; this takes cli() itself around the ring update.
 */
int
vsound_drain_get(struct vsound_dev *d, unsigned char *to, int count)
{
    unsigned long flags;
    int got;

    if (d == NULL || d->hard.b.buf == NULL || to == NULL || count <= 0)
        return 0;

    save_flags(flags);
    cli();

    /*
     * ASK FOR WHAT IS THERE. The caller computes a count that fits -
     * that is sndbuf_feed's contract (buffer.c:705-706), where the
     * callee REFUSES rather than taking less, and it is the same
     * discipline design 07's section 7.3 is about.
     *
     * The first version passed `count' straight through and let
     * vsound_buf_dispose clamp it. On an idle device that meant asking
     * for 2048 from an empty buffer on every wakeup, and the clamp
     * counter dutifully reported each one - a line a second to the
     * console, which on the target made the machine hard to type into
     * and nearly cost the user control of it.
     *
     * The clamp complaint was right. The caller was wrong.
     */
    got = vsound_buf_ready(&d->hard.b);
    if (got > count)
        got = count;
    if (got > 0) {
        got = vsound_buf_dispose(&d->hard.b, to, got);
        d->hard.written += (unsigned long) got;
    } else {
        /*
         * Nothing ready. COUNTED, NOT PRINTED: an idle device is not an
         * anomaly, and the count is visible through VSOUND_IOC_STAT for
         * anyone who wants it.
         */
        d->hard.underruns++;
    }
    restore_flags(flags);

    /* STAMPED - design/25 I3. These two lines were the only
     * per-fragment traces in the module without a jiffies stamp, which
     * is why B6's one-chunk discard could be counted but never placed
     * in time against the tick and release lines around it. */
    if (got > 0 && VSOUND_RL(vsound_rl_drain))
        printk(KERN_DEBUG VSOUND_TS "vsound: drain %d of %d"
                          " (ready %d, out %d)\n",
               jiffies, got, count, vsound_buf_ready(&d->hard.b),
               vsound_hard_outstanding(&d->hard));

    return got;
}

/*
 * The pump's ack: what the card actually did.
 *
 * This is chn_dmaupdate() with a process in the middle
 * (channel.c:356-388). It reads a position, takes a DELTA, and clamps
 * it - never uses an absolute, so a counter that resets or wraps cannot
 * corrupt anything.
 */
int
vsound_drain_ack(struct vsound_dev *d, const struct vsound_ack *a)
{
    unsigned long flags, delta, out;

    if (d == NULL || a == NULL)
        return -1;

    save_flags(flags);
    cli();

    if (a->flags & VSOUND_ACK_PLAYED) {
        if (!d->hard.have_played) {
            /*
             * FIRST ACK: baseline only, retire nothing. The card's
             * counter starts wherever it starts - it is not reset when
             * we open - so the first absolute value carries no
             * information about what WE sent.
             */
            d->hard.card_played = (unsigned long) a->played;
            d->hard.have_played = 1;
        } else {
            /* The delta since the last ack, wrap-safe because the
             * card's counter is monotonic u32. */
            delta = (unsigned long)
                    (a->played - (__u32) d->hard.card_played);
            d->hard.card_played = (unsigned long) a->played;

            /*
             * CLAMPED BY WHAT COULD PLAUSIBLY HAVE BEEN PLAYED, which
             * is what we handed out and have not yet retired.
             * channel.c:366-376 does the same with sndbuf_getready.
             * A card whose counter jumps - a reset, or a wrap we cannot
             * interpret - then costs us nothing.
             */
            out = (unsigned long) vsound_hard_outstanding(&d->hard);
            if (delta > out)
                delta = out;

            /*
             * ROUND DOWN TO A WHOLE FRAME, which the reference does
             * immediately after the same clamp - channel.c:370-371:
             *
             *     amt = min(delta, sndbuf_getready(b));
             *     amt -= amt % sndbuf_getalign(b);
             *
             * GETOPTR reflects a DMA pointer and can land mid-frame.
             * Retiring a partial frame offsets every subsequent read
             * by 1-3 bytes and shifts the channel interleave - silent,
             * and audible only as a stereo image that is subtly wrong.
             *
             * Latent on the Acer, where every observed odelay was
             * divisible by 4. The design should not depend on the
             * card's good manners.
             */
            if (d->hard.frame > 1)
                delta -= delta % (unsigned long) d->hard.frame;

            d->hard.played += delta;
        }
    }

    /*
     * ODELAY CORRECTS OUTSTANDING, DOWNWARD ONLY.
     *
     * written - played is what we BELIEVE is in flight, accumulated
     * from two counters that can drift apart. odelay is what the card
     * SAYS is queued, and it cannot drift. Where they disagree the card
     * is right - but only when it claims LESS: a card reporting more
     * queued than we believe we sent is a figure we cannot act on, so
     * it is ignored rather than allowed to shrink our free space on
     * evidence that contradicts our own accounting.
     */
    if (a->flags & VSOUND_ACK_ODELAY) {
        out = (unsigned long) vsound_hard_outstanding(&d->hard);
        if ((unsigned long) a->odelay < out)
            d->hard.played = d->hard.written - (unsigned long) a->odelay;
    }

    if (a->card_frag > 0)
        d->hard.card_frag = a->card_frag;

    restore_flags(flags);

    if (VSOUND_RL(vsound_rl_ack))
        printk(KERN_DEBUG VSOUND_TS "vsound: ack played %u odelay %u"
                          " (out %d, ready %d)\n",
               jiffies, a->played, a->odelay,
               vsound_hard_outstanding(&d->hard),
               vsound_buf_ready(&d->hard.b));

    return 0;
}

/* For VSOUND_IOC_STAT. Read-only; nothing here changes behaviour. */
void
vsound_drain_stat(struct vsound_dev *d, struct vsound_stat *s)
{
    unsigned long flags;

    if (d == NULL || s == NULL)
        return;

    memset(s, 0, sizeof *s);

    save_flags(flags);
    cli();
    s->nbusy       = (__u32) d->nbusy;
    s->nrunning    = (__u32) d->nrunning;
    s->outstanding = (__u32) vsound_hard_outstanding(&d->hard);
    s->depth       = (__u32) vsound_depth;
    s->hard_ready  = (__u32) vsound_buf_ready(&d->hard.b);
    s->hard_size   = d->hard.b.bufsize;
    s->underruns   = (__u32) d->hard.underruns;
    s->clip_passes = (__u32) vsound_mix_clip_passes;
    restore_flags(flags);
}
