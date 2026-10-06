/*
 * vsound_dev.c - the device: module init, the OSS surface, and the
 *                clock that paces it.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 * The FreeBSD-derived parts are BSD-2-Clause; their notice is below.
 *
 * design/07-vsound.md sections 3 and 7a. The reference's equivalent is
 * split between sound.c (device lifetime, channel allocation) and
 * dsp.c (the OSS ioctls); freebsd-pcm-reference/, FreeBSD stable/13,
 * BSD-2-Clause:
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
 * STEP 1 OF 7a: THE OUTPUT IS DISCARDED.
 *
 * There is no daemon and no pump yet. A client opens, negotiates,
 * writes or maps, and the timer consumes its audio at the correct rate
 * and throws it away. That is deliberate and it is the whole point of
 * doing this first: it makes the channel layer, the mixer, the
 * converter and the mapping testable on the target with nothing else
 * existing. A client that runs to completion at the right speed, with
 * no stutter, verifies four files at once.
 *
 * It is also the old tree's null-driver behaviour, kept for the reason
 * that tree kept it: an application must never hang because the daemon
 * is absent.
 *
 * WHY register_sound_dsp AND NOT sound_install_audiodrv. The legacy
 * core would give us audio_devs[] membership and DMAbuf's machinery,
 * but it also assumes an ISA DMA engine underneath - get_dma_residue(),
 * disable_dma() - which we do not have. Owning the file_operations
 * means every ioctl is answered here, from state we hold, with no
 * DMAbuf path to satisfy. esssolo1 registers the same way
 * (esssolo1.c:2245), which is precisely why it is invisible to
 * audio_open() and why the mixing has to be ours (section 2).
 */

#ifndef __KERNEL__
#error "vsound_dev.c is kernel-only"
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/soundcard.h>
#include <linux/proc_fs.h>   /* create_proc_entry - /proc/vsound */
#include <linux/sound.h>       /* register_sound_dsp */
#include <linux/poll.h>
#include <linux/malloc.h>     /* kmalloc - the program-level ioctls */
#include <asm/uaccess.h>

#include "vsound_chan.h"
#include "vsound.h"

/* --- module parameters ---------------------------------------------- */

int vsound_trace;

/*
 * THE LATENCY KNOB (07's sections 4.1 and 6), in bytes.
 *
 * Caps how much audio may be outstanding to the pump, which caps the
 * card's occupancy, which is what a listener hears. Unused in step 1 -
 * there is no pump - but declared here so the parameter exists from the
 * start and the trace can print it.
 *
 * Default 0 means "two card fragments", resolved once the card's
 * geometry is known. A parameter rather than a constant because the
 * right value differs between 86Box's SB16 and the Acer's ESS, and
 * finding it means trying values on the target.
 */
int vsound_depth;

/*
 * How much per-fragment trace to keep - see VSOUND_RL in vsound_chan.h
 * for why this exists and why the default is 10 rather than 1.
 * Lifecycle events ignore it entirely.
 */
int vsound_ratelimit = 100;

/* One counter per site, so a busy site cannot silence a quiet one. */
static unsigned long vsound_rl_tick;
static unsigned long vsound_rl_stall;
static unsigned long vsound_rl_write;

/*
 * Consecutive ticks with a full hard buffer. Several rather than one,
 * because a pump that simply has not been scheduled yet is normal and
 * must not be mistaken for a pump that is not there.
 */
#define VSOUND_STALL_TICKS  10
static int vsound_stalled;
/* Capped: a stall that cannot be fixed by discarding must not bury the
 * line that explains it. */
static int vsound_stall_said;

/*
 * RESERVE A CHANNEL FOR vmidi - design/p3-midi-plan.md, and the
 * VSOUND_MIDI_SLOT comment in vsound_chan.h for the mechanism.
 *
 * OFF by default. When set, the last slot (VSOUND_MIDI_SLOT) exists
 * for a userspace MIDI synth, which claims it by opening /dev/dsp
 * with O_EXCL - see vsound_dev_open(). Ordinary clients are allocated
 * from slots 0..VSOUND_MAX_CHAN-1 exactly as when it is off, so the
 * user still gets VSOUND_MAX_CHAN of them and nothing is taken away.
 *
 * A MODULE PARAMETER rather than an ioctl because whether the slot
 * EXISTS is a property of the machine, decided once: the setup tool
 * writes this into the config alongside the channel and drive counts.
 * Whether it is HELD is the synth's business, open() to close().
 */
static int vsound_midi;

MODULE_PARM(vsound_midi, "i");
MODULE_PARM_DESC(vsound_midi,
                 "add a channel reserved for a MIDI synth (0/1)");
MODULE_PARM(vsound_trace, "i");
MODULE_PARM_DESC(vsound_trace, "trace to the kernel log (0/1)");
MODULE_PARM(vsound_depth, "i");
MODULE_PARM_DESC(vsound_depth, "max bytes outstanding to the pump; 0 = auto");
MODULE_PARM(vsound_ratelimit, "i");

/*
 * PER-CATEGORY RATES.
 *
 * `vsound_ratelimit' (-r) stays what it always was: the PER-FRAGMENT
 * rate, for tick/drain/ack. These add independent control of the other
 * sites, so a run that needs every channel line does not also need
 * 4 MB of per-fragment output - which is what happened on 2026-08-27
 * and made a 48000-line trace where a few hundred lines would have
 * answered the question.
 *
 * 0 disables a category entirely. 1 is every event.
 */
int vsound_rate_mix  = 1;       /* chan[N] lines, one per slot per mix */
int vsound_rate_life = 1;       /* opens, releases, triggers           */
/*
 * THE WRITE LOOP, AND WHY IT DEFAULTS TO OFF.
 *
 * This site fires once per pass of vsound_dev_write()'s loop - which
 * is once per client write AND once per spurious wake while a writer
 * is blocked. The mixer wakes every BUSY channel on every mix, ~100
 * times a second (design/13-starvation-diagnosis.md M-7), so a single
 * blocked writer generates ~100 lines a second on its own.
 *
 * 0 by default for the reason vsound_rate_mix exists at all: on
 * 2026-08-27 a run that needed one category's lines also produced 4 MB
 * of another's. Turn it on deliberately - `-w 1' for every pass, which
 * is what diagnosing a single stuck writer wants.
 */
int vsound_rate_write = 0;      /* write-loop passes; 0 = off          */

/*
 * HOW LONG A BLOCKED write() MAY SLEEP, IN MILLISECONDS.
 *
 * **THIS IS THE FIX FOR THE 2026-08-31 SERIALISATION BUG, and it is
 * the whole of it.**
 *
 * THE BUG: 2.2's sys_write takes a per-INODE semaphore and holds it
 * for the entire duration of the driver's write:
 *
 *     down(&inode->i_sem);                  fs/read_write.c
 *     ret = write(file, buf, count, &file->f_pos);
 *     up(&inode->i_sem);
 *
 * /dev/dsp is ONE inode, so every client contends for the SAME
 * semaphore, and __down sleeps in TASK_UNINTERRUPTIBLE
 * (kernel/sched.c:1020). While one client sleeps inside this
 * function waiting for buffer space, every other writer is stuck in
 * `D' state at down(), never reaching us at all.
 *
 * Measured on 86Box AND the real Acer: one client plays, the rest sit
 * at flags 0x1 with `mixed 0 bytes, 0 empty pulls' - the mixer never
 * even sees them - and then die 100 jiffies apart as this deadline
 * fires in turn. `ps axl' showed all of them in D at one shared
 * wchan. See tests/logs/2026-08-31-vsound-86box-nofork/RUN.txt.
 *
 * QUAKE WAS NEVER AFFECTED because it MMAPs: an mmap client issues no
 * write() at all, so it never queues on i_sem. That is why
 * `quake + CD' worked and `lxdoom + CD' did not, for months, on both
 * machines and on the old vcdsnd tree too.
 *
 * THE FIX: hold the semaphore for as short a time as possible. A
 * write that cannot be satisfied returns a SHORT COUNT rather than
 * sleeping for up to a second. write(2) permits this and audio code
 * generally loops.
 *
 * WHY NOT ZERO BY DEFAULT: lxdoom does NOT loop. l_soundgen.c:473 is
 *
 *     write(audio_fd, mixbuffer, MIXBUFFERSIZE);
 *
 * with the result discarded, so a short write silently drops the rest
 * of that buffer - gappy audio rather than none. A few milliseconds
 * of grace lets the mixer free a fragment (it runs ~100 times a
 * second) so the common case still completes in full, while the
 * semaphore is never held long enough to stall another client for
 * any audible time.
 *
 * 20 ms is two mixer ticks: long enough for room to appear in normal
 * operation, short enough that three blocked clients cost 60 ms
 * rather than 3 seconds.
 *
 *   0    never sleep - pure short-write behaviour, for measuring
 *   20   the default
 *   1000 the old behaviour, for reproducing the bug
 */
/*
 * THE HARD BOUND ON A WHOLE write() CALL.
 *
 * vsound_write_ms bounds each SLEEP, so i_sem is released often. This
 * bounds the CALL, so a writer with no space can never wait forever -
 * which is what happened when the sleep deadline was re-armed with
 * nothing taken, and it hung the guest on 2026-08-31.
 *
 * One second, which is what the original flat deadline was. The
 * difference is that it now only fires when NOTHING has been
 * transferred in that whole second, which means the device really is
 * wedged rather than merely contended.
 */
#define VSOUND_WRITE_HARD_MS 1000

/*
 * THE DRAIN-ON-CLOSE BOUNDS.  See vsound_dev_release().
 *
 * VSOUND_DRAIN_TICK is how long one sleep waits before re-checking.
 * The mixer runs at ~100 Hz, so 2 jiffies is two mixer passes at
 * HZ=100 - short enough to notice progress promptly, long enough not
 * to spin.
 *
 * VSOUND_DRAIN_STALL is how many CONSECUTIVE no-progress sleeps to
 * tolerate before giving up.  25 x 2 jiffies is half a second of
 * nothing moving, against a flush that should take one hard buffer's
 * worth - 16384 bytes at 176400 B/s, about 93 ms.  So a healthy drain
 * finishes well inside the budget and a wedged one costs half a
 * second of close().
 *
 * **THE WORST CASE IS WHAT THESE ARE FOR.**  With no pump attached
 * nothing ever consumes, every sleep is a stall, and close() returns
 * after 500 ms having flushed nothing - which is the right answer.
 * Unbounded, it would hang the process in close() holding /dev/dsp
 * and the module's use count, which is worse than the hanging note
 * this is fixing.
 */
#define VSOUND_DRAIN_TICK    2
#define VSOUND_DRAIN_STALL   25

/*
 * 200 SINCE 2026-09-30 - WAS 20, AND 20 MS WAS A DATA-LOSS RATE.
 *
 * This is how long a blocking write that has made SOME progress waits
 * for more room before returning short (the "write short" path in
 * vsound_dev_write). A card driver never returns short to a blocking
 * writer, and OSS clients rely on that: Doom's sound server writes and
 * ignores the return; so do most period programs. Every short return
 * to such a client DROPS the tail of its buffer.
 *
 * On the Acer, whose card frees room in a continuous trickle, 20 ms
 * nearly always sufficed and short writes ran at 2% - called "normal"
 * in CLAUDE.md, and in fact a 2% rate of dropped tails. On 86Box's
 * Solo-1, which frees room in one lump per 20 ms host period, the
 * deadline and the period coincided and 491 of 628 writes in one run
 * returned exactly 1480 of 2048 bytes: 763 phase splices in 10 s of a
 * pure tone, heard as crackle and chased as a vsound fault until a
 * spectrum scan of an OBS capture and the kernel trace agreed
 * (tests/logs/2026-09-30-86box-solo1-idleack-v3, run 181919). 200 ms
 * rides out ten such periods and returns short only when the pump is
 * genuinely stuck; the hard 1000 ms EIO is unchanged. Trial v4 on
 * 86Box: the four-tone chord at 82-87 dB over the between-tone floor,
 * against 10-30 before.
 */
int vsound_write_ms = 200;
MODULE_PARM(vsound_write_ms, "i");
MODULE_PARM_DESC(vsound_write_ms,
                 "max ms a blocked write may sleep (0 = never, was 1000)");
/*
 * THE LIMITER AND THE ATTENUATION - 2026-10-02. Defined in vsound_mix.c,
 * where the host test reaches them; vsound_mix.c has the account.
 * Passed by `vlhe apply' from [Sound Settings] Limiter and Attenuation.
 */
extern int vsound_limit;
extern int vsound_atten;
extern unsigned long vsound_mix_limit_passes;
extern long vsound_mix_limit_low(void);
MODULE_PARM(vsound_limit, "i");
MODULE_PARM_DESC(vsound_limit,
                 "the mix's limiter: 0 off, 1 attack/release (default),"
                 " 2 soft knee");
MODULE_PARM(vsound_atten, "i");
MODULE_PARM_DESC(vsound_atten,
                 "fixed attenuation of the mix, per cent (100 = none)");
MODULE_PARM(vsound_rate_mix, "i");
MODULE_PARM_DESC(vsound_rate_mix,
                 "trace every Nth mix's channel lines (0 = off)");
MODULE_PARM(vsound_rate_life, "i");
MODULE_PARM_DESC(vsound_rate_life,
                 "trace every Nth lifecycle event (0 = off)");
MODULE_PARM(vsound_rate_write, "i");
MODULE_PARM_DESC(vsound_rate_write,
                 "trace every Nth write-loop pass (0 = off, 1 = all)");
MODULE_PARM_DESC(vsound_ratelimit,
    "trace every Nth per-fragment line (1 = all, 0 = none, default 100). "
    "Lifecycle events are always traced.");

/* --- the device ----------------------------------------------------- */

static struct vsound_dev vsound_dev;
static int vsound_dsp_minor = -1;

/* From the other files. */
extern void vsound_dev_init(struct vsound_dev *d);
extern struct vsound_chan *vsound_chan_alloc(struct vsound_dev *d, int pid);
extern struct vsound_chan *vsound_chan_alloc_midi(struct vsound_dev *d,
                                                  int pid);

/*
 * THE TWO HEADERS MUST AGREE ON THE SLOT COUNT. vsound.h is the
 * userspace ABI and cannot include vsound_chan.h, so it carries its
 * own number; this fails the build if either moves alone. A negative
 * array size is the C89 way to say static_assert.
 */
typedef char vsound_list_chan_matches_slots
        [(VSOUND_LIST_CHAN == VSOUND_SLOTS) ? 1 : -1];
extern void vsound_chan_free(struct vsound_dev *d, struct vsound_chan *c);
extern void vsound_chan_start(struct vsound_dev *d, struct vsound_chan *c);
extern void vsound_chan_stop(struct vsound_dev *d, struct vsound_chan *c);
extern int  vsound_pump_claim(struct vsound_dev *d, void *who);
extern int  vsound_release_arm(struct vsound_dev *d, void *who, int on);
extern int  vsound_release_consume(struct vsound_dev *d);
extern int  vsound_pump_gone(struct vsound_dev *d, void *who);
extern int  vsound_chan_bps(const struct vsound_chan *c);
extern int  vsound_chan_setvol(struct vsound_dev *d, int index, int pid,
                               int vol);
extern int  vsound_chan_setmute(struct vsound_dev *d, int index, int pid,
                                int muted);
extern int  vsound_chan_setgeom(struct vsound_chan *c, unsigned int blkcnt,
                                unsigned int blksz,
                                unsigned char *(*alloc)(unsigned int),
                                void (*release)(unsigned char *, unsigned int));
extern unsigned char *vsound_dsp_alloc(unsigned int size);
extern void vsound_dsp_free(unsigned char *mem, unsigned int size);
extern int  vsound_dsp_mmap(struct file *file, struct vm_area_struct *vma);
extern void vsound_conv_setup(struct vsound_chan *c, int mix_rate);
extern int  vsound_drain_init(struct vsound_dev *d, unsigned char *buf,
                              unsigned int size);
