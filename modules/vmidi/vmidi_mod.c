/*
 * vmidi_mod.c - a MIDI device that a userspace synth reads.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/p3-midi-plan.md has the whole design. In short:
 *
 *     lxdoom --pipe(MUS)--> musserv -m -u N --/dev/sequencer--> HERE
 *                                                                |
 *                                                       /dev/vmidi (read)
 *                                                                |
 *                                                        the synth, userspace
 *                                                                |
 *                                                    /dev/dsp -> vsound -> card
 *
 * WHY A MIDI DEVICE AND NOT A SYNTH. `musserv' finds three kinds of
 * destination: AWE (SNDCTL_SYNTH_INFO -> SAMPLE_TYPE_AWE32), FM
 * (SYNTH_TYPE_FM), and MIDI (SNDCTL_MIDI_INFO, counted by
 * SNDCTL_SEQ_NRMIDIS). The first two require a SYNTH device, which
 * means implementing synthesis in kernel space. The third needs only
 * somewhere to send bytes.
 *
 * AND `struct midi_operations' CARRIES NO DMA - open, close, ioctl,
 * outputc, start_read, end_read, kick, command, buffer_status,
 * prefix_cmd (dev_table.h). That is what makes this viable where an
 * AUDIO-side registration is not: sound_install_audiodrv() would bring
 * DMAbuf's machinery, which assumes an ISA DMA engine underneath
 * (get_dma_residue, disable_dma) that we do not have. The same reason
 * vsound registers with register_sound_dsp - see vsound_dev.c:38.
 *
 * MODELLED ON emu10k1's SEQUENCER MIDI DEVICE, which is the closest
 * working example in reach: emu10k1-v0.20a/main.c:448-471 for the
 * registration, midi.c for the callbacks. Its shape is followed
 * deliberately, including the midi_devs[dev] guard at the top of every
 * callback and kick() being a no-op.
 *
 * THE SEQUENCER DOES THE HARD PART. midi_devs[]->converter is set to
 * &std_midi_synth, the kernel's generic converter, so note events
 * arrive here ALREADY SERIALISED INTO MIDI BYTES. This module never
 * decodes a note-on; it receives a byte stream and hands it on.
 *
 * Linux 2.2.16-usb, uniprocessor, MODVERSIONS, GCC 2.95.2, C89.
 */

#ifndef __KERNEL__
#error "vmidi_mod.c is kernel-only"
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/malloc.h>
#include <linux/poll.h>
#include <linux/soundcard.h>
#include <linux/sound.h>
#include <linux/proc_fs.h>     /* create_proc_entry - /proc/vmidi */
#include <asm/uaccess.h>

#include "sound_config.h"
#include "dev_table.h"
/*
 * midi_synth.h DEFINES std_midi_synth rather than declaring it - it is
 * a `static struct synth_operations' at midi_synth.h:23 - so it may be
 * included exactly ONCE per module. emu10k1 does the same
 * (emu10k1-v0.20a/main.c:113) and for the same reason.
 *
 * AND THE TWO MACROS MUST BE DEFINED FIRST: the header initialises
 * `std_synth_info' with them at :21 and does not supply defaults.
 * emu10k1 sets them immediately before its own include (main.c:109).
 *
 * NO SYNTH_CAP_INPUT. emu10k1 declares input because its MPU-401 can
 * receive from a physical port; a software synth has nothing to send
 * back, and claiming otherwise would advertise a capability that does
 * not exist.
 *
 * LOAD DEPENDENCY: sound.o MUST BE RESIDENT BEFORE THIS MODULE.
 * std_midi_synth's function pointers are the kernel's midi_synth_*
 * routines (midi_synth_start_note, _send_sysex, ...), and this module
 * also calls sound_alloc_mididev/sound_unload_mididev - 19 imports in
 * all, every one exported by the OSS core sound.o. If it is not
 * loaded, insmod fails with "unresolved symbol
 * midi_synth_send_sysex_R..." and vmidi never registers.
 *
 * sound.o is installed with the kernel (/lib/modules/.../sound.o), NOT
 * carried in a test bundle, so it comes in via `modprobe sound' or is
 * held resident by any synth driver - on the Acer, opl3 from
 * /etc/modules at boot. A `rmmod opl3' drops it to use-count 0 and it
 * goes; the next insmod of this module then fails. See
 * design/09-cleanup-step.md (the installer owns this, not load.sh) and
 * the click/release note in design/16.
 */
#define MIDI_SYNTH_NAME "vmidi"
#define MIDI_SYNTH_CAPS 0
#include "midi_synth.h"

/* --- parameters ----------------------------------------------------- */

int vmidi_trace;
MODULE_PARM(vmidi_trace, "i");
MODULE_PARM_DESC(vmidi_trace, "trace to the kernel log (0/1). "
                              "Lifecycle events and errors are always "
                              "traced when this is on.");

/*
 * ONE KNOB PER CATEGORY, EACH WITH ITS OWN COUNTER.
 *
 * Modelled on vsound, which arrived at this the expensive way. From
 * vsound_chan.h's macro header: "ONE COUNTER PER SITE, so a quiet site
 * is not silenced by a busy one" - a single shared counter makes a
 * rarely-hit site invisible whenever a busy one is running, because
 * the busy site advances the counter past every multiple.
 *
 * And from vsound_dev.c, the failure that produced the split: on
 * 2026-08-27 a run that needed one category's lines "also produced
 * 4 MB of another's", 48000 lines where a few hundred held the answer.
 *
 * BOTH DEFAULT TO 0. These are the high-frequency sites - `outputc'
 * fires once per MIDI byte, `buffer_status' and `kick' once per event
 * - and vsound_rate_write sets the precedent: a site that can generate
 * a hundred lines a second defaults to off, so it never contaminates a
 * capture taken for some other reason. Turn one on deliberately.
 *
 * 0 = off, 1 = every event, N = every Nth.
 */
int vmidi_rate_byte;            /* bytes in/out; 0 = off */
MODULE_PARM(vmidi_rate_byte, "i");
MODULE_PARM_DESC(vmidi_rate_byte,
                 "trace every Nth MIDI byte (0 = off, 1 = all)");

int vmidi_rate_status;          /* buffer_status and kick; 0 = off */
MODULE_PARM(vmidi_rate_status, "i");

