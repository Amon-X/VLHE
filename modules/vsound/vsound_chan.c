/*
 * vsound_chan.c - channel lifecycle, and the geometry a channel is
 *                 sized from.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 * The FreeBSD-derived parts are BSD-2-Clause; their notice is below.
 *
 * design/07-vsound.md sections 4a, 5 and 7. The reference is FreeBSD's
 * sound/pcm (stable/13), in freebsd-pcm-reference/ under BSD-2-Clause:
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
 * A CHANNEL PER CLIENT, claimed at open and released at close. This is
 * what makes N applications possible, and it is the whole reason the
 * per-client state cannot stay in file-scope variables.
 */

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#include <linux/soundcard.h>
#include <linux/sched.h>        /* wake_up_interruptible */
#else
#include <string.h>
#include <stddef.h>
#ifndef AFMT_S16_LE
#include <sys/soundcard.h>
#endif
#endif

#include "vsound_chan.h"

/*
 * The device's own defaults, and what the mixed stream is produced in.
 *
 * CD rate, because that is what the CD path produces and what the card
 * is opened at. FreeBSD's equivalent is the vchan format and rate
 * (sound.h:392, vchan.h:53-54), which defaults to S16_LE stereo at
 * 48000 there for the same kind of reason - it is the format everything
 * converts INTO, chosen once for the device rather than per client.
 */
#define VSOUND_RATE      44100
#define VSOUND_CHANNELS  2
#define VSOUND_FORMAT    AFMT_S16_LE

/*
 * Bring a device to its initial state.
 *
 * Every channel is idle, the mix format is the device default, and the
 * hard buffer is empty. Called once at module load.
 */
void
vsound_dev_init(struct vsound_dev *d)
{
    if (d == NULL)
        return;

    memset(d, 0, sizeof *d);
    d->mix_rate     = VSOUND_RATE;
    d->mix_channels = VSOUND_CHANNELS;
    d->mix_format   = VSOUND_FORMAT;
}

/*
 * Find a free channel and claim it. Callers hold cli().
 *
 * FreeBSD scans its channel list for one that is not CHN_F_BUSY, sets
 * the flag and records the owning pid (sound.c:315-334), creating a new
 * virtual channel if none is free. A fixed table cannot grow that way,
 * and on 2.2 that is worth more than the flexibility: an allocation
 * that cannot fail at open time is one failure path that does not need
 * handling in a path the client is waiting on.
 *
 * The BUFFER is not allocated here. It is sized from the geometry the
 * client negotiates, which is not known yet at open - see
 * vsound_chan_setgeom(). A channel with c->b.buf == NULL is claimed but
 * not yet usable, and every consumer already tests for that.
 */
/*
 * Claim one slot. Callers hold cli(), and have checked it is free.
 */
static void
vsound_chan_claim(struct vsound_dev *d, struct vsound_chan *c, int pid)
{
    /*
     * The wait queue survives the memset. vsound_chan_free()
     * drains it (finding 6), so it should be NULL here - but
     * zeroing a queue that still has a sleeper on it loses them
     * permanently, and a released channel is exactly where a
     * stray sleeper would be. Preserving the head costs
     * nothing and removes the failure mode rather than relying
     * on the drain having worked.
     */
    struct wait_queue *keep = c->wait;

    memset(c, 0, sizeof *c);
    c->wait = keep;
    c->flags    = VSOUND_CHN_BUSY;
    c->pid      = pid;
    c->rate     = VSOUND_RATE;
    c->channels = VSOUND_CHANNELS;
    c->format   = VSOUND_FORMAT;

    /*
     * UNITY BY DEFAULT. A client that never asks for a volume
     * must sound exactly as it did before per-stream gain
     * existed - anything else would be a silent behaviour
     * change nobody requested.
     */
    c->vol      = VSOUND_VOL_UNITY;

    d->nbusy++;
    d->generation++;        /* a tool's channel list is stale */
}

