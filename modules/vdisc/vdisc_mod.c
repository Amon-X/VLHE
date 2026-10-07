/*
 * vdisc_mod.c - the virtual CD-ROM block device.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/07-vsound.md section 1 for the four components; section 3.1
 * for how CD audio reaches the mixer now. vdisc.h has the protocol and
 * the reasoning behind the drive count.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT.
 *
 * This is a rewrite of vcd_mod.c, not a rename of it. Section 7's rule
 * governs: nothing crosses over without answering three questions -
 * does it still have a job, do its assumptions still hold, was it right
 * in the first place. Most of the block path answered all three and is
 * here on merit; the audio path did not and is not.
 *
 * THE SHAPE, which is unchanged because it was right:
 *
 *   do_vdisc_request()  runs under io_request_lock with interrupts off
 *                       and MUST NOT SLEEP. It only moves requests off
 *                       CURRENT onto our queue and wakes the thread.
 *
 *   vdisc_thread()      a kernel thread, so it may sleep. Hands each
 *                       request to the daemon and completes it when the
 *                       reply arrives.
 *
 *   /dev/vdiscctl       the daemon's channel: ATTACH/DETACH to bind an
 *                       image, GET_REQ/PUT_REPLY to serve I/O.
 *
 * Requests complete through our own vdisc_end_request(), NOT the
 * end_request() macro in <linux/blk.h>: that macro operates on CURRENT,
 * and by the time a reply arrives ours is long off the queue. Same
 * approach as nbd_end_request() in include/linux/nbd.h. This is the
 * detail the project has recorded as expensive to get wrong -
 * end_that_request_last() needs io_request_lock, and without it the
 * request free list corrupts.
 *
 * WHAT CHANGED, AND WHY.
 *
 * THE AUDIO PATH IS GONE FROM THE MODULE. vcd_mod.c tracked playback
 * position here, in the kernel, from jiffies:
 *
 *     CDDA runs at a fixed 75 frames/second, so elapsed jiffies, give
 *     the position exactly
 *
 * That reasoning was sound when the daemon wrote /dev/dsp itself and
 * the OSS buffer paced it. It is not sound now. vdiscd is a vsound
 * CLIENT, its audio sits in a vchan being mixed, and the position the
 * card has actually reached is something vsound knows and jiffies, do
 * not - this is the same error as predicting a play position instead of
 * asking for it, which cost this project real time on the sound side
 * (GETOPTR, section 6). So SUBCHNL asks the daemon, and the daemon
 * knows because it is the one feeding the channel.
 *
 * WHAT IT ACTUALLY REPORTS - corrected 2026-10-04 (design/26 section 7,
 * design/54 D40). Not where the card has reached: the play child's
 * WRITE position, sent every 75 frames - about 75 ms ahead of what is
 * heard, at one-second granularity. That is right for every player
 * here (they show seconds); it is not sample-accurate, and nothing in
 * this module asks vsound where the card is.
 *
 * The kernel keeps only what a poller needs without a round trip:
 * status and track number, both written by the daemon as they change.
 *
 * WHAT DID NOT CHANGE, because it was right:
 *
 *   - the TOC is cached here at attach. audio_ioctl can be called with
 *     locks held, so it must never round-trip to userspace.
 *   - the daemon is UNTRUSTED. Geometry and TOC are bounds-checked at
 *     attach, because n_tracks indexes track[] and the track numbers
 *     are what CDROMREADTOCENTRY selects on.
 *   - generic_packet stays NULL. Deliberate: raw MMC passthrough is for
 *     burning, ripping and copy protection, none of which apply to a
 *     read-only virtual drive.
 *
 * C89 only: GCC 2.95.2.
 */

#include <linux/module.h>
#include <linux/version.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/cdrom.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/wait.h>
#include <linux/smp_lock.h>
#include <linux/proc_fs.h>   /* create_proc_entry - /proc/vdisc */

#include <asm/uaccess.h>
#include <asm/atomic.h>

#include "vdisc.h"

/*
 * TIMESTAMP EVERY TRACE LINE. Defined here rather than pulled from
 * vsound_chan.h: vdisc is a SEPARATE module and must not depend on
 * vsound's headers.
 *
 * printk carries no time on 2.2, so a long capture has no time axis at
 * all - two lines a minute apart look like two in the same
 * microsecond. On 2026-08-27 that had `vdisc: unloaded' followed by an
 * oops read as cause and effect when they were separated by however
 * long it took to type the next command.
 */
#define VSOUND_TS   "[%lu] "

#define VDISC_VERSION "0.1"

#define VDISC_DEFAULT_MAJOR 44

/*
 * MAJOR_NR IS FOR <linux/blk.h>'s #if CHAIN ONLY. It must be a constant
 * so blk.h can pick its "unknown device" branch; NOTHING ELSE IN THIS
 * FILE MAY USE IT, and in particular not blk.h's CURRENT or
 * INIT_REQUEST, which expand to blk_dev[MAJOR_NR] - a compile-time 44.
 *
 * design/26 B3, fixed 2026-09-14: do_vdisc_request() used those macros
 * while init_module() registered `vdisc_major', a module parameter. So
 * `insmod vdisc.o vdisc_major=45' installed the request function on
 * major 45's queue and had it service major 44's, which was empty: the
 * attach succeeded, the mount slept in D state forever, nothing was
 * logged. The parameter exists for the machine where 44 is taken, and
 * refusing it would not help that machine - so the request function
 * now indexes blk_dev by the RUNTIME major, VDISC_CURRENT below.
 */
#define MAJOR_NR        VDISC_DEFAULT_MAJOR
#define DEVICE_NAME     "vdisc"
#define DEVICE_REQUEST  do_vdisc_request
#define DEVICE_NR(dev)  (MINOR(dev))
#define DEVICE_ON(dev)
#define DEVICE_OFF(dev)
#define DEVICE_NO_RANDOM

/* We complete off-queue, so blk.h's CURRENT-based end_request() is not
 * what we want. */
#define LOCAL_END_REQUEST

#include <linux/blk.h>

MODULE_AUTHOR("vdisc");
MODULE_DESCRIPTION("Virtual CD-ROM device");
/* THE LICENCE TAG, as vsound and vmidi - 2026-10-02, the user (design/31
 * A4: this module declared none). Not compiled on 2.2.16, whose headers
 * have no MODULE_LICENSE; read only by a later kernel. */
#ifdef MODULE_LICENSE
MODULE_LICENSE("Dual BSD/GPL");
#endif

/* ------------------------------------------------------------------ *
 * Module parameters
 * ------------------------------------------------------------------ */

static int vdisc_major = VDISC_DEFAULT_MAJOR;

/* The request queue of the major we were actually given. */
#define VDISC_CURRENT   (blk_dev[vdisc_major].current_request)
static int vdisc_ndevs = VDISC_DEF_DEVS;
static int vdisc_trace;

/*
 * DOES THIS KERNEL HAVE THE CD-ROM PACKET INTERFACE? - 2026-10-06,
 * design/39 section 3h.
 *
 * generic_packet in struct cdrom_device_ops, struct
 * cdrom_generic_command, CDC_GENERIC_PACKET and the GPCMD_* set all
 * arrived in kernel.org 2.2.16 (patch-2.2.16; none of 2.2.11-2.2.15 has
 * them). Tested by the kernel's own definition rather than its version
 * number, so a vendor kernel that backported the interface - or left it
 * out - is handled by what it actually has.
 *
 * WITHOUT IT vdisc still builds and works - mounting, reading, the TOC,
 * CD audio, multi-session, CD-TEXT and CD+G (those last two are read out
 * of the image in userspace and never touch the kernel) - but the
 * packet handler, the raw-read path behind it and the packet ops table
 * are not compiled, vdisc_packet=1 is ignored with a log line, and
 * ripping (cdparanoia, cdda2wav: CDROMREADAUDIO) and Video CD
 * (CDROMREADMODE2) are unavailable. design/39 3f/3g have the list.
 */
#ifdef CDC_GENERIC_PACKET
#define VDISC_HAVE_PACKET 1
#endif

/*
 * THE PACKET EXPERIMENT, DEFAULT OFF - see vdisc_generic_packet().
 *
 * Setting this advertises CDC_GENERIC_PACKET, which makes the uniform
 * layer route EIGHT CURRENTLY-WORKING IOCTLS through mmc_ioctl before
 * they reach the handlers they use today. The handler refuses
 * everything with -ENOTTY so they fall back, and the point of the run
 * is to prove that fallback works.
 *
 * Default off so an ordinary run is not carrying an experiment.
 */
static int vdisc_packet;

MODULE_PARM(vdisc_major, "i");
MODULE_PARM_DESC(vdisc_major, "block major number (default 44)");

MODULE_PARM(vdisc_ndevs, "i");
MODULE_PARM_DESC(vdisc_ndevs,
    "drives to advertise, 1.." __MODULE_STRING(VDISC_MAX_DEVS)
    " (default " __MODULE_STRING(VDISC_DEF_DEVS) "). One disc is the "
    "normal case; a second drive costs a node and desktop clutter, so "
    "ask for it. Discs attach and detach into these slots at runtime "
    "with no reload - only raising the COUNT needs one, because the "
    "CD-ROM layer registers each drive by name at init.");

MODULE_PARM(vdisc_trace, "i");
MODULE_PARM_DESC(vdisc_trace, "1 = log requests except the data reads, "
                 "2 = the data reads too (very noisy)");

MODULE_PARM(vdisc_packet, "i");
MODULE_PARM_DESC(vdisc_packet,
    "1 = answer MMC packet commands (default 0; VLHE loads with 1). "
    "Advertises CDC_GENERIC_PACKET and serves READ_CD - the CD audio "
    "and Mode 2 reads that rippers such as cdda2wav and cdparanoia, "
    "and Video CD players, send. Every other command is refused with "
    "-ENOTTY, so the kernel falls back to the ordinary ioctls. With "
    "vdisc_trace=1 every packet command is logged.");

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

struct vdisc_device {
    struct cdrom_device_info info;
    int  minor;
    int  attached;
    int  media_changed;
    int  tray_open;
    int  registered;
    /*
     * THE DOOR LOCK, AND IT IS OURS RATHER THAN THE LAYER'S.
     *
     * 2.2's uniform layer keeps `keeplocked' as ONE FILE-SCOPE STATIC
     * for every CD-ROM on the system (cdrom.c:260) and offers no way
     * to read it back. So a per-drive lock cannot be stored there,
     * and the GUI was keeping its own array and polling - which meant
     * OPENING the node, and every close ran the layer's release path
     * and its unconditional lock_door(0) (cdrom.c:660). Measured
     * 2026-09-19: a burst of lock_door(0) every ~101 jiffies, our own
     * GUI fighting its own lock.
     *
     * cdemu keeps the flag in the DEVICE and reports it back through
     * mode page 0x2A (device-commands.c:981), which is the
     * arrangement this copies: the driver owns the state, so it is
     * per-drive, readable, and survives anyone opening the node.
     */
    int  locked;

    /* geometry, from the daemon at attach */
    int  total_sectors;     /* lead-out lba                             */
    int  data_sectors;      /* mountable size, VDISC_SECTOR_SIZE units  */
    int  data_start;        /* file lba the block window begins at      */

    /*
     * TOC, cached at attach. Kept in the module deliberately:
     * audio_ioctl can be called with the device spinlock held, and a
     * round trip to the daemon could deadlock. The daemon sends the
     * whole table in struct vdisc_attach, so there is nothing to ask
     * for at ioctl time.
     */
    int  first_track;
    int  last_track;
    int  n_tracks;
    struct vdisc_track_info track[VDISC_MAX_TRACKS];
    /*
     * CDROMMULTISESSION's answer, derived once at attach from each
     * track's session byte: the first track of the highest session,
     * and whether there is more than one. ide-cd.c:1745 defines the
     * flag exactly so; isofs (inode.c:464) uses the address only when
     * it is set.
     */
    int  n_sessions;
    int  last_session_lba;

    /*
     * Audio state - ONLY what a poller needs without a round trip.
     *
     * vcd_mod.c also kept audio_start/end/pos and a jiffies, timestamp,
     * and computed the position here from elapsed time. That is wrong
     * now: the audio is in a vchan being mixed, and where the card has
     * actually reached is something vsound knows. The daemon owns the
     * position and pushes status changes down; SUBCHNL asks it - and
     * gets the child's write position, ~75 ms ahead of the ear (see the
     * top of this file, corrected 2026-10-04).
     */
    int  audio_status;      /* CDROM_AUDIO_*, pushed by the daemon      */
    int  audio_trk;         /* track playing, for SUBCHNL               */
    int  audio_pos;         /* lba the DAEMON last reported reaching    */
    /*
     * THE STALE-REPORT RULE - Acer run 85, 2026-09-15. The child
     * reports its position every chunk (about 107 ms) while playing.
     * When the media under it vanishes it blocks in an uninterruptible
     * read and the reports simply stop; nothing else changes, so the
     * drive said PLAY with a frozen position for two minutes and the
     * daemon found out only when told to shut down. The reports and
     * the clock are both here, so the rule lives here: SUBCHNL, which
     * every player polls, degrades a PLAY with no report for
     * VDISC_REPORT_STALE to ERROR (reported once, as any error) and
     * tells the daemon with VDISC_OP_STALL, once per play.
     */
    unsigned long audio_report_jiffies; /* when the daemon last reported */
    int  audio_stalled;     /* STALL sent for this play; sent once      */
    /*
     * The CD channel's volume, 0-255 per the CD-ROM API. Cached here so
     * CDROMVOLREAD answers without a round trip - audio_ioctl can be
     * called with locks held, same reason the TOC is cached.
     */
    int  vol_left;
    int  vol_right;
};

/*
 * Always VDISC_MAX_DEVS wide, whatever vdisc_ndevs says.
 *
 * This is the point vdisc.h's long comment is about: blk_size[major] is
 * handed to the block layer as a bare int * and indexed by MINOR with
 * no bound check (ll_rw_blk.c:646), so it must cover every minor a
 * device node can name - not merely the drives we advertise. Sizing
 * these by vdisc_ndevs would put an out-of-bounds read one mknod away.
 */
static struct vdisc_device vdisc_devs[VDISC_MAX_DEVS];

static int vdisc_blocksizes[VDISC_MAX_DEVS];
static int vdisc_hardsects[VDISC_MAX_DEVS];
static int vdisc_sizes[VDISC_MAX_DEVS];

/*
 * A request in flight. `req' is the block-layer request; `buf' points
 * into it. The daemon addresses these BY HANDLE, never by pointer, so a
 * confused or hostile daemon cannot make the kernel dereference an
 * address of its choosing. Kept exactly as it was - it was right.
 */
struct vdisc_pending {
    struct vdisc_pending *next;
    struct request       *req;
    __u32                 handle;
    int                   minor;
    int                   lba;
    int                   nsectors;  /* VDISC_SECTOR_SIZE units         */
    int                   offset;    /* byte offset within the first    */
    int                   length;    /* bytes wanted                    */
    char                 *buf;
    unsigned long         queued;    /* jiffies when queued - D07       */
};

/*
 * A SYNCHRONOUS RAW-AUDIO READ, for CDROMREADAUDIO via generic_packet.
 *
 * WHY IT IS NOT A vdisc_pending. That structure is built around a
 * `struct request': every completion path above calls
 * vdisc_end_request_locked(p->req, ...), and a packet read has no
 * block request to end. It has a CALLER, asleep, waiting for bytes.
 *
 * So the two differ in exactly the way that matters - how they finish -
 * and sharing the structure would mean a NULL-req branch at every
 * completion point, which is three places to get wrong instead of one
 * small structure.
 *
 * SLEEPING HERE IS SAFE, and that was checked rather than assumed: the
 * uniform layer reaches generic_packet from cdrom_ioctl, and
 * cdrom.c:1917 does a GFP_KERNEL kmalloc immediately before calling us
 * - the kernel's own code vouching for the context.
 *
 * ONE AT A TIME. `raw_busy' serialises; a second caller waits its
 * turn rather than queueing, because nothing issues concurrent raw
 * reads (a ripper is one process reading one track) and a queue would
 * be machinery with no user. If that changes, this is the place.
 */
struct vdisc_raw {
    __u32   handle;
    int     minor;
    int     lba;
    int     frames;
    /*
     * WHICH read, and therefore the UNIT of `frames' and the size of
     * each one:
     *
     *     VDISC_OP_READ_AUDIO  2352, AUDIO TRACKS ONLY
     *     VDISC_OP_READ_RAW    2352, any track
     *     VDISC_OP_READ_MODE2  2336, any track
     *
     * The three differ ONLY in the opcode sent and the multiplier used
     * to size the reply, so they share this structure and every line
     * of the wait path. The first two carry the same BYTES and differ
     * only in the daemon's refusal - see the cmd[1] decode in
     * vdisc_generic_packet().
     */
    __u32   opcode;
    __u32   want;               /* VDISC_SEC_* mask, READ_CD only     */
    int     unit;               /* bytes per frame/sector             */
    int     done;               /* set by the reply, tested on wake   */
    int     claimed;            /* a reply is writing into this - D67 */
    int     status;             /* 0 or -errno                        */
    unsigned char *buf;         /* kernel buffer, frames * unit       */
};

/* vdisc_raw_busy and vdisc_raw_sem are used only by vdisc_raw_read(),
 * which exists only with the packet interface; the reply path uses the
 * two pointers alone. Guarded one by one, in their original order, so a
 * 2.2.16 build lays the variables out exactly as before the gate. */