extern int  vsound_drain_room(struct vsound_dev *d);
extern int  vsound_drain_put(struct vsound_dev *d, const unsigned char *src,
                             int count);
extern int  vsound_drain_get(struct vsound_dev *d, unsigned char *to,
                             int count);
extern int  vsound_drain_ack(struct vsound_dev *d, const struct vsound_ack *a);
extern void vsound_drain_stat(struct vsound_dev *d, struct vsound_stat *s);
extern int  vsound_mix_run(struct vsound_dev *d, unsigned char *dst,
                           unsigned char *tmp, unsigned char *raw,
                           int rawsz, int count, unsigned char silence,
                           int zoh);

/*
 * THE ONE PLACE A FORMAT CHANGE IS APPLIED.
 *
 * Every ioctl that moves rate, channels or format calls this and
 * nothing else re-derives the conversion. That is the lesson of
 * 06-softoss-design.md section 4e, paid for twice on the old tree:
 * there, four ioctls duplicated the body of the function that called
 * conv_setup, so all four bypassed it, z_gy stayed 0, vsound_conv_run()
 * refused every call, and the mixer produced silence with nothing in
 * the trace to say why.
 *
 * The rewrite managed to be worse for a while - conv_setup existed and
 * was called from NOWHERE at all (review finding 1). A guard that four
 * of five callers skip is not a guard; a guard nobody calls is not
 * even that.
 *
 * There is nowhere else to go: SPEED, STEREO, CHANNELS and SETFMT all
 * end here, and so does open.
 */
static void
vsound_dev_format_changed(struct vsound_chan *c)
{
    vsound_conv_setup(c, vsound_dev.mix_rate);

    if (vsound_trace)
        printk(KERN_DEBUG VSOUND_TS "vsound: format now %d Hz %d ch fmt 0x%x"
                          " (ratio %d:%d)\n",
               jiffies,
               c->rate, c->channels, c->format, c->z_gx, c->z_gy);
}

/*
 * The default client geometry, before a client says otherwise.
 *
 * 4 x 1024 is what quake negotiates, and 07's section 6 costs it at
 * 23 ms at quake's byte rate - inside the 50 ms bar on its own. The old
 * tree's fixed 64 KB was 372 ms, sixteen times more, and that is a
 * sixteenth of the latency budget spent before anything else.
 */
#define VSOUND_DEF_BLKCNT   4
#define VSOUND_DEF_BLKSZ    1024

/*
 * The mixer's scratch space.
 *
 * File-scope rather than on the stack because the timer runs with a
 * small kernel stack, and not kmalloc'd because a timer must not sleep.
 * Sized for one drain at the mixing format.
 */
#define VSOUND_SCRATCH      8192

/*
 * THE HARD BUFFER'S MEMORY. Small on purpose - section 6: this is a
 * SECOND buffer feeding the card's, so the two add, and every byte here
 * is latency on top of whatever the card already holds. 16 KB is 93 ms
 * at CD rate, which is headroom for the pump between wakeups and not a
 * place to store audio.
 */
#define VSOUND_HARD_SIZE    16384
static unsigned char vsound_hard_mem[VSOUND_HARD_SIZE];

static unsigned char vsound_mix_dst[VSOUND_SCRATCH];
static unsigned char vsound_mix_tmp[VSOUND_SCRATCH];
static unsigned char vsound_mix_raw[VSOUND_SCRATCH];

/* --- the clock ------------------------------------------------------ */

static struct timer_list vsound_timer;
static int vsound_timer_armed;

/*
 * Set while the mix is running with interrupts ON (finding 4).
 *
 * The scratch buffers are file-scope and shared, so two overlapping
 * mixes would corrupt each other's output. Nothing can preempt a timer
 * on 2.2 uniprocessor EXCEPT another timer, and vsound_dev_arm() is
 * reachable from write() and SETTRIGGER - both of which can run while
 * this one is outside its critical section. This is the flag that
 * stops a second tick starting inside the first.
 */
static int vsound_mixing;

static unsigned long vsound_last;       /* jiffies, at the last tick */

/* The pump sleeps here when the hard buffer is empty. */
static struct wait_queue *vsound_drain_wait;

/*
 * How many bytes of MIXED output one jiffy represents.
 *
 * Step 1 has no card, so the clock is the only thing that says how fast
 * audio is consumed. That is a PREDICTION, which 07's section 4 is
 * emphatic about not doing - and it is correct here for exactly as long
 * as there is nothing measuring: with the output discarded there is no
 * consumer to disagree with. Step 3 replaces this with the card's own
 * position and demotes the timer to the stall fallback.
 */
static int
vsound_dev_bps(const struct vsound_dev *d)
{
    int bytes = (d->mix_format == AFMT_S16_LE
                 || d->mix_format == AFMT_U16_LE) ? 2 : 1;

    bytes *= (d->mix_channels > 1) ? 2 : 1;
    return bytes * d->mix_rate;
}

static void vsound_dev_tick(unsigned long data);

static void
vsound_dev_arm(void)
{
    if (vsound_timer_armed)
        return;
    vsound_timer.expires  = jiffies + 1;
    vsound_timer.function = vsound_dev_tick;
    vsound_timer.data     = 0;
    add_timer(&vsound_timer);
    vsound_timer_armed = 1;
}

/*
 * ONE PASS OF THE MIXER.
 *
 * **REVERTED 2026-08-27: the pump no longer calls this.** The split
 * below is kept because it is harmless and because the wake at the end
 * now runs on every mix rather than only on a tick that reached the
 * bottom - but `vsound_dev_read()' calling it directly BROKE AUDIO
 * COMPLETELY. A single lxdoom, nothing else running, no leaked
 * channels: the client opened, configured, and never got a byte
 * through. `mixed 0 bytes' at release.
 *
 * WHY IT WAS TRIED, and it is still the right idea: the reference is
 * driven by the card's interrupt (`chn_intr', channel.c:660), which
 * cannot be skipped, while our timer can. The pump's blocking write()
 * to /dev/dsp1 returns because the card's ISR woke it, so the pump's
 * next read() IS that interrupt one process hop away. See
 * design/11-reference-recheck-2.md.
 *
 * WHY IT FAILED IS NOT YET KNOWN. The obvious candidate is ordering: a
 * client's first write starts its channel and arms the TIMER, but
 * nothing wakes a pump already asleep in schedule_timeout(), so the
 * first mix waits on the timer anyway - and with the pump now expected
 * to drive, whatever it was that used to make progress no longer does.
 * That is a hypothesis, not a diagnosis; it needs the sequence traced
 * rather than reasoned about.
 *
 * DO NOT RE-APPLY without instrumenting the open -> first-write ->
 * first-mix sequence first.
 *
 * CORRECTED 2026-10-04 (design/25 I4): THE READ DOES NOT CALL THIS.
 * The revert recorded above took that call out, and vsound_dev_tick()
 * is now the ONLY caller - the mixer is timer-driven, with the pump's
 * read() collecting what the tick produced. The list below is the
 * design as first written, kept as the record of what was tried:
 *
 * CALLED FROM ONE PLACE now:
 *
 *   1. `vsound_dev_read()' - THE PUMP asking for audio. This is the
 *      normal path and it is INTERRUPT-DRIVEN: the pump only comes
 *      back for more when its blocking write() to the card returned,
 *      and that write returns because the card's DMA-completion
 *      interrupt woke it. The card is the clock.
 *
 *   2. `vsound_dev_tick()' - the timer, as a BACKSTOP for when no pump
 *      is attached. Without it a client writing to /dev/dsp with no
 *      daemon running would block forever.
 *
 * WHY THIS SPLIT EXISTS, added 2026-08-27. Everything used to hang off
 * the timer, and a timer can be skipped (a mix already running), can
 * exit early, and can be delayed under load. The wake for blocked
 * writers sat at the END of the tick, after those early returns - so a
 * skipped tick woke nobody.
 *
 * The reference has no equivalent problem because it is driven by
 * `chn_intr()`, called by the driver from the card's ISR
 * (channel.c:660). It cannot be skipped.
 *
 * **We can have the same thing.** vsoundd sits in write() on
 * /dev/dsp1, which sleeps on esssolo1's `dma_dac.wait'; the card's ISR
 * wakes exactly that queue (esssolo1.c:1656 -> solo1_update_ptr ->
 * wake_up). Every driver in this kernel does the equivalent - the
 * legacy ones via DMAbuf_outputintr, the other PCI ones via their own
 * ISR. So the pump's read() IS chn_intr, one process hop away, and it
 * needs no per-card code. See design/11-reference-recheck-2.md.
 *
 * `design/07-vsound.md`'s "The shape adopted" already specified this:
 * "Pull, not push ... The kernel never decides how much to send from a
 * clock." This makes that true.
 *
 * Returns the bytes mixed into the hard buffer.
 */
