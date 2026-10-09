/*
 * vsound_chan.h - the buffer, channel and device objects.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 * The FreeBSD-derived parts are BSD-2-Clause; their notice is below.
 *
 * design/07-vsound.md sections 5 and 7. Modelled on FreeBSD's
 * sound/pcm, which is in freebsd-pcm-reference/ under BSD-2-Clause.
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
 * The objects mirrored, all verified against the reference 2026-08-24:
 *
 *     struct snd_dbuf      -> struct vsound_buf     (buffer.h:39-61)
 *     struct pcm_channel   -> struct vsound_chan    (channel.h:74-166)
 *     struct snddev_info   -> struct vsound_dev     (sound.h:369-398)
 *
 * WHAT CHANGED FROM vcd/vcdsnd_chan.h, and why this is not a copy.
 *
 * That header says "we deliberately do NOT mirror bufhard... a channel
 * here has one buffer". Reading the reference end to end showed the
 * conclusion was right and the reason was wrong, and the difference
 * matters:
 *
 *   - A vchan genuinely has NO second buffer. chn_init gives bufsoft
 *     memory and leaves bufhard empty (channel.c:1206-1207);
 *     vchan_init never touches it (vchan.c:63-98); and
 *     channel.c:1238-1239 makes a zero-sized bufhard an error for every
 *     channel EXCEPT a virtual one. So per-channel is correct.
 *
 *   - But FreeBSD has ONE bufhard, on the hardware channel, and vcd
 *     had no equivalent at all - the role was split across a 40 KB
 *     fragment slot, whatever the daemon held, and the card's own
 *     371 ms, with no piece knowing the free-space figure. That figure
 *     is what the whole structure earns its keep through
 *     (chn_wrfeed's `wasfree', channel.c:403-407).
 *
 * So: one buffer per channel, as before, PLUS one device-level hard
 * buffer that did not exist. See struct vsound_dev.
 */

#ifndef _VSOUND_CHAN_H
#define _VSOUND_CHAN_H

/* THE TRACE - design/54 section 8, 2026-10-08. Every file of the
 * module calls vsound_vt_printf() where it called printk(KERN_DEBUG
 * ...); vsound_dev.c includes the implementation once. */
#define VTRACE_MOD vsound
#include "../common/vtrace.h"

/*
 * How many streams can exist at once.
 *
 * FOUR, and it is the machine that decides that rather than the code.
 * What a 1999 desktop actually has playing:
 *
 *     desktop sound effects       occasional, brief
 *     a messenger notification    occasional, brief
 *     the active application      continuous
 *     CD audio                    continuous
 *
 * Four, of which only two are sustained - and those two, an application
 * plus CD audio, are the pair already tested together. This is not a
 * modern machine with a dozen things making noise at once.
 *
 * The shape matters as much as the count: the occasional streams open,
 * play a few hundred milliseconds and close, so slots free constantly.
 * A fixed table handles that better than a growing one would - a
 * notification reuses slot 2 forever instead of extending the table
 * every time.
 *
 * ON CPU COST, and an earlier version of this comment overstated it:
 * "86Box struggles with two clients, so four is near the ceiling" turns
 * one measurement into a general claim, and it is wrong.
 *
 * Two 3D games at once is close to the worst possible pair - both
 * rendering, both generating audio continuously, quake software-
 * rasterising on a 450 MHz machine. That says something about THOSE
 * CLIENTS, not about four channels.
 *
 * The cost is per RUNNING, RESAMPLING channel, not per channel:
 *
 *     idle channel            zero - vsound_mix_run skips it entirely
 *     CD audio at 44100       a copy; ratio 1:1, no conversion
 *     quake at 11025          4:1 interpolation
 *     a brief notification    4:1, but for a few hundred ms
 *
 * So four channels where three are idle or 1:1 is far cheaper than two
 * channels both resampling. The realistic four is not a heavy load.
 *
 * Cost of a slot when idle: 152 bytes of static table. Buffers are
 * allocated at open and freed at close, so a free channel holds no
 * memory.
 *
 * Growing this is FUTURE WORK, not a missing feature - see
 * design/07-vsound.md section 3.15.
 */
#define VSOUND_MAX_CHAN     4