struct vsound_chan *
vsound_chan_alloc(struct vsound_dev *d, int pid)
{
    int i;

    if (d == NULL)
        return NULL;

    /*
     * THE FIRST VSOUND_MAX_CHAN SLOTS AND NO OTHER.
     *
     * THE UPPER BOUND IS VSOUND_MAX_CHAN, NOT VSOUND_SLOTS, and
     * getting that wrong was caught by the existing tests: the array
     * is always VSOUND_SLOTS long, so bounding by its SIZE hands out
     * the reserved MIDI slot to whoever opens fifth. The promise is
     * that an ordinary client is never offered it - no test inside
     * the loop and no privileged-client concept.
     */
    for (i = 0; i < VSOUND_MAX_CHAN; i++) {
        struct vsound_chan *c = &d->chan[i];

        if (!(c->flags & VSOUND_CHN_BUSY)) {
            vsound_chan_claim(d, c, pid);
            return c;
        }
    }
    return NULL;
}

/*
 * The reserved MIDI slot, or NULL if it is held. Callers hold cli().
 *
 * WHETHER THE SLOT EXISTS AT ALL is the caller's decision, not this
 * function's: vsound_dev_open() asks only when vsound_midi is set
 * and answers -ENODEV otherwise. Keeping the policy there means this
 * file, and the unit test built from it, know nothing about module
 * parameters.
 */
struct vsound_chan *
vsound_chan_alloc_midi(struct vsound_dev *d, int pid)
{
    struct vsound_chan *c;

    if (d == NULL)
        return NULL;

    c = &d->chan[VSOUND_MIDI_SLOT];
    if (c->flags & VSOUND_CHN_BUSY)
        return NULL;
    vsound_chan_claim(d, c, pid);
    return c;
}

/*
 * Give it back. Callers hold cli().
 *
 * The buffer is NOT freed here: freeing memory a mapping may still
 * reference is the caller's problem to sequence, and on 2.2 vfree()
 * can sleep. vsound_chan_setgeom(c, 0, 0) releases it explicitly, from
 * a context that can afford to.
 */
void
vsound_chan_free(struct vsound_dev *d, struct vsound_chan *c)
{
    if (d == NULL || c == NULL || !(c->flags & VSOUND_CHN_BUSY))
        return;

    if ((c->flags & VSOUND_CHN_RUNNING) && d->nrunning > 0)
        d->nrunning--;
    if (d->nbusy > 0)
        d->nbusy--;

    /*
     * DRAIN THE WAIT QUEUE BEFORE THE SLOT IS REUSABLE. Review
     * finding 6.
     *
     * vsound_chan_alloc() memsets the whole channel on claiming it,
     * including `wait'. If anyone is still sleeping on that queue when
     * the next open zeroes it, the pointer linking them in is lost and
     * they sleep forever - and the queue is corrupt for whoever comes
     * next.
     *
     * Waking first means every sleeper re-tests its condition, sees
     * CLOSING or a NULL buffer, and leaves before the slot can be
     * handed out again.
     *
     * Currently hard to reach: release() cannot run while write() is in
     * progress on the same fd. It becomes reachable the moment two fds
     * share a channel, or a signal races a close - and a bug that only
     * appears under a signal is one nobody reproduces on demand.
     */
#ifdef __KERNEL__
    wake_up_interruptible(&c->wait);
#endif

    c->flags = 0;
    c->pid   = 0;
    c->comm[0] = '\0';
    d->generation++;            /* the slider for this one must go */
}

/*
 * Set one channel's volume, checking the caller has the right one.
 *
 * THE INDEX COMES FROM USERSPACE AND IS NOT TRUSTED. An index of 7
 * with VSOUND_MAX_CHAN at 4 is an out-of-bounds array write reachable
 * from an ordinary ioctl - the same class as the audio_devs[-1] bug
 * CLAUDE.md section 1 records, and the only failure in this feature
 * that could take the machine down. Bounds first, always.
 *
 * THE PID CHECK IS NOT BELT-AND-BRACES. Indices are reused: a tool
 * holding "index 1 = lxdoom" from a poll 200 ms ago would otherwise set
 * QUAKE's volume after doom exited and quake took the slot. Verifying
 * and setting here, under the caller's cli(), closes that window rather
 * than narrowing it.
 *
 * pid 0 means "whatever is there", which is only correct for a caller
 * that has just read the list.
 *
 * Callers hold cli(). Returns 0, or a negative errno.
 */