static int
vsound_dev_mix_once(void)
{
    unsigned long flags, now, elapsed;
    int want, mixed;

    save_flags(flags);
    cli();

    vsound_timer_armed = 0;

    /*
     * A mix is already in progress with interrupts on. Re-arm and
     * leave rather than running a second one over the same scratch.
     */
    /*
     * A mix is already in progress with interrupts on. Re-arm and
     * leave rather than running a second one over the same scratch.
     *
     * NOW ALSO GUARDS THE PUMP against the timer and vice versa: since
     * 2026-08-27 vsound_dev_read() calls this directly, so two callers
     * exist. The pump gets 0 back and falls through to its sleep,
     * which is correct - the mix it collided with will wake it.
     */
    if (vsound_mixing) {
        vsound_dev_arm();
        restore_flags(flags);
        return 0;
    }

    if (vsound_dev.nrunning == 0) {
        restore_flags(flags);
        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: tick stopping (nothing running)\n", jiffies);
        return 0;
    }

    now     = jiffies;
    elapsed = now - vsound_last;
    vsound_last = now;

    /*
     * HOW MUCH TO PRODUCE: THE CLOCK IS A FLOOR, THE DEFICIT IS THE
     * TARGET.
     *
     * The reference does not use a clock at all. chn_wrfeed asks how
     * far the hard buffer is from FULL and fills the gap
     * (channel.c:404-406):
     *
     *     want = min(sndbuf_getsize(b),
     *         imax(0, sndbuf_xbytes(sndbuf_getsize(bs), bs, b)
     *                 - sndbuf_getready(b)));
     *
     * Ours used only `elapsed * bps / HZ' - a RATE. It produces
     * exactly real-time and can never get ahead, so the card runs with
     * whatever it started with and never recovers a cushion it loses.
     * On the Acer that meant less than one tick in hand and continuous
     * starvation: 15710 of 15972 acks reported odelay 0.
     *
     * WHY THE CLOCK STAYS. It is load-bearing for the no-reader stall
     * path below, which discards `want' bytes per tick at the clock's
     * rate to keep clients from blocking forever when no pump is
     * attached. Removing it would need that path rewritten too. So:
     * take the LARGER of the clock's demand and the deficit, and let
     * the existing min(want, room) clamp bound the result. The clock
     * guarantees progress; the deficit builds the cushion.
     *
     * See design/10-reference-recheck.md, divergence 1.
     */
    want = (int) ((elapsed * (unsigned long) vsound_dev_bps(&vsound_dev))
                  / (unsigned long) HZ);

    {
        /* THE DEFICIT: how far the hard buffer is from the depth we
         * are willing to have in flight. `outstanding' is what the
         * pump holds and has not acked; `ready' is what is waiting
         * here for it. Together they are what the card will get
         * without any further mixing. */
        int have = vsound_hard_outstanding(&vsound_dev.hard)
                   + vsound_buf_ready(&vsound_dev.hard.b);
        int target = vsound_depth;
        int deficit;

        /* NOT CONFIGURED MEANS THE BUFFER'S OWN SIZE, matching
         * vsound_drain_room()'s reading of vsound_depth <= 0. The
         * reference's target is the soft buffer's size expressed in
         * hard-buffer bytes; ours is the hard buffer itself, which is
         * the only size we have. */
        if (target <= 0)
            target = vsound_dev.hard.b.bufsize;

        deficit = target - have;
        if (deficit > want)
            want = deficit;
    }

    if (want > VSOUND_SCRATCH)
        want = VSOUND_SCRATCH;
    if (want < 0)
        want = 0;

    /*
     * BOUNDED BY THE HARD BUFFER, not only by the clock. This is the
     * change that makes the drain correct rather than plausible:
     * chn_wrfeed takes min(wasfree, want) (channel.c:403-407), where
     * wasfree exists only because the card genuinely consumed
     * something. Ours is vsound_drain_room(), which subtracts what is
     * outstanding to the pump and applies the depth cap.
     *
     * Without this the mixer would produce at the clock's rate
     * regardless of whether anything was draining, and the ring would
     * saturate exactly as the old tree's did.
     */
    {
        int room = vsound_drain_room(&vsound_dev);

        /*
         * NOBODY DRAINING? DISCARD, DO NOT STALL.
         *
         * With no pump the hard buffer fills once and stays full, room
         * goes to 0, the mixer produces nothing, and every client
         * blocks forever waiting for space that will never come. That
         * happened on the target: one tick, "hard ready 1760", then
         * silence for the rest of the run.
         *
         * An application must never hang because the daemon is absent.
         * The old tree kept a null-driver fallback for exactly this and
         * so does this: if the buffer has been full for a while with no
         * reader, throw the oldest audio away at the clock's rate,
         * which is what an unattended card would effectively do.
         *
         * `stalled' counts consecutive ticks with no room. One tick of
         * fullness is normal - the pump may simply not have run yet -
         * so this waits for several before deciding nothing is there.
         */
        if (room <= 0) {
            if (++vsound_stalled > VSOUND_STALL_TICKS) {
                int drop = want;
                int had  = vsound_buf_ready(&vsound_dev.hard.b);

                if (drop > had)
                    drop = had;

                /*
                 * NOTHING TO DISCARD IS NOT A STALL WE CAN FIX.
                 *
                 * The buffer being empty while room is 0 means the
                 * space is OUTSTANDING - handed to the pump and never
                 * acked - not sitting here unread. Discarding cannot
                 * help, and saying so 474 times (as it did on
                 * 2026-08-25) buries the one line that would have
                 * explained it.
                 *
                 * Report the real state instead, once, capped.
                 */
                if (drop > 0) {
                    vsound_buf_dispose(&vsound_dev.hard.b,
                                       (unsigned char *) 0, drop);
                    if (VSOUND_RL(vsound_rl_stall))
                        printk(KERN_DEBUG VSOUND_TS "vsound: no reader -"
                                          " discarding %d bytes\n",
               jiffies, drop);
                } else if (vsound_stall_said < 3) {
                    vsound_stall_said++;
                    printk(KERN_DEBUG VSOUND_TS "vsound: STALLED - %d bytes"
                                      " outstanding to the pump and"
                                      " unacked; nothing to discard\n",
               jiffies,
                           vsound_hard_outstanding(&vsound_dev.hard));
                }
                room = vsound_drain_room(&vsound_dev);
            }
        } else {
            vsound_stalled    = 0;
            vsound_stall_said = 0;
        }

        /*
         * THE XRUN COUNT IS TAKEN AFTER THE MIX, NOT HERE - design/25
         * I1, fixed 2026-09-16.
         *
         * This site used to do `if (room < want) xruns++', which reads
         * like channel.c:414 and is not it. `want' has by now taken
         * the DEFICIT whenever that exceeds the clock's demand (the
         * block above), and `room' is essentially that same deficit
         * seen from the other side - so `room < want' fired exactly
         * when the deficit was under one tick's worth, which is the
         * pipeline NEARLY FULL. It counted health as starvation.
         *
         * MEASURED, `2026-09-13-acer-65`: 2175 -> 4304 across 4025
         * jiffies of clean two-channel music, about 53 a second. A
         * counter that reads 53/s in a good run cannot tell anyone
         * anything about a bad one, and `design/13` M-4 did read it
         * as "two thirds of ticks short".
         *
         * The reference feeds FIRST and then asks whether the buffer
         * came up short (`channel.c:403-415`: `amt = min(wasfree,
         * want); sndbuf_feed(...); if (sndbuf_getready(b) < want)
         * c->xruns++`). Ours now does the same, after the mix.
         */
        if (want > room)
            want = room;
    }

    /*
     * THE MIX RUNS WITH INTERRUPTS ON. Review finding 4.
     *
     * Everything above decides HOW MUCH - the clock, the room, the
     * stall check - and that is pointer and counter work, which is
     * what a critical section is for. The mix itself is a loop over
     * every running channel, each doing a rate conversion: at 8 KB of
     * scratch and 4:1 upsampling that is thousands of interpolations,
     * and holding cli() across it on a 450 MHz Pentium II is long
     * enough to matter for timer and serial latency.
     *
     * vsound_chan.h's own header already said so - "list and pointer
     * updates may be guarded, the mix loop must not be" - and the code
     * did not follow it.
     *
     * The reference locks PER CHILD, inside the loop
     * (feeder_mixer.c:316-381), so no single lock ever spans the whole
     * mix. We cannot do exactly that on 2.2 with cli(), which is global
     * rather than per-object, so the equivalent is to leave the section
     * entirely for the duration of the mix.
     *
     * WHAT MAKES THAT SAFE: `want' is already computed and the hard
     * buffer's space is already accounted for by outstanding, so
     * nothing decided above can be invalidated by a concurrent ack -
     * an ack only RETIRES space, which makes room rather than taking
     * it. A channel closing mid-mix sets CLOSING first, which
     * vsound_mix_pull already tests, and its buffer is freed in
     * release() outside cli() only after vsound_chan_stop() has taken
     * it out of the running set.
     */
    vsound_mixing = 1;
    restore_flags(flags);

    mixed = 0;
    if (want > 0) {
        mixed = vsound_mix_run(&vsound_dev, vsound_mix_dst, vsound_mix_tmp,
                               vsound_mix_raw, VSOUND_SCRATCH, want, 0, 0);
    }

    /*
     * THE LIMITER, ONCE A SECOND, WHEN IT ACTED - 2026-10-02, for the
     * headroom tests (design/36 row 123). Only the total at unload
     * existed, which says nothing about WHEN. One line per second it
     * worked, none when it did not, so a quiet run adds nothing.
     */
    if (vsound_trace) {
        static unsigned long lim_t, lim_p, lim_c;

        if ((long) (jiffies - lim_t) >= HZ) {
            unsigned long dp = vsound_mix_limit_passes - lim_p;
            unsigned long dc = vsound_mix_clipped - lim_c;
            long low = vsound_mix_limit_low();

            if (dp > 0 || dc > 0)
                printk(KERN_DEBUG VSOUND_TS "vsound: last second: limiter"
                       " (%s) acted in %lu mix passes, lowest gain %ld%%,"
                       " %lu samples clipped\n", jiffies,
                       vsound_limit == 0 ? "off" : vsound_limit == 2
                           ? "soft knee" : "attack/release",
                       dp, vsound_limit == 1 ? low : 100L, dc);
            lim_t = jiffies;
            lim_p = vsound_mix_limit_passes;
            lim_c = vsound_mix_clipped;
        }
    }

    /* Back under cli() for the ring update and the re-arm. */
    save_flags(flags);
    cli();
    vsound_mixing = 0;

    /*
     * THE XRUN COUNT - design/25 I1. The mixer was asked for `want'
     * and produced `mixed'; a short production means the channels
     * could not fill the room, which IS starvation and is what the
     * reference counts (`channel.c:414`, after its feed).
     *
     * `want > 0' guards it because a tick with nothing to do is not
     * an xrun: `want' is zero when the pipeline is already at depth,
     * which is the healthy state the old test used to count.
     */
    if (want > 0 && mixed < want)
        vsound_dev.hard.xruns++;

    if (mixed > 0) {
        /* Into the hard buffer, where the pump reads it. */
        vsound_drain_put(&vsound_dev, vsound_mix_dst, mixed);

        /*
         * THINNED BY EVENT COUNT, not by wall clock. This used
         * `now % (HZ * 2)', which thins by elapsed time: at 100 Hz it
         * printed two adjacent lines every two seconds and gave no
         * control at all. A counter prints a defined FRACTION, so the
         * trace stays proportional to what actually happened and the
         * rate is settable at load.
         */
        if (VSOUND_RL(vsound_rl_tick))
            printk(KERN_DEBUG VSOUND_TS "vsound: tick want %d mixed %d"
                              " (running %d, hard ready %d, xruns %lu)\n",
               jiffies,
                   want, mixed, vsound_dev.nrunning,
                   vsound_buf_ready(&vsound_dev.hard.b),
                   vsound_dev.hard.xruns);
    }

    vsound_dev_arm();
    restore_flags(flags);

    /* The pump first: it is what makes room for everything else. */
    wake_up_interruptible(&vsound_drain_wait);

    /*
     * EVERY BUSY CHANNEL, not chan[0]. A writer on any other channel
     * would never be woken and would sleep until something else
     * happened to wake it - correct only while exactly one client
     * exists, which is the assumption this rewrite is here to remove.
     *
     * Same family as the three -ENODEV bugs: code that is right with
     * one client and wrong with two. Found by auditing every path that
     * touches a channel rather than by hitting it.
     *
     * NOW REACHED ON EVERY MIX, not only on a tick that ran to
     * completion - because the pump calls this directly. That is the
     * point of the split.
     */
    /*
     * WAKE ONLY WHAT THE MIX ACTUALLY TOOK FROM - review finding A-8,
     * design/12-reference-audit.md. The reference's condition is
     * channel.c:417:
     *
     *     if (sndbuf_getfree(b) < wasfree)
     *             chn_wakeup(c);
     *
     * This woke every BUSY channel on every mix regardless, ~100 times
     * a second each. A writer with no space woke, re-tested, found
     * none and slept again - so it could not HANG, but it cost two
     * things:
     *
     *   - it made the write timeout unfirable, because
     *     schedule_timeout() returns 0 only on a FULL expiry and a
     *     wake returns the remainder. Fixed separately by making the
     *     deadline absolute, and that stays regardless: the reference
     *     has a deadline too (CHN_TIMEOUT) and it guards against
     *     causes other than spurious wakes.
     *   - it is noise in the traces used to chase bugs. A channel that
     *     wakes constantly looks busy whether or not it is being
     *     served, and the write-loop trace ran to 29000 lines largely
     *     because of it.
     *
     * KEPT UNCONDITIONAL UNTIL NOW ON PURPOSE, at the user's
     * direction on 2026-08-27: the concern was that the wake might be
     * CAUSING the two-client symptom, and changing a suspect
     * mid-investigation loses the evidence. The symptom turned out to
     * be 2.2's per-inode write semaphore (design/14) and had nothing
     * to do with waking, so the deferral has come due.
     *
     * `freed_this_pass' is set by vsound_mix_run() from what
     * vsound_mix_pull() returned, and cleared here once acted on.
     */
    {
        int i;

        for (i = 0; i < VSOUND_SLOTS; i++) {
            struct vsound_chan *wc = &vsound_dev.chan[i];

            if (!(wc->flags & VSOUND_CHN_BUSY))
                continue;
            if (wc->freed_this_pass <= 0)
                continue;
            wc->freed_this_pass = 0;
            wake_up_interruptible(&wc->wait);
        }
    }

    return mixed;
}

/*
 * THE TIMER, WHICH IS NOW ONLY A BACKSTOP.
 *
 * It exists for the case where NO PUMP IS ATTACHED: a client writing
 * to /dev/dsp with no vsoundd running would otherwise fill its channel
 * and block forever, because nothing would ever consume. The stall
 * path inside vsound_dev_mix_once() handles that by discarding at the
 * clock's rate.
 *
 * With a pump attached this still fires, and that is harmless: the
 * mixer produces what the deficit and the room allow, and a mix that
 * has nothing to do costs one guarded early return.
 */
static void
vsound_dev_tick(unsigned long data)
{
    (void) data;
    (void) vsound_dev_mix_once();
}

/* --- the OSS surface ------------------------------------------------ */

static int
vsound_dev_open(struct inode *inode, struct file *file)
{
    struct vsound_chan *c;
    unsigned long flags;
    int remembered = 0;     /* a level from the per-program table */

    (void) inode;

    /*
     * A READER IS THE PUMP, NOT A CLIENT.
     *
     * O_RDONLY means "drain the mixed stream" - vsoundd, or a plain
     * `cat /dev/dsp > file'. It gets NO channel: it is not producing
     * audio, it is taking the result away, and giving it one would
     * consume a slot and appear in the mix as a silent participant.
     *
     * This check used to refuse every read outright with "playback
     * only", which was right when the device could only receive audio
     * and became wrong the moment read() was added in the same commit
     * as the drain. It cost a target run: vsoundd opens O_RDONLY, was
     * refused -EINVAL, and exited immediately with no audio at all.
     *
     * private_data stays NULL for a reader, and every path that uses a
     * channel already tests for that.
     */
    if ((file->f_mode & FMODE_WRITE) == 0) {
        if ((file->f_mode & FMODE_READ) == 0)
            return -EINVAL;         /* neither: nothing to do */
        file->private_data = NULL;
        MOD_INC_USE_COUNT;
        /*
         * NOT LOGGED HERE ANY MORE - 2026-10-02. Every read-only open
         * printed "open for READ - the pump", but a read-only open is
         * also how every control tool asks a question (open, one
         * ioctl, close): the control centre's Volume page did it twice
         * per 250 ms poll, and a slider drag once per step. 1410 of a
         * run's 2212 trace lines were that one message
         * (tests/logs/2026-10-02-86box-guest-var-log), and none of
         * them was the pump. The real pump is named where it takes
         * the role, on its first read - "this reader is the pump",
         * vsound_dev_read().
         */
        return 0;
    }

    /*
     * O_EXCL MEANS "THE RESERVED MIDI SLOT" - the vsound_chan.h
     * comment on VSOUND_MIDI_SLOT has why that flag and why it is
     * safe. It is still in f_flags here because the VFS strips it
     * only after this function returns (fs/open.c:676).
     *
     * -ENODEV when the machine has no such slot, so the synth can
     * tell "not enabled" from "someone has it" (-EBUSY) and fall back
     * to an ordinary channel for the first and not the second. BOTH
     * ARE LOGGED UNCONDITIONALLY, not under vsound_trace: the synth
     * retries at most once per lifetime, and a run whose music took
     * an ordinary slot has to say so in the capture, because that is
     * a run in which a game could have been refused its audio.
     */
    if (file->f_flags & O_EXCL) {
        if (!vsound_midi) {
            printk(KERN_INFO VSOUND_TS "vsound: pid %d (%s) asked for the"
                             " MIDI slot but vsound_midi=0 - -ENODEV,"
                             " it will take an ordinary channel\n",
                   jiffies, current->pid, current->comm);
            return -ENODEV;
        }
        save_flags(flags);
        cli();
        c = vsound_chan_alloc_midi(&vsound_dev, current->pid);
        restore_flags(flags);
        if (c == NULL) {
            printk(KERN_INFO VSOUND_TS "vsound: pid %d (%s) asked for the"
                             " MIDI slot but pid %d holds it - -EBUSY\n",
                   jiffies, current->pid, current->comm,
                   vsound_dev.chan[VSOUND_MIDI_SLOT].pid);
            return -EBUSY;
        }
    } else {
        save_flags(flags);
        cli();
        c = vsound_chan_alloc(&vsound_dev, current->pid);
        restore_flags(flags);

        if (c == NULL) {
            if (vsound_trace)
                printk(KERN_DEBUG VSOUND_TS "vsound: open REFUSED -EBUSY"
                                  " (%d channels in use)\n",
                   jiffies, vsound_dev.nbusy);
            return -EBUSY;
        }
    }

    /*
     * The buffer, sized from the default geometry. Outside cli()
     * because vmalloc can sleep.
     *
     * FATAL if it fails, unlike the old tree where it was tolerated and
     * the symptom appeared later at the mapping. A channel with no
     * buffer cannot do anything useful, and saying so at open is the
     * difference between one clear error and a confusing one.
     */
    if (vsound_chan_setgeom(c, VSOUND_DEF_BLKCNT, VSOUND_DEF_BLKSZ,
                            vsound_dsp_alloc, vsound_dsp_free) < 0) {
        save_flags(flags);
        cli();
        vsound_chan_free(&vsound_dev, c);
        restore_flags(flags);
        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: open REFUSED -ENOMEM"
                              " (buffer allocation failed)\n", jiffies);
        return -ENOMEM;
    }

    /* The channel starts at the device defaults, so the conversion is
     * set up before any ioctl arrives - a client that never negotiates
     * still gets a working converter rather than z_gy == 0. */
    vsound_dev_format_changed(c);

    /*
     * WHO THIS IS, for a control tool's slider label. The reference
     * records the same at the same moment (sound.c:330-331). current
     * is the opening process here, which is exactly the one we want -
     * doing it later would risk a different context.
     */
    strncpy(c->comm, current->comm, sizeof c->comm - 1);
    c->comm[sizeof c->comm - 1] = '\0';

    /* AND THE LEVEL THIS PROGRAM HAD LAST - design/33 section 1c.
     * Here, before the first sample: no poll, no race, no GUI needed.
     * The channel is not running yet, but cli() as every table
     * access does. */
    save_flags(flags);
    cli();
    remembered = vsound_prog_apply(&vsound_dev, c);
    restore_flags(flags);

    file->private_data = (void *) c;
    MOD_INC_USE_COUNT;

    if (vsound_trace)
        printk(KERN_DEBUG VSOUND_TS "vsound: open pid %d chan %p slot %d%s"
                          " (%u x %u = %u bytes)\n",
               jiffies,
               current->pid, (void *) c, (int) (c - vsound_dev.chan),
               (c == &vsound_dev.chan[VSOUND_MIDI_SLOT]) ? " (MIDI)" : "",
               c->b.blkcnt, c->b.blksz, c->b.bufsize);
    /* AND THE LEVEL IT WAS GIVEN, when it was a remembered one - the
     * per-program table at work (design/33 1c), 2026-10-02. */
    if (vsound_trace && remembered)
        printk(KERN_DEBUG VSOUND_TS "vsound: %s opens at %d%%%s"
                          " (remembered)\n", jiffies, c->comm, c->vol,
               (c->flags & VSOUND_CHN_MUTED) ? ", muted" : "");
    return 0;
}

