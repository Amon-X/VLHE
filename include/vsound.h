/*
 * vsound.h - the vsound/vsoundd interface.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Shared by the kernel module and the daemon, so it is the one place
 * the wire format is defined. design/07-vsound.md section 4.1.
 *
 * C89, and no long long in anything that crosses the boundary: the
 * module is built by GCC 2.95.2 and the daemon may not be.
 */

#ifndef _VSOUND_H
#define _VSOUND_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <sys/types.h>
#ifndef __u32
#define __u32 unsigned int
#define __s32 int
#endif
#endif

/*
 * THE ACK. Two monotonic byte counters plus two informational fields.
 *
 * The pump sends one after every write to the card. There is no handle,
 * no status, and nothing per-fragment: the device has one format and
 * the buffer is shared, so there is nothing per-fragment to describe.
 * That is the difference from the old tree's protocol, where unmixed
 * audio crossed the boundary and each piece needed its own metadata.
 */
struct vsound_ack {
    __u32   played;         /* SNDCTL_DSP_GETOPTR cinfo.bytes, verbatim */
    __u32   written;        /* bytes vsoundd has written to the card -
                             * sent, but NOT READ by the kernel
                             * (design/25 I7); kept so the struct's
                             * layout does not change            */
    __u32   odelay;         /* SNDCTL_DSP_GETODELAY - queued, unplayed  */
    __u32   card_frag;      /* GETOSPACE fragsize; NOT the total        */
    __u32   flags;          /* VSOUND_ACK_* below                       */
};

/* The card answered GETOPTR / GETODELAY. Without these the kernel
 * cannot tell "the card reports zero" from "the card does not report",
 * and the two need different handling: the first is a real measurement,
 * the second means falling back to the timer. */
#define VSOUND_ACK_PLAYED   0x0001
#define VSOUND_ACK_ODELAY   0x0002

#define VSOUND_IOC_MAGIC    'V'

/* Report what the card did. The whole protocol, in one call. */
#define VSOUND_IOC_ACK      _IOW(VSOUND_IOC_MAGIC, 1, struct vsound_ack)

/*
 * What the kernel thinks is going on, for the daemon's diagnostics and
 * for `vsoundd -s'. Read-only: nothing here changes behaviour.
 */
struct vsound_stat {
    __u32   nbusy;          /* channels open                            */
    __u32   nrunning;       /* channels feeding the mix                 */
    __u32   outstanding;    /* handed out, not yet reported played      */
    __u32   depth;          /* the cap on outstanding - the latency knob */
    __u32   hard_ready;     /* bytes mixed and waiting for the pump     */
    __u32   hard_size;      /* the hard buffer's size                   */
    __u32   underruns;      /* times the pump asked and we had nothing  */
    /*
     * MIX PASSES IN WHICH A SAMPLE SATURATED - the headroom measure.
     *
     * Takes the `reserved' slot rather than growing the struct, so the
     * ioctl's size and therefore its _IOR number are unchanged and an
     * older tool still works.
     *
     * PASSES, not samples: the sample count grows at 44100 a second
     * per channel and reads as a large number for a negligible amount
     * of clipping. Passes are comparable between runs. The sample
     * total is `vsound_mix_clipped' in the module for anyone reading
     * a trace.
     */
    __u32   clip_passes;
};

#define VSOUND_IOC_STAT     _IOR(VSOUND_IOC_MAGIC, 2, struct vsound_stat)

/*
 * PER-CHANNEL VOLUME - design/07-vsound.md section 3.14.
 *
 * OSS cannot express this: its 25 mixer controls all name physical
 * signal paths and none of them means "channel 2", so /dev/mixer stays
 * the real card's and this is ours.
 */
#define VSOUND_VOL_MAX      100     /* 0..100, like every OSS control  */

/*
 * THE BOOST CEILING - 2026-10-02. A channel's volume may go ABOVE
 * VSOUND_VOL_MAX, which stays UNITY (100 = as the program sent it), up
 * to this many per cent. The user: 200, adjustable at build time -
 * `make VOL_BOOST=150' passes -DVSOUND_VOL_BOOST to the modules and
 * the tools alike (defs.mk). design/09's BOOST section: lxdoom can
 * only ever hand us half scale, so it needs 2x to match the rest.
 */
#ifndef VSOUND_VOL_BOOST
#define VSOUND_VOL_BOOST    200
#endif

/* How many characters of the owning process's name we keep. 2.2's
 * task_struct has comm[16], so there is no point storing more. */
#define VSOUND_COMM_LEN     16

/*
 * One channel, as a control tool sees it.
 *
 * `pid' and `comm' are the IDENTITY. A slider labelled "quake" needs
 * the second; the first is what makes an index safe to write to across
 * a poll interval, since indices are reused when a client exits and
 * another opens.
 */