/*
 * THE CHARACTER DEVICE'S MINOR, AND WHY IT IS NOT 15 ANY MORE.
 *
 * /dev/vmidi was registered at sound minor 15 from 2026-09-02 to
 * 2026-09-07, copied from esssolo1.c:2251 - "15 is what esssolo1 uses
 * for /dev/dmfm, the same 'this is ours' slot". That is exactly
 * backwards: esssolo1 REGISTERS 15 for its direct-FM device, so on
 * the one machine with a Solo-1 - the Acer, the machine vmidi is for -
 * whichever module loads second fails. Seven runs on 2026-09-07, all
 * "solo1: cannot register misc device", every one traced to this
 * (tests/logs/2026-09-07-acer-vmidi-M-minor15-collision). It never
 * showed on 86Box (sb) or the P3 (emu10k1) because neither driver
 * claims a special minor at all.
 *
 * cmpci and sonicvibes take 15 the same way, all three with the
 * driver author's own "??" comment beside the number, unchanged into
 * 2.4.37. The OSS core holds 1, 6 and 8. Nothing in the 2.2.16 tree,
 * the 2.4.37 sound tree, either emu10k1, or usbmidi claims 10-14, and
 * neither Documentation/devices.txt nor sound_config.h names them.
 * 11 is the default: not 10, which is commercial OSS's /dev/dmfm and
 * the number those three drivers were probably reaching for.
 *
 * A PARAMETER RATHER THAN A CONSTANT so that load.sh can pass the same
 * number it uses for mknod - one source of truth, in the machine's
 * .conf (VMIDI_MINOR) or load.sh's default. The banner prints what
 * was actually registered.
 */
#define VMIDI_MINOR_DEFAULT 11
static int vmidi_minor = VMIDI_MINOR_DEFAULT;
MODULE_PARM(vmidi_minor, "i");
MODULE_PARM_DESC(vmidi_minor, "sound minor for /dev/vmidi (default 11; "
                 "15 collides with esssolo1/cmpci/sonicvibes)");
MODULE_PARM_DESC(vmidi_rate_status,
                 "trace every Nth pacing call (0 = off, 1 = all)");

/*
 * ONE COUNTER PER SITE. Deliberately not under cli(): a missed
 * increment costs one trace line, and taking a lock for a diagnostic
 * would change the timing being diagnosed (vsound_chan.h says the
 * same).
 */
static unsigned long vmidi_rl_in;
static unsigned long vmidi_rl_out;
static unsigned long vmidi_rl_read;
static unsigned long vmidi_rl_status;
static unsigned long vmidi_rl_kick;

#define VMIDI_RL(counter, rate)                                         \
    (vmidi_trace && (rate) > 0 && ((counter)++ % (rate)) == 0)

/*
 * TIMESTAMP EVERY LINE.
 *
 * printk carries no time on 2.2, so a long capture has NO TIME AXIS:
 * two lines a minute apart look exactly like two in the same
 * microsecond. vsound_chan.h records what that cost - on 2026-08-27 an
 * oops was read as following `vdisc: unloaded' when the two were
 * separated by however long it took a person to type a command.
 *
 * jiffies at HZ=100 is centisecond resolution, ample for that.
 */
#define VMIDI_TS   "[%lu] "

/*
 * THE RING BETWEEN THE SEQUENCER AND USERSPACE.
 *
 * 4096 is deliberate and not arbitrary: MIDI is 31250 baud, so 4096
 * bytes is over a second of the densest stream anything can send. A
 * synth that cannot keep up with that has a problem no buffer size
 * fixes, and every byte here is latency between a note being sent and
 * being heard.
 */
#define VMIDI_BUFSZ     4096

static unsigned char vmidi_buf[VMIDI_BUFSZ];
static int vmidi_head;                  /* where outputc writes  */
static int vmidi_tail;                  /* where read() consumes */
static int vmidi_count;

static struct wait_queue *vmidi_wait;

static int vmidi_seq_dev = -1;          /* our midi_devs[] index */
static int vmidi_char_dev = -1;         /* our /dev/vmidi minor  */
static int vmidi_opened;                /* the sequencer has us  */
static int vmidi_reader;                /* a synth is attached   */

static unsigned long vmidi_bytes_in;
static unsigned long vmidi_bytes_out;
static unsigned long vmidi_dropped;

/* --- the ring ------------------------------------------------------- */

/*
 * cli() around both ends. The sequencer can call outputc from a
 * timer, and read() runs in process context, so this is the one place
 * the two meet. Uniprocessor, so disabling interrupts is sufficient
 * and a spinlock would be theatre.
 */

/*
 * Returns 1 if the byte was queued, 0 if the ring was full.
 * `outputc' passes that straight back to the sequencer - see
 * vmidi_seq_out for why the distinction has to survive.
 */
static int
vmidi_put(unsigned char b)
{
    unsigned long flags;
    int queued = 1;

    save_flags(flags);
    cli();
    if (vmidi_count < VMIDI_BUFSZ) {
        vmidi_buf[vmidi_head] = b;
        vmidi_head = (vmidi_head + 1) % VMIDI_BUFSZ;
        vmidi_count++;
        vmidi_bytes_in++;
        if (VMIDI_RL(vmidi_rl_in, vmidi_rate_byte))
            printk(KERN_DEBUG VMIDI_TS "vmidi: in 0x%02x (#%lu, %d queued,"
                              " reader %d)\n",
                   jiffies, (unsigned) b, vmidi_bytes_in, vmidi_count,
                   vmidi_reader);
    } else {
        /*
         * DROPPED, AND COUNTED. Silently discarding MIDI produces
         * stuck notes - a note-on whose note-off was dropped plays
         * forever - so this is the one thing that must never be
         * invisible. The counter is reported at unload.
         */
        vmidi_dropped++;
        /* THE FIRST FEW DROPS ARE ALWAYS PRINTED, whatever the rate.
         * A dropped note-off is a stuck note, so the transition from
         * "flowing" to "full" is the single most important moment in
         * this module's life and must not be rate-limited away. */
        if (vmidi_trace && vmidi_dropped <= 4)
            printk(KERN_DEBUG VMIDI_TS "vmidi: DROPPED 0x%02x - ring full at %d,"
                              " reader %d (drop %lu)\n",
                              jiffies,
                   (unsigned) b, vmidi_count, vmidi_reader, vmidi_dropped);
        queued = 0;
    }
    restore_flags(flags);

    wake_up_interruptible(&vmidi_wait);
    return queued;
}