static int
vsound_dev_release(struct inode *inode, struct file *file)
{
    struct vsound_chan *c = (struct vsound_chan *) file->private_data;
    unsigned long flags;

    (void) inode;

    if (c == NULL) {
        /*
         * THE PUMP IS GONE: the role and its release-on-idle arming
         * go with it, and the next reader may claim it.
         *
         * The flags are module state, not process state (design/25
         * B3): a `-R' pump that died left both set, and the next pump
         * - started WITHOUT -R - read 0 on its first idle and spun,
         * with nothing it did able to clear them. Only the pump
         * clears them: any O_RDONLY opener is a reader (vsoundvol is
         * one), and one closing beside a live pump must not disarm it.
         */
        save_flags(flags);
        cli();
        if (vsound_pump_gone(&vsound_dev, (void *) file) && vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: the pump closed -"
                              " release-on-idle disarmed\n", jiffies);
        restore_flags(flags);
        MOD_DEC_USE_COUNT;
        return 0;
    }

    /*
     * CLOSING first, so anything still running sees it. The mixer's
     * mapped acquire is guarded on it for the reason FreeBSD guards its
     * own (feeder_mixer.c:330): during a drain the buffer must not
     * manufacture audio out of whatever the departing client left.
     */
    /*
     * NO FLUSH-ON-CLOSE HERE. Two attempts on 2026-08-27, both removed:
     *
     *   1. filling the HARD buffer with silence in vsound_chan_stop().
     *      Did nothing - the tick returns as soon as nrunning hits 0,
     *      so the silence was staged and never drained.
     *   2. appending silence to the CLIENT's ring here, which is what
     *      the reference's chn_sync does (channel.c:812-816,
     *      `sndbuf_clear(bs, ...)' where bs is bufsoft).
     *
     * **The second did not fix the hanging note EITHER, and UQM's exit
     * began hard-locking the machine** - looping audio, corrupted
     * display, nothing responding. F1-to-quit was reproducible. The
     * loop ran under cli() and its length was proportional to the
     * ring size; UQM has the largest ring of any client at 32 KB and
     * UQM is what locked.
     *
     * Reading the loop did not fault it - the arithmetic terminates.
     * But a reproducible hard lock on a code path changed the same day
     * is not something to reason about further while it is in the
     * tree.
     *
     * The hanging note is still open. `design/12-reference-audit.md'
     * A-9 has the reference's shape; whatever is tried next needs to
     * account for BOTH failures, not just the first.
     */
    /*
     * ===================================================================
     * DRAIN BEFORE STOPPING - the hanging note.  Review A-9,
     * design/12-reference-audit.md.
     * ===================================================================
     *
     * THE SYMPTOM: after the last client closes, the card's DMA ring
     * replays its final fragment.  Confirmed on the Acer on
     * 2026-08-31, after the i_sem fix had corrected everything else,
     * so it is independent of that bug.
     *
     * THE MECHANISM: the last client closes, nrunning hits 0, and the
     * tick returns immediately (`tick stopping (nothing running)').
     * Whatever the hard buffer held and whatever the pump had already
     * handed the card is never followed by anything, and the card has
     * no reason to stop.
     *
     * **TWO EARLIER ATTEMPTS FAILED, BOTH ON 2026-08-27, AND THE
     * REASON IS THE SAME.**  One filled the HARD buffer; the other
     * appended to the client's ring, which is the reference's own
     * shape.  Neither WAITED.  Both returned at once, the channel
     * stopped, the tick stopped, and the silence was never carried
     * out.  **Staging silence is not flushing it.**
     *
     * WHAT THE REFERENCE ACTUALLY DOES (chn_sync, channel.c:790-860)
     * is append AND THEN SLEEP until it has drained, topping up as
     * space frees:
     *
     *     threshold = min(minflush, sndbuf_getfree(bs));
     *     sndbuf_clear(bs, threshold);            <- silence into the
     *     sndbuf_acquire(bs, NULL, threshold);       CLIENT ring
     *     ...
     *     while (count > 0 && (resid > 0 || minflush > 0)) {
     *             ret = chn_sleep(c, c->timeout);   <- and WAITS
     *
     * and `minflush' includes a whole hard buffer's worth
     * (channel.c:800), explicitly "to avoid audible truncation".
     *
     * THE ORDER MATTERS AND IS THE WHOLE FIX: this runs BEFORE
     * vsound_chan_stop(), so the channel is still RUNNING and the tick
     * still mixes from it.  Stopping first is what made the earlier
     * attempts no-ops.
     *
     * **BOUNDED TWICE, BECAUSE close() BLOCKING IS WORSE THAN A
     * HANGING NOTE.**  If the drain stalls - no pump attached, a
     * wedged card - a process stuck in close() cannot be killed by
     * anything short of a signal, holds /dev/dsp, and keeps the
     * module's use count up.  So:
     *
     *   - each sleep is bounded (VSOUND_DRAIN_TICK jiffies), and
     *   - `stall' counts consecutive sleeps that made NO progress and
     *     gives up after VSOUND_DRAIN_STALL of them.
     *
     * That is the reference's own discipline: its `count' decrements
     * on a sleep where `resid == residp' and the loop ends at zero
     * (channel.c:846-849).  Progress resets ours rather than
     * incrementing a budget - simpler, same effect.
     *
     * AND IT NEVER FAILS THE CLOSE.  chn_sync returns 0 whatever
     * happened (channel.c:867); a close that could not flush is still
     * a close.  Nothing below this is conditional on it.
     */
    /*
     * NO DRAIN ON CLOSE, AND THAT IS A DECISION - see
     * design/16-hanging-note.md.
     *
     * The card's DMA ring replays its last fragment after the final
     * client closes, on esssolo1 and every other OSS PCI driver in
     * this kernel that shares its underrun pattern - es1370, es1371,
     * cmpci, sonicvibes. Not on `sb', which uses the legacy dmabuf.c
     * path and calls halt_output().
     *
     * **FOUR ATTEMPTS TO FIX IT FROM HERE FAILED, and the fourth
     * proved the approach cannot work.** `vsound/tests/drainsim.c'
     * pushed 640 KB of silence - 3.6 seconds, 43 times the card's
     * whole queue - from userspace on the Acer. It played silently.
     * **The note hung afterwards anyway.**
     *
     * Silence plays, and then the ring goes on looping whatever the
     * DMA pointer reaches. **The card is never told to STOP**, and
     * nothing in the OSS write path can express that - only "play
     * this instead".
     *
     * So a drain here cannot fix the note however it is written, and
     * the one that was here cost ~90 lines of interrupt-context
     * logic, a bounded wait on every last close, and one real bug (a
     * dropped restore_flags, 80e36a3). Removed 2026-09-01 at the
     * user's direction.
     *
     * **WHAT WOULD WORK IS DESIGN B**: vsoundd closing /dev/dsp1 when
     * the last client goes, so solo1_release runs drain_dac,
     * stop_dac and the DMA disable - the card stopping itself with
     * code that is already correct. That is a vsoundd change, not a
     * kernel one, and design/16 section 4 has the reclaim risk and
     * how to test it.
     *
     * **THE WORK IS NOT LOST.** Branch `drain-attempt' is the tree
     * with the drain complete and building, and the five commits that
     * built it are on this branch's history too:
     *
     *   950cf70  the drain, appending silence and waiting
     *   593f9e1  cover the PIPELINE, not just the channel
     *   0d1715f  only the LAST client drains
     *   d30d4a9  abandon if a client starts mid-drain
     *   80e36a3  restore interrupts - a bug the above introduced
     *
     * `git checkout drain-attempt' brings it back whole. Every one of
     * those corrections was right for the problem it addressed; the
     * approach they serve is what does not work.
     */

    save_flags(flags);
    cli();
    c->flags |= VSOUND_CHN_CLOSING;
    vsound_chan_stop(&vsound_dev, c);
    restore_flags(flags);

    /* Outside cli(): vfree can sleep. */
    vsound_chan_setgeom(c, 0, 0, vsound_dsp_alloc, vsound_dsp_free);

    save_flags(flags);
    cli();
    vsound_chan_free(&vsound_dev, c);
    restore_flags(flags);

    file->private_data = NULL;
    MOD_DEC_USE_COUNT;

    /*
     * RELEASE-ON-IDLE - design/16 section 4, Design B.
     *
     * THE LAST CLIENT HAS GONE. This is the moment the pump cannot
     * otherwise see: the tick stops at nrunning == 0, and
     * vsound_dev_read()'s loop only exits when it has data, so without
     * this it would poll forever and never return to userspace.
     *
     * Five attempts at the hanging note have died on that one fact -
     * see design/16 section 3a. The flag is the smallest thing that
     * carries it across: set here, the read woken to see it, cleared
     * when a client next feeds a channel.
     *
     * ONLY IF THE PUMP ASKED (release_armed, set by -R through
     * VSOUND_IOC_RELEASE). A pump that did not ask must never get a
     * zero-length read, which conventionally means EOF.
     *
     * nbusy is already final here - vsound_chan_free() has run.
     */
    if (vsound_dev.release_armed && vsound_dev.nbusy == 0) {
        vsound_dev.release_idle = 1;
        /*
         * AND WAKE THE PUMP, 2026-09-14. Its read sleeps on this
         * queue and tests the level after every sleep however the
         * sleep ended, so the signal now arrives at scheduler latency
         * rather than at the end of the timeout (10 ms since
         * b87d728, 50 ms when this was written). Nothing else
         * wakes the queue once nrunning is 0: vsound_dev_mix_once()
         * returns before its wake_up. The flag still carries the
         * signal if this wake is lost - a read between sleeps.
         */
        wake_up_interruptible(&vsound_drain_wait);
    }

    if (vsound_trace)
        /* WHAT THIS CHANNEL ACTUALLY PRODUCED, at the one moment it is
         * certainly final. `empty' counting up while `mixed' stays
         * near zero is a client that held a channel and never fed it -
         * which is invisible in the device-level trace because the
         * other channels fill `want' and the tick reports success. */
        printk(KERN_DEBUG VSOUND_TS "vsound: release chan %p (%d still open,"
                          " mixed %lu bytes, %lu empty pulls)\n",
               jiffies,
               (void *) c, vsound_dev.nbusy,
               c->mixed_bytes, c->empty_pulls);
    return 0;
}

/*
 * Accept audio from a write() client.
 *
 * copy_from_user OUTSIDE cli(), because it can sleep. That is the same
 * trade chn_write makes (channel.c:473-476), and the reference states
 * the assumption it rests on: free space can never DECREASE around the
 * unlock. Here that holds because only the timer consumes, and
 * consuming increases free space.
 */