static struct vdisc_raw     *vdisc_raw_inflight;
#ifdef VDISC_HAVE_PACKET
static int                   vdisc_raw_busy;
#endif
static struct wait_queue    *vdisc_raw_wq;
#ifdef VDISC_HAVE_PACKET
static struct semaphore      vdisc_raw_sem = MUTEX;
#endif

/* `todo' awaits pickup by the daemon; `inflight' awaits its reply. */
static struct vdisc_pending *vdisc_todo_head;
static struct vdisc_pending *vdisc_todo_tail;
static struct vdisc_pending *vdisc_inflight;

/*
 * THE RECORDS COME FROM A FIXED POOL - design/54 D38, 2026-10-04.
 *
 * do_vdisc_request() runs with io_request_lock held and interrupts off,
 * so it allocated each record with kmalloc(GFP_ATOMIC) - and when that
 * failed under memory pressure the read failed with EIO. It need never
 * fail: every record belongs to one struct request taken off the queue,
 * and 2.2 has exactly NR_REQUEST (128) of those for the whole machine
 * (blk.h:22). So a pool that size cannot run dry.
 *
 * PLUS A MARGIN, because every completion path ends the request BEFORE
 * returning its record (end, then put), and in that window the kernel
 * may hand the freed slot to a new request. On this uniprocessor kernel
 * each path holds at most one record in that state, and there are four
 * (PUT_REPLY, vdisc_fail_all(), vdisc_expire(), GET_REQ's take-back);
 * 16 covers them with room. An empty pool is still refused as before -
 * a fault, said once - rather than trusted never to happen.
 *
 * Every get and put is under io_request_lock: get from the request
 * function, which already holds it; put through vdisc_pending_put(),
 * which takes it, or vdisc_pending_put_locked() where it is held.
 */
#define VDISC_POOL_SIZE  (NR_REQUEST + 16)

static struct vdisc_pending  vdisc_pool[VDISC_POOL_SIZE];
static struct vdisc_pending *vdisc_pool_free;
static int                   vdisc_pool_dry_said;

static void
vdisc_pool_init(void)
{
    int i;

    vdisc_pool_free = NULL;
    for (i = 0; i < VDISC_POOL_SIZE; i++) {
        vdisc_pool[i].next = vdisc_pool_free;
        vdisc_pool_free = &vdisc_pool[i];
    }
}

/* io_request_lock HELD. NULL only if the pool is dry, which it cannot
 * be - see above. */
static struct vdisc_pending *
vdisc_pending_get(void)
{
    struct vdisc_pending *p = vdisc_pool_free;

    if (p != NULL)
        vdisc_pool_free = p->next;
    else if (!vdisc_pool_dry_said++)
        printk(KERN_ERR "vdisc: the request pool ran dry - a read is"
                        " failed; this should not be possible\n");
    return p;
}

/* io_request_lock HELD. */
static void
vdisc_pending_put_locked(struct vdisc_pending *p)
{
    p->next = vdisc_pool_free;
    vdisc_pool_free = p;
}

static void
vdisc_pending_put(struct vdisc_pending *p)
{
    unsigned long flags;

    spin_lock_irqsave(&io_request_lock, flags);
    vdisc_pending_put_locked(p);
    spin_unlock_irqrestore(&io_request_lock, flags);
}

/*
 * Audio control messages.
 *
 * They carry no block request and expect no reply payload, so they
 * cannot ride on vdisc_pending, which is built around a struct request.
 * Fire-and-forget: audio_ioctl queues one and returns immediately,
 * never waiting for the daemon.
 *
 * THAT IS THE WHOLE POINT, and it is worth being explicit about because
 * it is the kernel-side half of section 3.1b. The data path's failure
 * mode is processes stuck in D state; playback must never be able to
 * cause it. The daemon forks a child for CDDA so a wedged read can be
 * killed without taking the daemon down - and this queue is why a
 * wedged child cannot reach back and hang an ioctl caller either. Both
 * halves are needed: containment in userspace, no waiting in here.
 *
 * Bounded, so a program spamming play/stop cannot make the kernel
 * allocate without limit.
 */
#define VDISC_MAX_AUDIO_MSGS  32

/*
 * How long a PLAYing drive may go without a position report before
 * SUBCHNL calls the audio child stalled. See the field's comment in
 * struct vdisc_device and the rule in CDROMSUBCHNL for why 10 s.
 */
#define VDISC_REPORT_STALE    (10 * HZ)

/*
 * How long a raw-audio read waits for the daemon before giving up.
 *
 * 10 s, the same figure and for the same reason as VDISC_REPORT_STALE
 * above: the Acer's IDE layer busy-waits 5 s per attempt under
 * CONFIG_APM, so a genuinely slow read can take seconds without
 * anything being wrong. Shorter would fail healthy reads on that
 * machine; longer is indistinguishable from a hang to the user.
 */
#define VDISC_RAW_TIMEOUT     (10 * HZ)

/*
 * AND A BLOCK READ IS BOUNDED TOO - design/54 D07, 2026-10-04 (the
 * user: "D07 do 10 second"). A daemon alive but wedged - stuck in a
 * read of a dead disk, stopped, livelocked - used to leave every reader
 * of the drive in D state for as long as it lasted: only the daemon
 * CLOSING failed what was queued (vdisc_fail_all()). The worker thread
 * now ticks once a second and fails, with EIO, any request older than
 * this. The same 10 s as the raw path, for its reason; a disk spinning
 * up from standby can take longer (2.2's ide.h allows 30 s), and the
 * user chose 10 knowing that.
 */
#define VDISC_DATA_TIMEOUT    (10 * HZ)

struct vdisc_audio_msg {
    struct vdisc_audio_msg *next;
    __u32 handle;
    __u32 opcode;
    int   minor;
    int   lba;
    int   end_lba;
    int   arg0;
};

static struct vdisc_audio_msg *vdisc_audio_head;
static struct vdisc_audio_msg *vdisc_audio_tail;
static int vdisc_audio_queued;

static __u32 vdisc_next_handle = 1;

static struct wait_queue *vdisc_thread_wq = NULL;  /* worker sleeps here */
static struct wait_queue *vdisc_daemon_wq = NULL;  /* daemon sleeps here */

static int vdisc_thread_pid;
static int vdisc_thread_running;
static int vdisc_thread_exit;
static struct semaphore vdisc_thread_sem = MUTEX_LOCKED;

static int vdisc_daemon_open;   /* is /dev/vdiscctl held by a daemon? */

/*
 * All queue manipulation happens under io_request_lock. The kernel is
 * uniprocessor (CONFIG_SMP is not set), so this is really about keeping
 * the interrupt-context request_fn from racing the worker thread.
 */

/* ------------------------------------------------------------------ *
 * Request completion
 * ------------------------------------------------------------------ */

/*
 * Complete a request. MUST be called with io_request_lock held.
 *
 * end_that_request_last() returns the request to the block layer's
 * global free list and does wake_up(&wait_for_request); both are
 * protected by that lock, so completing without it races the block
 * layer and can corrupt the free list. nbd does the same under the lock
 * (nbd.c:335).
 *
 * The caller holding the lock is also why the helper below re-acquires
 * it around completion rather than dropping it first.
 */
static void
vdisc_end_request(struct request *req, int uptodate)
{
    if (end_that_request_first(req, uptodate, DEVICE_NAME))
        return;
    end_that_request_last(req);
}

/* Complete from a context that does NOT already hold the lock. */
static void
vdisc_end_request_locked(struct request *req, int uptodate)
{
    unsigned long flags;

    spin_lock_irqsave(&io_request_lock, flags);
    vdisc_end_request(req, uptodate);
    spin_unlock_irqrestore(&io_request_lock, flags);
}

/* ------------------------------------------------------------------ *
 * Block request handling  (INTERRUPT CONTEXT - MUST NOT SLEEP)
 * ------------------------------------------------------------------ */

static void
do_vdisc_request(void)
{
    struct request *req;
    struct vdisc_device *dev;
    struct vdisc_pending *p;
    int minor, sectors_per_block;

    while (1) {
        /*
         * blk.h's INIT_REQUEST, spelled out against the runtime major
         * (see MAJOR_NR above). Same three checks, same panics.
         */
        req = VDISC_CURRENT;
        if (req == NULL)
            return;
        if (MAJOR(req->rq_dev) != vdisc_major)
            panic(DEVICE_NAME ": request list destroyed");
        if (req->bh && !buffer_locked(req->bh))
            panic(DEVICE_NAME ": block not locked");

        minor = MINOR(req->rq_dev);
        if (minor < 0 || minor >= vdisc_ndevs)
            goto fail;

        dev = &vdisc_devs[minor];
        if (!dev->attached || !vdisc_daemon_open)
            goto fail;
        if (req->cmd != READ)       /* read-only device */
            goto fail;

        /*
         * The block layer counts 512-byte sectors; a CD logical sector
         * is 2048. blksize_size is 2048 so requests are normally
         * 4-sector aligned, but handle the general case.
         */
        sectors_per_block = VDISC_SECTOR_SIZE / VDISC_HARDSECT;   /* 4 */

        p = vdisc_pending_get();          /* the pool - design/54 D38 */
        if (!p)
            goto fail;

        p->req      = req;
        p->handle   = vdisc_next_handle++;
        p->minor    = minor;
        /*
         * THE BLOCK DEVICE IS A WINDOW ONTO THE DATA TRACK, not onto
         * the file. Block LBA 0 is the data track's first sector, so
         * the track's start is added here - see vdisc.h's comment on
         * data_start for the mixed-mode disc this gets right and that
         * vcd_mod.c:306 got wrong.
         */
        p->lba      = dev->data_start + req->sector / sectors_per_block;
        p->offset   = (req->sector % sectors_per_block) * VDISC_HARDSECT;
        p->length   = req->current_nr_sectors * VDISC_HARDSECT;
        p->nsectors = (p->offset + p->length + VDISC_SECTOR_SIZE - 1)
                          / VDISC_SECTOR_SIZE;
        p->buf      = req->buffer;
        p->next     = NULL;
        p->queued   = jiffies;

        if (p->nsectors > VDISC_MAX_SECTORS_PER_REQ ||
            p->length > VDISC_MAX_XFER) {
            vdisc_pending_put_locked(p);  /* the lock is held here */
            goto fail;
        }

        /* Off the queue and onto ours; the thread does the sleeping. */
        VDISC_CURRENT = req->next;
        req->next = NULL;

        if (vdisc_todo_tail)
            vdisc_todo_tail->next = p;
        else
            vdisc_todo_head = p;
        vdisc_todo_tail = p;

        wake_up_interruptible(&vdisc_daemon_wq);
        wake_up(&vdisc_thread_wq);
        continue;

    fail:
        vdisc_end_request(req, 0);
    }
}

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/*
 * CD_FRAMES (75) and CD_MSF_OFFSET (150) come from the kernel's own
 * <linux/cdrom.h>:328-330, and THE KERNEL SIDE USES THOSE - one less
 * place for the two to drift, and they are fixed by the Red Book
 * anyway.
 *
 * vdisc.h defines VDISC_FRAMES_PER_SEC too, for userspace, which has no
 * <linux/cdrom.h>. Undefined first rather than left to redefine,
 * because a redefinition warning is noise that hides real ones. Same
 * value either way; this is about which header owns the constant.
 */
#undef  VDISC_FRAMES_PER_SEC
#define VDISC_FRAMES_PER_SEC  CD_FRAMES

/*
 * THE DRIVER ADDS THE +150, AND THE UNIFORM LAYER STRIPS IT.
 *
 * Backend LBAs are file-relative and stay that way. Getting this
 * backwards shifts every address by two seconds in both formats and
 * looks entirely plausible until diffed against a real drive.
 */
static void
vdisc_lba_to_msf(int lba, struct cdrom_msf0 *msf)
{
    int f = lba + CD_MSF_OFFSET;    /* 150 */

    if (f < 0)
        f = 0;
    msf->minute = f / (VDISC_FRAMES_PER_SEC * 60);
    msf->second = (f / VDISC_FRAMES_PER_SEC) % 60;
    msf->frame  = f % VDISC_FRAMES_PER_SEC;
}

static struct vdisc_device *
vdisc_from_info(struct cdrom_device_info *cdi)
{
    return (struct vdisc_device *) cdi->handle;
}

/*
 * IS THIS SECTOR IN ANY TRACK AT ALL? - design/54 D39. vdisc_track_at()
 * below answers first_track for a sector in none, which hides exactly
 * the case that matters: the gap between two sessions, which no track
 * owns (backend_ccd.c ends a session's last track at THAT session's
 * lead-out; backend_cue.c runs every track to the next, so it has no
 * holes; an ISO is one track). So in this table "in no track" means
 * "in a session gap". 1 or 0.
 */
static int
vdisc_lba_in_track(struct vdisc_device *dev, int lba)
{
    int i;

    for (i = 0; i < dev->n_tracks; i++)
        if (lba >= (int) dev->track[i].start_lba &&
            lba <  (int) (dev->track[i].start_lba + dev->track[i].length))
            return 1;
    return 0;
}

/* Which track an LBA falls in. first_track if it falls in none. */
static int
vdisc_track_at(struct vdisc_device *dev, int lba)
{
    int i;

    for (i = 0; i < dev->n_tracks; i++) {
        struct vdisc_track_info *t = &dev->track[i];

        if (lba >= (int) t->start_lba &&
            lba <  (int) (t->start_lba + t->length))
            return dev->first_track + i;
    }
    return dev->first_track;
}

/*
 * Queue an audio message for the daemon. Never waits - see the comment
 * on struct vdisc_audio_msg for why that is the whole point.
 *
 * Callable from ioctl context with locks held, so GFP_ATOMIC.
 */
static int
vdisc_audio_send(int minor, __u32 opcode, int lba, int end_lba, int arg0)
{
    struct vdisc_audio_msg *m;
    unsigned long flags;

    if (!vdisc_daemon_open)
        return -ENXIO;

    m = (struct vdisc_audio_msg *) kmalloc(sizeof *m, GFP_ATOMIC);
    if (!m)
        return -ENOMEM;

    m->handle  = 0;
    m->opcode  = opcode;
    m->minor   = minor;
    m->lba     = lba;
    m->end_lba = end_lba;
    m->arg0    = arg0;
    m->next    = NULL;

    spin_lock_irqsave(&io_request_lock, flags);
    if (vdisc_audio_queued >= VDISC_MAX_AUDIO_MSGS) {
        spin_unlock_irqrestore(&io_request_lock, flags);
        kfree(m);
        return -EBUSY;
    }
    m->handle = vdisc_next_handle++;
    if (vdisc_audio_tail)
        vdisc_audio_tail->next = m;
    else
        vdisc_audio_head = m;
    vdisc_audio_tail = m;
    vdisc_audio_queued++;
    spin_unlock_irqrestore(&io_request_lock, flags);

    wake_up_interruptible(&vdisc_daemon_wq);
    return 0;
}

/* ------------------------------------------------------------------ *
 * cdrom_device_ops
 * ------------------------------------------------------------------ */

/*
 * THE MODULE USE COUNT, TAKEN ONLY ON A SUCCESSFUL OPEN.
 *
 * design/26 B14, 2026-09-14. This used to take no count at all, on the
 * record that an earlier version's MOD_INC here "leaks on every FAILED
 * mount": on 2026-08-25 a mount failed and rmmod then refused. The
 * consequence of taking none was the opposite hole - with the daemon
 * gone and /dev/vdisc0 still MOUNTED, `rmmod vdisc' succeeded,
 * unregister_cdrom took the drive out of the layer's list, and the
 * later umount ran cdrom_release(), whose
 *
 *     struct cdrom_device_info *cdi = cdrom_find_device(dev);   NULL
 *     struct cdrom_device_ops *cdo = cdi->ops;                  oops
 *
 * (cdrom.c:650-651) has no NULL check. A kernel oops in umount, or in
 * shutdown's, from the ordinary end of a test session gone slightly
 * wrong. sr.c:401-403 holds its own count for exactly this reason.
 *
 * WHY THE LEAK DOES NOT COME BACK. Reading open_for_data() (cdrom.c:
 * 500-584) against what vdisc sets: every check that can fail AFTER
 * cdo->open is behind CDO_LOCK, and vdisc's options are 0. The media
 * checks that fail on an unattached drive run BEFORE cdo->open, so
 * this function is not called and nothing is counted. cdrom_release()
 * calls cdo->release on EVERY close (cdrom.c:667), not only the last,
 * so each counted open gets exactly one release. The earlier leak was
 * the increment sitting above the -ENOMEDIUM return below: an open
 * that this function itself refused had been counted.
 *
 * So: count after the checks, on the path that returns 0, and rmmod
 * refuses while anything holds the device - which is what a mounted
 * filesystem wants. cleanup_module cannot refuse in 2.2, so the count
 * is the only way to say no.
 */
static int
vdisc_open(struct cdrom_device_info *cdi, int purpose)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);

    if (!dev)
        return -ENXIO;
    if (!dev->attached)
        return -ENOMEDIUM;
    MOD_INC_USE_COUNT;
    return 0;
}

static void
vdisc_release(struct cdrom_device_info *cdi)
{
    (void) cdi;
    MOD_DEC_USE_COUNT;
}


static int
vdisc_drive_status(struct cdrom_device_info *cdi, int slot)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);

    if (!dev)
        return CDS_NO_INFO;
    if (dev->tray_open)
        return CDS_TRAY_OPEN;
    return dev->attached ? CDS_DISC_OK : CDS_NO_DISC;
}