/*
 * THE RESERVED SLOT, for vmidi - design/p3-midi-plan.md section 4 and
 * design/09-cleanup-step.md.
 *
 * A userspace MIDI synth writing PCM to /dev/dsp is an ordinary client
 * and would compete for one of the four - so starting music could deny
 * a game its audio, and which one loses would depend on start order.
 *
 * SO IT GETS A SLOT NOTHING ELSE CAN BE GIVEN. The array is one longer
 * than VSOUND_MAX_CHAN, `vsound_chan_alloc()' never looks past index
 * VSOUND_MAX_CHAN - 1, and only `vsound_chan_alloc_midi()' hands out
 * the last one. Ordinary clients still get four. Nobody loses anything.
 *
 * IT IS THE LAST SLOT, NOT SLOT 0 - moved 2026-09-07. With the
 * reserved slot first, every ordinary index shifted by one whenever
 * MIDI was on, which needed a `first_chan' in the allocator, a second
 * counter in the CHANS ioctl, and produced an off-by-one in
 * vsoundvol that set the wrong stream's volume. With it last, slots
 * 0..3 mean the same thing on every machine and the reserved one is
 * simply present or not.
 *
 * WHO GETS IT: whoever opens /dev/dsp with O_EXCL. The VFS keeps that
 * flag in f_flags until after the driver's open() has run
 * (fs/open.c:648, :676) and gives it no meaning of its own on a
 * device node (fs/namei.c: consulted only under O_CREAT), so it is a
 * bit that reaches exactly one place. No OSS client passes it -
 * quake, lxdoom, vdiscd and tonetest were all checked - and it costs
 * no second minor, which the alternative did. See vsound_dev_open().
 *
 * HELD WHILE THE SYNTH HAS IT OPEN, not from insmod to rmmod. The
 * synth closes it when nothing has sounded for a while so that a
 * pump running -R can release the card, and reopens on the next MIDI
 * byte. That is safe precisely because nothing else opens with
 * O_EXCL: the slot cannot be taken in the gap.
 *
 * AND IT NEEDS NO PRIVILEGED-CLIENT CONCEPT. vsound does not identify
 * a caller; it acts on a flag the caller chose to pass. Ordinary
 * clients cannot be given the slot because they are never offered it.
 *
 * Cost when MIDI is off: 152 bytes of table that alloc_midi refuses
 * to touch. When on: the same table, and a buffer only while the synth
 * holds it.
 */
#define VSOUND_MIDI_SLOT    VSOUND_MAX_CHAN
#define VSOUND_SLOTS        (VSOUND_MAX_CHAN + 1)

/* 100 is unity, matching every OSS control's 0..100 scale. Kept here
 * as well as in vsound.h so the kernel side does not depend on the
 * userspace header for a bound it enforces. */
#define VSOUND_VOL_UNITY    100

/* The ceiling ABOVE unity - include/vsound.h has the account; the build
 * passes the same -DVSOUND_VOL_BOOST to both. */
#ifndef VSOUND_VOL_BOOST
#define VSOUND_VOL_BOOST    200
#endif

/*
 * THE RATES A CLIENT MAY ASK FOR. design/25 B9, 2026-09-14: SPEED
 * accepted any v > 0, and at 1048576 Hz and above `(long) (count / 4)
 * * c->rate' in the converting pull overflows a 32-bit long, inbytes
 * goes negative and the channel is silent. The reference clamps; so
 * do we, to the range every rate pair in NOTES-freebsd-rate.md was
 * checked for. A client asking outside it is granted the bound and
 * told so through the ioctl's return, which is OSS's contract.
 */
#define VSOUND_RATE_MIN     4000
#define VSOUND_RATE_MAX     48000


/*
 * A ring buffer: ONE POINTER AND ONE LENGTH.
 *
 * buffer.h:46-47, and not a stylistic choice. FreeBSD tracks `rp'
 * (where the ready data starts) and `rl' (HOW MUCH is ready), and
 * derives the rest - verified against buffer.c:
 *
 *     ready    = rl                       (sndbuf_getready)
 *     free     = bufsize - rl             (sndbuf_getfree)
 *     readyptr = rp                       (sndbuf_getreadyptr)
 *     freeptr  = (rp + rl) % bufsize      (sndbuf_getfreeptr)
 *
 * A pointer plus a length cannot be ambiguous: rl == 0 is empty,
 * rl == bufsize is full. Two indices, where head == tail means either
 * empty or full, is a class of bug this structure does not have
 * available to it.
 *
 * `volatile' on rp and rl matches the reference (buffer.h:46-48): they
 * are read from a timer and written from process context.
 *
 * `total' is monotonic and never wrapped-for, so position arithmetic
 * needs no wrap handling. FreeBSD uses 64 bits (buffer.h:49); 32 is
 * enough here - at 176400 B/s it wraps after about six and three
 * quarter hours, and only DIFFERENCES are ever taken.
 */