static ssize_t
vsound_dev_write(struct file *file, const char *buf, size_t count,
                 loff_t *ppos)
{
    struct vsound_chan *c = (struct vsound_chan *) file->private_data;
    struct wait_queue wait = { current, NULL };
    unsigned long flags;
    unsigned long deadline = 0;      /* per-SLEEP bound; 0 = not waiting */
    unsigned long hard_deadline;     /* whole-CALL bound; never re-armed */
    struct inode *w_inode;           /* whose i_sem we release, see below */
    int done = 0;
    int ret = 0;

    (void) ppos;

    if (c == NULL || c->b.buf == NULL)
        return -ENODEV;
    if (count == 0)
        return 0;

    /*
     * ===================================================================
     * RELEASE THE VFS's PER-INODE SEMAPHORE FOR THE DURATION OF THIS
     * WRITE.  **This is the fix for the serialisation bug.**
     * ===================================================================
     *
     * 2.2's sys_write holds inode->i_sem across the whole driver call
     * (fs/read_write.c:169).  /dev/dsp is ONE inode, so while we sleep
     * here waiting for buffer space, EVERY OTHER CLIENT is stuck in
     * TASK_UNINTERRUPTIBLE at down() and never reaches this function
     * at all.  Measured on 86Box and the real Acer: one client plays,
     * the rest sit at flags 0x1 with `mixed 0 bytes, 0 empty pulls'
     * and die as our deadline fires in turn.  `ps axl' showed them all
     * in D at one shared wchan.  design/14-write-serialisation.md.
     *
     * MMAP clients were never affected because they issue no write()
     * and so never queue on i_sem - which is why `quake + CD' worked
     * and `lxdoom + CD' did not, for months, on both machines.
     *
     * THE KERNEL'S OWN TTY LAYER DOES EXACTLY THIS, in this same tree:
     * do_tty_write() at drivers/char/tty_io.c:664 releases i_sem on
     * entry, does its work under tty->atomic_write, and re-acquires at
     * :696 before returning.  It is a sanctioned pattern, not a trick.
     *
     * WE NEED NO SUBSTITUTE LOCK.  tty uses tty->atomic_write because
     * several openers share one tty; our channels are allocated
     * per-`file' by vsound_dev_open, so two clients never touch the
     * same channel.  The shared state - the hard buffer and the mixer
     * - is already guarded by cli()/restore_flags().
     *
     * AND NOTHING HERE NEEDS i_sem.  It exists to serialise f_pos on
     * regular files; we take `ppos' and discard it above.  Later
     * kernels skip it for character devices for this exact reason.
     *
     * **EVERY EXIT MUST RE-ACQUIRE.**  sys_write calls up() on our
     * return regardless, so leaving without down() corrupts the
     * count for every future writer.  This function has ONE exit
     * block, at the end, and the two early returns above are placed
     * BEFORE this release deliberately.
     */
    w_inode = file->f_dentry->d_inode;
    up(&w_inode->i_sem);

    /* SET ONCE, ON ENTRY, AND NEVER RE-ARMED. This is what stops a
     * writer waiting forever - see the two bounds explained at the
     * deadline check below. */
    hard_deadline = jiffies + (VSOUND_WRITE_HARD_MS * HZ + 999) / 1000;

    /*
     * ON THE QUEUE BEFORE TESTING FOR SPACE.
     *
     * interruptible_sleep_on() puts the caller on the queue and sleeps
     * as one step, which is too late: between reading `room' and
     * sleeping, the timer can consume, wake the queue, and find nobody
     * on it. The writer then sleeps with space available and nothing
     * scheduled to wake it - and at nrunning == 0 the tick stops
     * entirely, so the wake never comes. A writer blocked forever with
     * a non-full buffer.
     *
     * The fix is the standard 2.2 sequence, and solo1_write() in this
     * kernel's own esssolo1.c is the model: join the queue first, set
     * TASK_INTERRUPTIBLE before re-testing, then schedule(). A wake
     * arriving any time after add_wait_queue() makes schedule() return
     * immediately instead of being missed.
     */
    add_wait_queue(&c->wait, &wait);

    while (done < (int) count) {
        int room, chunk, at;

        save_flags(flags);
        cli();
        room = vsound_buf_free(&c->b);
        at   = vsound_buf_freeptr(&c->b);
        restore_flags(flags);

        /*
         * WHICH BRANCH THIS PASS ACTUALLY TOOK.
         *
         * design/13-starvation-diagnosis.md closes the accounting
         * cause of the multi-client starvation but leaves one thing
         * undetermined: on the 4x3s tone run chan[1] and chan[2] read
         * flags 0x1 - BUSY, never RUNNING - on all 471 jiffies they
         * were open, even though BOTH call sites of
         * vsound_chan_start() below are reached unconditionally from
         * this loop. Three candidate explanations were identified and
         * the data contradicts all three, so none was picked.
         *
         * The trace could not settle it because every existing site
         * reports state OBSERVED FROM ANOTHER CONTEXT - the mixer's
         * chan[] line, sampled once per mix. This one reports the
         * branch THIS pass took, from inside the writer, which is the
         * only thing that distinguishes the candidates:
         *
         *   room > 0   -> the copy path runs, and starts the channel
         *   room <= 0  -> the sleep path runs, and starts it first
         *
         * Either way `flags' on the NEXT pass must show 0x5. A capture
         * where it does not is the measurement that was missing.
         *
         * OUTSIDE cli(): printk is slow and this must not extend the
         * critical section that reads the ring - see the mix loop's
         * own note about holding cli() across long work.
         *
         * Rate-limited with its own counter and its own knob, both
         * defaulting to off: see vsound_rate_write above for why this
         * site in particular cannot default to on.
         */
        if (VSOUND_RL_AT(vsound_rl_write, vsound_rate_write))
            printk(KERN_DEBUG VSOUND_TS "vsound: write pid %d chan %p"
                              " done %d of %d room %d at %d flags 0x%x\n",
                   jiffies,
                   c->pid, (void *) c, done, (int) count, room, at,
                   c->flags);

        if (room <= 0) {
            if (file->f_flags & O_NONBLOCK) {
                ret = done > 0 ? done : -EAGAIN;
                break;
            }

            /* Nothing consumes until output is running. */
            save_flags(flags);
            cli();
            if (!(c->flags & VSOUND_CHN_RUNNING)) {
                vsound_chan_start(&vsound_dev, c);
                vsound_last = jiffies;
                vsound_dev_arm();
            }
            /* Set the state INSIDE cli() and re-test after: a wake
             * between the test above and here has already made the
             * state TASK_RUNNING, so schedule() returns at once. */
            current->state = TASK_INTERRUPTIBLE;
            if (vsound_buf_free(&c->b) > 0)
                current->state = TASK_RUNNING;
            restore_flags(flags);

            /*
             * BOUNDED, NOT FOREVER.
             *
             * This was a bare `schedule()'. A writer whose wake never
             * arrived slept until a signal - and a process blocked in
             * write() cannot reach its own signal check, so it could
             * not be killed, held /dev/dsp open, and kept the module's
             * use count up. On the Acer 2026-08-27 that presented as
             * one leaked lxdoom `sndserv' per run and, with UQM, a
             * mutual hang where neither client could finish.
             *
             * THREE INDEPENDENT SOURCES say an unbounded wait here is
             * the outlier:
             *
             *   - FreeBSD's chn_write sleeps with a timeout
             *     (channel.c:507) and on expiry marks the channel DEAD
             *     and returns EINVAL. `CHN_TIMEOUT' is **5 seconds**
             *     (channel.h:434).
             *   - This kernel's own i810_audio uses
             *     interruptible_sleep_on_timeout().
             *   - esssolo1's solo1_write does the same.
             *
             * WHY 1 SECOND AND NOT THE REFERENCE'S 5. Their timeout
             * covers a real card whose interrupt may be slow to
             * arrive. Ours covers a mixer feeding a userspace pump: a
             * stall beyond a few hundred milliseconds already means
             * something is wrong, and a client that gives up while the
             * user is still watching is easier to diagnose than one
             * that hangs for five seconds. **Provisional** - if 1 s
             * proves too tight under load, the reference's 5 is the
             * value with precedent behind it.
             *
             * ON EXPIRY we return a short write rather than adopting
             * the reference's DEAD-channel concept, which vsound does
             * not have. The client sees fewer bytes accepted than it
             * asked for - which OSS allows - and decides for itself.
             */
            /*
             * THE DEADLINE IS ABSOLUTE, NOT PER-SLEEP.
             *
             * schedule_timeout() returns 0 only when the FULL period
             * elapsed; a wake returns the remaining jiffies. So
             * `if (schedule_timeout(HZ) == 0)' restarts the clock on
             * every wake - and the tick wakes every busy channel
             * unconditionally, ~43 times a second (see
             * design/12-reference-audit.md A-8).
             *
             * A writer with no space would therefore be woken, find
             * none, and sleep another full second, forever. **The
             * guard against hanging would never fire.** Caught by the
             * user asking whether spurious wakes could pile up before
             * this shipped.
             *
             * `deadline' is set once, on entering the wait, and the
             * remaining time is recomputed each pass. Wrap-safe
             * because jiffies, comparisons use signed difference.
             */
            /*
             * THE DEADLINE IS NOW SHORT AND TUNABLE - see
             * vsound_write_ms at the top of this file for why. It was
             * a flat HZ (one second), and holding i_sem that long is
             * what serialised every other client into `D' state.
             *
             * ms -> jiffies, rounded UP so a sub-jiffy setting still
             * gives one jiffy rather than zero. At HZ=100 one jiffy is
             * 10 ms, so 20 ms is two ticks.
             */
            if (deadline == 0) {
                long ticks;

                if (vsound_write_ms <= 0) {
                    /* NEVER SLEEP. Take what fits and tell the caller.
                     * The pure short-write path, for measuring. */
                    ret = done > 0 ? done : -EAGAIN;
                    break;
                }
                ticks = ((long) vsound_write_ms * HZ + 999) / 1000;
                if (ticks < 1)
                    ticks = 1;
                deadline = jiffies + (unsigned long) ticks;
            }

            if (current->state == TASK_INTERRUPTIBLE) {
                long left = (long) deadline - (long) jiffies;

                if (left <= 0) {
                    current->state = TASK_RUNNING;
                } else {
                    schedule_timeout(left);
                }
            }

            if ((long) jiffies - (long) deadline >= 0) {
                /*
                 * TWO BOUNDS, AND BOTH ARE NEEDED.
                 *
                 * **This has now been wrong twice in one day; the
                 * history is here so it is not gone round a third
                 * time.**
                 *
                 * 1. The original returned -EIO after a flat one
                 *    second.  Holding i_sem for that long is the
                 *    serialisation bug (see vsound_write_ms above).
                 *
                 * 2. The first correction returned
                 *    `done > 0 ? done : -EAGAIN'.  EAGAIN on a
                 *    BLOCKING fd is a contract violation - tonetest
                 *    retries only on EINTR, so all four tones died on
                 *    their first buffer.  Worse than the bug.
                 *    tests/logs/2026-08-31-vsound-86box-wfix-01.
                 *
                 * 3. The second correction re-armed the deadline when
                 *    nothing had been taken - and that loops FOREVER.
                 *    A writer with no space never returns, cannot be
                 *    killed except by a signal, holds i_sem and the
                 *    module use count.  It hung the guest.  That is
                 *    the exact bug d623335 added the deadline to
                 *    prevent.
                 *
                 * SO: `deadline' bounds each SLEEP - short, so i_sem
                 * is released often - and `hard_deadline' bounds the
                 * whole CALL, so a writer can never wait forever.
                 *
                 * On the sleep deadline with something taken, return
                 * the short count: legal, and it frees i_sem.
                 * On the sleep deadline with nothing taken, re-arm and
                 * go round - but the hard deadline is still counting.
                 * On the hard deadline, give up: -EIO if nothing was
                 * ever taken, which is what a genuinely wedged device
                 * looks like.
                 */
                if (done > 0) {
                    if (vsound_trace)
                        printk(KERN_DEBUG VSOUND_TS "vsound: write short on"
                                          " chan %p pid %d (%d of %d done)\n",
                   jiffies,
                               (void *) c, c->pid, done, (int) count);
                    ret = done;
                    break;
                }

                if ((long) jiffies - (long) hard_deadline >= 0) {
                    if (vsound_trace)
                        printk(KERN_DEBUG VSOUND_TS "vsound: write TIMEOUT on"
                                          " chan %p pid %d (0 of %d, %d ms)\n",
                   jiffies,
                               (void *) c, c->pid, (int) count,
                               VSOUND_WRITE_HARD_MS);
                    ret = -EIO;
                    break;
                }

                /* Nothing taken, and time remains: re-arm the SLEEP
                 * deadline only.  hard_deadline is untouched. */
                deadline = 0;
            }

            if (signal_pending(current)) {
                ret = done > 0 ? done : -ERESTARTSYS;
                break;
            }
            continue;
        }

        chunk = (int) count - done;
        if (chunk > room)
            chunk = room;
        /* One contiguous run: the ring wraps, so stop at the end. */
        if (at + chunk > (int) c->b.bufsize)
            chunk = (int) c->b.bufsize - at;

        if (copy_from_user(c->b.buf + at, buf + done, (unsigned long) chunk)) {
            ret = done > 0 ? done : -EFAULT;
            break;
        }

        save_flags(flags);
        cli();
        vsound_buf_acquire(&c->b, (const unsigned char *) 0, chunk);
        c->wpos += (unsigned int) chunk;
        /* PROGRESS RESETS THE DEADLINE. The timeout is for "no space
         * for a whole second", not "this write took a second" - a
         * large write that is being served steadily must not be cut
         * short. */
        deadline = 0;
        if (!(c->flags & VSOUND_CHN_RUNNING)) {
            vsound_chan_start(&vsound_dev, c);
            vsound_last = jiffies;
            vsound_dev_arm();
        }
        restore_flags(flags);

        done += chunk;
    }

    /* One exit, so the queue is always left. Leaving a dead task on it
     * corrupts the list for whoever wakes it next. */
    current->state = TASK_RUNNING;
    remove_wait_queue(&c->wait, &wait);

    /*
     * RE-ACQUIRE BEFORE RETURNING, on every path.  sys_write does
     * up(&inode->i_sem) unconditionally when we return, so returning
     * without this leaves the count wrong and corrupts the semaphore
     * for every future writer on this inode.
     *
     * Placed after remove_wait_queue so the sequence mirrors
     * do_tty_write (tty_io.c:696): finish everything that can sleep,
     * THEN take the lock back.
     *
     * down(), not down_interruptible(): we are on our way out and a
     * signal here would leave sys_write's up() unbalanced.  The tty
     * layer makes the same choice at :696.
     */
    down(&w_inode->i_sem);

    if (ret != 0)
        return ret;
    return done;
}

/*
 * THE PUMP READS HERE, and so does `cat /dev/dsp > file'.
 *
 * The output is raw PCM at the device format - 44100 Hz, 16-bit signed
 * little-endian, stereo - with no header. That makes `cat' a valid pump
 * and, more usefully, a way to capture what the MIXER produced without
 * the card path being involved at all:
 *
 *     cat /dev/dsp > out.raw
 *     aplay -f S16_LE -r 44100 -c 2 out.raw        (on the workstation)
 *
 * That matters because the old tree could never tell a mixer fault from
 * a pump fault - both produced wrong audio and neither could be
 * isolated. If the captured file sounds right, the mixer and converter
 * are right and the fault is downstream.
 *
 * Blocks until something is ready, because a pump that spins is a pump
 * that starves the client it is draining.
 */
static ssize_t
vsound_dev_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    struct wait_queue wait = { current, NULL };
    unsigned char tmp[2048];
    unsigned long flags;
    int want, got, done = 0, claimed;

    (void) ppos;

    if (count == 0)
        return 0;

    /*
     * ONE PUMP, since 2026-09-14 - vsound_pump_claim(). A second
     * reader draining would take alternate chunks of the mixed stream
     * and, with -R, could take the arming from a live pump. It is
     * refused here, at the first thing only a pump does, rather than
     * at open() - the mixer tools open O_RDONLY for the ioctls.
     */
    save_flags(flags);
    cli();
    claimed = vsound_pump_claim(&vsound_dev, (void *) file);
    restore_flags(flags);
    if (claimed < 0)
        return -EBUSY;
    if (claimed > 0 && vsound_trace)
        printk(KERN_DEBUG VSOUND_TS "vsound: this reader is the pump"
               " (pid %d, %s)\n", jiffies, current->pid, current->comm);

    /* No channel needed: the pump reads the DEVICE's mixed output, not
     * any one client's stream. */
    add_wait_queue(&vsound_drain_wait, &wait);

    for (;;) {
        want = (int) count;
        if (want > (int) sizeof tmp)
            want = (int) sizeof tmp;

        got = vsound_drain_get(&vsound_dev, tmp, want);
        if (got > 0) {
            if (copy_to_user(buf, tmp, (unsigned long) got))
                done = -EFAULT;
            else
                done = got;
            break;
        }

        if (file->f_flags & O_NONBLOCK) {
            /* A non-blocking reader never reaches the sleep below, so
             * the idle signal is delivered here or not at all. Pacing
             * is that reader's own business - it asked not to wait. */
            done = vsound_dev.release_idle ? 0 : -EAGAIN;
            break;
        }

        current->state = TASK_INTERRUPTIBLE;
        if (vsound_buf_ready(&vsound_dev.hard.b) > 0) {
            /* Arrived between the get and here: hand it over first,
             * before the idle test below can return ahead of it. */
            current->state = TASK_RUNNING;
            continue;
        }
        schedule_timeout(HZ / 100);  /* 10 ms since 2026-09-30; was HZ/20
                                      * (50 ms) - see the idle ack below */

        if (signal_pending(current)) {
            done = -ERESTARTSYS;
            break;
        }

        /*
         * THE IDLE ACK - 2026-09-30, found on 86Box's ESS Solo-1 and
         * a liveness bug on any card. The mixer feeds the hard buffer
         * only while fewer than vsound_depth bytes are outstanding to
         * the pump; bytes are retired only by an ACK; and the pump
         * sends an ACK only after a card write. So once the window is
         * full of bytes the card has not yet been seen to play,
         * nothing is ready, the pump sleeps HERE, and no ACK can ever
         * arrive: a deadlock. A real Solo-1's position counter moves
         * continuously, so by the fourth write's ack it has moved a
         * little, the window opens a little, and the loop bootstraps
         * itself - the Acer never showed it. 86Box's moves once per
         * 20 ms host period and is frozen in between, so four writes
         * a millisecond apart retire nothing and the loop closes
         * every time: 16384 bytes play (five interrupts' worth), then
         * silence. tests/logs/2026-09-30-86box-solo1-fresh-stock-pump-stall
         * has the seven runs; design/07 the account.
         *
         * Returning 0 while something is outstanding hands the pump a
         * turn to ask the card where it is and ACK it (vsoundd.c's
         * idle ack). Paced by the sleep above, so it is not the
         * instant-return spin 5658f87 removed; an idle pump with
         * nothing in flight sleeps as before. The sleep itself went
         * from 50 ms to 10 ms in the same change: at 50 ms the card
         * was fed in bursts coarse enough to run dry between turns
         * (trial v1, gaps and clicks); at 10 ms it is not.
         */
        if (vsound_hard_outstanding(&vsound_dev.hard) > 0) {
            done = 0;
            break;
        }

        /*
         * IDLE, AND THE PUMP ASKED TO BE TOLD - design/16 section 4.
         *
         * Return 0 so the pump wakes and can release the real card.
         * Tested AFTER vsound_drain_get() above, so anything still
         * mixed is handed over first and nothing is truncated - and
         * AFTER THE SLEEP, since 2026-09-14, so a zero-length read
         * costs the pump one 50 ms sleep before it can get another.
         *
         * THE FLAG IS NOT CLEARED HERE. The pump declines to release
         * when its STAT cross-check reports nbusy or hard_ready
         * non-zero, because releasing then would truncate audio. The
         * kernel cannot see that decision. Clearing here would spend
         * the signal on a release that never happened and nothing
         * would raise it again - a hanging note, which is what this
         * whole mechanism exists to prevent. So the level stays set
         * and a refused cross-check retries on the next read: the
         * pump's `continue' re-enters here, sleeps 50 ms, reads 0
         * again. The pump clears it with VSOUND_IOC_RELEASED once it
         * has actually closed the card.
         *
         * WHY THE TEST SITS BELOW THE SLEEP. Until 2026-09-14 it sat
         * above it, and the comment beside it said a refused
         * cross-check "retries on the next read, 50 ms later" - but
         * the read returned before reaching the sleep, so the retry
         * was immediate and a refusal spun a core for as long as it
         * lasted: a client open but not yet started (unbounded for
         * one that holds /dev/dsp silent), or a client that opened
         * and closed without starting after the card was already
         * released (100% until some client started). design/25 B2.
         * 5658f87's commit message and the 2026-09-13 comments
         * describe the 50 ms retry as if it existed; this is where it
         * now does.
         *
         * WHAT IT COSTS. Reacquire: nothing. The sleep is on
         * vsound_drain_wait, which the mixer tick wakes, so a client
         * starting during the 50 ms wakes this read at once with
         * data. The signal itself: still seen within 50 ms, and that
         * bound does not depend on what wakes this queue. The level
         * test follows EVERY sleep, however the sleep ended - the
         * timeout, or the wake_up_interruptible that every mix fires
         * (vsound_dev_mix_once(), after its restore_flags, from the
         * timer and from the pump's own call alike) - so a wake that
         * lands after the flag is set delivers it sooner, and one
         * that lands before costs one pass: a vsound_drain_get() that
         * finds nothing and a fresh 50 ms sleep, which cannot push
         * the bound out. And release() wakes this queue after setting
         * the flag (also 2026-09-14), so the usual case is scheduler
         * latency; 50 ms is the bound when that wake finds the read
         * between sleeps and is lost. The one case that moves is a
         * read entered AFTER the flag is set, which returned 0 at once
         * and now sleeps first - and the wake that came with the flag
         * was lost on it, so it pays the full 50 ms: still inside
         * design/16's ~100 ms reclaim window.
         * And vsoundd's `reclaim' counts passes that RETURNED DATA, which
         * never come through here, so its 30-pass budget is
         * unchanged - design/07 and design/25 B2 flagged it as
         * dropping to 20/s, and it does not.
         *
         * AND vsound_chan_start() CLEARS IT ON THE WAY BACK - NOT
         * vsound_dev_write(). That site is load-bearing and is not
         * obvious: MMAP CLIENTS ISSUE NO write(). quake mmaps, so a
         * clear in the write path would never fire for it and the flag
         * would still be set while quake was audibly playing - the
         * same asymmetry that hid the i_sem bug for months
         * (design/14). Both paths reach chan_start: write() through
         * its start, and SETTRIGGER for mmap clients. See
         * vsound_chan.c's vsound_chan_start().
         *
         * THE SPIN 5658f87 FIXED: with no client left, nothing could
         * ever clear the level, so read() returned 0 instantly and
         * forever and vsoundd's `continue' had nothing to pace it -
         * 100% of a core about three seconds after a bare `-R -M'
         * load. design/07's RESUME POINT has the eleven-run isolation.
         * The flags also outlived the pump that set them: see
         * vsound_pump_gone() in the reader's release().
         */
        if (vsound_dev.release_idle) {
            done = 0;
            break;
        }
    }

    current->state = TASK_RUNNING;
    remove_wait_queue(&vsound_drain_wait, &wait);
    return done;
}