/*
 * Report a change exactly once per attach.
 *
 * The uniform layer calls this to decide whether to invalidate the
 * buffer cache; clearing the flag here is what stops a re-read on every
 * poll. The daemon sets it at attach.
 */
static int
vdisc_media_changed(struct cdrom_device_info *cdi, int disc_nr)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);
    int changed;

    if (!dev)
        return 0;
    changed = dev->media_changed;
    dev->media_changed = 0;
    return changed;
}

/*
 * There is no tray. Track the flag anyway: eject and close are what a
 * desktop sends, and a drive that refuses them looks broken. Opening
 * the tray does NOT detach the image - the daemon owns that.
 */
static int
vdisc_tray_move(struct cdrom_device_info *cdi, int position)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);

    if (!dev)
        return -ENXIO;

    /*
     * A LOCKED DRIVE REFUSES, AND THAT IS THE POINT OF THE LOCK.
     *
     * cdemu does exactly this: unload_disc_private() checks its own
     * `locked' first and fails the eject with MEDIUM_REMOVAL_PREVENTED
     * (device-load.c:280). -EBUSY is the same answer in this API, and
     * it is what CDROMEJECT already returns from the layer when
     * keeplocked is set - so a client sees one consistent errno
     * whichever guard stops it.
     */
    if (position && dev->locked) {
        printk(KERN_INFO VSOUND_TS "vdisc: vdisc%d eject REFUSED"
                         " - drive is locked\n",
                   jiffies, dev->minor);
        return -EBUSY;
    }

    /*
     * CLOSING THE TRAY IS OURS TO DO; OPENING IT IS NOT.
     *
     * A close is just a flag - there is no medium to fetch, and the
     * layer calls it on open() to auto-close (cdrom.c:500). An OPEN
     * is a request to remove the medium, and only the detach can say
     * whether that happened, so `tray_open' is set by
     * vdisc_do_detach() when the kernel has actually let go.
     */
    if (!position)
        dev->tray_open = 0;

    /*
     * AN EJECT UNLOADS THE IMAGE - cdemu's model, and a real drive's.
     *
     * Every hardware driver in this tree stops the audio and removes
     * the medium (mcd.c:313, cdu31a.c:2759), so the TOC stops reading
     * because there is no disc. The daemon owns our image, so the
     * driver asks it to drop one; the request is fire-and-forget
     * because tray_move() is called from contexts that cannot wait.
     *
     * THE FLAG IS NOT SET HERE, AND THAT WAS A BUG ON 2026-09-19 -
     * the same shape as f0b6e5e one layer up. This used to set
     * tray_open before asking, so a detach the kernel then REFUSED
     * (-EBUSY, something still holding the drive) left the drive
     * advertising an open tray over an image that was still attached.
     * Measured: four `tray OPEN (image still attached)' lines against
     * four `NOT ejected - the kernel says it is in use' in the
     * daemon's log, same run.
     *
     * State follows the authority. The request goes out; if it
     * succeeds the daemon's DETACH sets the flag, and if it is
     * refused nothing changed and the disc is still there - which is
     * the truth either way.
     */
    if (position && dev->attached)
        (void) vdisc_audio_send(dev->minor, VDISC_OP_EJECT, 0, 0, 0);

    /*
     * LOGGED BECAUSE THE TRACE COULD NOT SEE THIS PATH AT ALL.
     * vdisc_audio_ioctl() names every ioctl it is handed, which made
     * the capture look complete - but CDROMEJECT and CDROM_LOCKDOOR
     * never reach it. The uniform layer handles both itself and calls
     * straight in here (cdrom.c:1466-1470), so a KsCD eject left no
     * mark of any kind and a run could not be told from one where it
     * never happened. Found 2026-09-19 trying to confirm exactly
     * that and having to report the logs could not answer it.
     *
     * `attached' is on the line because it is the whole question: an
     * open tray over a still-attached image is the zombie state the
     * user sees, where playback stops and the disc is still there.
     */
    printk(KERN_INFO VSOUND_TS "vdisc: vdisc%d tray %s (image %s)\n",
               jiffies,
           dev->minor, position ? "OPEN" : "closed",
           dev->attached ? "still attached" : "gone");
    return 0;
}

static int
vdisc_lock_door(struct cdrom_device_info *cdi, int lock)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);

    /*
     * WE HAVE NO DOOR, BUT WHO ASKS AND WHEN IS THE POINT.
     * `keeplocked' in the uniform layer is ONE FILE-SCOPE STATIC for
     * every drive (cdrom.c:260), not per-device, and only an explicit
     * CDROM_LOCKDOOR changes it. THE LAYER ITSELF calls this callback
     * too: lock on a data open (cdrom.c:567) and unlock on the last
     * close (cdrom.c:661) - and that unlock only while `keeplocked' is
     * CLEAR, so it never undoes a lock someone set.
     *
     * CORRECTED 2026-10-05: this said "KsCD locks on open and unlocks
     * on close, which clears a lock the user set". KsCD issues no
     * CDROM_LOCKDOOR at all (its Linux code's only door call is
     * CDROMEJECT; design/33 3j measured it weeks earlier) - the changes
     * around a KsCD open/close are the layer's own calls above. The
     * user confirmed it on 86Box: a lock held with KsCD open.
     *
     * VLHE NO LONGER SETS A LOCK - the GUI's Lock box was removed the
     * same day (design/54 D64). This callback stays because the layer
     * calls it, and `locked' still answers VDISC_IOC_GET_LOCK and
     * /proc/vdisc for anything that asks.
     */
    if (!dev)
        return -ENXIO;

    /*
     * RECORD IT. This used to `return 0' and keep nothing, on the
     * reasoning that a virtual drive has no door - true, and it made
     * the lock meaningless to everything below the layer's own
     * keeplocked check. An eject that reaches tray_move() has already
     * passed that check, so without a flag here the driver cannot
     * tell a permitted eject from one that should be refused.
     */
    /*
     * ON A CHANGE ONLY - 2026-10-01, design/47 G1. The uniform layer
     * calls lock_door(0) from cdrom_release() on every last close, so
     * a poller that opens and closes the node fifteen times a second
     * wrote this line fifteen times a second: 751-864 per trace. The
     * observation above is about the lock CHANGING, and that is when
     * it prints now.
     */
    if (dev->locked != (lock ? 1 : 0))
        printk(KERN_INFO VSOUND_TS "vdisc: vdisc%d lock_door(%d)\n",
                   jiffies,
               dev->minor, lock);
    dev->locked = lock ? 1 : 0;
    return 0;
}

/*
 * OUR ONE DEVICE-SPECIFIC IOCTL: read the drive's lock back.
 *
 * vdisc.h's VDISC_IOC_GET_LOCK has the why. In short: 2.2's
 * keeplocked is a single global with no read-back, so the GUI was
 * drawing its Lock checkbox from a cache it did not exclusively own
 * and the box could disagree with the kernel. `dev->locked' is the
 * real per-drive flag; this hands it over.
 *
 * ANYTHING ELSE IS -ENOTTY, not -EINVAL. The layer reaches here only
 * for codes it does not recognise itself, so an unknown one is "this
 * device has no such ioctl" - which is what ENOTTY means and what a
 * caller probing for a feature expects.
 */
static int
vdisc_dev_ioctl(struct cdrom_device_info *cdi, unsigned int cmd,
                unsigned long arg)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);

    if (!dev)
        return -ENXIO;

    switch (cmd) {
    case VDISC_IOC_GET_LOCK:
        return put_user(dev->locked ? 1 : 0, (int *) arg);
    default:
        return -ENOTTY;
    }
}

static int
vdisc_get_last_session(struct cdrom_device_info *cdi,
                       struct cdrom_multisession *ms)
{
    /*
     * How isofs finds a second session: it asks here, and if xa_flag
     * is set it reads the volume descriptor at addr.lba + 16 as an
     * ABSOLUTE block address (fs/isofs/inode.c:464, :549). So this
     * only works because block LBA equals disc LBA - see vdisc.h on
     * data_start. A single-session disc answers 0 with the flag clear,
     * which is what ide-cd answers for one, and isofs then reads from
     * 16 as it always did. design/09's defect 2, fixed 2026-09-15.
     */
    struct vdisc_device *dev = vdisc_from_info(cdi);

    if (!dev || !dev->attached)
        return -ENOMEDIUM;