struct vsound_chaninfo {
    __u32   index;
    __u32   pid;                        /* 0 when the slot is free     */
    char    comm[VSOUND_COMM_LEN];
    __u32   flags;                      /* VSOUND_CI_* below           */
    __u32   rate;
    __u32   channels;
    __u32   format;
    __u32   vol;                        /* 0..VSOUND_VOL_MAX           */
    /*
     * WHAT THIS CHANNEL HAS ACTUALLY CONTRIBUTED, cumulative for its
     * lifetime, in bytes of mixed output.
     *
     * IT IS HERE BECAUSE `VSOUND_CI_RUNNING' DOES NOT MEAN "MAKING
     * SOUND". That flag is set when a write BLOCKS FOR ROOM or
     * acquires a chunk (vsound_dev.c:1409 and :1607), so a client
     * whose writes always fit in the free space never sets it - and a
     * control tool drew a synth that was audibly playing as "idle".
     * Found on 86Box 2026-09-18: sndserv showed idle while producing
     * sound.
     *
     * A TOOL COMPARES IT ACROSS TWO POLLS. Moved means producing;
     * unchanged means genuinely quiet. The kernel does not decide
     * what "active" means - it reports the count and lets the caller
     * pick its own interval, which is what makes this honest at any
     * poll rate.
     */
    __u32   mixed;
};

#define VSOUND_CI_BUSY      0x0001
#define VSOUND_CI_RUNNING   0x0002
#define VSOUND_CI_MMAP      0x0004
#define VSOUND_CI_MIDI      0x0008      /* the reserved MIDI slot      */
/* MUTED IS REPORTED SEPARATELY FROM `vol', WHICH KEEPS ITS VALUE. A
 * muted channel reads back the level the user set, with this bit on -
 * so a control tool draws the slider where it belongs and lights the
 * mute, rather than showing 0 and having to remember the real one
 * somewhere that outlives it. See VSOUND_CHN_MUTED in vsound_chan.h. */
#define VSOUND_CI_MUTED     0x0010

/*
 * Everything a tool needs for one refresh, in one call.
 *
 * THE GENERATION COUNTER IS THE POINT. It is bumped on a volume
 * change, on open AND on close, so a poll is one ioctl and one integer
 * comparison - no redraw, no formatting, no X traffic unless something
 * actually moved. That is what makes polling at 250 ms cheap enough not
 * to argue about.
 *
 * Covering open and close matters as much as volume: if a client exits
 * while a tool is showing it, the slider has to disappear, and that is
 * the same staleness problem wearing a different hat.
 */
/*
 * ROOM FOR EVERY SLOT: the VSOUND_MAX_CHAN ordinary channels plus the
 * reserved MIDI one, which is always last. `nchan' says how many are
 * real on this machine - VSOUND_MAX_CHAN, or one more when vsound was
 * loaded with vsound_midi=1 - and a tool reads that, never this
 * constant. The MIDI entry carries VSOUND_CI_MIDI whether or not it
 * is held, so a tool can label it "reserved for MIDI" while free.
 *
 * Kept here as well as VSOUND_SLOTS in vsound_chan.h for the reason
 * VSOUND_VOL_MAX is; vsound_dev.c checks the two agree at compile
 * time.
 *
 * GROWING THIS FROM 4 CHANGED THE _IOR NUMBER, deliberately, on
 * 2026-09-07: the size is encoded in the ioctl, so a tool built
 * against the old header gets -EINVAL rather than an overrun. Every
 * tool ships with the module, so nothing built against 4 is in use.
 */
#define VSOUND_LIST_CHAN    5

struct vsound_chanlist {
    __u32   generation;
    __u32   nchan;                      /* how many entries follow     */
    struct vsound_chaninfo chan[VSOUND_LIST_CHAN];
};

#define VSOUND_IOC_CHANS    _IOR(VSOUND_IOC_MAGIC, 3, struct vsound_chanlist)

/*
 * Set one channel's volume.
 *
 * `pid` IS CHECKED, and that is not belt-and-braces. Indices are
 * reusable: a tool holding "index 1 = lxdoom" from a poll 200 ms ago
 * would otherwise set QUAKE's volume after doom exited and quake took
 * the slot. Verifying and setting under one cli() closes the window
 * rather than narrowing it.
 *
 * Pass pid 0 to mean "whatever is there", which is only correct for a
 * tool that has just read the list and is not holding it across
 * anything.
 *
 *   -EINVAL   index out of range
 *   -ENODEV   that channel is not open
 *   -ESRCH    it is open, but not by that pid - somebody else's stream
 */
struct vsound_vol {
    __u32   index;
    __u32   pid;                        /* 0 = do not check            */
    __u32   vol;                        /* 0..VSOUND_VOL_MAX           */
    __u32   reserved;
};