static unsigned int
vsound_dev_poll(struct file *file, poll_table *wait)
{
    struct vsound_chan *c = (struct vsound_chan *) file->private_data;
    unsigned long flags;
    unsigned int mask = 0;
    int room;

    /* A reader has no channel - it is the pump. Report whether the
     * mixed stream has anything, not whether some client has room. */
    if (c == NULL) {
        poll_wait(file, &vsound_drain_wait, wait);
        /* THE IDLE SIGNAL IS READABLE TOO, since 2026-09-14. It is
         * delivered as a zero-length read, so a pump that polls before
         * reading would otherwise never learn of it - nothing else
         * wakes this queue once nrunning is 0. release_idle is only
         * ever set when -R armed it. Nothing polls the reader today. */
        return (vsound_buf_ready(&vsound_dev.hard.b) > 0
                || vsound_dev.release_idle)
               ? (POLLIN | POLLRDNORM) : 0;
    }

    poll_wait(file, &c->wait, wait);

    save_flags(flags);
    cli();
    room = vsound_buf_free(&c->b);
    restore_flags(flags);

    /*
     * A FRAGMENT, NOT A BYTE.
     *
     * The reference's `chn_polltrigger' compares free space against
     * `c->lw', which is `sndbuf_getblksz(bs)' - one fragment
     * (channel.c:292, :1957):
     *
     *     return ((delta < c->lw) ? 0 : 1);
     *
     * Ours reported writable whenever ONE BYTE was free, so a client
     * that polls before every write - quake and UQM both do - could be
     * woken to write a byte at a time. A syscall per byte instead of
     * per fragment, and every one of them takes cli() twice.
     *
     * `b.blksz` is what SETFRAGMENT settled on for this channel, so the
     * threshold is the client's own idea of a useful unit. Falls back
     * to "any space" if it was never set, which is better than
     * refusing to report writable at all.
     *
     * Found by the audit, design/12-reference-audit.md A-5. No
     * observed symptom - the cost is CPU, not correctness.
     */
    if (c->b.blksz > 0) {
        if (room >= (int) c->b.blksz)
            mask |= POLLOUT | POLLWRNORM;
    } else if (room > 0) {
        mask |= POLLOUT | POLLWRNORM;
    }
    return mask;
}