    ms->addr_format = CDROM_LBA;
    ms->addr.lba    = dev->last_session_lba;
    ms->xa_flag     = dev->n_sessions > 1 ? 1 : 0;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Audio ioctls
 *
 * NEVER WAIT IN HERE. Every command that reaches the daemon is queued
 * fire-and-forget; nothing in this file blocks on userspace. A wedged
 * CDDA child must not be able to put an ioctl caller into D state.
 * ------------------------------------------------------------------ */

static const char *
vdisc_status_name(int s)
{
    switch (s) {
    case CDROM_AUDIO_INVALID:   return "INVALID";
    case CDROM_AUDIO_PLAY:      return "PLAY";
    case CDROM_AUDIO_PAUSED:    return "PAUSED";
    case CDROM_AUDIO_COMPLETED: return "COMPLETED";
    case CDROM_AUDIO_ERROR:     return "ERROR";
    case CDROM_AUDIO_NO_STATUS: return "NO_STATUS";
    default:                    return "?";
    }
}

/*
 * WHERE THE AUDIO IS - THE ONE ANSWER, FOR SUBCHNL AND FOR /proc/vdisc.
 *
 * Factored out 2026-10-01 so the proc entry below reports exactly what
 * the ioctl would: the daemon's last position, the stale-report rule
 * (which may move the drive to ERROR and tell the daemon once - a
 * state transition, so it belongs to whichever reader arrives first,
 * as it always did), and the track clamp. What is NOT here is the
 * "ERROR is reported once" step: that is a CONSUMPTION, and only the
 * player's ioctl may do it, or a /proc reader would eat an error the
 * player was about to see.
 */
static void
vdisc_audio_sample(struct vdisc_device *dev, int *pos_out, int *trk_out)
{
    int pos, trk;

    pos = dev->audio_pos;

    /*
     * THE STALE-REPORT RULE (the field's comment has the run).
     * PLAY with no report for VDISC_REPORT_STALE means the child
     * is no longer feeding the channel, whatever the reason. The
     * drive goes to ERROR - which the once-only rule below turns
     * into NO_STATUS on this same read, so the player sees one
     * error and then a stopped drive, exactly as it does when the
     * child reports a read failure itself - and the daemon is told
     * once, so it can stop sending commands into a pipe nobody
     * will read and can detach at once when asked to shut down.
     *
     * 10 s, NOT 3: the Acer's IDE layer busy-waits 5 s per attempt
     * on a vanished drive (ide_wait_stat, WAIT_READY under
     * CONFIG_APM) and NOTHING in user space runs during it - the
     * pump included (run 85: xruns and no-reader discards). A
     * healthy child silenced by that freeze must not be declared
     * dead by it. Ten seconds is two freezes with room to spare,
     * and still within a poll or two of what a player shows.
     *
     * PAUSED is excluded: it sends no reports by design.
     */
    if (dev->audio_status == CDROM_AUDIO_PLAY &&
        (long) (jiffies - dev->audio_report_jiffies) >
            (long) VDISC_REPORT_STALE) {
        printk(KERN_INFO VSOUND_TS "vdisc: vdisc%d: no position"
               " report for %d s while playing - the audio child"
               " has stalled, status now ERROR\n",
               jiffies, dev->minor, (int) (VDISC_REPORT_STALE / HZ));
        dev->audio_status = CDROM_AUDIO_ERROR;
        if (!dev->audio_stalled) {
            dev->audio_stalled = 1;
            (void) vdisc_audio_send(dev->minor, VDISC_OP_STALL,
                                    pos, 0, 0);
        }
    }

    trk = vdisc_track_at(dev, pos);

    /*
     * Clamp to the track that was asked for while a play is in
     * progress. Following the position is right for a multi-track
     * play - the reported track really should advance - but a poll
     * landing on a boundary, or after a stop when the position is
     * no longer inside the played range, must never name a track
     * outside it.
     *
     * THIS IS NOT DEFENSIVE PADDING. On a game disc track 1 is
     * DATA, so such a sample told the caller an audio track had
     * become a data track, and Quake pauses on exactly that. It was
     * found in the field and the fix is kept verbatim.
     */
    if ((dev->audio_status == CDROM_AUDIO_PLAY ||
         dev->audio_status == CDROM_AUDIO_PAUSED) &&
        dev->audio_trk >= dev->first_track &&
        dev->audio_trk <= dev->last_track) {
        if (trk < dev->audio_trk)
            trk = dev->audio_trk;
        if (dev->track[trk - dev->first_track].is_data)
            trk = dev->audio_trk;
    }

    if (trk < dev->first_track || trk > dev->last_track)
        trk = dev->first_track;
    *pos_out = pos;
    *trk_out = trk;

}

static int
vdisc_audio_ioctl(struct cdrom_device_info *cdi, unsigned int cmd, void *arg)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);

    if (!dev)
        return -ENXIO;
    if (!dev->attached)
        return -ENOMEDIUM;

    /* SUBCHNL is excluded: it is polled continuously - Quake calls it
     * every frame - and would drown everything else. */
    if (vdisc_trace && cmd != CDROMSUBCHNL) {
        const char *n = "?";
        switch (cmd) {
        case CDROMPLAYTRKIND:   n = "PLAYTRKIND";   break;
        case CDROMPLAYMSF:      n = "PLAYMSF";      break;
        case CDROMPAUSE:        n = "PAUSE";        break;
        case CDROMRESUME:       n = "RESUME";       break;
        case CDROMSTOP:         n = "STOP";         break;
        case CDROMSTART:        n = "START";        break;
        case CDROMVOLCTRL:      n = "VOLCTRL";      break;
        case CDROMREADTOCHDR:   n = "READTOCHDR";   break;
        case CDROMREADTOCENTRY: n = "READTOCENTRY"; break;
        default: break;
        }
        printk(KERN_INFO VSOUND_TS "vdisc: ioctl %s (status now %s)\n",
               jiffies,
               n, vdisc_status_name(dev->audio_status));
    }

    switch (cmd) {

    case CDROMREADTOCHDR: {
        struct cdrom_tochdr *hdr = (struct cdrom_tochdr *) arg;

        hdr->cdth_trk0 = dev->first_track;
        hdr->cdth_trk1 = dev->last_track;
        return 0;
        }

    case CDROMREADTOCENTRY: {
        struct cdrom_tocentry *e = (struct cdrom_tocentry *) arg;
        struct vdisc_track_info *t = NULL;      /* NULL for the lead-out */
        int lba, idx;

        if (e->cdte_track == CDROM_LEADOUT) {
            lba = dev->total_sectors;
            /* The lead-out carries the data bit of nothing; report it
             * as a data track, which is what real drives do. */
            e->cdte_ctrl = CDROM_DATA_TRACK;
            e->cdte_adr  = 1;
        } else {
            if (e->cdte_track < dev->first_track ||
                e->cdte_track > dev->last_track)
                return -EINVAL;

            idx = e->cdte_track - dev->first_track;
            if (idx < 0 || idx >= dev->n_tracks)    /* belt and braces */
                return -EINVAL;
            t = &dev->track[idx];

            lba = t->start_lba;
            /*
             * ALL FOUR Q-CHANNEL CONTROL BITS. The daemon sends three
             * of them in `control' (pre-emphasis, copy-permitted,
             * four-channel) and the data bit in is_data, which is
             * what the block path keys on; they are recombined here.
             * design/27 4b: copy-permitted and four-channel used to
             * be dropped and always reported 0.
             *
             * The nibble is masked, because it reaches a 4-bit
             * bitfield (cdrom.h's cdte_ctrl) and an untrusted daemon
             * fills it.
             */
            e->cdte_ctrl = t->control & (VDISC_CTRL_PRE_EMPHASIS |
                                         VDISC_CTRL_COPY_PERMIT |
                                         VDISC_CTRL_FOUR_CHANNEL);
            if (t->is_data)
                e->cdte_ctrl |= CDROM_DATA_TRACK;
            e->cdte_adr  = 1;
        }

        e->cdte_format = CDROM_MSF;
        vdisc_lba_to_msf(lba, &e->cdte_addr.msf);
        /* sr_ioctl.c:357 derives this as data-track-or-not; we know
         * more. A Mode 2 (XA) track says 2, a Mode 1 track 1, audio 0.
         * The daemon tells us which per track (VDISC_FORM_MODE2); the
         * Form 1/2 split within a Mode 2 track stays its business. */
        if (!(e->cdte_ctrl & CDROM_DATA_TRACK))
            e->cdte_datamode = 0;
        else if (t && t->sector_form == VDISC_FORM_MODE2)
            e->cdte_datamode = 2;
        else
            e->cdte_datamode = 1;
        return 0;
        }

    case CDROMPLAYTRKIND: {
        struct cdrom_ti *ti = (struct cdrom_ti *) arg;
        struct vdisc_track_info *t0, *t1;
        int i0, i1, i, start, end, rc;

        if (ti->cdti_trk0 < dev->first_track ||
            ti->cdti_trk1 > dev->last_track ||
            ti->cdti_trk0 > ti->cdti_trk1)
            return -EINVAL;

        i0 = ti->cdti_trk0 - dev->first_track;
        i1 = ti->cdti_trk1 - dev->first_track;
        if (i0 < 0 || i0 >= dev->n_tracks ||
            i1 < 0 || i1 >= dev->n_tracks)
            return -EINVAL;
        t0 = &dev->track[i0];
        t1 = &dev->track[i1];

        /*
         * REFUSE TO PLAY A DATA TRACK.
         *
         * vcd_mod.c checked the track NUMBER was in range and then
         * played whatever was there. The is_data flag was parsed
         * correctly from the CCD Control nibble and reported correctly
         * by READTOCENTRY - but nothing enforced it, so a player that
         * seeks by LBA rather than consulting the TOC got 2352-byte
         * frames of filesystem streamed to the card as if they were
         * samples.
         *
         * Observed on the target: the data track of a game disc was
         * playable in a CD player on both 86Box and the Acer. That it
         * happened on both is what says it is ours and not an
         * emulation artefact.
         *
         * Real drives return an error here.
         */
        if (t0->is_data)
            return -EINVAL;

        start = t0->start_lba;
        end   = t1->start_lba + t1->length;

        /*
         * Stop at the first data track rather than playing into it. A
         * multi-track play that spans one should end where the audio
         * ends - which is what a real drive does, and what an
         * application asking for "tracks 1 to 5" on a mixed disc
         * expects.
         */
        for (i = i0; i <= i1; i++) {
            if (dev->track[i].is_data) {
                end = dev->track[i].start_lba;
                break;
            }
        }
        if (end <= start)
            return -EINVAL;

        rc = vdisc_audio_send(dev->minor, VDISC_OP_PLAY, start, end,
                              ti->cdti_trk0);
        if (rc)
            return rc;

        /*
         * STATUS, TRACK AND POSITION FROM THE SAME REQUEST. A drive's
         * PLAY is a seek: from the ioctl on, its subchannel reports the
         * head at the START address, and the track and the position
         * describe one moment. This set the status and the track here
         * and left the position to the child's first report - after
         * the chunk it was in, ~110 ms on 86Box - so SUBCHNL said
         * "playing track 3" with an address inside track 2, which no
         * drive does. KsCD derives its track from the position and
         * pressed next into the track it was already on (86Box run
         * 81, 2026-09-15). The child's reports take over from here.
         */
        dev->audio_status = CDROM_AUDIO_PLAY;
        dev->audio_trk    = ti->cdti_trk0;
        dev->audio_pos    = start;
        dev->audio_report_jiffies = jiffies;    /* the stale rule's clock */
        dev->audio_stalled = 0;
        return 0;
        }

    case CDROMPLAYMSF: {
        struct cdrom_msf *m = (struct cdrom_msf *) arg;
        int start, end, trk, rc;

        /*
         * Inbound MSF carries the +150 and we strip it, the mirror of
         * vdisc_lba_to_msf(). The layer hands us MSF whatever the
         * caller asked for.
         */
        start = (m->cdmsf_min0 * 60 + m->cdmsf_sec0) * VDISC_FRAMES_PER_SEC
                    + m->cdmsf_frame0 - CD_MSF_OFFSET;
        end   = (m->cdmsf_min1 * 60 + m->cdmsf_sec1) * VDISC_FRAMES_PER_SEC
                    + m->cdmsf_frame1 - CD_MSF_OFFSET;

        if (start < 0 || end <= start || end > dev->total_sectors)
            return -EINVAL;

        /* NOT FROM INSIDE A SESSION GAP - design/54 D39. No track owns
         * it, and vdisc_track_at()'s fall-back to the first track would
         * otherwise let it through as track 1. */
        if (!vdisc_lba_in_track(dev, start))
            return -EINVAL;

        /*
         * Same rule as PLAYTRKIND, reached by address rather than by
         * track number - which is exactly how a player that ignores the
         * TOC arrives here.
         */
        trk = vdisc_track_at(dev, start);
        if (trk >= dev->first_track && trk <= dev->last_track &&
            dev->track[trk - dev->first_track].is_data)
            return -EINVAL;

        /*
         * AND THE END IS CLIPPED AT THE FIRST DATA TRACK, as PLAYTRKIND
         * does. design/26 B6, fixed 2026-09-14: only the START was
         * checked, so a range that crossed into a data track played it
         * - Mode-1 sectors, sync patterns and ECC, as 44.1 kHz PCM at
         * full level. KsCD plays its LAST playlist track to the disc
         * end through PLAYMSF (kscd/cdrom.c:586, workman's core), so on
         * any disc with a data track AFTER its audio it reached this.
         * Every disc this project holds is data-first, which is why no
         * run showed it.
         */
        {
            int i;

            for (i = 0; i < dev->n_tracks; i++) {
                if (dev->track[i].is_data &&
                    dev->track[i].start_lba > start &&
                    dev->track[i].start_lba < end)
                    end = dev->track[i].start_lba;
            }
        }

        /*
         * AND AT THE END OF A SESSION - design/54 D39, 2026-10-04 (the
         * user: "D39 do B"). KsCD takes a track's end from the NEXT
         * track's start, so on a multi-session disc its play of the last
         * track of session 1 ran on through the gap to session 2: run 92
         * (tests/logs/2026-09-15-86box-92-bluebook-multisession) played
         * track 2 for ~49 s where it is 40, the 750-sector gap included;
         * on a real Enhanced CD that gap is some 2.5 minutes. A track
         * whose end is followed by a sector in no track ends its
         * session, and the play stops there - where a real drive stops,
         * so the child reports COMPLETED, which KsCD expects. Refusing
         * the gap's sectors during the play instead (design/26 B18's
         * preference) ends in ERROR, which KsCD shows as "Ejected" and
         * locks the player out over (run 77).
         */
        {
            int i;

            for (i = 0; i < dev->n_tracks; i++) {
                int tend = (int) (dev->track[i].start_lba
                                  + dev->track[i].length);

                if (tend > start && tend < end
                    && !vdisc_lba_in_track(dev, tend))
                    end = tend;
            }
        }

        rc = vdisc_audio_send(dev->minor, VDISC_OP_PLAY, start, end,
                              trk);

        if (rc)
            return rc;

        dev->audio_status = CDROM_AUDIO_PLAY;
        dev->audio_trk    = trk;
        dev->audio_pos    = start;      /* as PLAYTRKIND: one moment */
        dev->audio_report_jiffies = jiffies;
        dev->audio_stalled = 0;
        return 0;
        }

    case CDROMPAUSE:
        /* Not stamped: PAUSED sends no reports and is not subject to
         * the stale rule. RESUME below stamps, or a long pause would
         * be stale the moment it resumed. */
        if (dev->audio_status != CDROM_AUDIO_PLAY)
            return 0;
        dev->audio_status = CDROM_AUDIO_PAUSED;
        return vdisc_audio_send(dev->minor, VDISC_OP_PAUSE, 0, 0, 0);

    case CDROMRESUME:
        if (dev->audio_status != CDROM_AUDIO_PAUSED)
            return 0;
        dev->audio_status = CDROM_AUDIO_PLAY;
        dev->audio_report_jiffies = jiffies;
        return vdisc_audio_send(dev->minor, VDISC_OP_RESUME, 0, 0, 0);

    case CDROMSTOP:
        dev->audio_status = CDROM_AUDIO_NO_STATUS;
        return vdisc_audio_send(dev->minor, VDISC_OP_STOP, 0, 0, 0);

    case CDROMSTART:
        /* "Spin up." Nothing to do, and refusing it looks broken. */
        return 0;

    case CDROMVOLCTRL: {
        struct cdrom_volctrl *v = (struct cdrom_volctrl *) arg;
        int l = v->channel0, r = v->channel1;

        /*
         * THE CD CHANNEL'S VOLUME. This is what the ioctl has always
         * meant, and vdiscd holds exactly one channel - so there is
         * something to do here.
         *
         * It was a no-op until 2026-08-26, on the reasoning that vcdd
         * implemented it badly (it set the CARD's global PCM). That
         * criticism was right; the conclusion was not. A user moved
         * KsCD's slider 128 times in one run with no effect.
         *
         * BALANCE IS REFUSED RATHER THAN FAKED. A vsound channel has
         * ONE volume, so unequal left and right cannot be honoured.
         * xmcd handles that properly - on failure it retries with the
         * channels equal and warps its balance slider to centre
         * (slioc.c:1385-1405) - so refusing tells the truth and the
         * client corrects itself. Accepting and taking the louder side
         * would leave its slider claiming a balance that does nothing.
         */
        if (l != r)
            return -EINVAL;
        if (l < 0 || l > 255)
            return -EINVAL;

        dev->vol_left  = l;
        dev->vol_right = r;

        /*
         * Fire-and-forget like the transport commands - see the comment
         * on struct vdisc_audio_msg. A volume change must never block
         * an ioctl caller behind a wedged child.
         */
        return vdisc_audio_send(dev->minor, VDISC_OP_VOLCTRL, 0, 0, l);
        }

    case CDROMVOLREAD: {
        struct cdrom_volctrl *v = (struct cdrom_volctrl *) arg;

        /*
         * ANSWERED FROM THE CACHE, never from the daemon: audio_ioctl
         * can be called with locks held. KsCD never asks - it pushes at
         * startup and keeps its own slider - but a client that does ask
         * should see the real value, including one set by the volume
         * tool rather than by a CD player.
         */
        v->channel0 = v->channel2 = (__u8) dev->vol_left;
        v->channel1 = v->channel3 = (__u8) dev->vol_right;
        return 0;
        }

    case CDROMSUBCHNL: {
        struct cdrom_subchnl *q = (struct cdrom_subchnl *) arg;
        struct vdisc_track_info *t;
        int pos, trk, idx, rel;

        /*
         * THE POSITION COMES FROM THE DAEMON, NOT FROM JIFFIES.
         *
         * vcd_mod.c computed it here as elapsed_jiffies * 75/HZ, on the
         * reasoning that CDDA runs at a fixed 75 frames a second and the
         * OSS buffer paces the writer. That held when the daemon wrote
         * /dev/dsp itself. It does not hold now: the audio goes into a
         * vchan and is mixed, so what the CARD has reached is something
         * vsound knows and a clock in here does not.
         *
         * This is the same mistake as predicting a play position rather
         * than asking for it, which cost this project real time on the
         * sound side before GETOPTR replaced the prediction (section 6).
         * Not repeating it: the daemon pushes the position down as it
         * feeds the channel, and we report what it last said.
         */
        vdisc_audio_sample(dev, &pos, &trk);
        idx = trk - dev->first_track;
        t   = &dev->track[idx];

        rel = pos - (int) t->start_lba;
        if (rel < 0)
            rel = 0;

        q->cdsc_audiostatus = dev->audio_status;
        /*
         * ERROR IS REPORTED ONCE. A drive's audio-status byte says
         * "stopped due to error" on the request that finds it and "no
         * status" after that; ours kept saying ERROR until the next
         * report, and KsCD maps a status it does not know to "Ejected"
         * and then sends nothing at all (workman's play_cd returns
         * early in that state), so one error locked the player out
         * for the session - 86Box run 77, 2026-09-15. COMPLETED stays
         * until the next command, as mcd.c keeps it: Quake advances
         * only on seeing it, and a second poller must not eat it.
         */
        if (dev->audio_status == CDROM_AUDIO_ERROR)
            dev->audio_status = CDROM_AUDIO_NO_STATUS;
        q->cdsc_adr  = 1;

        /* The same nibble READTOCENTRY reports, and for the same
         * reason it is masked - invariant 2: adr and ctrl are two
         * nibbles of one Q-channel byte and are set together. */
        q->cdsc_ctrl = t->control & (VDISC_CTRL_PRE_EMPHASIS |
                                     VDISC_CTRL_COPY_PERMIT |
                                     VDISC_CTRL_FOUR_CHANNEL);
        if (t->is_data)
            q->cdsc_ctrl |= CDROM_DATA_TRACK;
        q->cdsc_trk  = trk;
        q->cdsc_ind  = 1;

        /*
         * The layer forces MSF in and converts out, exactly as for
         * READTOCENTRY. The ABSOLUTE address carries the +150; the
         * track-relative one does NOT - it is a duration, not a disc
         * address.
         */
        q->cdsc_format = CDROM_MSF;
        vdisc_lba_to_msf(pos, &q->cdsc_absaddr.msf);
        q->cdsc_reladdr.msf.minute = rel / (VDISC_FRAMES_PER_SEC * 60);
        q->cdsc_reladdr.msf.second = (rel / VDISC_FRAMES_PER_SEC) % 60;
        q->cdsc_reladdr.msf.frame  = rel % VDISC_FRAMES_PER_SEC;
        return 0;
        }

    default:
        return -ENOSYS;
    }
}

#ifdef VDISC_HAVE_PACKET     /* to the end of vdisc_generic_packet() */

/* ------------------------------------------------------------------ *
 * The synchronous raw-audio read
 * ------------------------------------------------------------------ */

/*
 * Ask the daemon for `frames' raw frames at `lba' and sleep until they
 * arrive. Returns 0 and fills buf, or -errno.
 *
 * TIMED OUT, NOT OPEN-ENDED. design/27 section 9 records that a daemon
 * which is ALIVE BUT WEDGED is the one failure vdisc does not recover
 * from - it still holds the pipe, so vdisc_fail_all() never runs, and
 * a caller waits forever. That is the Acer run 85 shape. This is a new
 * waiting path and it is not going to add another way to reach D
 * state, so the sleep is bounded and a timeout is an error like any
 * other. MMC-2 4.1.6 makes the same argument for real drives.
 */
static int
vdisc_raw_read(int minor, int lba, int frames, unsigned char *buf,
               __u32 opcode, int unit, __u32 want)
{
    struct vdisc_raw raw;
    unsigned long flags;
    int ret, interrupted = 0;

    if (frames <= 0 || frames > VDISC_MAX_RAW_FRAMES)
        return -EINVAL;
    if (unit <= 0)
        return -EINVAL;

    /* One at a time - see the structure's comment. */
    down(&vdisc_raw_sem);

    memset(&raw, 0, sizeof raw);
    raw.minor  = minor;
    raw.lba    = lba;
    raw.frames = frames;
    raw.opcode = opcode;
    raw.want   = want;
    raw.unit   = unit;
    raw.buf    = buf;
    raw.done   = 0;
    raw.status = 0;

    spin_lock_irqsave(&io_request_lock, flags);
    if (!vdisc_daemon_open) {
        spin_unlock_irqrestore(&io_request_lock, flags);
        up(&vdisc_raw_sem);
        return -ENXIO;
    }
    raw.handle = vdisc_next_handle++;
    vdisc_raw_inflight = &raw;
    vdisc_raw_busy = 1;
    spin_unlock_irqrestore(&io_request_lock, flags);

    /* Wake the daemon's GET_REQ, which will pick this up. */
    wake_up_interruptible(&vdisc_daemon_wq);

    /*
     * Wait, bounded. interruptible_sleep_on_timeout returns the
     * remaining jiffies, or 0 if the time ran out.
     */
    /*
     * A SIGNAL ENDS THE WAIT - design/54 D69, 2026-10-04. With one
     * pending, interruptible_sleep_on_timeout() returns at once with the
     * time nearly unspent, so this loop SPUN for up to the whole 10 s -
     * Ctrl+C on a ripper burned the CPU instead of stopping it. Now a
     * signal leaves the loop and the read returns -EINTR, unless a reply
     * has already claimed the request (D67): then the claimed wait below
     * finishes it and the reply's own result is returned.
     */
    {
        unsigned long left = VDISC_RAW_TIMEOUT;
        while (!raw.done && left > 0) {
            left = interruptible_sleep_on_timeout(&vdisc_raw_wq, left);
            if (!raw.done && signal_pending(current)) {
                interrupted = 1;
                break;
            }
        }
    }

    spin_lock_irqsave(&io_request_lock, flags);
    /*
     * A REPLY THAT HAS CLAIMED THIS IS STILL WRITING INTO IT - design/54
     * D67, 2026-10-04. `raw' is on THIS stack and `buf' is the caller's;
     * vdisc_do_put_reply() claims the request under this lock and then
     * copies with the lock dropped, and that copy can sleep on a page
     * fault. Returning now would leave it writing into a dead frame and
     * a freed buffer. So once claimed we wait for it to finish, however
     * late - the copy is one bounded transfer from the daemon. Polled at
     * HZ/10 and uninterruptible: a wake that lands between the test and
     * the sleep cannot strand us, and a pending signal cannot spin us.
     * An UNCLAIMED request is ours to abandon, as before: the pointer is
     * cleared under the lock and a late reply finds no handle.
     */
    while (!raw.done && raw.claimed) {
        spin_unlock_irqrestore(&io_request_lock, flags);
        sleep_on_timeout(&vdisc_raw_wq, HZ / 10);
        spin_lock_irqsave(&io_request_lock, flags);
    }
    vdisc_raw_inflight = NULL;
    vdisc_raw_busy = 0;
    ret = raw.done ? raw.status : interrupted ? -EINTR : -ETIMEDOUT;
    spin_unlock_irqrestore(&io_request_lock, flags);

    if (!raw.done && !interrupted)
        printk(KERN_ERR VSOUND_TS "vdisc: raw read at lba %d timed out"
                        " after %d s - the daemon did not answer\n",
               jiffies, lba, (int) (VDISC_RAW_TIMEOUT / HZ));

    up(&vdisc_raw_sem);
    return ret;
}

/* ------------------------------------------------------------------ *
 * generic_packet - THE SKELETON. It refuses everything, on purpose.
 * ------------------------------------------------------------------ */

