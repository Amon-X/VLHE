/*
 * vsound_buf.c - the ring buffer primitives.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 * The FreeBSD-derived parts are BSD-2-Clause; their notice is below.
 *
 * Transcribed from FreeBSD's buffer.c (buffer.c:618-676), which is in
 * freebsd-pcm-reference/ under BSD-2-Clause. See NOTES-freebsd-core.md
 * section 1 for why a pointer plus a length is the right shape.
 *
 * Attribution, per the licence those files carry:
 *
 *     Copyright (c) 2005-2009 Ariff Abdullah <ariff@FreeBSD.org>
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
 * The algorithms are FreeBSD's; the surrounding code is not a port -
 * there is no bus_dma, no per-channel mutex, and the 64-bit counters
 * are 32-bit here.
 */

/*
 * Self-contained as far as it can be. In the module this is compiled
 * with the kernel's headers (linux/string.h for memcpy/memset); the
 * host test provides the C library's. Neither is assumed here beyond
 * NULL and size_t, which both spell the same way.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#include <linux/soundcard.h>    /* AFMT_* for the silence table */
#include <linux/sched.h>        /* jiffies, HZ - the clamp rate limit */
#else
#include <string.h>
#include <stddef.h>
#ifndef AFMT_S16_LE
#include <sys/soundcard.h>
#endif
#endif

#include "vsound_chan.h"

/*
 * THE CLAMP COUNTER, and why it is visible.
 *
 * Every clamp means a caller computed a count that did not fit, which
 * the reference treats as the caller being wrong (buffer.c:705-706).
 * Counting them makes "is anything computing bad counts?" answerable
 * from outside, and the printk names which side and by how much.
 *
 * In the kernel this is rate-limited; in the host tests it is a plain
 * counter the test can assert on, which is what makes 7.3 testable
 * rather than merely traced.
 */
unsigned long vsound_buf_clamped;

#ifdef __KERNEL__
extern int vsound_trace;
/*
 * ONE LINE PER SECOND IS STILL TOO MANY on a console someone is trying
 * to type into. On 2026-08-25 an idle pump produced a clamp per wakeup,
 * this limiter thinned it to roughly one a second, and the target
 * became hard to use - the user had to type `halt' between messages.
 *
 * So: rate-limited AND capped. After VSOUND_BUF_COMPLAIN_MAX lines it
 * stops entirely and says so once. The COUNTER keeps counting either
 * way, and vsound_buf_clamped is what a diagnosis should read - the
 * printk is only there to make a clamp noticeable the first few times.
 *
 * A diagnostic that makes the machine unusable has stopped being a
 * diagnostic.
 */
#define VSOUND_BUF_COMPLAIN_MAX 8

#define VSOUND_BUF_COMPLAIN(what, want, got)                            \
    do {                                                                \
        static unsigned long _last;                                     \
        static int _n;                                                  \
        if (vsound_trace && _n <= VSOUND_BUF_COMPLAIN_MAX               \
            && (_last == 0 || jiffies - _last > HZ)) {                  \
            _last = jiffies ? jiffies : 1;                              \
            if (++_n > VSOUND_BUF_COMPLAIN_MAX)                         \
                vsound_vt_printf("%s clamped (further"         \
                                  " reports suppressed; see"            \
                                  " VSOUND_IOC_STAT)\n", (what));       \
            else                                                        \
                vsound_vt_printf("%s clamped %d -> %d\n",      \
                       (what), (int) (want), (int) (got));              \
        }                                                               \
    } while (0)
#else
#define VSOUND_BUF_COMPLAIN(what, want, got)    ((void) 0)
#endif

/*
 * Add data to the ring, increasing the ready area.
 *
 * `from == NULL' is not an error and is not a copy: it declares `count'
 * bytes ready that are ALREADY in the buffer. That is the mmap case -
 * a mapped client writes into the buffer directly and issues no
 * syscall, so there is nothing to copy and nothing to observe. FreeBSD
 * does exactly this at feeder_mixer.c:330-332 and channel.c:400-401:
 *
 *     sndbuf_acquire(ch->bufsoft, NULL, sndbuf_getfree(ch->bufsoft));
 *
 * consuming the whole free region every cycle rather than trying to
 * work out what the client wrote. The old tree's harvest tried to know,
 * and that machinery is upstream of the sliced-audio and stuck-tone
 * reports. There is no harvest here.
 *
 * Returns the bytes acquired, which is `count' unless it was clamped.
 */