/* --- the sequencer side: struct midi_operations --------------------- */

/*
 * Every callback guards midi_devs[dev] exactly as emu10k1's do
 * (midi.c). The sequencer indexes this array itself, and a stale index
 * after unload would otherwise be a NULL dereference in kernel space.
 */

static int
vmidi_seq_open(int dev, int mode,
               void (*input)(int dev, unsigned char data),
               void (*output)(int dev))
{
    (void) mode; (void) input; (void) output;

    /* BOTH REFUSALS ARE ALWAYS TRACED. A caller that is turned away
     * here still holds an open /dev/sequencer, so the failure surfaces
     * later and somewhere else - which is precisely the shape of the
     * bug being hunted. Silence here costs a whole run. */
    if (dev < 0 || dev >= MAX_MIDI_DEV || midi_devs[dev] == NULL) {
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: seq open REFUSED dev %d (-EINVAL)\n",
            jiffies,
                   dev);
        return -EINVAL;
    }

    if (vmidi_opened) {
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: seq open REFUSED dev %d (-EBUSY,"
                              " already open)\n", jiffies, dev);
        return -EBUSY;
    }

    vmidi_opened = 1;
    MOD_INC_USE_COUNT;

    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: sequencer opened dev %d%s\n",
               jiffies, dev,
               vmidi_reader ? "" : " (NO READER - bytes will be dropped)");
    return 0;
}

static void
vmidi_seq_close(int dev)
{
    /*
     * A BAD dev MUST NOT SKIP THE TEARDOWN.
     *
     * This used to `return' here, which stepped over BOTH
     * vmidi_opened = 0 and MOD_DEC_USE_COUNT below. The first
     * consequence was already noted - the next open gets -EBUSY
     * forever - but the second is worse and was not: the use count
     * never comes back down, so rmmod returns "Device or resource
     * busy" PERMANENTLY. No sequence of opens and closes recovers it.
     * On a machine that is re-staged by carrying a CF card, that is a
     * reboot.
     *
     * So the report happens, and then the teardown happens anyway.
     * There is nothing in it that needs a valid dev: both are module
     * state, not device state.
     *
     * NOT REACHABLE TODAY, and the check is kept for the same reason
     * usb-midi keeps its equivalents - the caller cannot pass a bad
     * index (seq_reset loops i < max_mididev, and max_mididev =
     * num_midis at sequencer.c:1022), and a NULL midi_devs[dev] would
     * have oopsed one line earlier at sequencer.c:1208 where the
     * pointer is dereferenced to reach us. The guard defends against
     * something that has already crashed. That is precisely why it
     * must not itself leak.
     */
    if (dev < 0 || dev >= MAX_MIDI_DEV || midi_devs[dev] == NULL) {
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: seq close BAD dev %d - releasing"
                              " module state anyway (was open: %d)\n",
                              jiffies,
                   dev, vmidi_opened);
        /*
         * GUARDED ON vmidi_opened, so this cannot UNDER-count either.
         * The increment is at :265, paired with vmidi_opened = 1 one
         * line above it, so the flag is an exact record of whether we
         * are holding a reference. A close arriving without an open -
         * which should be impossible - then decrements nothing rather
         * than driving the count negative.
         */
        if (vmidi_opened) {
            vmidi_opened = 0;
            MOD_DEC_USE_COUNT;
        }
        return;
    }

    /* Counters on the close line, so one message says how the whole
     * session went without needing the per-byte trace on. */
    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: seq closing dev %d - %lu in, %lu out,"
                          " %lu dropped, %d still queued\n",
                          jiffies,
               dev, vmidi_bytes_in, vmidi_bytes_out, vmidi_dropped,
               vmidi_count);

    vmidi_opened = 0;
    MOD_DEC_USE_COUNT;

    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: sequencer closed dev %d\n", jiffies, dev);
}

/*
 * ONE BYTE, WHICH IS THE WHOLE PROTOCOL.
 *
 * The sequencer's std_midi_synth converter has already turned note
 * events into MIDI bytes by the time they arrive here, so there is
 * nothing to decode - see the header comment.
 *
 * Returns 1 when the byte was taken. emu10k1 returns 1 unconditionally
 * on success; we do the same, because a queue-full is reported through
 * buffer_status() and by the dropped counter rather than by failing
 * the write, which the sequencer does not retry.
 */
static int
vmidi_seq_out(int dev, unsigned char midi_byte)
{
    if (dev < 0 || dev >= MAX_MIDI_DEV || midi_devs[dev] == NULL) {
        /* ALWAYS PRINTED. If this ever fires the sequencer is calling
         * us with an index we do not own, and every byte after it is
         * lost - it must not be rate-limited or silent. */
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: outputc REJECTED dev %d"
                              " (max %d, devs[dev] %s)\n",
                   jiffies, dev, MAX_MIDI_DEV,
                   (dev >= 0 && dev < MAX_MIDI_DEV && midi_devs[dev])
                       ? "set" : "NULL");
        /*
         * 1, NOT AN ERRNO, AND NOT 0.
         *
         * NOT AN ERRNO because none of the three callers reads one -
         * all three test this as a boolean:
         *
         *     sequencer.c:840   if (!midi_devs[dev]->outputc(dev, q[1]))
         *     midibuf.c:141     ok = midi_devs[dev]->outputc(dev, c);
         *     midi_synth.c:102  if (midi_devs[midi_dev]->outputc(...))
         *
         * -EINVAL is -22, which is TRUE, so an errno here already read
         * as "sent": the byte was counted as delivered and never
         * retried. Returning it was harmless only by accident.
         *
         * NOT 0 because 0 means "no room, try again", and there is
         * nothing to retry for when the device index is wrong. Nobody
         * can fix it - midi_outc (midi_synth.c:100-103) would spin
         * 3200 times with no sleep and then report "Midi send timed
         * out". uart401_out makes exactly this choice for its own
         * reject path (uart401.c:152).
         *
         * So 1: accepted and discarded, which is the truth. The byte
         * is not deliverable and the caller must not wait for it.
         *
         * THE CONTRACT IS PER CALLBACK, which is what makes this easy
         * to get wrong. vmidi_seq_open MUST keep returning an errno -
         * its caller tests err < 0 at sequencer.c:307.
         */
        return 1;
    }

    /*
     * THE RETURN VALUE IS BACKPRESSURE, AND IT IS ONLY SAFE WHEN
     * SOMEONE CAN RELIEVE IT.
     *
     * midi_outc (midi_synth.c:101) treats 0 as "try again" and spins
     * up to 3200 times WITH NO SLEEP before giving up with "Midi send
     * timed out". That busy-loop is the right behaviour when a reader
     * is draining the ring - it is a brief wait for space that is
     * about to appear.
     *
     * WITH NO READER IT IS A TRAP. Nothing will ever drain the ring,
     * so every byte from then on costs 3200 wasted iterations inside
     * the sequencer, and the writer stays there. That is why this
     * returned 1 unconditionally at first, which was wrong the other
     * way: it claimed success for bytes that had been dropped, so a
     * dropped note-off looked delivered and the note hung.
     *
     * So: report the truth when a reader exists, and absorb the byte
     * when one does not. The drop is still counted and still traced -
     * vmidi_put does both - so nothing is hidden either way.
     */
    if (!vmidi_put(midi_byte) && vmidi_reader)
        return 0;               /* real backpressure; a reader can clear it */

    return 1;
}