#define VSOUND_IOC_VOL      _IOW(VSOUND_IOC_MAGIC, 4, struct vsound_vol)

/*
 * RELEASE-ON-IDLE - design/16-hanging-note.md section 4, Design B.
 *
 * The pump declares that it WANTS to be told when the last client
 * closes, so it can release the real card and let the driver's own
 * teardown run (drain_dac, stop_dac, the DMA disable). On esssolo1 and
 * four other 2.2 PCI drivers that is the only thing that stops the DAC
 * replaying its final fragment.
 *
 * WHY IT IS OPT-IN AND NOT ALWAYS ON. Being told means read() returns
 * 0 on the idle transition, and a zero-length read conventionally
 * means EOF. A pump that did not ask must never see one - so the
 * kernel stays silent unless this is set, and the semantics of the
 * device are unchanged for everything else.
 *
 * Argument: 1 to arm, 0 to disarm. The flag is cleared automatically
 * whenever a client feeds a channel again, so one arming covers the
 * whole session.
 *
 * -EBUSY, since 2026-09-14, if another reader is already the pump.
 * The module allows ONE pump: the first reader to read() or to send
 * this claims the role until it closes, and every other reader gets
 * -EBUSY from read() and from here. Two `-R' pumps ran side by side
 * on the Acer that day, split the mixed stream between them, and left
 * one believing it was armed after the other died. vsoundd exits on
 * it - a second pump has nothing useful to do.
 */
#define VSOUND_IOC_RELEASE  _IOW(VSOUND_IOC_MAGIC, 5, int)

/*
 * THE PUMP HAS RELEASED THE CARD - the signal is spent.
 *
 * Added 2026-09-13 for the 100% CPU spin: `release_idle' is a LEVEL,
 * and the only thing that cleared it was a client starting. With no
 * client left there is nothing to start, so read() returned 0 forever
 * and the pump's loop had nothing to pace it. Measured on the Acer,
 * tests/logs/2026-09-13-acer-64-vsoundd-spin-isolated-to-vmidid-release.
 *
 * WHY THE PUMP CONSUMES IT AND NOT THE READ. The kernel cannot know
 * whether the pump will actually release: vsoundd cross-checks
 * VSOUND_IOC_STAT first and declines if nbusy or hard_ready are
 * non-zero, because releasing then would truncate audio. Clearing in
 * read() would spend the signal on a release that did not happen, and
 * nothing would raise it again - a hanging note, which is the defect
 * this whole mechanism exists to prevent.
 *
 * So the level STAYS SET until the pump says it acted. A refused
 * cross-check reads 0 again and retries, which is self-healing rather
 * than dependent on a re-arm - and since 2026-09-14 it does so after a
 * sleep, not at once: vsound_dev_read() tests the level only after its
 * schedule_timeout() - 10 ms since b87d728 (2026-09-30), 50 ms before.
 * Before 2026-09-14 the test sat above the sleep, this comment
 * claimed the 50 ms anyway, and a refusal spun a core for as long as
 * it lasted (design/25 B2).
 *
 * AND SAYING IT ACTED DOES NOT ALWAYS SPEND IT - since 2026-10-04
 * (design/25 B7, design/54 D31). The pump's close of the card blocks
 * 83-160 ms; a client that opened, played and closed inside it raised
 * the level again for its own tail. The kernel now spends the signal
 * only if no client is open and the hard ring is empty - the state the
 * pump released on - and otherwise keeps it, so the pump reads that
 * tail, reopens, and releases a second time. vsound_release_consume().
 *
 * THE FLAGS DIE WITH THE PUMP, also since 2026-09-14. Closing the
 * reader that armed them disarms and drops any pending signal, and
 * arming drops a stale one, so a pump restarted after a `-R' pump
 * died - with or without -R - starts clean (design/25 B3).
 *
 * WHY NOT VSOUND_IOC_RELEASE WITH on=0 THEN on=1. That does clear the
 * flag (the !on branch), but it disarms in between - and a client
 * opening in that window would have its idle transition missed. Two
 * syscalls and a race, against one syscall and none.
 *
 * Takes no argument: it reports an event, it does not set a mode.
 */
#define VSOUND_IOC_RELEASED _IO(VSOUND_IOC_MAGIC, 6)