static int
vsound_dev_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
                 unsigned long arg)
{
    struct vsound_chan *c = (struct vsound_chan *) file->private_data;
    unsigned long flags;
    int v, err;

    (void) inode;

    /*
     * THE PUMP HAS NO CHANNEL, AND ITS IOCTLS MUST STILL WORK.
     *
     * VSOUND_IOC_ACK is how the card's position gets back to us, and
     * VSOUND_IOC_STAT is diagnostics: neither touches a channel. This
     * guard returned -ENODEV for both because a reader deliberately has
     * private_data == NULL, so EVERY ack was rejected before reaching
     * the handler.
     *
     * The trace shows the cost exactly: one drain of 1760 bytes,
     * "out 1760", and then zero acks for the rest of the run - so
     * outstanding never fell, room stayed 0, and the mixer produced
     * nothing after the first tick. 474 lines of "no reader" while the
     * reader was in fact open.
     *
     * Third instance of the same mistake: a guard written when only
     * clients used this device, blocking the pump once the pump
     * existed. open() and poll() were fixed; ioctl was missed.
     *
     * FOURTH INSTANCE, 2026-09-01: VSOUND_IOC_RELEASE was added for
     * -R and not added to this list, so every arming attempt returned
     * -ENODEV and the pump correctly reported "this module does not
     * support release-on-idle" against a module that did. Two 86Box
     * runs were spent looking for a stale image before the guard was
     * suspected.
     *
     * **THE RULE, since three fixes have not stopped it: ANY IOCTL THE
     * PUMP ISSUES MUST BE LISTED HERE.** The pump holds the device
     * with private_data == NULL deliberately - it reads the mixed
     * output and owns no channel - so the default branch rejects
     * everything it sends. A new pump ioctl is two edits, not one.
     *
     * FIFTH ENTRY, 2026-09-13: VSOUND_IOC_RELEASED. Added to this list
     * in the SAME commit as its handler, deliberately - the four
     * previous instances were all a handler landing without its line
     * here, and the failure is silent in the worst way: the ioctl
     * returns -ENODEV and the pump reports that the module does not
     * support the feature, against a module that does.
     */
    switch (cmd) {
    case VSOUND_IOC_ACK:
    case VSOUND_IOC_STAT:
    case VSOUND_IOC_CHANS:
    case VSOUND_IOC_VOL:
    case VSOUND_IOC_RELEASE:
    case VSOUND_IOC_RELEASED:
    case VSOUND_IOC_MIXRATE:
    case VSOUND_IOC_MUTE:
        break;                  /* channel not required */
    default:
        if (c == NULL)
            return -ENODEV;
        break;
    }

    switch (cmd) {
    case SNDCTL_DSP_SPEED:
        if (get_user(v, (int *) arg))
            return -EFAULT;
        if (v > 0) {
            /* Bounded - design/25 B9; the granted rate goes back. */
            if (v < VSOUND_RATE_MIN)
                v = VSOUND_RATE_MIN;
            if (v > VSOUND_RATE_MAX)
                v = VSOUND_RATE_MAX;
            c->rate = v;
            vsound_dev_format_changed(c);
        }
        return put_user(c->rate, (int *) arg);


    case SNDCTL_DSP_STEREO:
        if (get_user(v, (int *) arg))
            return -EFAULT;
        c->channels = v ? 2 : 1;
        vsound_dev_format_changed(c);
        return put_user(v ? 1 : 0, (int *) arg);

    case SNDCTL_DSP_CHANNELS:
        if (get_user(v, (int *) arg))
            return -EFAULT;
        if (v > 0) {
            c->channels = (v > 1) ? 2 : 1;
            vsound_dev_format_changed(c);
        }
        return put_user(c->channels, (int *) arg);

    case SNDCTL_DSP_SETFMT:
        if (get_user(v, (int *) arg))
            return -EFAULT;
        if (v != AFMT_QUERY) {
            c->format = (v == AFMT_S16_LE || v == AFMT_U8) ? v : AFMT_S16_LE;
            vsound_dev_format_changed(c);
        }
        return put_user(c->format, (int *) arg);

    case SNDCTL_DSP_GETFMTS:
        return put_user(AFMT_S16_LE | AFMT_U8, (int *) arg);

    /*
     * WHAT WAS GRANTED, READ BACK - design/25 / design/54 D34
     * (2026-10-04). These fell through to "ioctl 0x80045002 unhandled":
     * vmidid asks SOUND_PCM_READ_RATE for the rate it really got, and
     * was told EINVAL. Harmless while the answer matched the request;
     * not once SPEED clamps. The same answers every 2.2 PCI driver gives
     * (esssolo1.c: the rate, the channels, and 8 or 16 bits).
     */
    case SOUND_PCM_READ_RATE:
        return put_user(c->rate, (int *) arg);
    case SOUND_PCM_READ_CHANNELS:
        return put_user(c->channels, (int *) arg);
    case SOUND_PCM_READ_BITS:
        return put_user(c->format == AFMT_U8 ? 8 : 16, (int *) arg);

    case SNDCTL_DSP_SETFRAGMENT:
        if (get_user(v, (int *) arg))
            return -EFAULT;

        /*
         * NOT WHILE MAPPED OR RUNNING. Review finding 2.
         *
         * vsound_chan_setgeom() frees the old buffer and allocates a
         * new one whenever the size differs. If the client has already
         * mapped it, its pages are freed underneath the mapping - and
         * vsound_dsp_nopage() then resolves faults against a c->b.buf
         * that points somewhere else, or the client touches an
         * already-faulted page of freed vmalloc space. A
         * use-after-free reachable from an ordinary ioctl.
         *
         * The reference makes it unreachable rather than guarding it:
         * chn_resizebuf returns EINVAL immediately for a mapped or
         * triggered channel (channel.c:1787-1789), and every geometry
         * path goes through it.
         *
         * Section 9 lists the geometry REFUSAL under "explicitly not adopted",
         * with two reasons: routing all four rate ioctls through one
         * guard regressed lxdoom once, and refusal is a bigger
         * behavioural change than a guard. Both reasons are about the
         * geometry MOVING under a client. NEITHER covers freeing memory
         * that is mapped, which is not a stale AND-mask but a dangling
         * pointer - so this one case takes the refusal.
         *
         * Narrower than what section 9 declined: one ioctl, and only in the
         * state where the alternative is a use-after-free.
         */
        if (c->flags & (VSOUND_CHN_MMAP | VSOUND_CHN_RUNNING)) {
            if (vsound_trace)
                printk(KERN_DEBUG VSOUND_TS "vsound: SETFRAGMENT REFUSED -EINVAL"
                                  " (%s%s - the buffer is in use)\n",
               jiffies,
                       (c->flags & VSOUND_CHN_MMAP) ? "mapped" : "",
                       (c->flags & VSOUND_CHN_RUNNING) ? " running" : "");
            return -EINVAL;
        }
        {
            unsigned int ln  = (unsigned int) (v & 0xffff);
            unsigned int cnt = (unsigned int) ((v >> 16) & 0xffff);
            unsigned int sz;

            /*
             * RANGE THE SHIFT BEFORE USING IT. dsp.c:1615 does
             * RANGE(fragln, 4, 16) then fragsz = 1 << fragln, so the
             * fragment is between 16 bytes and 64 KB before anything is
             * allocated. Ours took the shift unbounded: `1U << 31' is a
             * legal argument that reaches vmalloc as a multiplicand.
             * It failed safe - the allocation refuses and we return
             * -EINVAL - but "attempt something absurd, fail" is not the
             * same as rejecting an absurd request.
             */
            if (ln < 4)
                ln = 4;
            if (ln > 16)
                ln = 16;
            sz = 1U << ln;

            if (cnt == 0 || cnt > 0x7fff)
                cnt = VSOUND_DEF_BLKCNT;
            /* dsp.c:1620-1621 raises a too-small count rather than
             * refusing it; setgeom's own floor is the same 2, from
             * sndbuf_remalloc (buffer.c:219). Clamping here means a
             * client asking for 1 gets a working device instead of
             * -EINVAL. */
            if (cnt < 2)
                cnt = 2;

            /*
             * THE WINDOW MUST BE A POWER OF TWO, and nothing else says
             * so - NOTES-quake.md section 1 is the only record.
             *
             * quake computes
             *
             *     shm->samples = fragstotal * fragsize / (bits/8)
             *
             * and then uses it as an AND-mask, twice (snd_mix.c:118,
             * :166-167). If it is not a power of two the mask truncates
             * to the next lower one and every write lands at the wrong
             * offset - AUDIBLE AS GARBAGE, not as silence, and not as
             * an error anywhere.
             *
             * Dividing by the sample size cannot break the property, so
             * constraining the byte total is sufficient.
             *
             * THE REWRITE IS WHAT MADE THIS REACHABLE. The note says it
             * did not bite on the old tree only because the mmap branch
             * of GETOSPACE reported the full 64 KB ring rather than the
             * real window. Per-channel sizing removed that accident:
             * GETOSPACE now reports the true geometry, which is exactly
             * the condition the note says would make it bite.
             *
             * Rounded DOWN, so the client gets no more than it asked
             * for and the latency budget (07's section 6) cannot be
             * quietly exceeded.
             */
            while (cnt > 1 && ((cnt * sz) & ((cnt * sz) - 1)) != 0)
                cnt--;

            err = vsound_chan_setgeom(c, cnt, sz,
                                      vsound_dsp_alloc, vsound_dsp_free);
            if (err < 0)
                return -EINVAL;
            c->flags |= VSOUND_CHN_FRAG_SET;

            if (vsound_trace)
                printk(KERN_DEBUG VSOUND_TS "vsound: SETFRAGMENT asked %u x %u,"
                                  " gave %u x %u = %u bytes\n",
               jiffies,
                       (unsigned int) ((v >> 16) & 0xffff),
                       1U << (unsigned int) (v & 0xffff),
                       c->b.blkcnt, c->b.blksz, c->b.bufsize);

            /*
             * TELL THE CLIENT WHAT IT ACTUALLY GOT. The reference
             * recomputes the argument from the granted values rather
             * than echoing the request (dsp.c:1648-1654), and with the
             * rounding above there is now something to report: a client
             * that asked for 3 fragments has 2.
             *
             * Recomputed from c->b, not from what was asked, so this
             * cannot drift from what was allocated.
             */
            {
                unsigned int gsz = c->b.blksz;
                unsigned int gln = 0;

                while (gsz > 1) {
                    gln++;
                    gsz >>= 1;
                }
                v = (int) ((c->b.blkcnt << 16) | gln);
            }
        }
        return put_user(v, (int *) arg);

    case SNDCTL_DSP_GETBLKSIZE:
        c->flags |= VSOUND_CHN_GEOM_READ;
        return put_user((int) c->b.blksz, (int *) arg);

    case SNDCTL_DSP_GETOSPACE: {
        audio_buf_info info;

        save_flags(flags);
        cli();
        info.bytes      = vsound_buf_free(&c->b);
        info.fragsize   = (int) c->b.blksz;
        info.fragstotal = (int) c->b.blkcnt;
        info.fragments  = c->b.blksz ? info.bytes / (int) c->b.blksz : 0;
        restore_flags(flags);

        c->flags |= VSOUND_CHN_GEOM_READ;
        if (copy_to_user((void *) arg, &info, sizeof info))
            return -EFAULT;
        return 0;
    }

    /*
     * HOW FAR BEHIND IS THIS CLIENT - review finding 13,
     * design/08-vsound-review.md. The default case returned -EINVAL,
     * where every real driver answers: the reference has it
     * (dsp.c:1807) and so does esssolo1 (esssolo1.c:1424).
     *
     * THE REFERENCE REPORTS THE SOFT BUFFER ONLY:
     *
     *     *arg_i = sndbuf_getready(bs);      dsp.c:1811, bs = bufsoft
     *
     * i.e. what THIS CLIENT has written and the mixer has not yet
     * taken. Not the hard buffer, not the card.
     *
     * WE REPORT MORE, and the difference is deliberate. FreeBSD's
     * bufsoft sits directly against the hardware channel; ours has two
     * further stages before a sample is audible - the shared hard
     * buffer, and whatever the card still holds, which the pump tells
     * us in every ack. A client asking "how far behind am I" means the
     * whole path, and answering with only the first stage would
     * understate it by the pump's depth - typically ~14 KB, which is
     * 83 ms and the largest term of the three.
     *
     * So: this channel's unmixed bytes, plus what is mixed and waiting
     * in the hard buffer, plus what is outstanding to the card. Every
     * term is already tracked; none of this is new state.
     *
     * SCALED TO THE CLIENT'S OWN RATE. The last two terms are in
     * MIX-format bytes - 44100 stereo - and a client at 11025 mono
     * would otherwise be told a number four times too large in its own
     * units. vsound_xbytes() is the same conversion the mixer uses.
     */
    case SNDCTL_DSP_GETODELAY: {
        int odelay;

        save_flags(flags);
        cli();
        odelay = vsound_buf_ready(&c->b)
                 + vsound_xbytes(vsound_buf_ready(&vsound_dev.hard.b)
                                 + vsound_hard_outstanding(&vsound_dev.hard),
                                 vsound_dev_bps(&vsound_dev),
                                 vsound_chan_bps(c));
        restore_flags(flags);

        if (odelay < 0)
            odelay = 0;
        return put_user(odelay, (int *) arg);
    }

    case SNDCTL_DSP_GETOPTR: {
        count_info cinfo;

        save_flags(flags);
        cli();
        cinfo.bytes  = (int) c->b.total;
        cinfo.blocks = c->b.blksz
                       ? vsound_buf_ready(&c->b) / (int) c->b.blksz : 0;
        cinfo.ptr    = vsound_buf_readyptr(&c->b);
        restore_flags(flags);

        if (copy_to_user((void *) arg, &cinfo, sizeof cinfo))
            return -EFAULT;
        return 0;
    }

    case SNDCTL_DSP_SETTRIGGER:
        if (get_user(v, (int *) arg))
            return -EFAULT;
        save_flags(flags);
        cli();
        if (v & PCM_ENABLE_OUTPUT) {
            if (!(c->flags & VSOUND_CHN_RUNNING)) {
                vsound_chan_start(&vsound_dev, c);
                vsound_last = jiffies;
                vsound_dev_arm();
            }
        } else {
            vsound_chan_stop(&vsound_dev, c);
        }
        restore_flags(flags);
        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: SETTRIGGER 0x%x (running %d)\n",
               jiffies,
                   v, vsound_dev.nrunning);
        return 0;

    case SNDCTL_DSP_GETTRIGGER:
        return put_user((c->flags & VSOUND_CHN_RUNNING)
                        ? PCM_ENABLE_OUTPUT : 0, (int *) arg);

    case SNDCTL_DSP_GETCAPS:
        /* MMAP and TRIGGER, which is what a mapped client checks for.
         * No DUPLEX: playback only. */
        return put_user(DSP_CAP_MMAP | DSP_CAP_TRIGGER | DSP_CAP_REALTIME,
                        (int *) arg);

    case VSOUND_IOC_ACK: {
        struct vsound_ack ack;

        if (copy_from_user(&ack, (void *) arg, sizeof ack))
            return -EFAULT;
        if (vsound_drain_ack(&vsound_dev, &ack) < 0)
            return -EINVAL;

        /*
         * Space may have been retired, so blocked writers can move.
         * EVERY channel, not `c' - the caller is the pump and has no
         * channel of its own, and even if it did, retiring hard-buffer
         * space frees the mixer for all of them.
         *
         * **THIS ONE STAYS UNCONDITIONAL, and that is not an
         * oversight.** Review finding A-8 made the MIX's wake
         * conditional on having taken from a channel, which is the
         * reference's rule (channel.c:417). It does not apply here:
         * an ack frees HARD-buffer space, so NO channel gained room
         * and a per-channel condition would wake nobody, ever.
         *
         * What an ack changes is that the mixer can produce again -
         * vsound_drain_room() was 0 and now is not - and a writer
         * blocked on a full channel needs the mixer to run before it
         * can move. Waking it here is what gets that going.
         *
         * The cost is bounded in a way the mix's was not: an ack
         * arrives once per pump write, not on every tick per channel.
         */
        {
            int i;

            for (i = 0; i < VSOUND_SLOTS; i++)
                if (vsound_dev.chan[i].flags & VSOUND_CHN_BUSY)
                    wake_up_interruptible(&vsound_dev.chan[i].wait);
        }
        return 0;
    }

    case VSOUND_IOC_CHANS: {
        int n;
        struct vsound_chanlist cl;
        int i;

        memset(&cl, 0, sizeof cl);

        save_flags(flags);
        cli();
        cl.generation = (__u32) vsound_dev.generation;
        /*
         * FOUR, OR FIVE WITH THE MIDI SLOT - and the fifth is always
         * the last, so a tool's rows 0..3 mean the same thing on
         * every machine. `nchan' is what a tool iterates; the struct
         * has room for VSOUND_LIST_CHAN either way (vsound.h).
         *
         * The MIDI entry is flagged even while free, so a tool can
         * show it as reserved rather than as a fifth empty channel a
         * game might have taken.
         *
         * `index' reports the REAL slot number, so a tool showing
         * "channel 2" and the trace saying slot 2 agree - and a tool
         * SENDS that index back in VSOUND_IOC_VOL rather than its own
         * row number. Today they are equal; the field exists so that
         * they need not be.
         */
        cl.nchan = vsound_midi ? VSOUND_SLOTS : VSOUND_MAX_CHAN;
        for (i = 0, n = 0; n < (int) cl.nchan; i++, n++) {
            struct vsound_chan *ch = &vsound_dev.chan[i];

            cl.chan[n].index = (__u32) i;
            cl.chan[n].vol   = (__u32) ch->vol;
            if (i == VSOUND_MIDI_SLOT)
                cl.chan[n].flags = VSOUND_CI_MIDI;
            if (!(ch->flags & VSOUND_CHN_BUSY))
                continue;               /* free: pid 0, empty comm */

            cl.chan[n].pid      = (__u32) ch->pid;
            cl.chan[n].rate     = (__u32) ch->rate;
            cl.chan[n].channels = (__u32) ch->channels;
            cl.chan[n].format   = (__u32) ch->format;
            cl.chan[n].flags   |= VSOUND_CI_BUSY;
            if (ch->flags & VSOUND_CHN_RUNNING)
                cl.chan[n].flags |= VSOUND_CI_RUNNING;
            if (ch->flags & VSOUND_CHN_MMAP)
                cl.chan[n].flags |= VSOUND_CI_MMAP;
            /* `vol' above is the level the user set, muted or not -
             * this bit is the only thing that says it is silent. */
            if (ch->flags & VSOUND_CHN_MUTED)
                cl.chan[n].flags |= VSOUND_CI_MUTED;
            /* TRUNCATED TO 32 BITS DELIBERATELY. Callers compare two
             * samples for INEQUALITY, so a wrap is harmless - it
             * would have to land on exactly the previous value to be
             * missed, and the next poll catches it. */
            cl.chan[n].mixed = (__u32) ch->mixed_bytes;
            memcpy(cl.chan[n].comm, ch->comm, sizeof cl.chan[n].comm);
        }
        restore_flags(flags);

        /* Outside cli(): copy_to_user can sleep, and the snapshot is
         * already taken - a generation change while we copy is exactly
         * what the counter is for. */
        if (copy_to_user((void *) arg, &cl, sizeof cl))
            return -EFAULT;
        return 0;
    }

    case VSOUND_IOC_VOL: {
        struct vsound_vol vv;
        int err2;

        if (copy_from_user(&vv, (void *) arg, sizeof vv))
            return -EFAULT;

        save_flags(flags);
        cli();
        err2 = vsound_chan_setvol(&vsound_dev, (int) vv.index,
                                  (int) vv.pid, (int) vv.vol);
        restore_flags(flags);

        /* vsound_chan_setvol returns raw errnos so it can stay free of
         * kernel headers for the host tests. */
        if (err2 != 0)
            return err2;

        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: vol chan %u -> %u%%\n",
               jiffies,
                   vv.index, vv.vol);
        return 0;
    }

    case VSOUND_IOC_PROGGET:
    case VSOUND_IOC_PROGSET: {
        /*
         * PER-PROGRAM LEVELS - vsound.h. KMALLOC'D, not static and not
         * on the stack: ~780 bytes with the snapshot is a lot of a 2.2
         * kernel stack, and a static buffer would be shared by two
         * callers, one of which can sleep in the user copy while the
         * other refills it. An ioctl may sleep, so GFP_KERNEL is right.
         */
        struct prog_scratch {
            struct vsound_proglist pl;
            char cm[VSOUND_PROG_MAX][16];
            int  vo[VSOUND_PROG_MAX], mu[VSOUND_PROG_MAX];
        } *ps;
        struct vsound_proglist *pl;
        int  n, i, rc = 0;

        ps = (struct prog_scratch *) kmalloc(sizeof *ps, GFP_KERNEL);
        if (ps == NULL)
            return -ENOMEM;
        pl = &ps->pl;

        if (cmd == VSOUND_IOC_PROGGET) {
            /* SNAPSHOT UNDER cli(), COPY OUT AFTER - copy_to_user can
             * fault and sleep, and must not do so with interrupts off. */
            save_flags(flags);
            cli();
            n = vsound_prog_get(&vsound_dev, ps->cm, ps->vo, ps->mu,
                                VSOUND_PROG_MAX);
            restore_flags(flags);

            memset(pl, 0, sizeof *pl);
            pl->nprog = (__u32) n;
            for (i = 0; i < n; i++) {
                memcpy(pl->prog[i].comm, ps->cm[i], VSOUND_COMM_LEN);
                pl->prog[i].comm[VSOUND_COMM_LEN - 1] = '\0';
                pl->prog[i].vol   = (__u32) ps->vo[i];
                pl->prog[i].flags = ps->mu[i] ? VSOUND_PROG_MUTED : 0;
            }
            if (copy_to_user((void *) arg, pl, sizeof *pl))
                rc = -EFAULT;
        } else if (copy_from_user(pl, (void *) arg, sizeof *pl)) {
            rc = -EFAULT;
        } else if (pl->nprog > VSOUND_PROG_MAX) {
            rc = -EINVAL;
        } else {
            /* REPLACED WHOLE, in the order given - oldest first, as
             * PROGGET hands it out - so the ages come back the same. */
            n = (int) pl->nprog;
            save_flags(flags);
            cli();
            vsound_prog_clear(&vsound_dev);
            for (i = 0; i < n; i++) {
                pl->prog[i].comm[VSOUND_COMM_LEN - 1] = '\0';
                vsound_prog_note(&vsound_dev, pl->prog[i].comm,
                                 (int) (pl->prog[i].vol > VSOUND_VOL_BOOST
                                        ? VSOUND_VOL_BOOST
                                        : pl->prog[i].vol),
                                 (pl->prog[i].flags & VSOUND_PROG_MUTED)
                                     != 0);
            }
            restore_flags(flags);

            if (vsound_trace)
                printk(KERN_DEBUG VSOUND_TS "vsound: %d program levels"
                       " set\n", jiffies, n);
        }
        kfree(ps);
        return rc;
    }

    case VSOUND_IOC_MUTE: {
        struct vsound_mute vm;
        int err3;

        if (copy_from_user(&vm, (void *) arg, sizeof vm))
            return -EFAULT;

        save_flags(flags);
        cli();
        err3 = vsound_chan_setmute(&vsound_dev, (int) vm.index,
                                   (int) vm.pid, (int) vm.muted);
        restore_flags(flags);

        /* Raw errnos, as setvol does, so vsound_chan.c stays free of
         * kernel headers for the host tests. */
        if (err3 != 0)
            return err3;

        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: mute chan %u -> %s\n",
                   jiffies,
                   vm.index, vm.muted ? "on" : "off");
        return 0;
    }

    case VSOUND_IOC_STAT: {
        struct vsound_stat st;

        vsound_drain_stat(&vsound_dev, &st);
        if (copy_to_user((void *) arg, &st, sizeof st))
            return -EFAULT;
        return 0;
    }

    /*
     * THE PUMP ARMING RELEASE-ON-IDLE - design/16 section 4, -R.
     *
     * Opt-in, so a pump that did not ask never sees the zero-length
     * read this enables. Disarming clears any pending signal too: a
     * pump that has stopped wanting them should not find one waiting.
     * ARMING CLEARS ONE AS WELL, since 2026-09-14: a signal still set
     * when a pump arms was raised for a pump that is no longer there
     * to act on it, and left alone it would hand this one a
     * zero-length read on its first idle for a card it holds
     * (design/25 B3). Arming claims the PUMP ROLE for this file, so
     * its close can disarm (vsound_pump_gone()) - and so a second
     * reader cannot take the arming from a live pump: -EBUSY while
     * another reader holds the role, which vsoundd treats as fatal.
     */
    case VSOUND_IOC_RELEASE: {
        int on, rc;

        if (get_user(on, (int *) arg))
            return -EFAULT;
        save_flags(flags);
        cli();
        rc = vsound_release_arm(&vsound_dev, (void *) file, on);
        restore_flags(flags);
        if (rc < 0) {
            if (vsound_trace)
                printk(KERN_DEBUG VSOUND_TS "vsound: release-on-idle"
                                  " refused - another reader is the"
                                  " pump\n", jiffies);
            return -EBUSY;
        }
        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: release-on-idle %s\n",
                   jiffies, on ? "armed" : "disarmed");
        return 0;
    }

    /*
     * THE PUMP HAS RELEASED THE CARD - consume the signal.
     *
     * Sent by vsoundd immediately after close(cardfd) succeeds, so the
     * level that told it to release is spent exactly when the release
     * has happened and not before. A pump that reads 0 and DECLINES to
     * release (its STAT cross-check refused) sends nothing, the level
     * stays set, and the next read sleeps its 50 ms and retries.
     *
     * Deliberately NOT gated on release_armed. An unarmed pump can
     * never have been told to release, so it has nothing to consume,
     * and clearing an already-clear flag is harmless. Gating would add
     * a failure mode for no gain.
     */
    case VSOUND_IOC_RELEASED: {
        int spent;

        save_flags(flags);
        cli();
        /* SPENT ONLY IF NOTHING CAME AND WENT DURING THE CLOSE - design/54
         * D31, vsound_release_consume(). The counter reset below still
         * runs every time: the card really was closed. */
        spent = vsound_release_consume(&vsound_dev);
        /*
         * THE CARD'S COUNTER IS GONE WITH THE CARD - design/25 B6,
         * fixed 2026-09-16.
         *
         * The pump has just CLOSED /dev/dsp1. When it reopens, the
         * card's played counter starts again from wherever the driver
         * chooses, so the next ack's absolute value says nothing about
         * what WE sent. The ack path only baselines on its FIRST ack
         * (have_played == 0); without clearing that here it took a
         * delta against the old card's number instead, which wrapped
         * to about 4e9 and was clamped to `outstanding' - and
         * `outstanding' by then included the chunk just written and
         * not yet played. So the kernel retired one unplayed chunk,
         * and the odelay correction is downward-only, so it never
         * re-baselined.
         *
         * MEASURED, three -R -M Acer runs (`2026-09-12-acer-62`,
         * `2026-09-13-acer-63` and `-65`): `odelay - out` sits at 0
         * or 16 before any release and at 1728-1760 after the first
         * reacquire - one drain's worth, a fixed +10 ms in flight
         * beyond the depth target from the first release cycle on.
         *
         * `played = written` SETS OUTSTANDING TO ZERO, and that is
         * the truth at this instant rather than a convenience:
         * `vsound_hard_outstanding()` is written - played, and the
         * pump only reaches this ioctl after STAT told it `nbusy ==
         * 0 && hard_ready == 0` and its close() of the card returned
         * (vsoundd.c:406-447). Nothing is in flight - there is no
         * card to hold it. Zeroing `written` and `played` separately
         * would do the same, but keeping them equal leaves the
         * lifetime totals intact for the trace.
         */
        vsound_dev.hard.played      = vsound_dev.hard.written;
        vsound_dev.hard.have_played = 0;
        vsound_dev.hard.card_played = 0;
        restore_flags(flags);
        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS
                   "vsound: pump released the card - %s,"
                   " card counter re-baselined\n",
                   jiffies, spent ? "signal consumed"
                                  : "signal KEPT: a client came and went"
                                    " during the close, its tail waits");
        return 0;
    }

    case SNDCTL_DSP_SYNC:
    case SNDCTL_DSP_POST:
        return 0;

    case SNDCTL_DSP_RESET:
        save_flags(flags);
        cli();
        vsound_chan_stop(&vsound_dev, c);
        c->b.rp = 0;
        c->b.rl = 0;
        c->wpos = 0;
        /*
         * AND THE CONVERTER'S PHASE - design/25 B9a, fixed 2026-09-16.
         *
         * Emptying the ring left z_alpha, z_prev0/1 and z_primed
         * holding the last frames read BEFORE the reset, so the first
         * output after it interpolated from the old signal: one sample
         * of the previous stream at the head of the new one, a click.
         *
         * Unheard today because lxdoom resets once at init with
         * nothing played before it (`design/11` divergence 4), so the
         * stale sample is silence - but a client that resets BETWEEN
         * sounds would hear it, and the reference resets its feeder
         * chain here.
         *
         * vsound_conv_setup() recomputes the ratio and clears all of
         * it, which is what a re-open does, so the reset now means the
         * same thing as a fresh channel.
         */
        vsound_conv_setup(c, vsound_dev.mix_rate);
        restore_flags(flags);
        return 0;

    /*
     * NON-BLOCKING I/O - review finding 14,
     * design/08-vsound-review.md. This fell through to -EINVAL, and
     * the reference handles it (dsp.c:1370).
     *
     * OURS NEEDS NO NEW STATE. The write and read paths already test
     * `file->f_flags & O_NONBLOCK' (vsound_dev.c:1122, :1413), which
     * is what open(2) sets when a client passes O_NONBLOCK. Setting
     * the same flag here makes the ioctl and the open flag one
     * mechanism rather than two.
     *
     * ONE-WAY, LIKE THE REFERENCE. SNDCTL_DSP_NONBLOCK only SETS -
     * `if (cmd == SNDCTL_DSP_NONBLOCK || *arg_i)' at dsp.c:1374, so
     * the ioctl can never clear it and only FIONBIO can. FIONBIO
     * itself is handled by the VFS before reaching us, so it needs no
     * case here.
     *
     * The reference groups them and takes no argument for the
     * SNDCTL form; neither do we.
     */
    case SNDCTL_DSP_NONBLOCK:
        file->f_flags |= O_NONBLOCK;
        return 0;

    /*
     * FULL DUPLEX - review finding 14.
     *
     * **THE REFERENCE NEVER FAILS THIS** (dsp.c:1828-1837): it swaps
     * the SIMPLEX flag if a full-duplex switch is both needed and
     * possible, and `break's regardless - so a card that cannot do it
     * returns success having done nothing.
     *
     * We are PLAYBACK ONLY. A reader gets no channel at all (see
     * vsound_dev_open: O_RDONLY means the pump), so full duplex is
     * not something we can offer or need to arrange. Following the
     * reference, that is not an error: the caller asked for a mode
     * change that is already as arranged as it is going to be.
     *
     * Handled explicitly rather than by falling through, so the trace
     * shows it was understood and answered rather than unrecognised.
     */
    case SNDCTL_DSP_SETDUPLEX:
        return 0;

    /*
     * THE MIX RATE - what every client is resampled to. vsound.h has
     * the contract and design/25 section 7b the reason it exists.
     *
     * THE PUMP SETS THIS, once, after negotiating with the real card.
     * It cannot be a module parameter: the value is not knowable at
     * insmod time, because it depends on a device that may not be
     * open yet and on what that device grants rather than what it was
     * asked.
     */
    case VSOUND_IOC_MIXRATE: {
        unsigned long flags;
        int rate;

        if (get_user(v, (int *) arg))
            return -EFAULT;

        /* 0 QUERIES. A tool reading the current rate must not have to
         * risk setting it. */
        if (v == 0)
            return put_user(vsound_dev.mix_rate, (int *) arg);

        if (v < VSOUND_RATE_MIN || v > VSOUND_RATE_MAX)
            return -EINVAL;

        /*
         * NO CHANNELS OPEN, AND THIS IS NOT A FORMALITY.
         * vsound_conv_setup() computes each channel's ratio from the
         * mix rate AT THE MOMENT ITS FORMAT IS SET. Changing the rate
         * under a running channel leaves it converting to the old one
         * - the silent wrong-pitch bug this ioctl exists to prevent,
         * moved one layer inward and harder to see.
         *
         * Under cli() because nbusy is what open and release move.
         */
        save_flags(flags);
        cli();
        if (vsound_dev.nbusy != 0) {
            restore_flags(flags);
            return -EBUSY;
        }
        vsound_dev.mix_rate = v;
        rate = vsound_dev.mix_rate;
        restore_flags(flags);

        printk(KERN_INFO VSOUND_TS "vsound: mixing at %d Hz\n",
               jiffies, rate);

        /* THE ADOPTED VALUE GOES BACK, as SNDCTL_DSP_SPEED does - the
         * caller must not have to assume it got what it asked for,
         * which is the whole lesson of this change. */
        return put_user(rate, (int *) arg);
    }

    default:
        if (vsound_trace)
            printk(KERN_DEBUG VSOUND_TS "vsound: ioctl 0x%x unhandled\n",
               jiffies, cmd);
        return -EINVAL;
    }
}