/*
 * INPUT IS NOT SUPPORTED, and saying so is better than pretending.
 * A software synth receives; it has nothing to send back. The
 * sequencer copes with a device that never delivers input.
 */
static int
vmidi_seq_start_read(int dev)
{
    /* Traced because it is UNEXPECTED: input is not supported, so if
     * the sequencer calls this something is asking vmidi for MIDI IN
     * and will get silence. */
    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: start_read dev %d (input unsupported)\n",
        jiffies,
               dev);
    return 0;
}

static int
vmidi_seq_end_read(int dev)
{
    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: end_read dev %d\n", jiffies, dev);
    return 0;
}

/*
 * A flush hint. emu10k1's is empty too (midi.c) - there is nothing to
 * flush when the ring IS the buffer, and the reader is woken on every
 * byte.
 */
static void
vmidi_seq_kick(int dev)
{
    (void) dev;
    /* Rate-limited on the byte counter: kick can arrive per event. */
    if (VMIDI_RL(vmidi_rl_kick, vmidi_rate_status))
        printk(KERN_DEBUG VMIDI_TS "vmidi: kick (%d queued)\n",
               jiffies, vmidi_count);
}

/*
 * HOW MUCH ROOM IS LEFT, in bytes.
 *
 * The sequencer uses this to pace itself. Reporting honestly is what
 * keeps it from overrunning us - and when it does not, `vmidi_dropped'
 * records it.
 */
static int
vmidi_seq_buffer_status(int dev)
{
    if (dev < 0 || dev >= MAX_MIDI_DEV || midi_devs[dev] == NULL) {
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: buffer_status REJECTED dev %d\n",
            jiffies,
                   dev);
        /*
         * 0 MEANS "NOTHING PENDING", AND AN ERRNO IS NOT 0.
         *
         * seq_drain_midi_queues counts a non-zero as still draining:
         *
         *     if (midi_devs[i]->buffer_status(i))   sequencer.c:1144
         *             n++;
         *
         * and loops until every open device reads 0. -EINVAL (-22) is
         * non-zero, so the loop would never finish - sequencer_release
         * blocks with sequencer_busy still set, and every LATER open of
         * /dev/sequencer fails.
         *
         * NOT HYPOTHETICAL: the identical bug wedged /dev/sequencer
         * through usb-midi on the P3, which is why
         * usbmidi_seq_buffer_status returns a plain 0 too.
         *
         * uart401_buffer_status (uart401.c:200) and mpu401's (:765)
         * both return a plain 0. No errno anywhere in the OSS drivers.
         */
        return 0;
    }

    /*
     * BYTES STILL QUEUED. NOT free space.
     *
     * THIS RETURNED FREE SPACE UNTIL 2026-09-02 AND IT HUNG EVERY
     * CLIENT THAT CLOSED /dev/sequencer.
     *
     * seq_drain_midi_queues() (sequencer.c:1127-1155) loops until this
     * returns ZERO for every open MIDI device:
     *
     *     while (!signal_pending(current) && n) {
     *         for (i = 0; i < max_mididev; i++)
     *             if (midi_opened[i] && midi_written[i])
     *                 if (midi_devs[i]->buffer_status != NULL)
     *                     if (midi_devs[i]->buffer_status(i))
     *                         n++;                    <- :1144
     *         if (n) interruptible_sleep_on_timeout(&seq_sleeper, HZ/10);
     *     }
     *
     * and sequencer_release() (:1170-1179) calls it on close. Returning
     * free space meant an EMPTY ring answered VMIDI_BUFSZ - the
     * MAXIMUM - so the sequencer read "4096 bytes still pending" and
     * slept HZ/10 at a time forever. Only a signal broke it.
     *
     * THE EMPTIER THE RING, THE WORSE IT WAS. That is why a draining
     * reader did not help and why five earlier theories - ring size,
     * -R release-on-idle, the outputc return value, a musserv bug, and
     * "vmidi is exonerated" - all survived: none of them touches this
     * number.
     *
     * Measured, not reasoned: tests/logs/2026-09-02-vmidi-86box-
     * vmidicat-drained/ has the trace, a wall of "reader sleeping
     * (0 queued)" while the close refused to finish.
     *
     * THE CONTRACT, from the drivers that work:
     *
     *     uart401.c:200   return 0;
     *     mpu401.c:765    return 0;
     *     sb_midi.c       does not implement it (NULL)
     *     v_midi.c        does not implement it (NULL)
     *
     * sequencer.c:1143 tolerates NULL, so not implementing it is safe.
     * Implementing it BACKWARDS is not. We report the real count
     * because we genuinely have a queue - a reader may not have
     * drained it yet, and the sequencer should wait for that.
     */
    if (VMIDI_RL(vmidi_rl_status, vmidi_rate_status))
        printk(KERN_DEBUG VMIDI_TS "vmidi: buffer_status %d queued"
                          " of %d (reader %d)\n",
               jiffies, vmidi_count, VMIDI_BUFSZ, vmidi_reader);

    /*
     * WITH NO READER, REPORT ZERO - the same rule as vmidi_seq_out's.
     *
     * Reporting a true count is only useful if something will act on
     * it. Nothing drains this ring but a userspace synth, so with none
     * attached the bytes are stranded and waiting cannot unstrand
     * them: seq_drain_midi_queues (sequencer.c:1144) would loop until
     * a signal arrives, and sequencer_release (:1170-1179) with it.
     *
     * MEASURED, 2026-09-02: a no-reader run filled the ring and then
     * hung lxdoom's quit for 180 SECONDS until the user found the pid
     * and killed it by hand - tests/logs/2026-09-02-vmidi-86box-
     * noreader-droptest/. With a reader the same close took 13.
     *
     * THE PRECEDENT IS THE USER'S OWN P3. emu10k1 there registered a
     * sequencer device that was not set up; musserv got broken pipes,
     * and lxdoom LOADED AND QUIT FINE. So a MIDI device that cannot
     * deliver does not have to trap its client, and this is the same
     * situation reached by a different route.
     *
     * v_midi.c reaches the same place by having no buffer_status at
     * all - sequencer.c:1143 tolerates NULL and skips the wait.
     *
     * The dropped bytes are NOT hidden by this: vmidi_dropped counts
     * every one and both the close line and the unload banner report
     * the total.
     */
    if (!vmidi_reader)
        return 0;

    return vmidi_count;
}