/*
 * THE MIX RATE - what every client is resampled TO.
 *
 * WHY THIS EXISTS. It was a compile-time constant, and the card's own
 * answer was thrown away: vsoundd asked for VSOUND_RATE, read back
 * what the card actually gave, printed a warning and carried on using
 * the constant. On a card that cannot do 44100 everything played at
 * the wrong pitch and speed WITH NO ERROR ANYWHERE - the writes
 * succeed, the byte counts are right, the trace looks normal.
 *
 * NO 2.2 DRIVER RESAMPLES. They clamp to what the hardware can do and
 * return the truth, which is the OSS contract: SB Pro caps at 22050
 * IN STEREO (44100 mono only, sb_audio.c:557), SB 1.0 at 23000, and
 * esssolo1 picks the nearer of two crystal divisors and reports what
 * it landed on. Resampling is userspace's job - ours.
 *
 * design/25 section 7b has the driver survey, FreeBSD's vchanrate as
 * the model, and why emu10k1 is the exception (it resamples in
 * hardware, so it never refuses a rate and the shipped driver echoes
 * the request back unchanged).
 *
 * WHO SETS IT: the pump, once, after it has negotiated with the card
 * and knows what it really got. Not a module parameter, because the
 * value is not knowable at insmod time - it depends on a device that
 * may not even be open yet.
 *
 * ONLY WITH NO CHANNELS OPEN, and that is not a formality.
 * vsound_conv_setup() computes each channel's resampling ratio from
 * the mix rate AT THE MOMENT ITS FORMAT IS SET; changing the rate
 * underneath a running channel would leave it converting to the old
 * one, which is the silent-wrong-pitch bug this ioctl exists to
 * prevent, moved one layer in. So:
 *
 *   -EBUSY    a channel is open - set it before clients arrive
 *   -EINVAL   outside VSOUND_RATE_MIN..VSOUND_RATE_MAX
 *
 * IT RETURNS THE VALUE ACTUALLY ADOPTED, like SNDCTL_DSP_SPEED and
 * for the same reason: the caller must not have to assume. Passing 0
 * queries without changing anything, which is how a tool reads the
 * current rate.
 */
#define VSOUND_IOC_MIXRATE  _IOWR(VSOUND_IOC_MAGIC, 7, int)

/*
 * MUTE ONE CHANNEL - a state of its own, NOT a volume of zero.
 *
 * Same shape as VSOUND_IOC_VOL and the same stale-slot protection:
 * `pid' is verified against the slot under one cli(), so a tool acting
 * on a list read 200 ms ago cannot mute whoever inherited the index.
 * pid 0 means "whatever is there".
 *
 * `vol' is NOT touched. Muting and unmuting leave the level exactly as
 * the user set it, and the channel reports it throughout with
 * VSOUND_CI_MUTED set - so nothing in userspace has to shadow a value
 * and keep it alive. vsound_chan.h's VSOUND_CHN_MUTED has the full
 * reasoning and what the alternatives cost.
 *
 * Returns:
 *   0         set
 *   -EINVAL   index out of range
 *   -ENODEV   that slot is not open
 *   -ESRCH    it is open, but not by that pid - somebody else's stream
 */
struct vsound_mute {
    __u32   index;
    __u32   pid;                        /* 0 = do not check            */
    __u32   muted;                      /* 0 or 1                      */
    __u32   reserved;
};

#define VSOUND_IOC_MUTE     _IOW(VSOUND_IOC_MAGIC, 8, struct vsound_mute)

/*
 * PER-PROGRAM LEVELS - design/33 section 1c, built 2026-10-02.
 *
 * THE MODULE REMEMBERS EACH PROGRAM'S LEVEL BY NAME. Whenever a
 * channel's volume or mute is set (VSOUND_IOC_VOL, VSOUND_IOC_MUTE),
 * the level is recorded against that channel's `comm'; when a program
 * of that name opens a channel, the remembered level is applied
 * before its first sample. So Quake's level survives its track
 * changes - each one is a close and a reopen, which reset it to unity
 * until this - the way a real drive's CD volume does.
 *
 * A level at unity and unmuted is not stored: it is what a program
 * gets anyway. The table holds VSOUND_PROG_MAX names; when full, the
 * one set longest ago gives way.
 *
 * GET reads the table and SET replaces it whole - which is how VLHE
 * keeps it across a reload or a reboot (Save Program Levels: saved at
 * unload, pushed back at load, a file per machine).
 */
#define VSOUND_PROG_MAX     16
#define VSOUND_PROG_MUTED   0x0001

struct vsound_prog {
    char    comm[VSOUND_COMM_LEN];      /* NUL-terminated              */
    __u32   vol;                        /* 0..VSOUND_VOL_BOOST          */
    __u32   flags;                      /* VSOUND_PROG_*               */
};

struct vsound_proglist {
    __u32   nprog;                      /* entries used, from the start */
    __u32   reserved;
    struct vsound_prog prog[VSOUND_PROG_MAX];
};

#define VSOUND_IOC_PROGGET  _IOR(VSOUND_IOC_MAGIC, 9, struct vsound_proglist)
#define VSOUND_IOC_PROGSET  _IOW(VSOUND_IOC_MAGIC, 10, struct vsound_proglist)

#endif /* _VSOUND_H */