int
vsound_chan_setvol(struct vsound_dev *d, int index, int pid, int vol)
{
    struct vsound_chan *c;

    if (d == NULL)
        return -1;
    /*
     * VSOUND_SLOTS, so a tool can address the reserved slot when it
     * exists - but the caller's index is a RAW array index either way
     * and the array is always VSOUND_SLOTS long, so this cannot
     * overrun. A tool asking about slot 4 with MIDI off gets -ENODEV
     * from the BUSY test below, which is the honest answer: the slot
     * is in range and nobody is using it.
     */
    if (index < 0 || index >= VSOUND_SLOTS)
        return -22;                             /* -EINVAL */

    c = &d->chan[index];
    if (!(c->flags & VSOUND_CHN_BUSY))
        return -19;                             /* -ENODEV */
    if (pid != 0 && c->pid != pid)
        return -3;                              /* -ESRCH  */

    if (vol < 0)
        vol = 0;
    /* UP TO THE BOOST CEILING, not unity - 2026-10-02. The mixer now
     * applies gain into a wide accumulator, so a boosted channel
     * reaches the limiter intact rather than clipping on its own. */
    if (vol > VSOUND_VOL_BOOST)
        vol = VSOUND_VOL_BOOST;

    if (c->vol != vol) {
        c->vol = vol;
        d->generation++;
    }
    /* REMEMBERED BY NAME, so this program's next channel - Quake's
     * next track - starts here (vsound.h, VSOUND_IOC_PROGGET). */
    vsound_prog_note(d, c->comm, c->vol, (c->flags & VSOUND_CHN_MUTED) != 0);
    return 0;
}

/*
 * Set one channel's MUTE, checking the caller has the right one.
 *
 * THE SAME CHECKS AS setvol ABOVE, AND FOR THE SAME REASONS - bounds
 * first because the index is untrusted, then the pid because indices
 * are reused and a tool's list is always slightly stale. Read that
 * comment; this one does not repeat it.
 *
 * WHAT IT DELIBERATELY DOES NOT TOUCH IS `vol'. Mute is a state beside
 * the volume, not a volume of zero - see VSOUND_CHN_MUTED in
 * vsound_chan.h for why, and what went wrong in the designs that had
 * it the other way. A muted channel keeps its level, reports it, and
 * unmutes to exactly where the user left it with nothing having had to
 * remember it.
 *
 * Callers hold cli(). Returns 0, or a negative errno.
 */
int
vsound_chan_setmute(struct vsound_dev *d, int index, int pid, int muted)
{
    struct vsound_chan *c;
    unsigned int was;

    if (d == NULL)
        return -1;
    if (index < 0 || index >= VSOUND_SLOTS)
        return -22;                             /* -EINVAL */

    c = &d->chan[index];
    if (!(c->flags & VSOUND_CHN_BUSY))
        return -19;                             /* -ENODEV */
    if (pid != 0 && c->pid != pid)
        return -3;                              /* -ESRCH  */

    was = c->flags & VSOUND_CHN_MUTED;
    if (muted)
        c->flags |= VSOUND_CHN_MUTED;
    else
        c->flags &= ~VSOUND_CHN_MUTED;

    /* THE GENERATION BUMP IS WHAT MAKES A GUI NOTICE. Bumped only on a
     * real change, like setvol - an idempotent set must not make every
     * poll look like news. */
    if (was != (c->flags & VSOUND_CHN_MUTED))
        d->generation++;
    vsound_prog_note(d, c->comm, c->vol, (c->flags & VSOUND_CHN_MUTED) != 0);
    return 0;
}

/*
 * THE PER-PROGRAM TABLE - design/33 section 1c; vsound.h has the
 * account. All four are called with cli() held, like everything else
 * that touches the device.
 *
 * MATCHED ON THE WHOLE comm, which is what 2.2 gives a process: 15
 * characters and a NUL. Two programs whose names agree in those share
 * a level, which is the same thing every OSS tool of the period
 * would see.
 */