/*
 * The table itself. Field order matters and follows dev_table.h:
 * info, converter, in_info, then the function pointers.
 *
 * `converter' is &std_midi_synth - the kernel's generic MIDI-to-synth
 * shim, which is what makes note events arrive as bytes. emu10k1 sets
 * the same (main.c:1422-1440).
 *
 * ioctl, command and prefix_cmd are NULL, as emu10k1's are: nothing
 * this device does needs them.
 */
static struct midi_operations vmidi_operations =
{
    { "vmidi", 0, 0, SNDCARD_MPU401 },
    &std_midi_synth,
    { 0 },
    vmidi_seq_open,
    vmidi_seq_close,
    NULL,                       /* ioctl       */
    vmidi_seq_out,
    vmidi_seq_start_read,
    vmidi_seq_end_read,
    vmidi_seq_kick,
    NULL,                       /* command     */
    vmidi_seq_buffer_status,
    NULL                        /* prefix_cmd  */
};

/* --- the userspace side: /dev/vmidi --------------------------------- */

/*
 * The synth reads this. Blocking by default, because a synth with
 * nothing to play should sleep rather than spin - the same reasoning
 * as vsound's pump read.
 */
static ssize_t
vmidi_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
    struct wait_queue wait = { current, NULL };
    unsigned long flags;
    int done = 0;

    (void) ppos;

    if (count == 0)
        return 0;

    if (VMIDI_RL(vmidi_rl_read, vmidi_rate_byte))
        printk(KERN_DEBUG VMIDI_TS "vmidi: read(%d) - %d queued\n",
               jiffies, (int) count, vmidi_count);

    add_wait_queue(&vmidi_wait, &wait);
    for (;;) {
        int n = 0;

        save_flags(flags);
        cli();
        while (n < (int) count && vmidi_count > 0) {
            unsigned char b = vmidi_buf[vmidi_tail];

            vmidi_tail = (vmidi_tail + 1) % VMIDI_BUFSZ;
            vmidi_count--;
            restore_flags(flags);

            if (put_user(b, buf + n)) {
                /* ALWAYS. A partial copy loses MIDI bytes silently. */
                if (vmidi_trace)
                    printk(KERN_DEBUG VMIDI_TS "vmidi: read EFAULT after %d"
                                      " bytes\n", jiffies, n);
                /*
                 * REPORT WHAT ARRIVED, NOT THE FAILURE.
                 *
                 * This used to return -EFAULT unconditionally, which
                 * threw away the count of bytes ALREADY DELIVERED. The
                 * caller then cannot know that n bytes did arrive and
                 * re-reads from a stream that has already advanced -
                 * the ring is consumed, so those bytes are simply gone.
                 * A dropped note-off is a note that plays forever.
                 *
                 * -EFAULT is correct only when NOTHING arrived. Both
                 * precedents in the tree agree:
                 *
                 *   random.c:1280   if (!i) { ret = -EFAULT; }
                 *   usb-midi read   if (!ret) ret = -EFAULT;
                 *
                 * vmidi was already better than its OSS peer here -
                 * MIDIbuf_read calls copy_to_user and ignores the
                 * result entirely, on a line its own authors labelled
                 * BROKE BROKE BROKE. vmidi detected the failure; the
                 * gap was only in what it reported.
                 *
                 * The byte in hand is lost either way: it came off the
                 * ring at :644 and userspace would not take it. Step 6
                 * of the plan considered restructuring to avoid that
                 * and recommends against it - the re-validation window
                 * it opens is easier to get wrong than the one byte it
                 * saves, on a path that only triggers when userspace
                 * passes a bad buffer.
                 */
                if (n > 0)
                    done = n;           /* a short read, which is legal */
                else
                    done = -EFAULT;     /* nothing arrived */
                goto out;
            }
            n++;
            vmidi_bytes_out++;

            /*
             * THE OUTBOUND HALF OF vmidi_rate_byte.
             *
             * vmidi_rl_out was declared and used NOWHERE - four of the
             * five rate counters had a VMIDI_RL site and this one had
             * none, so the knob controlled nothing on this side. Same
             * class as musdbg_midiout, which read zero for weeks and
             * made a working MIDI path look dead.
             *
             * The asymmetry is what made a capture hard to read: bytes
             * could be seen arriving and never seen leaving, so a
             * reader that had stopped draining looked identical to a
             * sequencer that had stopped sending.
             *
             * OUTSIDE THE cli() SECTION, unlike the inbound trace at
             * :200. That one is already holding interrupts off to
             * update the ring and the printk costs nothing extra;
             * here the section has been released at :645 and there is
             * no reason to re-enter it just to trace.
             */
            if (VMIDI_RL(vmidi_rl_out, vmidi_rate_byte))
                printk(KERN_DEBUG VMIDI_TS "vmidi: out 0x%02x (#%lu, %d left,"
                                  " read %d of %d)\n",
                       jiffies, (unsigned) b, vmidi_bytes_out, vmidi_count,
                       n, (int) count);

            save_flags(flags);
            cli();
        }
        restore_flags(flags);

        if (n > 0) {
            done = n;
            break;
        }

        if (file->f_flags & O_NONBLOCK) {
            done = -EAGAIN;
            break;
        }

        /* THE BLOCK ITSELF, always traced. A reader asleep here with
         * bytes apparently queued is the difference between "nothing
         * arrived" and "nobody collected it" - and the ps output on
         * the last run could not tell those apart. */
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: reader sleeping (%d queued)\n",
            jiffies,
                   vmidi_count);

        current->state = TASK_INTERRUPTIBLE;
        schedule();

        if (signal_pending(current)) {
            if (vmidi_trace)
                printk(KERN_DEBUG VMIDI_TS "vmidi: reader interrupted by signal\n", jiffies);
            done = -ERESTARTSYS;
            break;
        }
    }