static struct file_operations vsound_dev_fops = {
    NULL,                   /* lseek    */
    vsound_dev_read,        /* read     */
    vsound_dev_write,       /* write    */
    NULL,                   /* readdir  */
    vsound_dev_poll,        /* poll     */
    vsound_dev_ioctl,       /* ioctl    */
    vsound_dsp_mmap,        /* mmap     */
    vsound_dev_open,        /* open     */
    NULL,                   /* flush    */
    vsound_dev_release,     /* release  */
    NULL,                   /* fsync    */
    NULL,                   /* fasync   */
    NULL,                   /* check_media_change */
    NULL                    /* revalidate */
};

/* --- module ---------------------------------------------------------- */

/* ------------------------------------------------------------------ *
 * /proc/vsound - WHICH MINOR WE GOT, READABLE WITHOUT A NODE          *
 * ------------------------------------------------------------------ *
 *
 * THE PROBLEM IT SOLVES, design/38 section 9f2. `MAKEDEV audio'
 * creates exactly two DSP nodes - /dev/dsp (minor 3) and /dev/dsp1
 * (19) - whatever hardware is present. An es1371 registers TWO dsp
 * minors and takes both, so on a stock machine vsound gets minor 35
 * and /dev/dsp2 DOES NOT EXIST. vsound is then loaded and
 * completely unreachable: nothing can open it, so nothing can ask it
 * anything, and userspace cannot even find out which node to make.
 *
 * PROBING CANNOT ANSWER THAT. Opening every node that DOES exist
 * tells you none of them is vsound; it does not tell you which one
 * to create. And a node can fail to open without meaning "not
 * vsound" - the esssolo1 open BLOCKS rather than returning EBUSY.
 *
 * SO THE MODULE SAYS IT, in a file that needs no node.
 *
 * NOT THE STYLE sound.o USES. soundcard.c:300 fills a static
 * proc_dir_entry whose first field is PROC_SOUND, a constant from an
 * enum in proc_fs.h - a module cannot add itself to that. 2.2.16 has
 * the dynamic API as well (proc_fs.h:435), and fs/proc/root.c:328
 * shows low_ino == 0 means "allocate one".
 *
 * AND IT CANNOT OUTLIVE THE MODULE. remove_proc_entry() in
 * cleanup_module means there is no stale file to clean up later and
 * nothing for an unload to get wrong - unlike a device node or a
 * symlink, which are real filesystem objects someone must put back.
 *
 * THE FORMAT IS key: value, ONE PER LINE, because that is what a
 * shell and a C reader both parse without a library. The dsp index
 * comes first: it
 * is the reason this file exists.
 */
#ifdef CONFIG_PROC_FS

static struct proc_dir_entry *vsound_proc_ent;

static int
vsound_proc_get_info(char *buffer, char **start, off_t offset,
                     int length, int inout)
{
    int len = 0;

    (void) start; (void) offset; (void) length; (void) inout;

    /* THE INDEX, NOT THE NAME. register_sound_dsp() returns a MINOR
     * (3, 19, 35 ...); the index userspace wants is (minor - 3) / 16,
     * and /dev/dsp is index 0 with no suffix. Publishing both saves
     * every reader from rediscovering that arithmetic. */
    len += sprintf(buffer + len, "minor: %d\n", vsound_dsp_minor);
    len += sprintf(buffer + len, "dsp: %d\n",
                   vsound_dsp_minor >= 3 ? (vsound_dsp_minor - 3) / 16 : -1);
    len += sprintf(buffer + len, "midi_slot: %d\n", vsound_midi ? 1 : 0);
    len += sprintf(buffer + len, "channels: %d\n", VSOUND_MAX_CHAN);
    return len;
}

static void
vsound_proc_start(void)
{
    vsound_proc_ent = create_proc_entry("vsound", S_IFREG | S_IRUGO, NULL);
    if (vsound_proc_ent != NULL)
        vsound_proc_ent->get_info = vsound_proc_get_info;
    /* A FAILURE IS NOT FATAL. The module works without it; only the
     * discovery in userspace gets harder, and that is a worse
     * outcome than refusing to load. */
}

static void
vsound_proc_stop(void)
{
    if (vsound_proc_ent != NULL) {
        remove_proc_entry("vsound", NULL);
        vsound_proc_ent = NULL;
    }
}

#else
static void vsound_proc_start(void) { }
static void vsound_proc_stop(void)  { }
#endif

int
init_module(void)
{
    int i;

    vsound_dev_init(&vsound_dev);

    /*
     * NOTHING TO DO FOR THE MIDI SLOT HERE. vsound_midi is read at
     * open() time - it decides whether an O_EXCL open is answered
     * with the slot or with -ENODEV - and the table is always
     * VSOUND_SLOTS long.
     */
    for (i = 0; i < VSOUND_SLOTS; i++)
        vsound_dev.chan[i].wait = NULL;

    init_timer(&vsound_timer);
    vsound_timer.function = vsound_dev_tick;
    vsound_timer.data     = 0;

    vsound_drain_init(&vsound_dev, vsound_hard_mem, VSOUND_HARD_SIZE);

    vsound_dsp_minor = register_sound_dsp(&vsound_dev_fops, -1);
    if (vsound_dsp_minor < 0) {
        printk(KERN_ERR VSOUND_TS "vsound: register_sound_dsp failed: %d\n",
               jiffies,
               vsound_dsp_minor);
        return vsound_dsp_minor;
    }

    /* AND SAY WHICH MINOR WE GOT, where a machine with no node for
     * it can still read it - see the block above. After the
     * registration, because the minor is what it publishes. */
    vsound_proc_start();

    /*
     * THE BUILD STAMP, FIRST LINE OF EVERY LOAD.
     *
     * Twice on 2026-08-27 a run tested a STALE module and the traces
     * were read as findings before anyone noticed: once because the
     * cross-build failed and left the previous object in place while
     * `make' still printed "linked" for an earlier target, and once
     * because the module was already resident and insmod used what was
     * in memory. Both cost a test run each.
     *
     * __DATE__ and __TIME__ are set by the compiler at every
     * compilation, so this cannot be forgotten, cannot be stale, and
     * needs nobody to remember to check it. Compare it against what
     * `make' printed; if they differ, the module is not the one you
     * just built.
     *
     * The workstation-side check is `strings -a vsound.o | grep built'.
     */
    printk(KERN_INFO VSOUND_TS "vsound: built %s %s\n",
               jiffies, __DATE__, __TIME__);

    /* THE RESERVED SLOT GOES IN THE BANNER, for the same reason the
     * write deadline does: it changes what a run MEANS. A capture
     * showing four clients where a fifth was refused reads very
     * differently depending on whether a synth had its own slot. */
    printk(KERN_INFO VSOUND_TS "vsound: dsp minor %d, %d channels%s,"
                     " mixing at %d Hz %d ch\n",
               jiffies,
           vsound_dsp_minor, VSOUND_MAX_CHAN,
           vsound_midi ? " + 1 reserved for MIDI (last slot, O_EXCL)" : "",
           vsound_dev.mix_rate, vsound_dev.mix_channels);
    printk(KERN_INFO VSOUND_TS "vsound: hard buffer %d bytes, depth %s\n",
               jiffies,
           VSOUND_HARD_SIZE,
           vsound_depth > 0 ? "capped" : "uncapped (0 = auto)");
    /* THE WRITE DEADLINE, IN THE CAPTURE. It decides how long i_sem is
     * held, which is the whole of the 2026-08-31 serialisation bug, so
     * a run's trace must say which value produced it. */
    /* OUT OF RANGE FALLS BACK, and says so - insmod takes any int. */
    if (vsound_limit < 0 || vsound_limit > 2) {
        printk(KERN_WARNING VSOUND_TS "vsound: vsound_limit=%d is not 0-2"
               " - using 1 (attack/release)\n", jiffies, vsound_limit);
        vsound_limit = 1;
    }
    if (vsound_atten < 1 || vsound_atten > 100) {
        printk(KERN_WARNING VSOUND_TS "vsound: vsound_atten=%d is not 1-100"
               " - using 100 (none)\n", jiffies, vsound_atten);
        vsound_atten = 100;
    }
    printk(KERN_INFO VSOUND_TS "vsound: limiter %s, attenuation %d%%,"
           " boost up to %d%%\n", jiffies,
           vsound_limit == 0 ? "OFF" : vsound_limit == 2 ? "soft knee"
                                                         : "attack/release",
           vsound_atten, VSOUND_VOL_BOOST);
    printk(KERN_INFO VSOUND_TS "vsound: write sleep %d ms%s\n",
               jiffies, vsound_write_ms,
           vsound_write_ms <= 0 ? " (never sleep - short writes only)" : "");
    /* In the capture, so a thin trace is never mistaken for a quiet
     * run. Whoever reads the log six weeks from now cannot ask. */
    if (vsound_trace)
        printk(KERN_INFO VSOUND_TS "vsound: trace on, every %d%s per-fragment"
                         " line (lifecycle always)\n",
               jiffies,
               vsound_ratelimit,
               vsound_ratelimit == 1 ? "" : "th");
    return 0;
}

void
cleanup_module(void)
{
    unsigned long flags;

    save_flags(flags);
    cli();
    if (vsound_timer_armed) {
        del_timer(&vsound_timer);
        vsound_timer_armed = 0;
    }
    restore_flags(flags);

    vsound_proc_stop();

    if (vsound_dsp_minor >= 0)
        unregister_sound_dsp(vsound_dsp_minor);

    /* HOW OFTEN THE LIMITER ACTED, so a run's log says whether it was
     * ever needed - 0 is a mix that never reached full scale. */
    printk(KERN_INFO VSOUND_TS "vsound: unloaded - the limiter acted in"
           " %lu mix passes\n", jiffies, vsound_mix_limit_passes);
}

/*
 * "Dual BSD/GPL", AS vmidi_mod.c SAYS - 2026-10-02, the user. This
 * said "GPL", against the BSD-3 licence the project ships under
 * (design/31 A4). The 2.2.16 headers do not define MODULE_LICENSE, so
 * on this kernel the line is not compiled at all; it matters only to a
 * later kernel, where "Dual BSD/GPL" is the BSD-compatible tag that
 * does not taint.
 */
#ifdef MODULE_LICENSE
MODULE_LICENSE("Dual BSD/GPL");
#endif