static int
prog_find(struct vsound_dev *d, const char *comm)
{
    int i;

    for (i = 0; i < 16; i++)
        if (d->prog[i].comm[0] != '\0'
            && strncmp(d->prog[i].comm, comm, 15) == 0)
            return i;
    return -1;
}

void
vsound_prog_note(struct vsound_dev *d, const char *comm, int vol, int muted)
{
    int i, j;

    if (d == NULL || comm == NULL || comm[0] == '\0')
        return;
    i = prog_find(d, comm);

    /* UNITY AND UNMUTED IS WHAT A PROGRAM GETS WITHOUT AN ENTRY - so
     * an entry that says only that is dropped, leaving room. */
    if (vol == VSOUND_VOL_UNITY && !muted) {
        if (i >= 0)
            d->prog[i].comm[0] = '\0';
        return;
    }

    if (i < 0) {
        /* A FREE ENTRY, or the one set longest ago. */
        i = 0;
        for (j = 0; j < 16; j++) {
            if (d->prog[j].comm[0] == '\0') {
                i = j;
                break;
            }
            if (d->prog[j].stamp < d->prog[i].stamp)
                i = j;
        }
        strncpy(d->prog[i].comm, comm, 15);
        d->prog[i].comm[15] = '\0';
    }
    d->prog[i].vol   = vol;
    d->prog[i].muted = muted ? 1 : 0;
    d->prog[i].stamp = ++d->prog_stamp;
}

/* AT OPEN, once the channel's comm is known and before any sample.
 * Returns 1 if a remembered level was applied - the trace says so. */
int
vsound_prog_apply(struct vsound_dev *d, struct vsound_chan *c)
{
    int i;

    if (d == NULL || c == NULL || c->comm[0] == '\0')
        return 0;
    i = prog_find(d, c->comm);
    if (i < 0)
        return 0;
    c->vol = d->prog[i].vol;
    if (c->vol > VSOUND_VOL_BOOST)
        c->vol = VSOUND_VOL_BOOST;
    if (c->vol < 0)
        c->vol = 0;
    if (d->prog[i].muted)
        c->flags |= VSOUND_CHN_MUTED;
    else
        c->flags &= ~VSOUND_CHN_MUTED;
    return 1;
}

/* COPY THE TABLE OUT, oldest first so a SET of the same list puts it
 * back in the same order. Returns how many. */
int
vsound_prog_get(struct vsound_dev *d, char comm[][16], int *vol,
                int *muted, int max)
{
    int n = 0, i, best;
    unsigned long last = 0;

    if (d == NULL)
        return 0;
    while (n < max) {
        best = -1;
        for (i = 0; i < 16; i++) {
            if (d->prog[i].comm[0] == '\0' || d->prog[i].stamp <= last)
                continue;
            if (best < 0 || d->prog[i].stamp < d->prog[best].stamp)
                best = i;
        }
        if (best < 0)
            break;
        memcpy(comm[n], d->prog[best].comm, 16);
        vol[n]   = d->prog[best].vol;
        muted[n] = d->prog[best].muted;
        last = d->prog[best].stamp;
        n++;
    }
    return n;
}

void
vsound_prog_clear(struct vsound_dev *d)
{
    int i;

    if (d == NULL)
        return;
    for (i = 0; i < 16; i++)
        d->prog[i].comm[0] = '\0';
}

/*
 * Start and stop output on a channel.
 *
 * Membership of the running set IS participation in the mix - the
 * mixer walks channels with RUNNING set and a stopped channel is simply
 * absent rather than skipped by a test inside the loop. FreeBSD says
 * the same thing with a list: vchan_trigger moves the channel on and
 * off parent->children.busy (vchan.c:159-168), and feed_mixer_feed
 * walks that list (feeder_mixer.c:316).
 *
 * Idempotent, because SETTRIGGER can legitimately be issued twice and
 * the count must not drift.
 */