out:
    current->state = TASK_RUNNING;
    remove_wait_queue(&vmidi_wait, &wait);
    return done;
}

static unsigned int
vmidi_poll(struct file *file, poll_table *wait)
{
    poll_wait(file, &vmidi_wait, wait);
    return vmidi_count > 0 ? (POLLIN | POLLRDNORM) : 0;
}

static int
vmidi_open(struct inode *inode, struct file *file)
{
    unsigned long flags;

    (void) inode;

    if ((file->f_mode & FMODE_READ) == 0) {
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: open REFUSED - not opened for"
                              " read (-EINVAL)\n", jiffies);
        return -EINVAL;         /* the synth READS; nothing writes here */
    }

    /*
     * TEST AND SET TOGETHER, WITH INTERRUPTS OFF.
     *
     * The test and the set used to be separated by a printk, so two
     * processes interleaving there would both pass the test and both
     * attach. vmidi_read CONSUMES, so each would get a random half of
     * one MIDI stream - interleaved note-ons and note-offs, which is
     * exactly the stuck-note failure this module exists to prevent.
     * vmidi_release compounds it: the first close clears the flag
     * while the second reader is still attached.
     *
     * IT IS NOT REACHABLE ON THIS KERNEL, and the reason is worth
     * stating because it is easy to assume otherwise: printk is NOT a
     * scheduling point on 2.2. It takes console_lock with
     * spin_lock_irqsave (printk.c:261) and never sleeps. On a
     * non-preemptive UP kernel with no schedule point in the window,
     * the sequence is already atomic with respect to other processes,
     * and only an interrupt could interleave - interrupts do not call
     * open.
     *
     * Fixed anyway because that correctness rests entirely on a kernel
     * property nothing in this file states, and it stops being true on
     * SMP or a preemptible kernel. Four lines is cheap insurance
     * against a future reader adding a trace between the two.
     *
     * cli() rather than a spinlock, as vmidi_put already does for the
     * ring: on this UP kernel spin_lock is (void)(lock) and
     * spin_lock_irqsave is exactly save_flags/cli anyway
     * (asm-i386/spinlock.h:24,31).
     *
     * THE printk IS OUTSIDE THE SECTION. Keeping it inside would work
     * - printk does not sleep - but there is no reason to hold
     * interrupts off across it, and doing so would reintroduce the
     * habit this fix exists to break.
     */
    save_flags(flags);
    cli();
    if (vmidi_reader) {
        restore_flags(flags);
        /* A synth that failed to detach leaves this set, and every
         * later run then has no reader and drops every byte. */
        if (vmidi_trace)
            printk(KERN_DEBUG VMIDI_TS "vmidi: open REFUSED - reader already"
                              " attached (-EBUSY)\n", jiffies);
        return -EBUSY;
    }
    vmidi_reader = 1;
    restore_flags(flags);

    MOD_INC_USE_COUNT;

    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: synth attached\n", jiffies);
    return 0;
}

static int
vmidi_release(struct inode *inode, struct file *file)
{
    (void) inode; (void) file;

    /*
     * A single store, so no section is needed to make it atomic on
     * i386 - and unlike open there is nothing to test first, so there
     * is no window to close. The pairing with MOD_DEC_USE_COUNT is
     * what matters here: release is only ever reached through a
     * successful open (the VFS does not call it otherwise), so the
     * decrement is unconditional and correct.
     */
    vmidi_reader = 0;
    MOD_DEC_USE_COUNT;

    if (vmidi_trace)
        printk(KERN_DEBUG VMIDI_TS "vmidi: synth detached - %lu in, %lu out,"
                          " %lu dropped, %d left in the ring\n",
                          jiffies,
               vmidi_bytes_in, vmidi_bytes_out, vmidi_dropped,
               vmidi_count);
    return 0;
}

static struct file_operations vmidi_fops = {
    NULL,               /* llseek   */
    vmidi_read,
    NULL,               /* write    */
    NULL,               /* readdir  */
    vmidi_poll,
    NULL,               /* ioctl    */
    NULL,               /* mmap     */
    vmidi_open,
    NULL,               /* flush    */
    vmidi_release
};

/*
 * WHAT IS AND IS NOT BEING TRACED, STATED IN THE CAPTURE ITSELF.
 *
 * A capture that does not record its own settings cannot be read
 * safely months later, and the failure is silent: a category that was
 * switched OFF looks exactly like a category that had nothing to
 * report. Absence of a line then reads as absence of the event.
 *
 * That is the trap CLAUDE.md section 2 is entirely about - three
 * separate instances of a conclusion resting on something not being
 * there, drawn from a method that could not have found it. A `head'
 * that truncated before the directory in question; a `grep -r' that
 * skipped every gitignored tree; a `grep' that silently returned
 * nothing on a Latin-1 file and nearly put "the driver was fixed
 * upstream" into design/16.
 *
 * So the log says what it is. Printed at load AND at unload: a module
 * whose parameters were changed by a reload mid-session would
 * otherwise leave one banner describing two different runs, and the
 * unload copy is also the one that survives when a capture is started
 * after insmod.
 *
 * ALWAYS PRINTED, EVEN WHEN TRACING IS OFF. A capture with no vmidi
 * lines at all is the most misleading case of the lot, and the one
 * line saying "trace OFF" is what distinguishes "nothing happened"
 * from "nobody was watching".
 */