struct vsound_buf {
    unsigned char  *buf;        /* vmalloc'd, or NULL when closed     */
    unsigned int    bufsize;    /* allocated bytes                    */
    volatile int    rp;         /* start of the ready area            */
    volatile int    rl;         /* LENGTH of the ready area           */
    unsigned long   total;      /* bytes ever acquired; never wrapped */
    unsigned int    blksz;      /* fragment size                      */
    unsigned int    blkcnt;     /* fragments in the window            */
};

/* Channel flags, after channel.h:352-377. Only the ones that earn their
 * place: a flag nothing tests is a comment with a bit pattern. */
#define VSOUND_CHN_BUSY      0x0001  /* opened                        */
#define VSOUND_CHN_MMAP      0x0002  /* has been mmap'd               */
#define VSOUND_CHN_RUNNING   0x0004  /* SETTRIGGER enabled output     */
#define VSOUND_CHN_CLOSING   0x0008  /* in release; suppress xruns    */
#define VSOUND_CHN_GEOM_READ 0x0010  /* client has read the geometry  */
#define VSOUND_CHN_GEOM_SET  0x0020  /* settled; must not move        */
#define VSOUND_CHN_FRAG_SET  0x0040  /* client named a fragment size  */
/*
 * MUTED IS A STATE, NOT A VOLUME OF ZERO - and getting that wrong is
 * what three designs in a row did before the user stopped them
 * (2026-09-18).
 *
 * Setting vol to 0 and remembering the old value somewhere else is the
 * easy way and the wrong one: it destroys the level to express the
 * mute, so something has to shadow it, and that shadow then has to
 * live as long as the stream. Every userspace home for it dies too
 * early - a GUI's struct dies with the window, leaving a muted client
 * stranded at volume 0 with nothing remembering what it was.
 *
 * The reference keeps them apart for the same reason: `mutedevs' is a
 * bitmask beside `level_muted[]' (mixer.c:59-63), and mixer_get()
 * returns the LEVEL of a muted device rather than 0 (:341). The level
 * is preserved, not overwritten.
 *
 * So: `vol' keeps whatever the user set, this flag gates whether it is
 * heard, and the two are read back independently. A slider at 60 with
 * the mute lit is the honest picture of a muted channel, and it is
 * what KMix draws.
 */
#define VSOUND_CHN_MUTED     0x0080  /* silent; vol is left intact    */

/*
 * One application's audio stream - a vchan, in the reference's terms.
 *
 * ONE READ POSITION. There is no `harvest' and no second pointer of any
 * kind. FreeBSD has nothing of the sort: a buffer is rp + rl + hp +
 * total, and for a mapped channel the acquire fills rl while the feed
 * drains through rp (feeder_mixer.c:330-332 then FEEDER_FEED). Two
 * positions is a bug waiting to happen and did happen - they held
 * counts in DIFFERENT UNITS, input bytes and output bytes, four apart
 * at 11025 -> 44100, and the read ran four times too fast.
 */
struct vsound_chan {
    struct vsound_buf b;

    unsigned int    flags;

    /* The negotiated format, per channel. */
    int             rate;
    int             channels;
    int             format;

    /*
     * Where the client has written to. Per-channel because this is the
     * state that leaked between clients on the old tree: it was reset
     * only inside the write path, so a write() client left it non-zero
     * and the next client inherited it.
     */
    unsigned int    wpos;

    /* Geometry, per channel. */
    unsigned int    frag;
    unsigned int    window;
    unsigned int    target_ms;
    unsigned int    rep_frag;   /* what the client was TOLD           */