void
vsound_chan_start(struct vsound_dev *d, struct vsound_chan *c)
{
    if (d == NULL || c == NULL || !(c->flags & VSOUND_CHN_BUSY))
        return;
    if (c->flags & VSOUND_CHN_RUNNING)
        return;

    /*
     * NO LONGER IDLE - design/16 section 4, Design B.
     *
     * Clearing a pending release-on-idle signal belongs HERE and not
     * in vsound_dev_write(), because MMAP CLIENTS ISSUE NO write().
     * That asymmetry is what hid the i_sem serialisation bug for
     * months (design/14) and it would have made this flag wrong in
     * exactly the same way: quake, which mmaps, would never have
     * cleared it.
     *
     * Both paths reach here - write() through its start, and
     * SETTRIGGER for mmap clients - and this is the moment audio is
     * actually flowing again, which is what the flag is about.
     */
    d->release_idle = 0;

    /*
     * PREFILL THE HARD BUFFER WITH SILENCE ON THE FIRST START.
     *
     * chn_start() does exactly this before the first trigger,
     * channel.c:725-727:
     *
     *     if (c->direction == PCMDIR_PLAY)
     *         sndbuf_fillsilence_rl(b, sndbuf_xbytes(...));
     *
     * ONLY ON THE 0 -> 1 TRANSITION. A second client starting while
     * audio is already flowing must not inject silence into a stream
     * in progress - that would be an audible gap, and the cushion it
     * would create already exists.
     *
     * WHY: without it the card starts empty, and because the mixer
     * produces one tick's worth per tick there is never anything in
     * hand to absorb a late feed. Measured on the Acer 2026-08-27:
     * 15710 of 15972 acks reported odelay 0 - continuous starvation,
     * audible as a looping DMA buffer. 86Box never showed it because
     * its SB16 reports a large enough odelay that the correction in
     * vsound_drain_ack() never fires, leaving a cushion by luck.
     *
     * See design/10-reference-recheck.md, divergence 2.
     */
    if (d->nrunning == 0) {
        /* THE FRAME SIZE, for the ack path's alignment. Derived here
         * because this is the moment the mix format is known to be
         * settled - a client has finished negotiating and is about to
         * produce. channel.c:371 calls the equivalent
         * sndbuf_getalign(). */
        int bytes = (d->mix_format == AFMT_S16_LE
                     || d->mix_format == AFMT_U16_LE) ? 2 : 1;
        bytes *= (d->mix_channels > 1) ? 2 : 1;
        d->hard.frame = bytes > 0 ? bytes : 1;

        /* A TAIL STILL WAITING IS KEPT - design/25 B8: silence goes in
         * after it rather than over it. An empty ring gets the full
         * prefill exactly as before. */
        if (vsound_buf_ready(&d->hard.b) > 0)
            vsound_buf_topup_silence(&d->hard.b, d->mix_format);
        else
            vsound_buf_fillsilence(&d->hard.b, d->mix_format, 0);
    }

    c->flags |= VSOUND_CHN_RUNNING;
    d->nrunning++;
}

void
vsound_chan_stop(struct vsound_dev *d, struct vsound_chan *c)
{
    if (d == NULL || c == NULL)
        return;
    if (!(c->flags & VSOUND_CHN_RUNNING))
        return;

    c->flags &= ~VSOUND_CHN_RUNNING;
    if (d->nrunning > 0)
        d->nrunning--;
}

/*
 * ONE PUMP. Callers hold cli().
 *
 * `who' is a reader's struct file, opaque here so this file builds on
 * the host. The first reader to read() or to arm release-on-idle
 * becomes the pump and keeps the role until it closes
 * (vsound_pump_gone()); every other reader is refused. Added
 * 2026-09-14 after two `-R' pumps ran side by side on the Acer
 * (tests/logs/2026-09-14-acer-71-...): two readers split the mixed
 * stream between them, and with -R the arming belonged to whichever
 * armed last, so the other could be left believing it was armed after
 * the owner died - the hanging note back with no symptom.
 *
 * NOT AT open(). vsoundvol and vsoundvol-gtk open O_RDONLY too, for
 * the ioctls, and never read; the role attaches to what only a pump
 * does. `cat /dev/dsp > file' beside a running pump therefore stops
 * working, deliberately - as the only reader it still claims the role
 * on its first read.
 *
 * Returns 1 if `who' just became the pump, 0 if it already was, -1 if
 * another reader is. The caller maps -1 to -EBUSY; errno values are
 * kept out of this file.
 */