static void
vmidi_banner(void)
{
    if (!vmidi_trace) {
        printk(KERN_INFO VMIDI_TS "vmidi: trace OFF"
                         " - no vmidi lines will appear in this capture"
                         " (load with vmidi_trace=1)\n", jiffies);
        return;
    }

    printk(KERN_INFO VMIDI_TS "vmidi: trace ON:"
                     " lifecycle+errors=always, bytes=%s, pacing=%s\n",
           jiffies,
           vmidi_rate_byte == 0 ? "OFF" :
               (vmidi_rate_byte == 1 ? "all" : "sampled"),
           vmidi_rate_status == 0 ? "OFF" :
               (vmidi_rate_status == 1 ? "all" : "sampled"));

    /* The numbers too, so "sampled" is never ambiguous. */
    printk(KERN_INFO VMIDI_TS "vmidi: rates: vmidi_rate_byte=%d"
                     " vmidi_rate_status=%d (0=off, 1=every, N=every Nth)\n",
           jiffies, vmidi_rate_byte, vmidi_rate_status);
}

/*
 * ------------------------------------------------------------------
 * /proc/vmidi - WHICH SEQUENCER SLOT WE GOT, WITHOUT READING
 *               /proc/sound
 * ------------------------------------------------------------------
 *
 * THE POINT IS TO STOP OPENING `/proc/sound' AT ALL, and it is the
 * user's, 2026-09-26: "can we get away with no /proc/sound calls.
 * thats just to tell us where vmidi landed right?" It is - the only
 * thing `vlhe_midi_seqdev()' ever took from that file was the index
 * on the line named `vmidi'.
 *
 * AND READING IT CRASHES SOME MACHINES. `sound_proc_get_info()'
 * walks `audio_devs[]' to `num_audiodevs', and `sound_alloc_audiodev'
 * (dev_table.c:526) can set that count past the array's five entries
 * because it derives the index from the shared dsp minor chain with
 * no bound test. Three es1371 plus `sb' is enough; the oops is at
 * `audio_devs[5]', which IS `num_audiodevs' - see design/36 row 84.
 * That is a 1999 kernel bug and not ours, but we were the only
 * program on the machine that opened the file, so not opening it
 * removes us from it entirely.
 *
 * SAME SHAPE AS `/proc/vsound' (vsound_dev.c:2667), deliberately -
 * that one exists because sweeping device nodes to find vsound was
 * inference where the module already knew the answer (row 78). This
 * is the same mistake one table over: parsing a listing to learn a
 * number the module is holding in a variable.
 *
 * NO FALLBACK TO `/proc/sound', AND THAT IS A DECISION. The user's:
 * "we dont need the fallback the code and modules right now are
 * private and get rebuilt every staging" - so a GUI can never meet a
 * vmidi.o older than itself. A fallback would also reintroduce the
 * exact read this change exists to remove, on the machines where it
 * is dangerous.
 */
static struct proc_dir_entry *vmidi_proc_ent;

static int
vmidi_proc_get_info(char *buffer, char **start, off_t offset,
                    int length, int inout)
{
    int len = 0;

    (void) start; (void) offset; (void) length; (void) inout;

    /* `midi' IS THE SEQUENCER INDEX - what `musserv -u N' takes, and
     * what the control centre reports. -1 until sound_alloc_mididev()
     * has returned, which cannot be observed: the entry is created
     * after it. */
    len += sprintf(buffer + len, "midi: %d\n", vmidi_seq_dev);
    len += sprintf(buffer + len, "minor: %d\n", vmidi_char_dev);
    return len;
}

static void
vmidi_proc_start(void)
{
    vmidi_proc_ent = create_proc_entry("vmidi", S_IFREG | S_IRUGO, NULL);
    if (vmidi_proc_ent != NULL)
        vmidi_proc_ent->get_info = vmidi_proc_get_info;
    /* A FAILURE IS NOT FATAL, as in vsound: the module works without
     * it and only discovery gets harder. Refusing to load over a
     * missing proc entry would be the worse outcome. */
}

static void
vmidi_proc_stop(void)
{
    if (vmidi_proc_ent != NULL) {
        remove_proc_entry("vmidi", NULL);
        vmidi_proc_ent = NULL;
    }
}

/* --- load and unload ------------------------------------------------ */