/*
 * WHAT THIS IS FOR, AND WHY IT ANSWERS NOTHING YET.
 *
 * Setting CDC_GENERIC_PACKET does not merely ADD commands. cdrom_ioctl
 * (cdrom.c:1684) then routes EVERY ioctl through mmc_ioctl FIRST, and
 * mmc_ioctl has its own cases for eight commands we already implement
 * and that already work:
 *
 *     CDROMPLAYMSF   CDROMPLAYBLK   CDROMVOLCTRL   CDROMVOLREAD
 *     CDROMSTART     CDROMSTOP      CDROMPAUSE     CDROMRESUME
 *
 * Each becomes a packet command and arrives here as bytes. The escape
 * is the kernel's own, and cdrom.c:1680 states it:
 *
 *     "if -ENOTTY is returned that particular ioctl is not implemented
 *      and we let it go through the device specific ones."
 *
 * SO THE WHOLE POINT OF THIS FUNCTION, TODAY, IS TO RETURN -ENOTTY AND
 * PROVE THAT FALLBACK RUNS. The read path is deliberately not here:
 * writing it before the reroute is observed would mix a new feature
 * with the risk of breaking playback, which is the project's core
 * function and works today.
 *
 * design/27 section 8 scopes what comes next (~170 lines: a
 * VDISC_OP_READ_AUDIO opcode on the daemon pipe, the kernel side of
 * that round trip, and a GPCMD_READ_CD case here). The image side is
 * already done - read_audio exists in every backend at 2352 bytes and
 * playback exercises it daily.
 *
 * THE ACCEPTANCE TEST IS ALREADY CAPTURED. 86Box run 98 straced
 * cdparanoia against a drive at major 25 with this flag OFF: every
 * ioctl we implement returned 0, and only CDROMREADAUDIO returned
 * ENOSYS. The same strace with vdisc_packet=1 must show the same
 * zeros. If it does not, the reroute is the problem and the tag
 * `pre-generic-packet' is where to go back to.
 *
 * GATED BEHIND A PARAMETER, DEFAULT OFF, so an ordinary run is not
 * carrying an experiment. Nothing changes until vdisc_packet=1.
 */
static int
vdisc_generic_packet(struct cdrom_device_info *cdi,
                     struct cdrom_generic_command *cgc)
{
    struct vdisc_device *dev = vdisc_from_info(cdi);
    unsigned int op;

    if (cgc == NULL)
        return -EINVAL;

    op = (unsigned int) cgc->cmd[0];

    /*
     * TRACE EVERY OPCODE, ACCEPTED OR REFUSED. This is the data the
     * skeleton exists to collect: which commands the layer actually
     * diverts here on a real machine with real players. Reading
     * mmc_ioctl says eight; only a run can say which eight arrive,
     * in what order, and from whom.
     *
     * Not rate-limited. It excluded nothing at first, on the grounds
     * that nothing polled this path and there was nothing to drown in;
     * "if something turns out to, that is itself a finding worth
     * having" - and it was (below).
     */
    /*
     * LEVEL 2 FOR THE DATA READS - 2026-10-02. "Nothing polls this path
     * yet" stopped being true: a ripper or a CD player streaming audio
     * sends READ_CD for every few sectors, and one run logged 183,077
     * of them - burying the ten lines a test wanted. READ_CD, its MSF
     * form, READ_10 and READ_SUBCHANNEL now need vdisc_trace=2; every
     * other packet, rare and informative, still logs at 1.
     */
    if (vdisc_trace >= 2
        || (vdisc_trace && op != GPCMD_READ_CD && op != GPCMD_READ_CD_MSF
            && op != GPCMD_READ_10 && op != GPCMD_READ_SUBCHANNEL)) {
        const char *n = "?";
        switch (op) {
        case GPCMD_READ_CD:         n = "READ_CD";         break;
        case GPCMD_READ_CD_MSF:     n = "READ_CD_MSF";     break;
        case GPCMD_READ_10:         n = "READ_10";         break;
        case GPCMD_READ_12:         n = "READ_12";         break;
        case GPCMD_READ_HEADER:     n = "READ_HEADER";     break;
        case GPCMD_READ_SUBCHANNEL: n = "READ_SUBCHANNEL"; break;
        case GPCMD_READ_TOC_PMA_ATIP: n = "READ_TOC";      break;
        case GPCMD_READ_CDVD_CAPACITY: n = "READ_CAPACITY"; break;
        case GPCMD_READ_DISC_INFO:  n = "READ_DISC_INFO";  break;
        case GPCMD_PLAY_AUDIO_10:   n = "PLAY_AUDIO_10";   break;
        case GPCMD_PLAY_AUDIO_MSF:  n = "PLAY_AUDIO_MSF";  break;
        case GPCMD_PLAY_CD:         n = "PLAY_CD";         break;
        case GPCMD_PAUSE_RESUME:    n = "PAUSE_RESUME";    break;
        case GPCMD_SCAN:            n = "SCAN";            break;
        case GPCMD_SEEK:            n = "SEEK";            break;
        case GPCMD_START_STOP_UNIT: n = "START_STOP_UNIT"; break;
        case GPCMD_MODE_SENSE_10:   n = "MODE_SENSE_10";   break;
        case GPCMD_MODE_SELECT_10:  n = "MODE_SELECT_10";  break;
        case GPCMD_MECHANISM_STATUS: n = "MECHANISM_STATUS"; break;
        case GPCMD_INQUIRY:         n = "INQUIRY";         break;
        case GPCMD_REQUEST_SENSE:   n = "REQUEST_SENSE";   break;
        case GPCMD_TEST_UNIT_READY: n = "TEST_UNIT_READY"; break;
        case GPCMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
                                    n = "PREVENT_ALLOW";   break;
        case GPCMD_GET_CONFIGURATION: n = "GET_CONFIG";    break;
        case GPCMD_GET_EVENT_STATUS_NOTIFICATION:
                                    n = "GET_EVENT_STATUS"; break;
        default:                    n = "(unnamed)";       break;
        }
        /*
         * SAY WHAT WILL HAPPEN, NOT WHAT USED TO.
         *
         * This line read "- REFUSED -ENOTTY" unconditionally when the
         * handler refused everything. Once READ_CD was answered it
         * became a lie: 86Box run 102 logged 752 requests as REFUSED
         * while the strace showed 331 CDROMREADAUDIO ioctls returning
         * 0. The trace said one thing and the measurement another, and
         * the measurement was right.
         *
         * A trace that misreports the code's own decision is worse
         * than no trace - it is what a later session would reason
         * from. The verdict is derived from the same tests the code
         * below makes.
         *
         * WIDENED 2026-09-17 when READ_CD stopped being one thing.
         * The command carries four requests distinguished by cmd[9],
         * we serve two of them, and a flat "SERVED" would have been
         * wrong again for the other two the moment Mode 2 landed -
         * the same bug, one day later. The line now names WHICH.
         */
        printk(KERN_DEBUG VSOUND_TS "vdisc: packet %s (0x%02x)"
                          " cmd9 0x%02x buflen %u dir %u\n",
               jiffies, n, op, (unsigned int) cgc->cmd[9], cgc->buflen,
               (unsigned int) cgc->data_direction);
    }

    /*
     * READ CD - THE ONE OPCODE WE ANSWER.
     *
     * The field layout is MMC-2 Table 88 (section 6.1.13,
     * refs/specs/mmc2r02.txt:4102), and it is what cdrom_read_block()
     * fills in at cdrom.c:1394-1423:
     *
     *     cmd[0]    BEh
     *     cmd[1]    Expected Sector Type in bits 4-2
     *     cmd[2..5] Starting Logical Block Address, big-endian
     *     cmd[6..8] Transfer Length in blocks, big-endian
     *     cmd[9]    SYNC / Header Codes / User Data / EDC&ECC / Error
     *     cmd[10]   Sub-Channel Selection
     *
     * BYTE INDEXING, NOT A STRUCT. cdemu declares this as bitfields
     * (mmc-packet-commands.h:1157) and that is fine for one compiler;
     * ours has to be right under gcc 2.95.2 for a 1999 kernel, where
     * bitfield layout is the compiler's business and the wire format
     * is not.
     */
    if (op == GPCMD_READ_CD) {
        unsigned int lba, nframes, subchan, want9, want;
        __u32 op2;
        int unit, rc;

        if (dev == NULL)
            return -ENXIO;

        lba = ((unsigned int) cgc->cmd[2] << 24) |
              ((unsigned int) cgc->cmd[3] << 16) |
              ((unsigned int) cgc->cmd[4] <<  8) |
               (unsigned int) cgc->cmd[5];
        nframes = ((unsigned int) cgc->cmd[6] << 16) |
                  ((unsigned int) cgc->cmd[7] <<  8) |
                   (unsigned int) cgc->cmd[8];
        subchan = (unsigned int) (cgc->cmd[10] & 0x07);

        /*
         * WHICH READ IS THIS? cmd[9] SAYS, AND THE LAYER SETS IT FROM
         * THE BLOCK SIZE IT WANTS.
         *
         * cdrom_read_block (cdrom.c:1416-1421) switches on blksize and
         * writes one of four values into cmd[9] - the SYNC / Header
         * Code / User Data / EDC-ECC bits of MMC-2 Table 88. So the
         * one command carries four different requests and the byte is
         * how they are told apart:
         *
         *   0xf8  2352  sync+hdr+subhdr+data+ecc   CDROMREADAUDIO,
         *                                          and READRAW
         *   0x58  2336  subheader onward           CDROMREADMODE2
         *   0x10  2048  user data only             CDROMREADMODE1
         *   0x78  2340  header onward              (no ioctl asks)
         *
         * WE SERVE THE FIRST TWO. 0x10 would be read_data()'s job and
         * no consumer is known; 0x78 has no caller in the layer at
         * all. Both are refused rather than approximated, because
         * returning the wrong slice silently is worse than saying no -
         * it would shift every byte the caller reads.
         */
        want9 = (unsigned int) cgc->cmd[9];

        /*
         * SUB-CHANNEL: ONLY 000b, AND THAT IS CONFORMANT.
         * MMC-2 Table 91 makes "no sub-channel data" MANDATORY and
         * RAW, Q and P-W all OPTIONAL. cdemu serves two of them and
         * refuses R-W; we serve none, which the standard permits.
         *
         * REFUSED EVEN WHEN nframes IS ZERO, and that is deliberate:
         * a zero-length READ CD is a CAPABILITY PROBE (cdemu's own
         * comment on this guard says `readcd' uses exactly that), and
         * accepting the probe while failing the real read makes the
         * application fail later and further from the cause.
         */
        if (subchan != 0) {
            if (vdisc_trace)
                printk(KERN_DEBUG VSOUND_TS "vdisc: packet READ_CD"
                       " sub-channel mode %u refused (only 0 served)\n",
                       jiffies, subchan);
            return -EINVAL;
        }

        /*
         * A zero-length probe with no sub-channel asked for: say yes.
         * MMC-2 6.1.13 says the same of an all-zero cmd[9]: "If all
         * the fields contain zero then no information is returned.
         * This condition shall not be considered an error." That case
         * falls out below - the mask is empty, the daemon assembles
         * nothing and returns a zero length. design/30 Finding 2.
         */
        if (nframes == 0)
            return 0;

        /*
         * The layer caps at 8 frames for audio (cdrom.c:1916) and run
         * 100 measured exactly that - 18816 and 7056 byte requests,
         * never more. CDROMREADMODE2 asks for ONE sector at a time
         * (cdrom.c:1877 passes nblocks=1), so the same cap is generous
         * there. Refuse rather than truncate: a caller that asked for
         * more than it can get should be told.
         */
        if (nframes > VDISC_MAX_RAW_FRAMES)
            return -EINVAL;

        /*
         * Pick the unit from cmd[9], per the table above - and for
         * 2352 bytes, pick the POLICY from cmd[1].
         *
         * 0xf8 IS TWO IOCTLS. CDROMREADAUDIO and CDROMREADRAW both
         * ask for 2352 bytes and build the same command
         * (cdrom.c:1858 and :1896), so cmd[9] cannot separate them -
         * it distinguishes the four BLOCK SIZES, not the two intents.
         *
         * THE EXPECTED SECTOR TYPE DOES, and it is MMC-2 Table 88's
         * byte 1, bits 4-2. `cdrom_read_block' sets it from its
         * `format' argument (cmd[1] = format << 2):
         *
         *     CDROMREADRAW     format 0  ->  type 0  "all types"
         *     CDROMREADAUDIO   format 1  ->  type 1  "CD-DA"
         *     CDROMREADMODE1   format 2  ->  type 2  "Mode 1"
         *     CDROMREADMODE2   format 0  ->  type 0  "all types"
         *
         * So the kernel DOES say which it meant, in the field the
         * standard provides for exactly this. The spec: a transfer
         * "is terminated as soon as data is encountered that does not
         * match one of those specified in the sector type field", and
         * type 0 filters nothing.
         *
         * THIS IS WHAT CDEMU DOES TOO, checked 2026-09-17:
         * `map_expected_sector_type' (device-commands.c:29) turns the
         * field into a sector class, and the read loop compares it per
         * sector ONLY when it is non-zero - "if we have
         * CDB->ExpectedSectorType set, we compare its translated value
         * with our sector type, period". It refuses with ILLEGAL
         * REQUEST / ILLEGAL MODE FOR THIS TRACK, which is MMC-2 Table
         * 3's own answer. It applies NO track-type rule of its own.
         *
         * **86Box RUN 107 IS WHY THIS IS NOT GUESSWORK.** Mapping
         * every 2352-byte read to the audio opcode made the daemon
         * refuse a DATA track - right for a ripper, and fatal here,
         * because ON A VIDEO CD EVERY TRACK IS DATA. All 75 of
         * MPlayer's reads were refused while the trace said SERVED.
         */
        {
            unsigned int sect_type = ((unsigned int) cgc->cmd[1] >> 2) & 7;
            unsigned int c2 = (want9 >> 1) & 3;
            unsigned int hdr = (want9 >> 5) & 3;

            /*
             * DECODE THE FIVE FIELDS, rather than matching the byte.
             * design/30: cdemu assembles its reply field by field and
             * 86Box does the same with one function per sector type;
             * we alone matched `cmd[9]' against two constants, which
             * is right for the four requests `cdrom_ioctl' makes and
             * wrong for anything arriving through CDROM_SEND_PACKET -
             * a path we opened by advertising the capability.
             */
            want = 0;
            if (want9 & 0x80)   want |= VDISC_SEC_SYNC;
            if (hdr & 1)        want |= VDISC_SEC_HEADER;
            if (hdr & 2)        want |= VDISC_SEC_SUBHDR;
            if (want9 & 0x10)   want |= VDISC_SEC_DATA;
            if (want9 & 0x08)   want |= VDISC_SEC_EDC;
            if (c2 == 1)        want |= VDISC_SEC_C2;
            else if (c2 == 2)   want |= VDISC_SEC_C2BLOCK;

            /*
             * THE COMBINATIONS 86Box REFUSES, and it is the only one
             * of the three references that names them
             * (src/cdrom/cdrom.c:805-830). MMC-2 saying an all-zero
             * byte is not an error does not make every non-zero
             * combination legal.
             */
            if (c2 == 3) {
                if (vdisc_trace)
                    printk(KERN_DEBUG VSOUND_TS "vdisc: READ_CD error"
                           " field 11b is reserved\n", jiffies);
                return -EINVAL;
            }
            if ((want9 & 0x18) == 0x08) {
                if (vdisc_trace)
                    printk(KERN_DEBUG VSOUND_TS "vdisc: READ_CD"
                           " EDC/ECC without user data is illegal\n",
                           jiffies);
                return -EINVAL;
            }
            if ((want9 & 0xf0) == 0x90 || (want9 & 0xf0) == 0xc0) {
                if (vdisc_trace)
                    printk(KERN_DEBUG VSOUND_TS "vdisc: READ_CD"
                           " cmd[9]=0x%02x is an illegal mode\n",
                           jiffies, want9);
                return -EINVAL;
            }

            /*
             * THE EXPECTED SECTOR TYPE, which is what separates
             * CDROMREADAUDIO from CDROMREADRAW - they build identical
             * commands otherwise (cdrom.c:1858 and :1896), and 86Box
             * run 107 cost an entire Video CD by ignoring it.
             *
             * Type 1 means the caller said CD-DA, so the daemon's
             * data-track refusal is what it wants.
             *
             * TYPE 2 IS `CDROMREADMODE1', AND IT IS REACHABLE - the
             * layer builds it at cdrom.c:1861 with format 2. Refusing
             * it would leave a fourth ioctl failing that we can
             * answer: it asks for user data only, which is what the
             * DATA piece already returns.
             *
             * WE DO NOT FILTER BY MODE, and types 2-5 ask us to - the
             * spec's rule is to terminate where the sector type stops
             * matching. Accepting type 2 without filtering is a
             * DELIBERATE narrowing: the caller gets its data and does
             * not get the guarantee that every sector was Mode 1.
             * Types 3-5 are refused rather than half-served, because
             * they are only reachable through CDROM_SEND_PACKET where
             * an explicit refusal is more use than a silent one.
             */
            if (sect_type == 1)
                want |= VDISC_SEC_AUDIO;
            else if (sect_type > 2) {
                if (vdisc_trace)
                    printk(KERN_DEBUG VSOUND_TS "vdisc: READ_CD"
                           " expected sector type %u not served"
                           " (we do not filter by sector mode)\n",
                           jiffies, sect_type);
                return -EINVAL;
            }

            op2  = VDISC_OP_READ_CD;

            /*
             * THE BUFFER CHECK MUST USE WHAT THIS REQUEST YIELDS, NOT
             * THE WORST CASE - and getting that wrong refused every
             * read on 86Box run 108.
             *
             * The first version set `unit' to the largest a sector
             * could ever produce (2352 + C2 + 2 = 2648) and then
             * required `buflen' to be at least that. But the layer
             * allocates EXACTLY the block size it asked for -
             * cdrom.c:1874 kmallocs `blocksize', which is 2352 for a
             * raw read - so every request failed the test and MPlayer
             * saw EINVAL.
             *
             * `buflen' is the caller's own statement of how much room
             * it has, and the layer always sizes it correctly for the
             * request it built. So the only check worth making is
             * that it is not ZERO and not absurd; the daemon returns
             * the real length and the reply path refuses a short one.
             */
            if (cgc->buflen == 0 || cgc->buflen > VDISC_MAX_XFER)
                return -EINVAL;
            unit = (int) (cgc->buflen / nframes);
            if (unit <= 0)
                return -EINVAL;
        }

        /*
         * READ STRAIGHT INTO THEIR BUFFER. cgc->buffer is KERNEL
         * memory, not user memory - cdrom_ioctl kmalloc'd it at
         * cdrom.c:1918 and does the copy_to_user itself afterwards
         * (:1930). Verified in the target's own source rather than
         * assumed, because the two cases want different primitives and
         * guessing wrong corrupts memory rather than failing.
         *
         * So no bounce buffer: vdisc_raw_read fills this directly.
         */
        if (cgc->buffer == NULL)
            return -EINVAL;

        rc = vdisc_raw_read(dev->minor, (int) lba, (int) nframes,
                            (unsigned char *) cgc->buffer, op2, unit, want);

        /*
         * THE VERDICT IS PRINTED HERE, WHERE IT IS KNOWN.
         *
         * This line has misreported three times in two days, each
         * time because it was ABOVE the code that decided:
         *
         *   2026-09-16  a hardcoded "REFUSED" survived the read path
         *               landing, so 752 served requests logged as
         *               refused (run 102)
         *   2026-09-17  "SERVED raw" keyed off cmd[9] alone, so 60
         *               requests that failed a buffer check logged as
         *               served while MPlayer saw EINVAL (run 108)
         *
         * A trace that guesses is worse than no trace: it is what the
         * next session reasons from, and twice it contradicted the
         * strace sitting beside it. So it now reports `rc' - the
         * value actually returned - and nothing else.
         */
        if (vdisc_trace >= 2)       /* per read: level 2, see above */
            printk(KERN_DEBUG VSOUND_TS "vdisc: packet READ_CD"
                   " lba %u n %u want 0x%02x -> %d\n",
                   jiffies, lba, nframes, want, rc);
        return rc;
    }