int
vsound_pump_claim(struct vsound_dev *d, void *who)
{
    if (d == NULL || who == NULL)
        return -1;
    if (d->pump_owner == NULL) {
        d->pump_owner = who;
        return 1;
    }
    return d->pump_owner == who ? 0 : -1;
}

/*
 * RELEASE-ON-IDLE ARMING - the pump's VSOUND_IOC_RELEASE. Callers hold
 * cli(). Claims the pump role for `who' first, so a second reader
 * cannot take the arming from a live pump: -1 if another reader is
 * the pump, 0 otherwise.
 *
 * Arming CLEARS a pending signal: one still set when a pump arms was
 * raised for a pump that is no longer there to act on it, and left
 * alone it would hand the new pump a zero-length read on its first
 * idle for a card it holds. Disarming clears it for the reason the
 * ioctl gives - a pump that has stopped wanting signals should not
 * find one waiting. Neither changes who the pump is; only its close
 * does.
 *
 * Here rather than in vsound_dev.c so the host tests can drive it.
 */
int
vsound_release_arm(struct vsound_dev *d, void *who, int on)
{
    if (d == NULL || vsound_pump_claim(d, who) < 0)
        return -1;
    d->release_armed = on ? 1 : 0;
    d->release_idle  = 0;
    return 0;
}

/*
 * THE PUMP SAYS IT RELEASED THE CARD - may the signal be spent? Callers
 * hold cli(). Returns 1 if it cleared `release_idle', 0 if it kept it.
 *
 * design/25 B7, design/54 D31, 2026-10-04. The pump checks STAT (no
 * client, hard ring empty), closes the card - which blocks 83-160 ms
 * in the driver's drain - and only then sends RELEASED. A client that
 * opened, played and closed inside that close raised the flag AGAIN for
 * its own tail, and an unconditional clear here spent that second
 * signal: the pump read the tail, reopened the card, and never saw a
 * 0-length read again - holding the card with no client, the hanging
 * note.
 *
 * So the signal is spent only if the state is still the one the pump
 * released on: no client open AND the hard ring empty. Any client that
 * started meanwhile went through vsound_chan_start()'s 0 -> 1 prefill,
 * so the ring is not empty until the pump has read it - and a kept flag
 * just gives the second release its own cycle. "nbusy == 0" alone,
 * design/25 4.10's suggestion, does not catch it: that client has
 * already closed when RELEASED arrives.
 */
int
vsound_release_consume(struct vsound_dev *d)
{
    if (d == NULL)
        return 0;
    if (d->nbusy != 0 || vsound_buf_ready(&d->hard.b) != 0)
        return 0;
    d->release_idle = 0;
    return 1;
}

/*
 * A READER HAS CLOSED. If it was the pump, give up the role, disarm,
 * and drop any pending signal. Callers hold cli().
 *
 * design/25 B3: the flags are module state and outlived the pump. A
 * `-R' pump that exited - a crash, a kill, unload.sh racing a client
 * - left both set, and the next pump, started WITHOUT -R, read 0 on
 * its first idle and spun forever: nothing it did could clear them,
 * since clearing happens when a client starts or when the pump says
 * it released, and an unarmed pump never says so.
 *
 * Gated on the owner because any O_RDONLY opener is a reader, not
 * just the pump. Returns 1 if it disarmed, so the caller can trace
 * that.
 */
int
vsound_pump_gone(struct vsound_dev *d, void *who)
{
    if (d == NULL || who == NULL || d->pump_owner != who)
        return 0;
    d->release_armed = 0;
    d->release_idle  = 0;
    d->pump_owner    = NULL;
    return 1;
}