int
vsound_buf_acquire(struct vsound_buf *b, const unsigned char *from,
                   int count)
{
    int l, avail;

    if (b == NULL || b->buf == NULL || count <= 0)
        return 0;

    /*
     * CLAMP RATHER THAN ASSERT, BUT SAY SO. FreeBSD has
     * KASSERT(count <= free) here (buffer.c:622) and sndbuf_feed
     * REFUSES outright - `return (EINVAL)' at buffer.c:705-706. The
     * invariant is that the caller has already computed a count that
     * fits; the callee does not defend itself by taking less.
     *
     * A 2.2 module cannot panic usefully, so the bound is enforced
     * instead. But a silent clamp is exactly what hid the units bug on
     * the old tree - the caller computed the wrong count, the clamp
     * absorbed it, and the trace showed nothing. Design 07's section
     * 7.3: the complaint goes in from the start, not after the next
     * time it costs a day.
     *
     * Rate-limited, because a genuinely wrong caller would otherwise
     * flood the log and make the trace unreadable.
     */
    avail = vsound_buf_free(b);
    if (count > avail) {
        vsound_buf_clamped++;
        VSOUND_BUF_COMPLAIN("acquire", count, avail);
        count = avail;
    }
    if (count <= 0)
        return 0;

    b->total += (unsigned long) count;

    if (from != NULL) {
        int n = count;
        while (n > 0) {
            l = (int) b->bufsize - vsound_buf_freeptr(b);
            if (l > n)
                l = n;
            memcpy(b->buf + vsound_buf_freeptr(b), from, (size_t) l);
            from += l;
            b->rl += l;
            n    -= l;
        }
    } else {
        b->rl += count;
    }

    return count;
}

/*
 * Remove data from the ring, shrinking the ready area.
 *
 * The mirror of acquire, and buffer.c:655-676. `to == NULL' discards
 * without copying - which is how a hardware pointer's progress becomes
 * free space in FreeBSD (chn_dmaupdate, channel.c:373-376), and how a
 * fragment already handed to the daemon is retired here.
 */
int
vsound_buf_dispose(struct vsound_buf *b, unsigned char *to, int count)
{
    int l, avail;

    if (b == NULL || b->buf == NULL || count <= 0)
        return 0;

    avail = vsound_buf_ready(b);
    if (count > avail) {
        vsound_buf_clamped++;
        VSOUND_BUF_COMPLAIN("dispose", count, avail);
        count = avail;
    }
    if (count <= 0)
        return 0;

    if (to != NULL) {
        int n = count;
        while (n > 0) {
            l = (int) b->bufsize - b->rp;
            if (l > n)
                l = n;
            memcpy(to, b->buf + b->rp, (size_t) l);
            to   += l;
            b->rl -= l;
            b->rp  = (int) (((unsigned int) b->rp + (unsigned int) l)
                            % b->bufsize);
            n    -= l;
        }
    } else {
        b->rl -= count;
        b->rp  = (int) (((unsigned int) b->rp + (unsigned int) count)
                        % b->bufsize);
    }

    return count;
}

/*
 * COPY OUT WITHOUT CONSUMING.
 *
 * `count' is an UPPER BOUND, not a requirement: the converting pull
 * (vsound_mix_pull_conv) asks for the most its converter could use this
 * pass, converts, and only then disposes exactly what the converter
 * reports having consumed. So a peek shorter than `count' is the
 * ordinary case and is NOT a clamp - the caller has not computed a
 * wrong count, it has asked how much there is - and it is not counted
 * in vsound_buf_clamped. Nothing moves: rp, rl and total are as they
 * were, and a later vsound_buf_dispose() retires the bytes.
 *
 * No counterpart in the reference. buffer.c's feeders are handed exact
 * counts by the chain, so they can consume as they copy; ours is fed an
 * estimate (design/25 section 4.1), and this is the primitive that lets
 * the ring wait for the converter's answer instead. Added 2026-09-14.
 *
 * Returns the bytes copied: the smaller of `count' and what is ready.
 */
int
vsound_buf_peek(struct vsound_buf *b, unsigned char *to, int count)
{
    int l, n, rp;

    if (b == NULL || b->buf == NULL || to == NULL || count <= 0)
        return 0;

    if (count > vsound_buf_ready(b))
        count = vsound_buf_ready(b);
    if (count <= 0)
        return 0;

    /* The same walk as dispose's copy, on a LOCAL rp. */
    rp = b->rp;
    n  = count;
    while (n > 0) {
        l = (int) b->bufsize - rp;
        if (l > n)
            l = n;
        memcpy(to, b->buf + rp, (size_t) l);
        to += l;
        rp  = (int) (((unsigned int) rp + (unsigned int) l) % b->bufsize);
        n  -= l;
    }

    return count;
}