    (void) dev;

    /*
     * -ENOTTY, NOT -ENOSYS, AND THE DIFFERENCE IS THE WHOLE POINT.
     *
     * mmc_ioctl's caller tests for -ENOTTY specifically and falls
     * through to the device-specific handlers on it. Any other error
     * is taken as a real answer and RETURNED TO THE CALLER - which
     * would break the eight working ioctls listed above rather than
     * passing them along.
     */
    return -ENOTTY;
}

#endif /* VDISC_HAVE_PACKET */

static struct cdrom_device_ops vdisc_dops = {
    vdisc_open,
    vdisc_release,
    vdisc_drive_status,
    vdisc_media_changed,
    vdisc_tray_move,
    vdisc_lock_door,
    NULL,                   /* select_speed     */
    NULL,                   /* select_disc      */
    vdisc_get_last_session,
    NULL,                   /* get_mcn          */
    NULL,                   /* reset            */
    vdisc_audio_ioctl,
    vdisc_dev_ioctl,        /* dev_ioctl        */

    /*
     * CDC_PLAY_AUDIO gates every TOC ioctl, not just playback: the
     * uniform layer checks CDROM_CAN(CDC_PLAY_AUDIO) and returns
     * -ENOSYS for CDROMREADTOCHDR/ENTRY without it (cdrom.c:1718,
     * 1731), so cdparanoia would never reach audio_ioctl. Omitting it
     * because "we do not play audio yet" is the trap; the capability is
     * about the TOC as much as the sound.
     */
    CDC_OPEN_TRAY | CDC_CLOSE_TRAY | CDC_LOCK |
    CDC_MEDIA_CHANGED | CDC_DRIVE_STATUS | CDC_MULTI_SESSION |
    CDC_PLAY_AUDIO |
    /* CDC_IOCTLS gates cdo->dev_ioctl (cdrom.c:1797), which is how
     * VDISC_IOC_GET_LOCK reaches us. ENSURE() at cdrom.c:371 drops
     * the bit if the handler is NULL, so the two must agree. */
    CDC_IOCTLS,

    0,
    /*
     * generic_packet - WAS INTENTIONALLY NULL, AND THE REASON GIVEN
     * FOR THAT WAS WRONG ABOUT RIPPING.
     *
     * The old comment here said "raw MMC passthrough is for burning,
     * ripping and copy protection; none apply to a read-only virtual
     * drive". Burning and copy protection, correct. RIPPING DOES
     * APPLY, and leaving this NULL is the only reason it does not
     * work: measured 2026-09-16, cdparanoia (9.7, 9.8 and 10.2 alike)
     * and cdda2wav both reach CDROMREADAUDIO and are told -ENOSYS
     * because the uniform layer routes it through
     * cdrom_read_block -> cdo->generic_packet (cdrom.c:1394-1423).
     * smpeg's Video CD mode wants CDROMREADMODE2 through the same
     * pointer.
     *
     * The handler is installed unconditionally; what gates the
     * behaviour is CDC_GENERIC_PACKET in the mask above, set at init
     * only when vdisc_packet=1. Without the capability the layer
     * never calls this (cdrom.c:1684 tests CDROM_CAN first), so an
     * ordinary run is unaffected either way.
     */
#ifdef VDISC_HAVE_PACKET
    vdisc_generic_packet
#endif
};

/*
 * THE SAME TABLE WITH CDC_GENERIC_PACKET ADDED, chosen at init when
 * vdisc_packet=1.
 *
 * TWO TABLES RATHER THAN ONE ASSIGNMENT, AND THE COMPILER IS WHY.
 * `capability' is declared `const int' in the target's
 * include/linux/cdrom.h:763. The first version of this set the bit at
 * init with `vdisc_dops.capability |= CDC_GENERIC_PACKET', which gcc
 * 2.95.2 accepted with "warning: assignment of read-only member" -
 * i.e. it compiled and would probably have worked, and is undefined
 * behaviour. A warning in a cross-build for a 1999 kernel is not
 * something to step over.
 *
 * Duplicating the table costs about 60 bytes of module and nothing
 * else. The two MUST stay in step: anything added to vdisc_dops
 * belongs here too.
 */
#ifdef VDISC_HAVE_PACKET
static struct cdrom_device_ops vdisc_dops_packet = {
    vdisc_open,
    vdisc_release,
    vdisc_drive_status,
    vdisc_media_changed,
    vdisc_tray_move,
    vdisc_lock_door,
    NULL,                   /* select_speed     */
    NULL,                   /* select_disc      */
    vdisc_get_last_session,
    NULL,                   /* get_mcn          */
    NULL,                   /* reset            */
    vdisc_audio_ioctl,
    vdisc_dev_ioctl,        /* dev_ioctl        */

    /*
     * CDC_PLAY_AUDIO gates every TOC ioctl, not just playback: the
     * uniform layer checks CDROM_CAN(CDC_PLAY_AUDIO) and returns
     * -ENOSYS for CDROMREADTOCHDR/ENTRY without it (cdrom.c:1718,
     * 1731), so cdparanoia would never reach audio_ioctl. Omitting it
     * because "we do not play audio yet" is the trap; the capability is
     * about the TOC as much as the sound.
     */
    CDC_OPEN_TRAY | CDC_CLOSE_TRAY | CDC_LOCK |
    CDC_MEDIA_CHANGED | CDC_DRIVE_STATUS | CDC_MULTI_SESSION |
    CDC_PLAY_AUDIO | CDC_GENERIC_PACKET |
    /* CDC_IOCTLS gates cdo->dev_ioctl (cdrom.c:1797), which is how
     * VDISC_IOC_GET_LOCK reaches us. ENSURE() at cdrom.c:371 drops
     * the bit if the handler is NULL, so the two must agree. */
    CDC_IOCTLS,

    0,
    /*
     * generic_packet - WAS INTENTIONALLY NULL, AND THE REASON GIVEN
     * FOR THAT WAS WRONG ABOUT RIPPING.
     *
     * The old comment here said "raw MMC passthrough is for burning,
     * ripping and copy protection; none apply to a read-only virtual
     * drive". Burning and copy protection, correct. RIPPING DOES
     * APPLY, and leaving this NULL is the only reason it does not
     * work: measured 2026-09-16, cdparanoia (9.7, 9.8 and 10.2 alike)
     * and cdda2wav both reach CDROMREADAUDIO and are told -ENOSYS
     * because the uniform layer routes it through
     * cdrom_read_block -> cdo->generic_packet (cdrom.c:1394-1423).
     * smpeg's Video CD mode wants CDROMREADMODE2 through the same
     * pointer.
     *
     * The handler is installed unconditionally; what gates the
     * behaviour is CDC_GENERIC_PACKET in the mask above, set at init
     * only when vdisc_packet=1. Without the capability the layer
     * never calls this (cdrom.c:1684 tests CDROM_CAN first), so an
     * ordinary run is unaffected either way.
     */
    vdisc_generic_packet
};
#endif /* VDISC_HAVE_PACKET */

/* ------------------------------------------------------------------ *
 * Control device - the daemon's channel
 * ------------------------------------------------------------------ */

static int
vdisc_do_attach(unsigned long arg)
{
    struct vdisc_attach *att;
    struct vdisc_device *dev;
    int minor;

    /*
     * kmalloc rather than a stack copy: struct vdisc_attach carries the
     * whole 99-entry TOC and is far too big for a kernel stack.
     */
    att = (struct vdisc_attach *) kmalloc(sizeof *att, GFP_KERNEL);
    if (!att)
        return -ENOMEM;

    if (copy_from_user(att, (void *) arg, sizeof *att)) {
        kfree(att);
        return -EFAULT;
    }

    if (att->proto_version != VDISC_PROTO_VERSION) {
        printk(KERN_ERR VSOUND_TS "vdisc: daemon speaks protocol %d, "
                        "kernel wants %d\n",
               jiffies,
               att->proto_version, VDISC_PROTO_VERSION);
        kfree(att);
        return -EINVAL;
    }

    minor = att->minor;
    if (minor < 0 || minor >= vdisc_ndevs) {
        kfree(att);
        return -EINVAL;
    }
    dev = &vdisc_devs[minor];

    /*
     * A DISC CAN BE PUT IN WHENEVER THE TRAY IS OPEN - 2026-09-28,
     * and this used to refuse whenever ANYTHING held the node.
     *
     * THE ASYMMETRY THE USER FOUND: *"a cdplayer like kscd can eject
     * our discs if not locked BUT we can not insert a disc if
     * something like kscd is open even if the drive/daemon is
     * empty"*. Exactly so - the eject path was given a deliberate
     * bypass (`VDISC_DETACH_FORCE', see vdisc.h) and attach never
     * got one, so a player merely SITTING on /dev/cdrom made the GUI
     * unable to load a disc at all.
     *
     * THE OLD COMMENT CONFLATED TWO THINGS - "mounted or otherwise
     * open". They are not the same:
     *
     *   a MOUNTED filesystem  - swapping the medium under a live
     *                           mount corrupts it. Must refuse.
     *   a PLAYER holding the  - a real drive accepts the disc and
     *   node open               signals a media change. Must allow.
     *
     * `use_count' cannot tell them apart. `tray_open' can, and it is
     * what a real drive gates on: the tray is open exactly when
     * there is no medium, which is when a disc may be put in.
     *
     * A MOUNT IS STILL REFUSED, because a mounted filesystem keeps
     * the tray SHUT - `tray_open' is cleared at attach and set in
     * ONE place only, `vdisc_do_detach()', after the kernel has
     * agreed to release the medium. A refused detach leaves it
     * alone. So a mount means attached means tray shut.
     *
     * AND MEDIA_CHANGED IS THE HONEST SIGNAL to whatever had the
     * node open: it is already set below, so a player polling for a
     * disc finds one, which is what it would do with real hardware.
     */
    if (!dev->tray_open && dev->info.use_count > 0) {
        kfree(att);
        return -EBUSY;          /* a medium is present and in use */
    }

    /*
     * THE DAEMON IS UNTRUSTED. Bound the geometry as well as rejecting
     * zero.
     *
     * These arrive as __u32 and are stored in int, so testing only for
     * "<= 0" catches nothing above zero: a huge value passes, becomes
     * negative on assignment, and makes data_sectors * SECTOR_SIZE
     * overflow below. The result is a device reporting nonsense rather
     * than one refusing to attach. VDISC_MAX_SECTORS is well above any
     * real disc and well below where either overflows.
     */
    /*
     * ZERO DATA SECTORS IS NOT IMPLAUSIBLE; IT IS AN AUDIO CD.
     * design/26 B1, fixed 2026-09-14: `data_sectors == 0' was refused
     * here, so a music disc image - every track audio - could not be
     * attached at all ("refusing implausible geometry: 300 total, 0
     * data sectors"). With no data track vdisc_sizes[minor] is 0 and
     * the block layer's own end-of-device check (ll_rw_blk.c:645-658)
     * refuses every read before do_vdisc_request sees one; the audio
     * side never needed a data track. The other bounds stay.
     */
    if (att->total_sectors == 0 ||
        att->total_sectors > VDISC_MAX_SECTORS ||

        att->data_sectors > VDISC_MAX_SECTORS ||
        att->data_sectors > att->total_sectors) {
        printk(KERN_ERR VSOUND_TS "vdisc: refusing implausible geometry: "
                        "%u total, %u data sectors (max %d)\n",
               jiffies,
               att->total_sectors, att->data_sectors, VDISC_MAX_SECTORS);
        kfree(att);
        return -EINVAL;
    }

    /*
     * The window must fit inside the disc. data_start is added to every
     * block LBA, so an unchecked value walks the backend off the end of
     * the file - and it arrives from an untrusted daemon like the rest.
     * The sum is what matters, not either half.
     */
    if (att->data_start > VDISC_MAX_SECTORS ||
        att->data_start + att->data_sectors > att->total_sectors) {
        printk(KERN_ERR VSOUND_TS "vdisc: refusing data window: start %u + "
                        "%u sectors exceeds %u total\n",
               jiffies,
               att->data_start, att->data_sectors, att->total_sectors);
        kfree(att);
        return -EINVAL;
    }

    /*
     * Validate the TOC before caching it. n_tracks indexes track[]
     * below, and the track numbers are what CDROMREADTOCENTRY selects
     * on, so a bad table here is an out-of-bounds read at ioctl time.
     */
    if (att->n_tracks < 1 || att->n_tracks > VDISC_MAX_TRACKS) {
        kfree(att);
        return -EINVAL;
    }
    if (att->first_track < 1 || att->last_track > VDISC_MAX_TRACKS ||
        att->first_track > att->last_track) {
        kfree(att);
        return -EINVAL;
    }
    if (att->last_track - att->first_track + 1 != att->n_tracks) {
        kfree(att);
        return -EINVAL;
    }

    /*
     * Sessions, from the per-track byte. 0 means the daemon did not
     * say, and reads as 1. They must not decrease along the disc, and
     * the last session's first track is CDROMMULTISESSION's answer.
     * Derived here, once, because get_last_session may be called with
     * locks held and must not compute anything it could get wrong.
     */
    {
        int i, prev = 1, hi = 1, lba = 0;

        for (i = 0; i < att->n_tracks; i++) {
            int sn = att->track[i].session ? att->track[i].session : 1;
            if (sn < prev || sn > VDISC_MAX_TRACKS) {
                printk(KERN_ERR VSOUND_TS "vdisc: refusing TOC: track %d in "
                                "session %d after session %d\n",
                       jiffies, att->first_track + i, sn, prev);
                kfree(att);
                return -EINVAL;
            }
            if (sn > hi) {
                hi  = sn;
                lba = (int) att->track[i].start_lba;
            }
            prev = sn;
        }
        dev->n_sessions       = hi;
        dev->last_session_lba = hi > 1 ? lba : 0;
    }

    dev->total_sectors = att->total_sectors;
    dev->data_sectors  = att->data_sectors;
    dev->data_start    = att->data_start;
    dev->first_track   = att->first_track;
    dev->last_track    = att->last_track;
    dev->n_tracks      = att->n_tracks;
    memcpy(dev->track, att->track, sizeof dev->track);
    dev->attached      = 1;
    dev->tray_open     = 0;
    dev->media_changed = 1;     /* force the buffer cache to re-read */
    dev->audio_status  = CDROM_AUDIO_NO_STATUS;
    dev->audio_trk     = 0;
    dev->audio_pos     = 0;
    dev->audio_report_jiffies = jiffies;
    dev->audio_stalled = 0;
    /* Full volume until told otherwise - a real drive powers up at
     * unity, and the channel does too. */
    dev->vol_left      = 255;
    dev->vol_right     = 255;

    /* blk_size is in 1KB units. The device runs from disc LBA 0 to the
     * end of the last data track (data_start is 0 since protocol 2);
     * a read that lands in an audio track on the way is refused by the
     * daemon, as a drive refuses it. */
    vdisc_sizes[minor]      = (att->data_sectors * VDISC_SECTOR_SIZE) / 1024;
    vdisc_blocksizes[minor] = VDISC_SECTOR_SIZE;
    vdisc_hardsects[minor]  = VDISC_HARDSECT;

    printk(KERN_INFO VSOUND_TS "vdisc: vdisc%d attached, %d sectors "
                     "(%d KB mountable), %d session%s%s\n",
               jiffies,
           minor, att->total_sectors, vdisc_sizes[minor],
           dev->n_sessions, dev->n_sessions == 1 ? "" : "s",
           dev->n_sessions > 1 ? " - last session reported to isofs" : "");

    kfree(att);
    return 0;
}