    /*
     * The resampler's state, carried ACROSS calls.
     *
     * z_gx/z_gy is the rate ratio after GCD reduction
     * (feeder_rate.c:1105-1110); z_dx is the interpolation distance
     * scaled into Z_LINEAR_ONE (:1235-1241); z_alpha is the fractional
     * position between the two frames in hand, in units of z_gy - and
     * between calls it may sit AT OR ABOVE z_gy, which means a crossing
     * is owed and the next call settles it before producing anything.
     * z_prev0/z_prev1 is the frame BEHIND the phase and z_cur0/z_cur1
     * the frame AHEAD of it: linear interpolation runs from prev
     * toward cur, zero-order hold repeats cur. BOTH are carried, so a
     * frame the phase has read but not yet passed survives the end of
     * a call - before 2026-09-14 only prev was, and the ahead frame was
     * lost with the caller's buffer once per pass (design/25 4.1).
     *
     * z_pos is the converter's REPORT: whole input frames its last
     * call consumed, which is what the caller then disposes from the
     * ring. Not a position within the stream - that is z_alpha and
     * the two frames.
     *
     * CARRYING THE PHASE IS THE POINT. Deriving the source index from
     * the output index instead restarts phase at every fragment
     * boundary: inaudible at an exact ratio like 1:4, and a dropped or
     * duplicated fraction of a frame at every boundary for anything
     * else. Only five of the fourteen rate pairs we can meet are exact
     * - see vcd/contrib/refs/NOTES-freebsd-rate.md.
     */
    int             z_gx, z_gy;
    unsigned long   z_dx;
    int             z_alpha;
    int             z_pos;
    int             z_prev0, z_prev1;
    int             z_cur0, z_cur1;
    int             z_primed;

    /*
     * Who owns this channel. The reference records BOTH - `pid_t pid'
     * and `char comm[MAXCOMLEN + 1]' (channel.h:86,108), set at open
     * from the caller (sound.c:330-331) - and both are needed here for
     * different reasons.
     *
     * `pid' is the IDENTITY a control tool checks against before
     * writing a volume: indices are reused when one client exits and
     * another opens, so an index alone is not safe to hold across a
     * poll interval (section 3.14).
     *
     * `comm' is what makes a slider say "quake" rather than "pid 541".
     * 2.2's task_struct has comm[16], so there is nothing longer to
     * copy.
     */
    int             pid;
    char            comm[16];

    /*
     * Per-stream gain, 0..VSOUND_VOL_MAX, applied before this channel
     * joins the mix. The reference does the same with feeder_volume as
     * a chain stage per channel.
     *
     * 100 is unity and the default: a client that never asks for a
     * volume must sound exactly as it did before this existed.
     */
    int             vol;

    struct wait_queue *wait;

    /* WHAT THIS CHANNEL ACTUALLY CONTRIBUTED, cumulative.
     *
     * A channel can be RUNNING and produce nothing - its ring empty
     * when the tick fires, or a stale channel left flagged by a client
     * that died. The device-level trace cannot show it: `mixed ==
     * want' whenever the OTHER channels filled the request, so a
     * silent client is indistinguishable from a working one.
     *
     * Found on the Acer 2026-08-27: three clients running, two
     * audible, and 100k lines of trace could not say which was mute.
     * Reported by VSOUND_IOC_STAT and in the release line. */
    unsigned long   mixed_bytes;
    unsigned long   empty_pulls;    /* pulls that returned nothing */
    /*
     * SPACE FREED IN THIS MIX PASS, for the wake condition.
     *
     * The reference wakes a channel only when the mix actually
     * consumed from it (channel.c:417):
     *
     *     if (sndbuf_getfree(b) < wasfree)
     *             chn_wakeup(c);
     *
     * Ours woke every BUSY channel every tick regardless - review
     * finding A-8 in design/12-reference-audit.md. Set by
     * vsound_mix_run() from what vsound_mix_pull() returned, and
     * cleared by the waker once it has been acted on. Not a running
     * total: it is a per-pass flag with a size attached.
     */
    int             freed_this_pass;
};