/*
 * How many bytes one second of this channel's audio occupies.
 *
 * The BYTE rate, not the sample rate - which is the distinction that
 * cost this project two days. quake is 11025 Hz STEREO 16-bit, so its
 * byte rate is 44100: the same number as the card's SAMPLE rate, and
 * that coincidence has misled twice. A frame there is 4 bytes, not 2.
 *
 * Anywhere a count crosses a rate boundary, this is what vsound_xbytes
 * wants on both sides.
 */
int
vsound_chan_bps(const struct vsound_chan *c)
{
    int bytes;

    if (c == NULL || c->rate <= 0)
        return 0;

    bytes = (c->format == AFMT_S16_LE || c->format == AFMT_U16_LE) ? 2 : 1;
    bytes *= (c->channels > 1) ? 2 : 1;
    return bytes * c->rate;
}

/*
 * Size a channel's buffer from its geometry.
 *
 * SIZED FROM GEOMETRY, NOT A FIXED 64 KB. This is the change 07's
 * section 4a asked for and the old tree never made. FreeBSD's
 * sndbuf_remalloc (buffer.c:214-257) is the model:
 *
 *     if (blkcnt < 2 || blksz < 16)
 *             return EINVAL;
 *     bufsize = blksz * blkcnt;
 *     allocsize = round_page(bufsize);
 *
 * The old tree allocated 64 KB per client regardless - 372 ms at CD
 * rate, far more than any client needs - inherited from "quake must see
 * a sane fragstotal", which is a REPORTED number rather than an
 * allocation requirement. At quake's negotiated 4 x 1024 this allocates
 * 4 KB, a sixteenth of that, which is what makes a 64 MB target
 * comfortable with several channels open.
 *
 * The floor is the reference's: fewer than two blocks cannot
 * double-buffer, and a block under 16 bytes is smaller than a single
 * frame at any format we support.
 *
 * blkcnt == 0 releases the buffer, which is how a channel gives its
 * memory back at close.
 *
 * The allocator is passed in rather than called directly: this file is
 * built for the host tests as well as for the module, and vmalloc plus
 * the PG_reserved walk only exist in the kernel. vsound_dsp.c supplies
 * the real pair.
 */
int
vsound_chan_setgeom(struct vsound_chan *c, unsigned int blkcnt,
                    unsigned int blksz,
                    unsigned char *(*alloc)(unsigned int),
                    void (*release)(unsigned char *, unsigned int))
{
    unsigned int bufsize;
    unsigned char *buf;

    if (c == NULL)
        return -1;

    /* Release. */
    if (blkcnt == 0 || blksz == 0) {
        if (c->b.buf != NULL && release != NULL)
            release(c->b.buf, c->b.bufsize);
        c->b.buf     = 0;
        c->b.bufsize = 0;
        c->b.blkcnt  = 0;
        c->b.blksz   = 0;
        c->b.rp      = 0;
        c->b.rl      = 0;
        c->b.total   = 0;
        return 0;
    }

    /* buffer.c:219-220, and the same reasons. */
    if (blkcnt < 2 || blksz < 16)
        return -1;

    bufsize = blkcnt * blksz;

    /*
     * Already the right size: keep it. Reallocating a buffer a client
     * may have mapped is not something to do casually, and the
     * reference takes the same view - sndbuf_remalloc leaves the
     * allocation alone unless the size genuinely moved
     * (buffer.c:224-225).
     */
    if (c->b.buf != NULL && c->b.bufsize == bufsize) {
        c->b.blkcnt = blkcnt;
        c->b.blksz  = blksz;
        return 0;
    }

    if (alloc == NULL)
        return -1;

    buf = alloc(bufsize);
    if (buf == NULL)
        return -1;

    if (c->b.buf != NULL && release != NULL)
        release(c->b.buf, c->b.bufsize);

    c->b.buf     = buf;
    c->b.bufsize = bufsize;
    c->b.blkcnt  = blkcnt;
    c->b.blksz   = blksz;
    c->b.rp      = 0;
    c->b.rl      = 0;
    c->b.total   = 0;

    /* Geometry the client is TOLD, which is not always what it asked
     * for - it is what it will actually get. */
    c->frag     = blksz;
    c->window   = bufsize;
    c->rep_frag = blksz;

    return 0;
}