static int
vdisc_do_detach(unsigned long arg)
{
    struct vdisc_device *dev;
    int minor;

    int force;

    if (get_user(minor, (int *) arg))
        return -EFAULT;

    /*
     * THE FLAG COMES IN ON THE MINOR - see VDISC_DETACH_FORCE in
     * vdisc.h for why a client eject is allowed past the use_count
     * check and a GUI eject is not.
     */
    force = (minor & VDISC_DETACH_FORCE) ? 1 : 0;
    minor &= ~VDISC_DETACH_FORCE;

    if (minor < 0 || minor >= vdisc_ndevs)
        return -EINVAL;

    dev = &vdisc_devs[minor];
    if (!force && dev->info.use_count > 0)
        return -EBUSY;

    dev->attached      = 0;
    /*
     * THE TRAY IS OPEN BECAUSE THE MEDIUM IS GONE - and this is the
     * only place that can say so. tray_move() asks; this is where the
     * kernel has agreed. A refused detach returns -EBUSY above,
     * leaving the flag alone and the disc present.
     */
    dev->tray_open     = 1;
    dev->media_changed = 1;
    dev->data_start    = 0;
    dev->n_sessions    = 0;
    dev->last_session_lba = 0;
    dev->audio_status  = CDROM_AUDIO_NO_STATUS;
    dev->audio_pos     = 0;
    dev->audio_trk     = 0;
    vdisc_sizes[minor] = 0;

    printk(KERN_INFO VSOUND_TS "vdisc: vdisc%d detached\n",
               jiffies, minor);
    return 0;
}

/*
 * Hand the daemon the next queued request. Blocks until one exists.
 */
static int
vdisc_do_get_req(unsigned long arg)
{
    struct vdisc_pending *p;
    struct vdisc_audio_msg *m;
    struct vdisc_req r;
    unsigned long flags;

    while (1) {
        spin_lock_irqsave(&io_request_lock, flags);

        /*
         * AUDIO FIRST. A CDROMSTOP queued behind a burst of reads would
         * otherwise keep playing until they drained, and stopping the
         * music is exactly the case where latency gets noticed.
         */
        /*
         * A RAW READ NEXT, after audio and before block reads. A
         * ripper's caller is ASLEEP waiting for it, where a block read
         * has the elevator behind it and can wait its turn.
         */
        if (vdisc_raw_inflight && !vdisc_raw_inflight->done
            && !vdisc_raw_inflight->claimed) {
            struct vdisc_raw *rw = vdisc_raw_inflight;

            spin_unlock_irqrestore(&io_request_lock, flags);

            memset(&r, 0, sizeof r);
            r.handle = rw->handle;
            r.opcode = rw->opcode;
            r.minor  = (__u32) rw->minor;
            r.lba    = (__u32) rw->lba;
            r.count  = (__u32) rw->frames;
            r.arg0   = rw->want;        /* the field mask, READ_CD */

            if (copy_to_user((void *) arg, &r, sizeof r)) {
                /*
                 * The daemon never received it, so it will never
                 * reply. Fail the sleeper now rather than leaving it
                 * to time out - same reasoning as the block path's
                 * take-it-back-off below.
                 */
                spin_lock_irqsave(&io_request_lock, flags);
                if (vdisc_raw_inflight == rw) {
                    rw->status = -EFAULT;
                    rw->done = 1;
                }
                spin_unlock_irqrestore(&io_request_lock, flags);
                wake_up_interruptible(&vdisc_raw_wq);
                return -EFAULT;
            }
            return 0;
        }

        m = vdisc_audio_head;
        if (m) {
            vdisc_audio_head = m->next;
            if (!vdisc_audio_head)
                vdisc_audio_tail = NULL;
            vdisc_audio_queued--;
            spin_unlock_irqrestore(&io_request_lock, flags);

            memset(&r, 0, sizeof r);
            r.handle = m->handle;
            r.opcode = m->opcode;
            r.minor  = m->minor;
            r.lba    = m->lba;
            r.arg0   = m->end_lba;
            r.arg1   = m->arg0;

            if (copy_to_user((void *) arg, &r, sizeof r)) {
                /*
                 * Put it back rather than dropping it. These are the
                 * commands that start and stop playback, so a lost one
                 * leaves music running that should have stopped. It goes
                 * back at the HEAD to keep the order it was queued in.
                 */
                spin_lock_irqsave(&io_request_lock, flags);
                m->next = vdisc_audio_head;
                vdisc_audio_head = m;
                if (!vdisc_audio_tail)
                    vdisc_audio_tail = m;
                vdisc_audio_queued++;
                spin_unlock_irqrestore(&io_request_lock, flags);
                return -EFAULT;
            }
            kfree(m);
            return 0;
        }

        p = vdisc_todo_head;
        if (p) {
            vdisc_todo_head = p->next;
            if (!vdisc_todo_head)
                vdisc_todo_tail = NULL;
            /* move to in-flight */
            p->next = vdisc_inflight;
            vdisc_inflight = p;
        }
        spin_unlock_irqrestore(&io_request_lock, flags);

        if (p)
            break;

        if (signal_pending(current))
            return -EINTR;

        interruptible_sleep_on(&vdisc_daemon_wq);

        if (signal_pending(current))
            return -EINTR;
    }

    memset(&r, 0, sizeof r);
    r.handle = p->handle;
    r.opcode = VDISC_OP_READ;
    r.minor  = p->minor;
    r.lba    = p->lba;
    r.count  = p->nsectors;

    if (copy_to_user((void *) arg, &r, sizeof r)) {
        /*
         * The daemon never received this, so it will never reply -
         * leaving it in-flight would strand the caller until the daemon
         * closed and vdisc_fail_all() ran. Take it back off and fail it.
         */
        struct vdisc_pending **pp;
        int found = 0;

        spin_lock_irqsave(&io_request_lock, flags);
        pp = &vdisc_inflight;
        while (*pp) {
            if (*pp == p) {
                *pp = p->next;
                found = 1;
                break;
            }
            pp = &(*pp)->next;
        }
        spin_unlock_irqrestore(&io_request_lock, flags);

        /*
         * ONLY IF IT WAS STILL OURS TO TAKE - design/54 D07. The copy
         * above can sleep, and the worker thread's expiry may have
         * failed and freed this request meanwhile; ending or freeing it
         * again would complete a request twice and free twice.
         */
        if (found) {
            p->req->errors++;
            vdisc_end_request_locked(p->req, 0);
            vdisc_pending_put(p);
        }
        return -EFAULT;
    }
    return 0;
}

/*
 * Accept a reply. The payload follows the struct in the daemon's
 * buffer; we read it straight into the request's own buffer.
 */
static int
vdisc_do_put_reply(unsigned long arg)
{
    struct vdisc_reply rep;
    struct vdisc_pending *p, **pp;
    unsigned long flags;
    char *payload;

    if (copy_from_user(&rep, (void *) arg, sizeof rep))
        return -EFAULT;

    /*
     * A RAW READ FIRST, by handle, for the same reason the block path
     * matches by handle: the daemon names what it is answering and we
     * never take a pointer from it.
     */
    spin_lock_irqsave(&io_request_lock, flags);
    if (vdisc_raw_inflight && vdisc_raw_inflight->handle == rep.handle
        && !vdisc_raw_inflight->claimed && !vdisc_raw_inflight->done) {
        struct vdisc_raw *rw = vdisc_raw_inflight;
        unsigned int want = (unsigned int) rw->frames *
                            (unsigned int) rw->unit;

        /*
         * CLAIMED UNDER THE LOCK, BEFORE IT IS DROPPED - design/54 D67.
         * The copy below can sleep, and the reader's timeout must not
         * return from under it: vdisc_raw_read() waits for `done' on a
         * claimed request instead of abandoning it.
         */
        rw->claimed = 1;
        spin_unlock_irqrestore(&io_request_lock, flags);

        if (rep.status != 0) {
            rw->status = (int) rep.status;
        } else if (rep.length < want) {
            /*
             * SHORT REPLY IS AN ERROR, NOT A PARTIAL READ. Copying
             * what arrived would hand the caller uninitialised kernel
             * memory for the tail, which is worse than failing - the
             * same reasoning as the block path's length check.
             */
            printk(KERN_ERR VSOUND_TS "vdisc: raw read short - wanted %u,"
                            " got %u (op %u)\n",
                   jiffies, want, rep.length, (unsigned int) rw->opcode);
            rw->status = -EIO;
        } else if (copy_from_user(rw->buf,
                                  (char *) ((struct vdisc_reply *) arg + 1),
                                  want)) {
            rw->status = -EFAULT;
        } else {
            rw->status = 0;
        }

        /* The last write through `rw': once `done' is set the reader
         * may return and its frame be gone. The wake below names only
         * the queue, which is static. */
        spin_lock_irqsave(&io_request_lock, flags);
        rw->done = 1;
        spin_unlock_irqrestore(&io_request_lock, flags);
        wake_up(&vdisc_raw_wq);
        return 0;
    }

    /* Find and unlink BY HANDLE - never by a pointer from userspace. */
    pp = &vdisc_inflight;
    while ((p = *pp) != NULL) {
        if (p->handle == rep.handle) {
            *pp = p->next;
            break;
        }
        pp = &p->next;
    }
    spin_unlock_irqrestore(&io_request_lock, flags);

    if (!p)
        return -ENOENT;         /* unknown or already-completed handle */

    if (rep.status != 0) {
        p->req->errors++;
        vdisc_end_request_locked(p->req, 0);
        vdisc_pending_put(p);
        return 0;
    }

    /*
     * We copy p->length bytes starting at p->offset, so the daemon must
     * have returned at least that much. Checking only p->length would
     * let a short reply read past the end of its buffer.
     */
    if (rep.length < (__u32) (p->offset + p->length)) {
        p->req->errors++;
        vdisc_end_request_locked(p->req, 0);
        vdisc_pending_put(p);
        return 0;
    }

    /* The daemon passes the buffer right after the reply struct. */
    payload = (char *) ((struct vdisc_reply *) arg + 1);

    if (copy_from_user(p->buf, payload + p->offset, p->length)) {
        p->req->errors++;
        vdisc_end_request_locked(p->req, 0);
        vdisc_pending_put(p);
        return -EFAULT;
    }

    vdisc_end_request_locked(p->req, 1);
    vdisc_pending_put(p);
    return 0;
}

/*
 * The daemon reports where playback has reached, and any status change
 * it decided on its own (a track ending, the child dying).
 *
 * This is the replacement for computing the position from jiffies. It
 * is a push, not a poll: SUBCHNL is called every frame by some games
 * and must never round-trip to userspace.
 */
static int
vdisc_do_put_pos(unsigned long arg)
{
    struct vdisc_pos pos;
    struct vdisc_device *dev;

    if (copy_from_user(&pos, (void *) arg, sizeof pos))
        return -EFAULT;

    if (pos.minor >= (__u32) vdisc_ndevs)
        return -EINVAL;
    dev = &vdisc_devs[pos.minor];

    if (!dev->attached)
        return -ENOMEDIUM;

    /*
     * Bound the position against the disc. The daemon is untrusted, and
     * this value is handed to vdisc_track_at() and used to index
     * track[] through the clamp in SUBCHNL.
     */
    if (pos.lba > (__u32) dev->total_sectors)
        return -EINVAL;

    switch (pos.status) {
    case CDROM_AUDIO_PLAY:
    case CDROM_AUDIO_PAUSED:
    case CDROM_AUDIO_COMPLETED:
    case CDROM_AUDIO_ERROR:
    case CDROM_AUDIO_NO_STATUS:
        break;
    default:
        return -EINVAL;         /* not a status the API defines */
    }

    /*
     * A STOPPED DRIVE STAYS STOPPED UNTIL A PLAY IOCTL SAYS OTHERWISE.
     *
     * CDROMSTOP sets NO_STATUS here at ioctl time; the child learns of
     * the stop through the daemon a moment later, and any report it
     * had in flight - a periodic PLAY, or the ERROR its own shutdown
     * produced on 2026-09-15 (86Box run 77) - used to land after the
     * STOP and overwrite it. design/26 B16 is the PLAY case; run 77
     * was the ERROR case, and it stuck: KsCD shows ERROR as "Ejected"
     * and sends nothing from that state, so the player was locked out
     * until the daemon restarted.
     *
     * Only PLAYMSF and PLAYTRKIND move the status off NO_STATUS; a
     * report cannot. The position is not taken either - a stopped
     * drive's position is where it stopped.
     */
    /* Any report at all proves the child alive - stamp before the
     * stopped-drive rule below discards what it says. */
    dev->audio_report_jiffies = jiffies;

    if (dev->audio_status == CDROM_AUDIO_NO_STATUS &&
        pos.status != CDROM_AUDIO_NO_STATUS)
        return 0;

    /*
     * AND A PAUSED DRIVE STAYS PAUSED UNTIL RESUME SAYS OTHERWISE - the
     * PAUSE half of design/26 B16, design/54 D37 (2026-10-04). CDROMPAUSE
     * sets PAUSED at ioctl time as STOP sets NO_STATUS, and a periodic
     * PLAY report the child had in flight flipped it back for up to a
     * chunk (~107 ms), so a poller - Quake polls every frame - saw
     * PAUSED, PLAY, PAUSED. Only CDROMRESUME (or a play ioctl) moves it
     * back to PLAY; the position stays where the pause left it. Every
     * other report - a stop, an error, the end - still lands.
     */
    if (dev->audio_status == CDROM_AUDIO_PAUSED &&
        pos.status == CDROM_AUDIO_PLAY)
        return 0;

    dev->audio_pos    = pos.lba;
    dev->audio_status = pos.status;

    if (pos.track >= (__u32) dev->first_track &&
        pos.track <= (__u32) dev->last_track)
        dev->audio_trk = pos.track;

    return 0;
}

/*
 * Fail everything outstanding. Called when the daemon goes away: those
 * requests will never be answered, and a process blocked on one would
 * otherwise sit in D state forever.
 */
static void
vdisc_fail_all(void)
{
    struct vdisc_pending *p, *n;
    struct vdisc_audio_msg *m;
    unsigned long flags;

    spin_lock_irqsave(&io_request_lock, flags);
    p = vdisc_todo_head;
    vdisc_todo_head = vdisc_todo_tail = NULL;
    n = vdisc_inflight;
    vdisc_inflight = NULL;
    m = vdisc_audio_head;
    vdisc_audio_head = vdisc_audio_tail = NULL;
    vdisc_audio_queued = 0;
    spin_unlock_irqrestore(&io_request_lock, flags);

    while (p) {
        struct vdisc_pending *next = p->next;
        p->req->errors++;
        vdisc_end_request_locked(p->req, 0);
        vdisc_pending_put(p);
        p = next;
    }
    while (n) {
        struct vdisc_pending *next = n->next;
        n->req->errors++;
        vdisc_end_request_locked(n->req, 0);
        vdisc_pending_put(n);
        n = next;
    }
    /*
     * The audio queue too. vcd_fail_all() left these allocated - they
     * hold no struct request so nothing hangs on them, but the daemon
     * that would have consumed them is gone and a reload would find
     * stale commands waiting. Freeing them is a leak fix, not a
     * behaviour change.
     */
    while (m) {
        struct vdisc_audio_msg *next = m->next;
        kfree(m);
        m = next;
    }
}

/*
 * FAIL WHAT HAS WAITED TOO LONG - design/54 D07. Run by the worker
 * thread once a second while the daemon is open. Anything on `todo'
 * (not yet fetched) or `inflight' (fetched, not answered) queued more
 * than VDISC_DATA_TIMEOUT ago is unlinked under the lock and failed
 * outside it, as vdisc_fail_all() does - so the reader gets EIO rather
 * than D state. Each path that completes a request unlinks it under
 * the lock first (PUT_REPLY by handle, GET_REQ's take-back only if it
 * still finds it), so whichever unlinks first owns it and nothing is
 * completed twice. A reply that arrives later finds no handle, PUT_REPLY
 * answers ENOENT, and vdiscd logs it and carries on (D68).
 */
static void
vdisc_expire(void)
{
    struct vdisc_pending *dead = NULL, *p, *last = NULL;
    struct vdisc_pending **pp;
    unsigned long flags;
    int n = 0;

    spin_lock_irqsave(&io_request_lock, flags);
    pp = &vdisc_todo_head;
    while (*pp) {
        p = *pp;
        if (time_after(jiffies, p->queued + VDISC_DATA_TIMEOUT)) {
            *pp = p->next;
            p->next = dead;
            dead = p;
            n++;
        } else {
            last = p;
            pp = &p->next;
        }
    }
    vdisc_todo_tail = last;
    pp = &vdisc_inflight;
    while (*pp) {
        p = *pp;
        if (time_after(jiffies, p->queued + VDISC_DATA_TIMEOUT)) {
            *pp = p->next;
            p->next = dead;
            dead = p;
            n++;
        } else {
            pp = &p->next;
        }
    }
    spin_unlock_irqrestore(&io_request_lock, flags);

    while (dead) {
        p = dead->next;
        dead->req->errors++;
        vdisc_end_request_locked(dead->req, 0);
        vdisc_pending_put(dead);
        dead = p;
    }
    if (n > 0)
        printk(KERN_ERR VSOUND_TS "vdisc: %d read(s) waited more than %d s"
                        " for the daemon - failed with an I/O error\n",
               jiffies, n, (int) (VDISC_DATA_TIMEOUT / HZ));
}