/*
 * THE HARD BUFFER - one, device level. This is what vcd never had.
 *
 * FreeBSD's bufhard lives on the single hardware channel and is the
 * thing the DMA engine reads from. Ours is read by vsoundd instead,
 * because a kernel module cannot reach esssolo1 (07's section 2), but
 * the ROLE is identical: it is the one object that knows how much room
 * exists downstream, and chn_wrfeed bounds the whole mix by its free
 * space (channel.c:403-407).
 *
 * `written' and `played' are the two monotonic counters of 07's
 * section 4.1. outstanding = written - played, and it increments the
 * moment read() hands bytes out rather than waiting for an ack - which
 * is what makes the measured ~40 ms ack quantisation delay the recovery
 * of space without ever delaying the kernel's knowledge of what is in
 * flight.
 *
 * `card_bytes' is the card's real buffer size, reported by vsoundd from
 * SNDCTL_DSP_GETOSPACE. Asked, never assumed: on 86Box it measured
 * 16 x 4096 = 371 ms at 44100 stereo.
 */
struct vsound_hard {
    struct vsound_buf b;        /* the ring vsoundd drains            */

    unsigned long   written;    /* bytes handed to vsoundd            */
    unsigned long   played;     /* OUR retired count, in our units    */
    unsigned long   card_played;/* the card's own counter, last seen  */
    unsigned int    card_frag;  /* GETOSPACE fragsize - NOT the total */

    int             have_played;/* the card answers GETOPTR at all    */
    unsigned long   underruns;  /* pump asked, we had nothing ready    */

    /* THE MIX FORMAT'S FRAME SIZE, so a retired delta can be rounded
     * down to a whole frame - `sndbuf_getalign' in the reference, used
     * by chn_dmaupdate at channel.c:371. Set when the format is fixed;
     * 0 means "not known yet", and the alignment is then skipped
     * rather than dividing by zero. */
    int             frame;

    /* XRUNS: the buffer held less than was wanted. channel.c:414-415
     * counts exactly this and vsound did not, which is why continuous
     * starvation on the Acer was invisible except by reading odelay
     * out of a 4 MB trace. See design/10-reference-recheck.md. */
    unsigned long   xruns;
};

/*
 * The device: every channel, the hard buffer, and the output format.
 *
 * FreeBSD keeps three channel lists - all, busy, opened
 * (sound.h:370-380) - because they answer different questions. A fixed
 * table plus flags expresses the same thing without list surgery under
 * cli().
 */
struct vsound_dev {
    /*
     * VSOUND_SLOTS, not VSOUND_MAX_CHAN: one spare for vmidi, at the
     * end. The extra slot is only ever ALLOCATED through
     * vsound_chan_alloc_midi(), which vsound_dev_open() calls only
     * when vsound_midi is set - so a machine with no MIDI pays 152
     * bytes of table and nothing else.
     */
    struct vsound_chan chan[VSOUND_SLOTS];
    struct vsound_hard hard;

    /*
     * The format the MIXED stream is produced in, and which vsoundd
     * writes to the card. FreeBSD calls this the vchan format and rate
     * (sound.h:392, vchan.h:53-54, S16_LE stereo at 48000); ours is CD
     * rate, because that is what the CD path produces and what the card
     * is opened at.
     *
     * Every channel converts INTO this. A channel that already matches
     * needs no conversion at all.
     */
    /*
     * DEVICE-WIDE AND DELIBERATELY FIXED. Set once in
     * vsound_dev_init() and never again - there is no ioctl to change
     * them and there should not be. Every channel converts INTO this
     * format (section 3 of the design), so moving it would invalidate
     * every channel's conversion state at once, and a client that
     * wanted a different rate would be imposing it on every other
     * client.
     *
     * Review finding 8: they LOOK configurable because they are plain
     * fields read in several places, and a reader may go looking for
     * the setter. There isn't one. Verified 2026-08-25 - the only
     * writes in the tree are the three in vsound_dev_init().
     *
     * If the device format ever does need to move, it is a
     * device-level operation that has to re-run vsound_conv_setup()
     * for every open channel, not a per-client ioctl.
     */
    int             mix_rate;
    int             mix_channels;
    int             mix_format;

    int             nbusy;      /* channels with BUSY set             */
    int             nrunning;   /* channels with RUNNING set          */

    /*
     * Bumped on every volume change, open and close - see section 3.14.
     * A polling tool compares this one integer and does nothing further
     * when it has not moved, which is what makes a 250 ms poll free.
     */
    unsigned long   generation;