int
init_module(void)
{
    printk(KERN_INFO "vmidi: built %s %s\n", __DATE__, __TIME__);

    /*
     * THE SEQUENCER ENTRY IS WHAT `musserv -m' FINDS.
     * sound_alloc_mididev() is what SNDCTL_SEQ_NRMIDIS counts;
     * register_sound_midi() alone would give a /dev/midiNN that the
     * sequencer never enumerates. emu10k1 does BOTH, for two different
     * purposes (main.c:401 and :448) - we need only the second.
     */
    {
        int had_midis = num_midis;      /* the allocator moves it - below */

        vmidi_seq_dev = sound_alloc_mididev();
        if (vmidi_seq_dev == -1) {
            printk(KERN_ERR "vmidi: sound_alloc_mididev failed\n");
            return -EBUSY;
        }

        /*
         * A SLOT PAST THE TABLE IS REFUSED, NOT STORED - design/36 7c,
         * design/54 D05, 2026-10-03. sound_alloc_mididev() (dev_table.c)
         * turns a minor into an index with `>>= 4' and no test against
         * MAX_MIDI_DEV (6), and the minor chain has more slots than the
         * table. MEASURED on 86Box 2026-09-26: four MIDI-registering cards
         * plus v_midi held 0-5, vmidi got 6, and midi_devs[6] IS num_midis
         * (adjacent declarations) - the kmalloc below wrote a heap pointer
         * into the count, which went negative, and every playmidi and
         * /proc/sound's Midi section failed with vmidid running normally.
         *
         * THREE THINGS TO UNDO, NONE THROUGH THE ARRAY:
         *   - the minor: unregister_sound_midi() directly. NOT
         *     sound_unload_mididev(), which does midi_devs[dev] = NULL -
         *     by the same aliasing that would ZERO num_midis
         *   - num_midis, which the allocator set to index + 1 before
         *     returning: left there the sequencer walks past the table,
         *     and the slot past it is the count itself. Put back as it was
         *   - our index, so nothing later believes a slot is held
         * One dead module rather than a dead MIDI subsystem; the kernel's
         * own overflow (any further driver) is design/54 S13.
         */
        if (vmidi_seq_dev >= MAX_MIDI_DEV) {
            printk(KERN_ERR "vmidi: the kernel gave MIDI slot %d, but its"
                   " table holds only %d (0..%d) - refusing rather than"
                   " corrupting it. Unload a MIDI driver (v_midi, or a"
                   " card's MPU/MIDI) and load vmidi again\n",
                   vmidi_seq_dev, MAX_MIDI_DEV, MAX_MIDI_DEV - 1);
            unregister_sound_midi((vmidi_seq_dev << 4) + 2);
            num_midis = had_midis;
            vmidi_seq_dev = -1;
            return -EBUSY;
        }
    }

    midi_devs[vmidi_seq_dev] =
        (struct midi_operations *) kmalloc(sizeof(struct midi_operations),
                                           GFP_KERNEL);
    if (midi_devs[vmidi_seq_dev] == NULL) {
        printk(KERN_ERR "vmidi: out of memory\n");
        sound_unload_mididev(vmidi_seq_dev);
        vmidi_seq_dev = -1;
        return -ENOMEM;
    }

    memcpy((char *) midi_devs[vmidi_seq_dev],
           (char *) &vmidi_operations, sizeof(struct midi_operations));
    midi_devs[vmidi_seq_dev]->devc = NULL;
    /*
     * SAY WHICH MIDI DEVICE THIS IS - design/36 row 62. With a card
     * loaded there are two MIDI devices, numbered in load order, and
     * lxdoom's musserv wants the index (`-u N'). The user tried
     * several by hand; nothing had ever printed it.
     */
    printk(KERN_INFO "vmidi: MIDI device %d (musserv/lxdoom: -u %d)\n",
           vmidi_seq_dev, vmidi_seq_dev);

    /*
     * AND A CHARACTER DEVICE FOR THE SYNTH TO READ. Registered as a
     * `sound_special' rather than a dsp or midi node: it is neither -
     * nothing plays audio through it and the sequencer does not
     * enumerate it. The minor is vmidi_minor - see its declaration
     * for why it is 11 and no longer 15.
     */
    if (vmidi_minor < 0 || vmidi_minor > 255) {
        printk(KERN_ERR "vmidi: vmidi_minor=%d is not a minor (0..255)\n",
               vmidi_minor);
        kfree(midi_devs[vmidi_seq_dev]);
        midi_devs[vmidi_seq_dev] = NULL;
        sound_unload_mididev(vmidi_seq_dev);
        vmidi_seq_dev = -1;
        return -EINVAL;
    }
    vmidi_char_dev = register_sound_special(&vmidi_fops, vmidi_minor);
    if (vmidi_char_dev < 0) {
        /* -16 (EBUSY) means another driver holds that minor - the
         * esssolo1 case. Say which minor, so the fix is obvious. */
        printk(KERN_ERR "vmidi: register_sound_special(minor %d) failed:"
                        " %d%s\n", vmidi_minor, vmidi_char_dev,
               vmidi_char_dev == -EBUSY
                   ? " - another driver holds it; insmod vmidi_minor=N" : "");
        kfree(midi_devs[vmidi_seq_dev]);
        midi_devs[vmidi_seq_dev] = NULL;
        sound_unload_mididev(vmidi_seq_dev);
        vmidi_seq_dev = -1;
        return vmidi_char_dev;
    }

    printk(KERN_INFO "vmidi: sequencer midi device %d, char minor %d\n",
           vmidi_seq_dev, vmidi_char_dev);
    printk(KERN_INFO "vmidi: find it with 'musserv -l',"
                     " use it with 'musserv -m -u %d'\n", vmidi_seq_dev);

    /*
     * WHO ELSE IS IN THE TABLE, and what our own index is among them.
     * `musserv -u N' selects by POSITION in this list, so a run that
     * used the wrong N looks exactly like a vmidi that never received
     * anything. Printing the table makes that answerable from the log
     * instead of by re-running with a different number.
     */
    if (vmidi_trace) {
        int i;

        printk(KERN_DEBUG VMIDI_TS "vmidi: midi device table, %d slots:\n",
               jiffies, MAX_MIDI_DEV);
        for (i = 0; i < MAX_MIDI_DEV; i++)
            if (midi_devs[i] != NULL)
                printk(KERN_DEBUG VMIDI_TS "vmidi:   [%d] %s%s\n",
                       jiffies, i, midi_devs[i]->info.name,
                       i == vmidi_seq_dev ? "   <- vmidi" : "");
    }

    /* LAST, so it only appears once both registrations have
     * succeeded - a reader that finds /proc/vmidi can trust both
     * numbers in it. Every error path above returns before here. */
    vmidi_proc_start();

    vmidi_banner();
    return 0;
}

void
cleanup_module(void)
{
    /* FIRST, so the settings are stated even if teardown oopses.
     * See vmidi_banner() for why the capture must describe itself. */
    vmidi_banner();

    vmidi_proc_stop();

    if (vmidi_char_dev >= 0)
        unregister_sound_special(vmidi_char_dev);

    if (vmidi_seq_dev >= 0) {
        if (midi_devs[vmidi_seq_dev] != NULL) {
            kfree(midi_devs[vmidi_seq_dev]);
            midi_devs[vmidi_seq_dev] = NULL;
        }
        sound_unload_mididev(vmidi_seq_dev);
    }

    /*
     * THE DROP COUNT IS THE ONE FIGURE WORTH PRINTING. Dropped MIDI
     * means stuck notes - a note-on whose note-off was lost plays
     * forever - so a run that dropped anything must say so.
     */
    printk(KERN_INFO "vmidi: %lu bytes in, %lu out, %lu DROPPED\n",
           vmidi_bytes_in, vmidi_bytes_out, vmidi_dropped);
}

#ifdef MODULE_LICENSE
MODULE_LICENSE("Dual BSD/GPL");
#endif