/* ------------------------------------------------------------------ *
 * The worker thread
 *
 * It does less than its name suggests, and that is deliberate. Replies
 * complete in the PUT_REPLY path, in the daemon's own context, so there
 * is nothing for a thread to carry. What it exists for is CLEANUP: when
 * the daemon vanishes, something has to fail every outstanding request,
 * and vdisc_end_request_locked() cannot run from the interrupt context
 * that would otherwise notice.
 * ------------------------------------------------------------------ */

static int
vdisc_thread(void *arg)
{
    (void) arg;

    lock_kernel();
    exit_mm(current);
    exit_files(current);
    current->session = 1;
    current->pgrp = 1;
    strcpy(current->comm, "vdisc");
    sigfillset(&current->blocked);
    unlock_kernel();

    vdisc_thread_running = 1;
    up(&vdisc_thread_sem);

    /*
     * ONCE A SECOND, NOT ONLY WHEN WOKEN - design/54 D07: the expiry
     * needs a clock. Every signal is blocked above, so the timeout is
     * the only thing besides a wake that ends the sleep; module unload
     * still wakes it at once.
     */
    while (!vdisc_thread_exit) {
        interruptible_sleep_on_timeout(&vdisc_thread_wq, HZ);
        if (vdisc_thread_exit)
            break;
        if (!vdisc_daemon_open)
            vdisc_fail_all();
        else
            vdisc_expire();
    }

    vdisc_thread_running = 0;
    up(&vdisc_thread_sem);
    return 0;
}

/* ------------------------------------------------------------------ *
 * /dev/vdiscctl
 * ------------------------------------------------------------------ */

/*
 * A REPORTER IS NOT THE DAEMON - design/26 B13, design/54 D36,
 * 2026-10-04.
 *
 * vdiscd's play child forks and never execs, and it needs to push its
 * position (VDISC_IOC_PUT_POS). It used to do that through the daemon's
 * own descriptor, inherited - so a child that outlived a killed daemon
 * kept the daemon's FILE open: vdisc_ctl_release() never ran, the drives
 * stayed attached with reads queueing for nobody, a new vdiscd got
 * EBUSY and rmmod was refused.
 *
 * Now the child opens the node WRITE-ONLY for itself. That open is a
 * reporter: it does not count as the daemon, is not refused while one
 * runs, may only PUT_POS, and its close tears nothing down. The daemon
 * opens O_RDWR as before. The node's mode (root:vlhe 0660) is still the
 * gate on who may report at all.
 */
#define VDISC_CTL_REPORTER  ((void *) 1)

static int
vdisc_ctl_ioctl(struct inode *inode, struct file *file,
                unsigned int cmd, unsigned long arg)
{
    (void) inode;

    if (file->private_data == VDISC_CTL_REPORTER
        && cmd != VDISC_IOC_PUT_POS)
        return -EPERM;

    switch (cmd) {
    case VDISC_IOC_ATTACH:
        return vdisc_do_attach(arg);
    case VDISC_IOC_DETACH:
        return vdisc_do_detach(arg);
    case VDISC_IOC_GET_REQ:
        return vdisc_do_get_req(arg);
    case VDISC_IOC_PUT_REPLY:
        return vdisc_do_put_reply(arg);
    case VDISC_IOC_PUT_POS:
        return vdisc_do_put_pos(arg);
    default:
        return -ENOTTY;
    }
}

static int
vdisc_ctl_open(struct inode *inode, struct file *file)
{
    (void) inode;

    /* A WRITE-ONLY OPEN IS A REPORTER, never the daemon - see above. */
    if (!(file->f_mode & FMODE_READ)) {
        file->private_data = VDISC_CTL_REPORTER;
        MOD_INC_USE_COUNT;
        return 0;
    }

    /* One daemon. A second would race the first for requests. */
    if (vdisc_daemon_open)
        return -EBUSY;
    vdisc_daemon_open = 1;
    MOD_INC_USE_COUNT;
    return 0;
}

static int
vdisc_ctl_release(struct inode *inode, struct file *file)
{
    int i;

    (void) inode;

    /* A reporter's close is only a reporter going away. */
    if (file->private_data == VDISC_CTL_REPORTER) {
        MOD_DEC_USE_COUNT;
        return 0;
    }

    vdisc_daemon_open = 0;

    /*
     * Every disc goes away with the daemon that served it. Leaving them
     * attached would advertise drives whose reads can only fail, and
     * isofs caches what it read from them.
     */
    for (i = 0; i < vdisc_ndevs; i++) {
        vdisc_devs[i].attached      = 0;
        vdisc_devs[i].media_changed = 1;
        vdisc_devs[i].audio_status  = CDROM_AUDIO_NO_STATUS;
        vdisc_devs[i].audio_pos     = 0;
        vdisc_devs[i].audio_trk     = 0;
        vdisc_sizes[i]              = 0;
    }

    vdisc_fail_all();
    MOD_DEC_USE_COUNT;
    return 0;
}

static struct file_operations vdisc_ctl_fops = {
    NULL,                   /* llseek   */
    NULL,                   /* read     */
    NULL,                   /* write    */
    NULL,                   /* readdir  */
    NULL,                   /* poll     */
    vdisc_ctl_ioctl,
    NULL,                   /* mmap     */
    vdisc_ctl_open,
    NULL,                   /* flush    */
    vdisc_ctl_release
};

static struct miscdevice vdisc_ctl_misc = {
    MISC_DYNAMIC_MINOR,
    "vdiscctl",
    &vdisc_ctl_fops
};

/*
 * NO BLOCK fops TABLE OF OUR OWN.
 *
 * The uniform CD-ROM layer exports a complete one (cdrom.h:771) wiring
 * block_read/block_write to cdrom_open, cdrom_release and cdrom_ioctl,
 * and register_blkdev() takes it directly. Hand-rolling an equivalent
 * duplicates a table the kernel already maintains and gets it wrong the
 * moment the layer's own changes.
 */

/* ------------------------------------------------------------------ *
 * Module init / cleanup
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * /proc/vdisc - WHERE EACH DRIVE'S AUDIO IS, READABLE WITHOUT A NODE  *
 * ------------------------------------------------------------------ */

/*
 * THE CD+G VIEWER POLLS THE PLAY POSITION AT 15 Hz (vlhe_mod_cdg.c),
 * and until 2026-10-01 the only way to ask was to open /dev/vdiscN,
 * CDROMSUBCHNL, close - and every close reached the uniform layer's
 * cdrom_release(), which calls lock_door(0) on the last opener: 15
 * opens and 15 lock_door lines a second for as long as a disc was
 * known (design/47 G1). This is the same answer with no node and no
 * layer: one block per drive, `key: value' lines as /proc/vsound.
 *
 * It reads vdisc_audio_sample(), so the stall rule and the track clamp
 * are the ioctl's; it does NOT consume an ERROR status (see the helper).
 * The same `#ifdef CONFIG_PROC_FS' shape as vsound's, and the user
 * space reader falls back to the ioctl when the entry is absent (an
 * older module).
 */
#ifdef CONFIG_PROC_FS

static struct proc_dir_entry *vdisc_proc_ent;

static int
vdisc_proc_get_info(char *buffer, char **start, off_t offset,
                    int length, int inout)
{
    int len = 0;
    int i;

    (void) start; (void) offset; (void) length; (void) inout;

    /* MODULE-WIDE, BEFORE THE FIRST `drive:' - 2026-10-06, design/39
     * 3h. Whether this build has the CD-ROM packet interface (2.2.16
     * and later): the CD page greys "Answer MMC packet commands" when
     * it does not. Readers of the drive blocks skip lines before the
     * first `drive:', so older ones are not confused by it. */
#ifdef VDISC_HAVE_PACKET
    len += sprintf(buffer + len, "packet_interface: 1\n");
#else
    len += sprintf(buffer + len, "packet_interface: 0\n");
#endif

    for (i = 0; i < vdisc_ndevs && len < 3800; i++) {
        struct vdisc_device *dev = &vdisc_devs[i];
        int pos = 0, trk = 0;
        const char *st;

        len += sprintf(buffer + len, "drive: %d\n", i);
        len += sprintf(buffer + len, "attached: %d\n", dev->attached ? 1 : 0);
        if (!dev->attached) {
            len += sprintf(buffer + len, "status: none\n");
            continue;
        }
        vdisc_audio_sample(dev, &pos, &trk);
        switch (dev->audio_status) {
        case CDROM_AUDIO_PLAY:      st = "play";      break;
        case CDROM_AUDIO_PAUSED:    st = "paused";    break;
        case CDROM_AUDIO_COMPLETED: st = "completed"; break;
        case CDROM_AUDIO_ERROR:     st = "error";     break;
        case CDROM_AUDIO_INVALID:   st = "invalid";   break;
        default:                    st = "stopped";   break;
        }
        len += sprintf(buffer + len, "status: %s\n", st);
        len += sprintf(buffer + len, "lba: %d\n", pos);
        len += sprintf(buffer + len, "track: %d\n", trk);
        /* OUR OWN LOCK FLAG, so vlhe_locked() need not open the node
         * (VDISC_IOC_GET_LOCK) once a second per drive - C5. */
        len += sprintf(buffer + len, "locked: %d\n", dev->locked ? 1 : 0);
    }
    return len;
}

static void
vdisc_proc_start(void)
{
    vdisc_proc_ent = create_proc_entry("vdisc", S_IFREG | S_IRUGO, NULL);
    if (vdisc_proc_ent != NULL)
        vdisc_proc_ent->get_info = vdisc_proc_get_info;
}

static void
vdisc_proc_stop(void)
{
    if (vdisc_proc_ent != NULL) {
        remove_proc_entry("vdisc", NULL);
        vdisc_proc_ent = NULL;
    }
}

#else
static void vdisc_proc_start(void) { }
static void vdisc_proc_stop(void)  { }
#endif

int
init_module(void)
{
    int i, rc;

    vdisc_pool_init();              /* before any request can arrive */

    /*
     * THE DRIVE COUNT.
     *
     * The ceiling is a compile-time 8 and the arrays are always that
     * wide - see the long comment in vdisc.h. What vdisc_ndevs controls
     * is how many drives are REGISTERED with the CD-ROM layer, and that
     * is fixed for the life of the load because registration happens
     * here, by name.
     *
     * Clamping rather than refusing: cdemu does the same
     * (virtualcd.c:742-745), and a typo in a modules.conf that stops a
     * machine booting its CD is worse than one that quietly gives the
     * default.
     */
    if (vdisc_ndevs < 1 || vdisc_ndevs > VDISC_MAX_DEVS) {
        printk(KERN_WARNING VSOUND_TS "vdisc: discs=%d out of range (1..%d), "
                            "using %d\n",
               jiffies,
               vdisc_ndevs, VDISC_MAX_DEVS, VDISC_DEF_DEVS);
        vdisc_ndevs = VDISC_DEF_DEVS;
    }

    /* Bounded before it indexes anything: blk_dev[], blk_size[] and the
     * rest are MAX_BLKDEV wide. design/09's open item. */
    if (vdisc_major <= 0 || vdisc_major >= MAX_BLKDEV) {
        printk(KERN_ERR VSOUND_TS "vdisc: vdisc_major=%d out of range"
                                  " (1..%d)\n",
               jiffies, vdisc_major, MAX_BLKDEV - 1);
        return -EINVAL;
    }

    /*
     * THE PACKET EXPERIMENT. The capability lives in a SECOND ops
     * table (`capability' is const, see the note beside it); each
     * drive is pointed at one or the other below.
     *
     * Announced loudly and unconditionally - not behind vdisc_trace -
     * because this changes how EIGHT WORKING IOCTLS are routed, and a
     * run carrying that must say so in its own log rather than leaving
     * it to be inferred from behaviour that does not match the code.
     */
#ifdef VDISC_HAVE_PACKET
    if (vdisc_packet) {
        printk(KERN_INFO VSOUND_TS "vdisc: vdisc_packet=1 -"
               " answering MMC packet commands: READ_CD is served,"
               " the rest fall back to the ordinary ioctls.\n",
               jiffies);
    }
#else
    if (vdisc_packet) {
        printk(KERN_WARNING VSOUND_TS "vdisc: this kernel has no CD-ROM"
               " packet interface (it arrived in 2.2.16) - vdisc_packet"
               " ignored; ripping and Video CD are unavailable\n",
               jiffies);
        vdisc_packet = 0;
    }
#endif

    if (register_blkdev(vdisc_major, DEVICE_NAME, &cdrom_fops)) {

        printk(KERN_ERR VSOUND_TS "vdisc: cannot get major %d\n",
               jiffies, vdisc_major);
        return -EIO;
    }

    blk_dev[vdisc_major].request_fn = DEVICE_REQUEST;

    /*
     * Fill the FULL width, not vdisc_ndevs. A minor beyond the drives
     * we advertise still has a slot the block layer can index - see
     * ll_rw_blk.c:646 - and it must read as a zero-sized device rather
     * than whatever was in memory.
     */
    for (i = 0; i < VDISC_MAX_DEVS; i++) {
        vdisc_blocksizes[i] = VDISC_SECTOR_SIZE;
        vdisc_hardsects[i]  = VDISC_HARDSECT;
        vdisc_sizes[i]      = 0;
    }
    blksize_size[vdisc_major]  = vdisc_blocksizes;
    hardsect_size[vdisc_major] = vdisc_hardsects;
    blk_size[vdisc_major]      = vdisc_sizes;

    read_ahead[vdisc_major] = 8;

    for (i = 0; i < vdisc_ndevs; i++) {
        struct vdisc_device *dev = &vdisc_devs[i];

        dev->minor    = i;
        dev->attached = 0;

#ifdef VDISC_HAVE_PACKET
        dev->info.ops       = vdisc_packet ? &vdisc_dops_packet
                                          : &vdisc_dops;
#else
        dev->info.ops       = &vdisc_dops;
#endif
        dev->info.next      = NULL;
        dev->info.handle    = dev;
        dev->info.dev       = MKDEV(vdisc_major, i);
        dev->info.mask      = 0;
        dev->info.speed     = 20;
        dev->info.capacity  = 1;
        dev->info.options   = 0;
        dev->info.mc_flags  = 0;
        dev->info.use_count = 0;
        strcpy(dev->info.name, DEVICE_NAME);

        if (register_cdrom(&dev->info) != 0) {
            printk(KERN_ERR VSOUND_TS "vdisc: register_cdrom failed for vdisc%d\n",
               jiffies, i);
            dev->registered = 0;
            continue;
        }
        dev->registered = 1;
    }

    rc = misc_register(&vdisc_ctl_misc);
    if (rc) {
        printk(KERN_ERR VSOUND_TS "vdisc: cannot register control device\n", jiffies);
        goto fail;
    }

    vdisc_thread_exit = 0;
    vdisc_thread_pid  = kernel_thread(vdisc_thread, NULL,
                                      CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
    if (vdisc_thread_pid < 0) {
        printk(KERN_ERR VSOUND_TS "vdisc: cannot start worker thread\n", jiffies);
        misc_deregister(&vdisc_ctl_misc);
        rc = -EAGAIN;
        goto fail;
    }
    down(&vdisc_thread_sem);        /* wait for it to be running */

    vdisc_proc_start();

    /* THE BUILD STAMP - see the same comment in vsound_dev.c. A stale
     * module cost two test runs on 2026-08-27 before anyone noticed. */
    printk(KERN_INFO VSOUND_TS "vdisc: built %s %s\n",
               jiffies, __DATE__, __TIME__);

    printk(KERN_INFO VSOUND_TS "vdisc: " VDISC_VERSION " loaded, major %d, "
                     "%d drive%s (max %d), control minor %d\n",
               jiffies,
           vdisc_major, vdisc_ndevs, vdisc_ndevs == 1 ? "" : "s",
           VDISC_MAX_DEVS, vdisc_ctl_misc.minor);
    return 0;

fail:
    for (i = 0; i < vdisc_ndevs; i++)
        if (vdisc_devs[i].registered)
            unregister_cdrom(&vdisc_devs[i].info);
    blk_size[vdisc_major]      = NULL;
    blksize_size[vdisc_major]  = NULL;
    hardsect_size[vdisc_major] = NULL;
    blk_dev[vdisc_major].request_fn = NULL;
    unregister_blkdev(vdisc_major, DEVICE_NAME);
    return rc;
}

void
cleanup_module(void)
{
    int i;

    /* Stop the thread first: it touches the queues we are about to
     * tear down. */
    vdisc_thread_exit = 1;
    wake_up(&vdisc_thread_wq);
    if (vdisc_thread_running)
        down(&vdisc_thread_sem);

    vdisc_proc_stop();
    misc_deregister(&vdisc_ctl_misc);

    for (i = 0; i < vdisc_ndevs; i++)
        if (vdisc_devs[i].registered)
            unregister_cdrom(&vdisc_devs[i].info);

    vdisc_fail_all();

    blk_size[vdisc_major]      = NULL;
    blksize_size[vdisc_major]  = NULL;
    hardsect_size[vdisc_major] = NULL;
    blk_dev[vdisc_major].request_fn = NULL;
    read_ahead[vdisc_major]    = 0;

    unregister_blkdev(vdisc_major, DEVICE_NAME);

    printk(KERN_INFO VSOUND_TS "vdisc: unloaded\n", jiffies);
}