    /*
     * RELEASE-ON-IDLE - design/16 section 4, Design B.
     *
     * `release_armed' is the pump saying it wants to know (-R, via
     * VSOUND_IOC_RELEASE). `release_idle' is the kernel saying it has
     * happened: the last client has closed and nothing is running.
     *
     * WHY A FLAG AND NOT ONLY A WAKEUP. An event missed is lost; a
     * flag stays set until it is acted on. vsound_dev_read() polls at
     * 20 Hz anyway (schedule_timeout(HZ/20)), so the flag is seen
     * within 50 ms even when the wake_up that vsound_dev_release()
     * sends with it (since 2026-09-14) finds the read between sleeps
     * and is lost - well inside the ~100 ms reclaim window measured
     * in design/16, and far inside the one second the user called
     * acceptable. When the wake lands, the signal is a scheduler
     * latency away instead.
     *
     * `pump_owner' is THE PUMP - the struct file of the one reader
     * allowed to drain, held as an opaque pointer so this header
     * stays host-buildable. Claimed by the first reader to read() or
     * to arm release-on-idle, held until that reader closes, and
     * every other reader gets -EBUSY from both. Two reasons, both
     * from 2026-09-14:
     *
     * - The flags outlive a process: a `-R' pump that died left them
     *   set in a module that stayed loaded, and the next pump -
     *   started WITHOUT -R - read 0 on its first idle and spun
     *   (design/25 B3). So the pump's close clears them, and ONLY the
     *   pump's: any O_RDONLY opener is a reader (vsoundvol is one),
     *   and one closing beside a live pump must not disarm it.
     * - Two pumps were run side by side on the Acer
     *   (tests/logs/2026-09-14-acer-71-...): two readers split the
     *   mixed stream between them, and the arming belonged to
     *   whichever armed last, so the other was left believing it was
     *   armed after the owner died - the hanging note with no symptom.
     *   The role stops a second reader at its first read().
     *
     * See vsound_pump_claim(), vsound_release_arm(), vsound_pump_gone().
     */
    int             release_armed;
    int             release_idle;
    void           *pump_owner;

    /*
     * EACH PROGRAM'S REMEMBERED LEVEL, by name - vsound.h's
     * VSOUND_IOC_PROGGET has the account. `stamp' orders the entries
     * by when they were last set, so a full table gives up the oldest.
     * An entry with comm[0] == '\0' is free.
     */
    struct {
        char            comm[16];
        int             vol;
        int             muted;
        unsigned long   stamp;
    }               prog[16];           /* VSOUND_PROG_MAX              */
    unsigned long   prog_stamp;
};

/*
 * The ring primitives, in vsound_buf.c.
 *
 * DECLARED HERE, and it took the target compiler to notice they were
 * not: gcc 2.95.2 warned "implicit declaration of function
 * `vsound_buf_acquire'" where the host gcc said nothing. C89 gives an
 * undeclared function an implicit int return, which is accidentally
 * correct for these and would stop being correct the moment one
 * returned anything else. Exactly what check-gcc295 exists to catch.
 */
/* The per-program table, in vsound_chan.c. Callers hold cli(). */
void vsound_prog_note(struct vsound_dev *d, const char *comm, int vol,
                      int muted);
int  vsound_prog_apply(struct vsound_dev *d, struct vsound_chan *c);
int  vsound_prog_get(struct vsound_dev *d, char comm[][16], int *vol,
                     int *muted, int max);
void vsound_prog_clear(struct vsound_dev *d);

int  vsound_buf_acquire(struct vsound_buf *b, const unsigned char *from,
                        int count);
int  vsound_buf_dispose(struct vsound_buf *b, unsigned char *to, int count);
int  vsound_buf_peek(struct vsound_buf *b, unsigned char *to, int count);
unsigned char vsound_buf_silence(int format);
void vsound_buf_reset(struct vsound_buf *b, int format);
void vsound_buf_fillsilence(struct vsound_buf *b, int format, int count);
/* Fill only the FREE space with ready silence, keeping what is ready -
 * the 0 -> 1 prefill when a tail is still waiting (design/25 B8). */
void vsound_buf_topup_silence(struct vsound_buf *b, int format);