/*
 * The silence byte for a format.
 *
 * buffer.c:591-600, and it is FOUR cases rather than the obvious one.
 * Signed formats are silent at 0x00; UNSIGNED formats are silent at
 * 0x80, and filling an unsigned-8 buffer with zeroes writes full-scale
 * negative DC, not silence. mu-law is 0x7f and A-law 0x55 - neither is
 * reachable from anything this project runs, but they are one line
 * each and the whole point of this file is to match the reference.
 */
unsigned char
vsound_buf_silence(int format)
{
    if (format & (AFMT_S8 | AFMT_S16_LE | AFMT_S16_BE))
        return 0x00;
    if (format & AFMT_MU_LAW)
        return 0x7f;
    if (format & AFMT_A_LAW)
        return 0x55;
    return 0x80;                /* unsigned */
}

/*
 * Reset to empty, keeping the allocation.
 *
 * buffer.c:326-331 (sndbuf_softreset). Note this is NOT
 * sndbuf_fillsilence: that sets rl = bufsize so the WHOLE BUFFER
 * becomes ready silence, which is what chn_start() uses to pre-fill
 * before the first trigger (channel.c:726-728). "Zeroed and empty" and
 * "full of silence" are different states and playback start wants the
 * second.
 */
void
vsound_buf_reset(struct vsound_buf *b, int format)
{
    if (b == NULL)
        return;
    b->rp = 0;
    b->rl = 0;
    b->total = 0;
    if (b->buf != NULL && b->bufsize > 0)
        memset(b->buf, vsound_buf_silence(format), b->bufsize);
}

/*
 * FILL THE WHOLE BUFFER WITH READY SILENCE.
 *
 * `sndbuf_fillsilence_rl' in the reference, and this is the state
 * chn_start() puts the hard buffer into before the first trigger
 * (channel.c:725-727):
 *
 *     if (c->direction == PCMDIR_PLAY)
 *         sndbuf_fillsilence_rl(b, sndbuf_xbytes(...));
 *
 * The difference from vsound_buf_reset() is `rl', and it is the whole
 * point: reset leaves the buffer EMPTY (rl = 0), this leaves it FULL
 * (rl = bufsize) of silence that is ready to be drained.
 *
 * WHY IT MATTERS, measured on the Acer 2026-08-27: without it the card
 * starts empty and vsound only ever produces one tick's worth per
 * tick, so the card runs with less than a tick in hand and starves
 * continuously - 15710 of 15972 acks reported odelay 0, audible as a
 * looping DMA buffer. The prefill IS the cushion: real audio displaces
 * the silence as it arrives, and what is left absorbs a late feed.
 *
 * `count' is capped at the buffer size, so a caller that asks for the
 * soft buffer's size on a smaller hard buffer gets a full one rather
 * than an error.
 */
void
vsound_buf_fillsilence(struct vsound_buf *b, int format, int count)
{
    if (b == NULL || b->buf == NULL || b->bufsize <= 0)
        return;

    if (count <= 0 || count > b->bufsize)
        count = b->bufsize;

    b->rp = 0;
    b->rl = count;
    b->total = 0;
    memset(b->buf, vsound_buf_silence(format), (size_t) b->bufsize);
}

/*
 * TOP THE BUFFER UP WITH SILENCE, KEEPING WHAT IS READY - design/25 B8,
 * design/54 D32 (2026-10-04).
 *
 * vsound_buf_fillsilence() starts from rp = 0, so on the 0 -> 1 start it
 * threw away whatever the hard ring still held: the last client's tail,
 * up to 23 ms, cut when the next one started (a blip was heard on the
 * Acer 2026-09-21 that may have been this). The cushion is still wanted
 * - the prefill is load-bearing (design/07) - so this writes silence into
 * the FREE region only, after the tail, and leaves the buffer as full as
 * fillsilence would: same cushion, nothing lost. The free region is set
 * to silence first, since what sits there is old audio.
 */
void
vsound_buf_topup_silence(struct vsound_buf *b, int format)
{
    int free_bytes, start, first;

    if (b == NULL || b->buf == NULL || b->bufsize <= 0)
        return;
    free_bytes = vsound_buf_free(b);
    if (free_bytes <= 0)
        return;
    start = vsound_buf_freeptr(b);
    first = (int) b->bufsize - start;
    if (first > free_bytes)
        first = free_bytes;
    memset(b->buf + start, vsound_buf_silence(format), (size_t) first);
    if (free_bytes > first)
        memset(b->buf, vsound_buf_silence(format), (size_t) (free_bytes - first));
    b->rl += free_bytes;
    b->total += (unsigned long) free_bytes;
}