/* Counts every clamped acquire or dispose - see vsound_buf.c.
 * NOTE: this is a BUFFER-LENGTH clamp, nothing to do with sample
 * clipping - the two share a word and answer different questions. */
extern unsigned long vsound_buf_clamped;

/* Sample saturation in the mix - see vsound_mix.c. `clipped' is
 * samples, `clip_passes' is mix passes in which any sample clipped,
 * and the second is what VSOUND_IOC_STAT reports. */
extern unsigned long vsound_mix_clipped;
extern unsigned long vsound_mix_clip_passes;

/*
 * TRACE RATE LIMITING, ported from the old tree where it was measured
 * rather than guessed: per-fragment traffic was 2516 of 6668 lines in
 * the 2026-08-24 capture - a quarter of the file describing a handoff
 * that was working correctly. The lines that settle questions (open,
 * close, mmap, geometry, trigger, refusals) are one-offs and get lost
 * in it.
 *
 *     vsound_ratelimit=1     every line
 *     vsound_ratelimit=10    every tenth
 *     vsound_ratelimit=100   every hundredth - THE DEFAULT
 *     vsound_ratelimit=0     none at all; lifecycle events only
 *
 * THE DEFAULT WAS 10 AND IS NOW 100, raised 2026-08-25 once the
 * pipeline stopped being the thing under suspicion.
 *
 * 10 was right while every tick was evidence. It is not now: audio
 * works on every run, the loop closes every time, and what a reader
 * actually wants is the lifecycle lines. In the 2026-08-25 vsoundvol
 * run there were 4332 per-fragment lines against 34 lifecycle ones -
 * the useful part was 0.8% of the file, and the two volume changes
 * that mattered were buried in it. That is the same ratio the old tree
 * measured: 2516 of 6668.
 *
 * At 100 a run samples about 144 of each message, which is enough to
 * see the steady state, confirm want == mixed and average the latency,
 * and a capture drops from ~253 KB to ~25 KB. 200 saves little more
 * and starts thinning the sample enough that an intermittent fault
 * could slip between prints; 100 keeps a 10x margin on that.
 *
 * `-r 10' or `-r 1' is one flag away when something is under suspicion
 * again, and THAT is when deep tracing earns its cost.
 *
 * The old comment records that an earlier attempt at thinning was
 * removed for "not capturing enough". That attempt was HARDCODED; this
 * is a parameter, which is the difference.
 *
 * LIFECYCLE EVENTS ARE NEVER RATE LIMITED, whatever this is set to.
 * They are what a capture is usually for.
 *
 * ONE COUNTER PER SITE, so a quiet site is not silenced by a busy one.
 * Deliberately NOT under cli(): a missed increment costs one trace
 * line, and taking a lock for a diagnostic would change the timing
 * being diagnosed.
 */
extern int vsound_ratelimit;

/* VSOUND_RL below uses BOTH of these, so both belong here rather than
 * relying on every file that expands the macro having its own extern.
 * vsound_mix.c did not, and the omission failed the cross-build with
 * `vsound_trace undeclared' - after `make' had already reported the
 * PREVIOUS object as linked, which reads as success. */
extern int vsound_trace;


#define VSOUND_RL(counter)                                              \
    (vsound_trace && vsound_ratelimit > 0                               \
     && ((counter)++ % vsound_ratelimit) == 0)

/*
 * THE SAME, WITH ITS OWN RATE. `vsound_ratelimit' is the PER-FRAGMENT
 * rate and keeps that meaning; a site that wants its own uses this and
 * names the variable holding it.
 *
 * 0 disables the site entirely, which is the point: on 2026-08-27 a
 * run that needed every channel line also produced 4 MB of
 * per-fragment output, and the answer was in a few hundred lines.
 */
#define VSOUND_RL_AT(counter, rate)                                     \
    (vsound_trace && (rate) > 0 && ((counter)++ % (rate)) == 0)

/*
 * TIMESTAMP EVERY TRACE LINE.
 *
 * printk carries no time on 2.2, so a 34000-line capture has NO TIME
 * AXIS: two lines a minute apart look exactly like two in the same
 * microsecond. On 2026-08-27 that led to reading `vdisc: unloaded'
 * followed by an oops as cause and effect when the two were separated
 * by however long it took a person to type the next command.
 *
 * jiffies at HZ=100 is centisecond resolution, which is far more than
 * enough to tell those apart.
 */
#define VSOUND_TS   "[%lu] "
#define VSOUND_TSA  jiffies,

/* Buffer arithmetic. Static rather than macros so the types are
 * checked; C89 has no inline. gcc 2.95.2 does NOT inline these at -O2 -
 * it emits calls, since -O2 there does not include -finline-functions
 * (design/25 I5; this comment said otherwise until 2026-10-04). Each is
 * the reference's accessor of the same name. */

static int vsound_buf_ready(const struct vsound_buf *b)
{
    return b->rl;
}

static int vsound_buf_free(const struct vsound_buf *b)
{
    return (int) b->bufsize - b->rl;
}

static int vsound_buf_readyptr(const struct vsound_buf *b)
{
    return b->rp;
}

static int vsound_buf_freeptr(const struct vsound_buf *b)
{
    return (int) (((unsigned int) b->rp + (unsigned int) b->rl)
                  % b->bufsize);
}

/*
 * Convert a byte count between two BYTE rates.
 *
 * snd_xbytes() (buffer.h:137-145), which exists because a count is
 * meaningless without the rate it was measured at. The reference
 * converts by `align * speed' (buffer.c:586), so channels and sample
 * width are included - pass byte rates, not sample rates.
 *
 * The units bug of 2026-08-24 is what the absence of this costs: a
 * count in output bytes used where the code wanted input bytes, four
 * apart at 11025 -> 44100, with nothing in the expression to mark the
 * difference. Anywhere a count crosses a rate boundary should say so by
 * calling this.
 *
 * The reference guards `from == to' before `from == 0'; same order
 * here. 32-bit throughout where FreeBSD uses a u_int64_t intermediate,
 * and design/25 B5 (fixed 2026-09-14) is why the arithmetic below is
 * not the obvious `v * to / from': the comment that stood here bounded
 * the product by "rates by 48000", but to_bps is a BYTE rate - 192000
 * at 48000 Hz stereo 16-bit - and v reaches 32768 (the ring plus what
 * is outstanding), so 32768 x 192000 is 6.29e9, past 2^32 on i386.
 * GETODELAY then reported 11318 where 35657 was right, for any client
 * above 131072 bytes/s that asked.
 *
 * So: reduce the two rates by their gcd, then split v into whole and
 * remainder parts of `from' and multiply each separately. The only
 * product left is `r * to' with r < from, both AFTER reduction; every
 * OSS rate is a multiple of 25 and the card's 176400 shares far more
 * with each (147:160 against 192000, 4:1 against 44100 mono, 441:20
 * against 8000), so the reduced product is a few thousand at worst.
 * No long long: the wire rule (C89) is kept, and nothing here needs it.
 */
static int vsound_xbytes(int v, int from_bps, int to_bps)
{
    unsigned long f, t, a, b, q, r;

    if (v <= 0 || from_bps <= 0)
        return 0;
    if (from_bps == to_bps)
        return v;

    f = (unsigned long) from_bps;
    t = (unsigned long) to_bps;
    a = f;
    b = t;
    while (b != 0) {                    /* gcd, feeder_rate.c:399-411 */
        unsigned long w = a % b;
        a = b;
        b = w;
    }
    f /= a;
    t /= a;

    q = (unsigned long) v / f;
    r = (unsigned long) v % f;
    return (int) (q * t + (r * t) / f);

}

/*
 * How much of the hard buffer is genuinely free.
 *
 * 07's section 4.1: physical room, minus what has been handed to
 * vsoundd but not yet reported played. This is the number chn_wrfeed
 * calls `wasfree' (channel.c:403), and the reason it can be trusted is
 * that its second term comes from the card rather than from a clock.
 */
static int vsound_hard_outstanding(const struct vsound_hard *h)
{
    unsigned long d = h->written - h->played;   /* wrap-safe: monotonic */

    if (d > (unsigned long) h->b.bufsize)
        return (int) h->b.bufsize;              /* cannot exceed the ring */
    return (int) d;
}

static int vsound_hard_free(const struct vsound_hard *h)
{
    int f = vsound_buf_free(&h->b) - vsound_hard_outstanding(h);

    return f > 0 ? f : 0;
}

#endif /* _VSOUND_CHAN_H */
