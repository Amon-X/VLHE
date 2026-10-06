/*
 * vdiscd.c - the disc daemon.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * Serves block reads for vdisc.o out of a disc image, and runs CD audio
 * through vsound as an ordinary client. design/07-vsound.md section 1
 * for the four components, 3.1 for how CD audio reaches the mixer.
 *
 * WHAT THIS IS NOT: a rename of vcdd.c. That file is 2435 lines, and
 * most of it is a sound path this daemon does not have - the collector
 * ring, the shared buffer, the fragment protocol, the card geometry
 * negotiation. vsound owns all of it now. What remains is a block
 * server and a supervisor for one audio child per drive.
 *
 * THE SHAPE:
 *
 *   main loop     blocks in GET_REQ until the kernel has work. An idle
 *                 daemon uses no CPU.
 *   block reads   answered here, synchronously, from the image.
 *   audio         handed to a CHILD over a pipe (3.1b). The child holds
 *                 the vsound channel and does the reading; if it wedges
 *                 it can be killed without taking the block server with
 *                 it.
 *
 * WHY AUDIO IS FIRE-AND-FORGET: the kernel queues play/stop without a
 * pending block request, so there is no handle for a reply to match.
 * The ioctl caller never waits on us, which is what stops a stuck child
 * putting an application into D state.
 *
 * C89 / GCC 2.95 clean.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "vdisc.h"
#include "image.h"
#include "vdiscd_child.h"
#include "vdiscd_play.h"
#include "vdiscd_ctl.h"
#include "vdiscd_state.h"
#include "vlhe_status.h"

#define CTL_DEVICE  "/dev/vdiscctl"

/*
 * A reply plus its payload in one allocation, because the kernel reads
 * the data from immediately after the header (vdisc_mod.c's
 * vdisc_do_put_reply). Two separate buffers would not be contiguous.
 */
struct reply_buf {
    struct vdisc_reply hdr;
    unsigned char      data[VDISC_MAX_XFER];
};

static volatile int stop_requested;

static void
on_signal(int sig)
{
    (void) sig;
    stop_requested = 1;
}

/* ------------------------------------------------------------------ *
 * Serving a block read
 * ------------------------------------------------------------------ */

static int
serve_read(struct vdisc_image *img, const struct vdisc_req *req,
           struct reply_buf *rb)
{
    unsigned int want;

    /*
     * Check the count BEFORE multiplying. count * SECTOR_SIZE overflows
     * an unsigned int above 2097151, and a wrapped product could slip
     * past a check made afterwards.
     *
     * The kernel already bounds this. The daemon checks anyway, because
     * it should not depend on the other side being correct - the same
     * reasoning that makes the kernel validate what the daemon sends.
     */
    if (req->count == 0 ||
        req->count > VDISC_MAX_XFER / VDISC_SECTOR_SIZE) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }
    want = req->count * VDISC_SECTOR_SIZE;

    if (img->backend->read_data(img, (int) req->lba, (int) req->count,
                                rb->data) < 0) {
        fprintf(stderr, "vdiscd: read failed at lba %lu count %lu\n",
                (unsigned long) req->lba, (unsigned long) req->count);
        rb->hdr.status = -EIO;
        rb->hdr.length = 0;
        return -1;
    }

    rb->hdr.status = 0;
    rb->hdr.length = want;
    return 0;
}

/*
 * Serving a RAW AUDIO read - CDROMREADAUDIO, via generic_packet.
 *
 * The same shape as serve_read above, in a different unit: 2352-byte
 * frames addressed by disc LBA, against read_audio() rather than
 * read_data(). design/27 section 8 has why the two are separate
 * opcodes rather than one with a flag.
 *
 * THE DATA TRACK REFUSAL IS THE POINT OF THE TRACK LOOKUP, and it is
 * the same rule vdisc_mod.c enforces for PLAYTRKIND (:862, :933).
 * MMC-2 Table 3 makes it the standard's requirement too: "attempt to
 * play a data block as audio" is ILLEGAL REQUEST. A ripper that asks
 * for a data track's frames should be refused, not handed 2352-byte
 * lumps of filesystem - which is exactly the bug vcd_mod.c had, where
 * a data track was playable on two machines.
 */
/*
 * SERVE READ_SUB - 96 bytes of raw subchannel a sector.
 *
 * NO CALLER TODAY (2026-10-05): the module never sends this op, and
 * the CD+G Viewer reads the image itself (design/34 section 6e2).
 * Kept for a module passthrough or a Q-channel CDROMSUBCHNL - vdisc.h
 * has the detail. Nothing exercises it.
 *
 * vdisc.h's VDISC_OP_READ_SUB has the why, and design/34 the scope.
 * NO TRACK-TYPE CHECK, unlike serve_raw(): subchannel exists on every
 * track of a disc that carries it, data tracks included, and CD-TEXT
 * lives in the lead-in rather than in any track at all. Refusing a
 * data track here would be serve_raw's audio_only bug in a new place
 * - 86Box run 107, where that check refused an entire Video CD.
 *
 * -ENODATA IS THE INTERESTING ANSWER. read_sub returns -2 when the
 * track's file has no room for subchannel, and that is not an error
 * in the request: it is the disc saying it was ripped without one.
 */
static int
serve_sub(struct vdisc_image *img, const struct vdisc_req *req,
          struct reply_buf *rb)
{
    unsigned int want;
    int rc;

    if (req->count == 0 || req->count > VDISC_MAX_RAW_FRAMES) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }
    want = req->count * VDISC_SUB_SIZE;

    if (want > sizeof rb->data) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }

    rc = vdisc_image_read_sub(img, (int) req->lba, (int) req->count,
                              rb->data);
    if (rc == -2) {
        /* The disc has no subchannel at all. A caller asking for
         * CD+G on a plain audio rip gets this, and it is the honest
         * answer rather than 96 zero bytes. */
        rb->hdr.status = -ENODATA;
        rb->hdr.length = 0;
        return -1;
    }
    if (rc != 0) {
        rb->hdr.status = -EIO;
        rb->hdr.length = 0;
        return -1;
    }

    rb->hdr.status = 0;
    rb->hdr.length = want;
    return 0;
}

static int
serve_raw(struct vdisc_image *img, const struct vdisc_req *req,
          struct reply_buf *rb, int audio_only)
{
    const struct vdisc_track *t;
    unsigned int want;
    int lba, n, i;

    /*
     * Count first, before multiplying - the same overflow reasoning as
     * serve_read, and the same principle: the kernel bounds this and
     * the daemon checks anyway.
     */
    if (req->count == 0 || req->count > VDISC_MAX_RAW_FRAMES) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }
    want = req->count * VDISC_RAW_FRAME;

    lba = (int) req->lba;
    n   = (int) req->count;

    /*
     * EVERY FRAME MUST BE IN AN AUDIO TRACK, not merely the first -
     * WHEN THE CALLER ASKED FOR AUDIO. A read starting one frame
     * before a data track and running into it would otherwise return
     * filesystem bytes for the tail. The layer asks for at most 8
     * frames, so this loop is bounded and cheap.
     *
     * `audio_only' IS WHAT SEPARATES THE TWO CALLERS, and 86Box run
     * 107 is why it exists. CDROMREADAUDIO is a play-shaped request
     * and the refusal is right. **CDROMREADRAW IS NOT**: it asks for
     * raw sectors from wherever they are, and on a VIDEO CD EVERY
     * TRACK IS DATA - so applying this check to it refused the entire
     * disc, 75 reads in a row, and MPlayer could not play anything.
     *
     * The bounds check below runs EITHER WAY. Only the track-type
     * refusal is conditional.
     */
    for (i = 0; i < n; i++) {
        t = vdisc_image_track_at(img, lba + i);
        if (t == NULL) {
            rb->hdr.status = -EINVAL;
            rb->hdr.length = 0;
            return -1;
        }
        if (audio_only && t->is_data) {
            fprintf(stderr, "vdiscd: read_audio refused - lba %d is in"
                            " a DATA track\n", lba + i);
            rb->hdr.status = -EINVAL;
            rb->hdr.length = 0;
            return -1;
        }
    }

    if (img->backend->read_audio(img, lba, n, rb->data) < 0) {
        fprintf(stderr, "vdiscd: read_audio failed at lba %d count %d\n",
                lba, n);
        rb->hdr.status = -EIO;
        rb->hdr.length = 0;
        return -1;
    }

    rb->hdr.status = 0;
    rb->hdr.length = want;
    return 0;
}

/*
 * Serving a MODE 2 read - CDROMREADMODE2, via generic_packet.
 *
 * The same shape as serve_read_audio, in the third unit: 2336-byte
 * sectors from the subheader onward.
 *
 * NO TRACK-TYPE CHECK, and that is the difference from the audio path.
 * serve_read_audio refuses a data track because playing one as audio
 * is a real error (MMC-2 Table 3, and the bug vcd_mod.c had). Here the
 * caller is ASKING for Mode 2 data and knows what it wants; a drive
 * returns the sector. An AUDIO track has no subheader to return, and
 * the backend fails that on stride, which is the right refusal and
 * comes for free.
 */
static int
serve_read_mode2(struct vdisc_image *img, const struct vdisc_req *req,
                 struct reply_buf *rb)
{
    unsigned int want;

    if (req->count == 0 || req->count > VDISC_MAX_MODE2_SECTORS) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }
    want = req->count * VDISC_MODE2_SECTOR;

    if (vdisc_image_read_mode2(img, (int) req->lba, (int) req->count,
                               rb->data) < 0) {
        fprintf(stderr, "vdiscd: read_mode2 failed at lba %lu count %lu\n",
                (unsigned long) req->lba, (unsigned long) req->count);
        rb->hdr.status = -EIO;
        rb->hdr.length = 0;
        return -1;
    }

    rb->hdr.status = 0;
    rb->hdr.length = want;
    return 0;
}

/*
 * Serving the GENERAL read - VDISC_OP_READ_CD, whose `arg0' names the
 * pieces wanted. design/30.
 *
 * THIS SUBSUMES THE THREE ABOVE and they remain only because the
 * kernel still sends them for the paths already proven on a target.
 * Each is one mask:
 *
 *   READ_AUDIO   SYNC|HEADER|SUBHDR|DATA|EDC|AUDIO
 *   READ_RAW     SYNC|HEADER|SUBHDR|DATA|EDC
 *   READ_MODE2   SUBHDR|DATA|EDC
 *
 * THE LENGTH IS NOT KNOWN UNTIL THE SECTORS ARE READ, because it
 * depends on each sector's own form - a Form 2 sector yields 2324
 * bytes of user data where a Form 1 yields 2048. So the reply length
 * is computed from what was actually assembled, not predicted.
 *
 * A ZERO MASK IS NOT AN ERROR. MMC-2 6.1.13: "If all the fields
 * contain zero then no information is returned. This condition shall
 * not be considered an error." A zero-length success is the answer,
 * and it is what a capability probe expects - `design/30' Finding 2.
 */
static int
serve_read_cd(struct vdisc_image *img, const struct vdisc_req *req,
              struct reply_buf *rb)
{
    unsigned int want = req->arg0;
    unsigned int max;
    int total = 0;

    if (req->count > VDISC_MAX_RAW_FRAMES) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }

    /*
     * BOUND THE WORST CASE BEFORE READING. The largest any sector can
     * yield is every piece of a Mode 1 sector plus the C2 block, and
     * the reply buffer must hold count of those.
     */
    max = (unsigned int) vdisc_sector_piece_len(want, 0, 0);
    if (max < (unsigned int) vdisc_sector_piece_len(want, 1, 0))
        max = (unsigned int) vdisc_sector_piece_len(want, 1, 0);
    if (max < (unsigned int) vdisc_sector_piece_len(want, 1, 1))
        max = (unsigned int) vdisc_sector_piece_len(want, 1, 1);
    if (req->count != 0 && max != 0 &&
        req->count > VDISC_MAX_XFER / max) {
        rb->hdr.status = -EINVAL;
        rb->hdr.length = 0;
        return -1;
    }

    if (req->count == 0 || want == 0) {
        rb->hdr.status = 0;         /* nothing asked for; not an error */
        rb->hdr.length = 0;
        return 0;
    }

    if (vdisc_image_read_pieces(img, (int) req->lba, (int) req->count,
                                want, rb->data, &total) < 0) {
        fprintf(stderr, "vdiscd: read_cd failed at lba %lu count %lu"
                        " want 0x%02x\n",
                (unsigned long) req->lba, (unsigned long) req->count,
                want);
        rb->hdr.status = -EIO;
        rb->hdr.length = 0;
        return -1;
    }

    rb->hdr.status = 0;
    rb->hdr.length = (__u32) total;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Attach / detach
 * ------------------------------------------------------------------ */

static int
do_attach(int ctl_fd, struct vdisc_image *img, int minor)
{
    struct vdisc_attach att;

    vdisc_image_fill_attach(img, minor, &att);

    if (ioctl(ctl_fd, VDISC_IOC_ATTACH, &att) < 0) {
        fprintf(stderr, "vdiscd: attach: %s\n", strerror(errno));
        return -1;
    }

    printf("vdiscd: /dev/vdisc%d <- %s%s\n", minor,
           img->n_files ? img->file[0].path : img->desc,
           img->n_files > 1 ? " (+ more files)" : "");
    printf("        %d track%s, lead-out %d",
           img->n_tracks, img->n_tracks == 1 ? "" : "s", img->leadout_lba);
    if (att.data_sectors)
        printf(", data track at %lu for %lu sectors",
               (unsigned long) att.data_start,
               (unsigned long) att.data_sectors);
    printf("\n");
    return 0;
}

/* A mount point, long enough for anything /proc/mounts will hand
 * back; the reply line itself is VDISCD_CTL_LINE and truncates. */
#define VLHE_MOUNT_MAX 256

/*
 * IS THIS DRIVE MOUNTED, AND WHERE?
 *
 * `eject' 1.5 does exactly this before it will eject (eject.c:418):
 * it reads the mount table, and with no -u or -f it REFUSES, naming
 * the mount point - "`%s' is mounted at `%s', not ejected". That
 * message is the useful half. A drive that is mounted is not "in use
 * by a program"; it is mounted, and the answer is umount.
 *
 * /proc/mounts FIRST, /etc/mtab as the fallback - the same order and
 * the same reason as eject.c:411-414. mtab can be stale or missing on
 * a read-only root; /proc/mounts is the kernel's own answer.
 *
 * Returns 0 and fills `where' if mounted, -1 otherwise.
 */
static int
mounted_at(int minor, char *where, size_t len)
{
    FILE *fp;
    char  node[64];
    char  line[512];
    char  dev[256];
    char  dir[256];

    if (where == NULL || len == 0)
        return -1;
    where[0] = '\0';

    sprintf(node, "/dev/vdisc%d", minor);

    fp = fopen("/proc/mounts", "r");
    if (fp == NULL)
        fp = fopen("/etc/mtab", "r");
    if (fp == NULL)
        return -1;

    while (fgets(line, sizeof line, fp) != NULL) {
        if (sscanf(line, "%255s %255s", dev, dir) != 2)
            continue;
        if (strcmp(dev, node) != 0)
            continue;
        /* BOTH ARE 256, so strncpy with len-1 is a truncation the
         * compiler is right to flag. A mount point longer than the
         * buffer is not something to half-report - say nothing and
         * let the caller print its generic message. */
        if (strlen(dir) >= len) {
            fclose(fp);
            return -1;
        }
        strcpy(where, dir);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return -1;
}

static int
do_detach(int ctl_fd, int minor, int force)
{
    /*
     * `force' IS FOR A CLIENT EJECT ONLY. vdisc.h's
     * VDISC_DETACH_FORCE has the measurement behind it: the CD-ROM
     * layer has already refused unless the caller is the sole
     * holder, so the use_count check in the driver would be
     * refusing that caller on its own behalf. cmd_detach() and
     * shutdown pass 0 and keep the strict guard - they mean
     * "nobody at all".
     */
    int m = force ? (minor | VDISC_DETACH_FORCE) : minor;

    /*
     * IT RETURNS THE FAILURE NOW, and that was the whole bug.
     *
     * This was `void' and printed to stderr, so every caller carried
     * on regardless - including cmd_detach(), which then closed the
     * image, cleared `open' and replied "ok detached". The kernel
     * refuses with -EBUSY while a client holds the drive
     * (vdisc_mod.c:2112), so on 86Box 2026-09-19 the GUI said "(no
     * disc)" and the daemon agreed, while the disc was STILL
     * ATTACHED - a freshly started Grip read all eleven tracks off a
     * drive everything claimed was empty.
     *
     * A REFUSAL IS INFORMATION. -EBUSY means "someone is using it",
     * which is exactly what the user needs to be told.
     */
    if (ioctl(ctl_fd, VDISC_IOC_DETACH, &m) < 0) {
        fprintf(stderr, "vdiscd: detach: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static void
usage(void)
{
    fprintf(stderr,
        "usage: vdiscd [-d N] [-o DSP] [-s FILE [-r]] [-q]"
        " [image.ccd|image.cue|image.iso]\n"
        "\n"
        "  -d N     drive to attach to (default 0)\n"
        "  -o DSP   where the CD audio goes (default /dev/dsp) - this is\n"
        "           vsound's node, which is the one the user picked\n"
        "  -s FILE  the drive state: kept up to date with every attach,\n"
        "           eject and `autoload' (/var/lib/vlhe/state/drives)\n"
        "  -r       reattach, at start, each drive the state marks\n"
        "           Autoload - [CD Settings] DrivesAutoLoad\n"
        "  -q       quiet: no per-command chatter\n"
        "\n"
        "Serves /dev/vdiscN from the image and plays its CD audio\n"
        "through vsound. Needs vdisc.o loaded, and vsound.o for audio.\n");
    exit(2);
}

/* ------------------------------------------------------------------ *
 * The drive table
 * ------------------------------------------------------------------ *
 *
 * ONE DAEMON, MANY DRIVES - and until 2026-09-19 this file had one
 * `struct vdisc_image' and ignored `req.minor' entirely.
 *
 * THE REST OF THE STACK WAS ALWAYS READY FOR THIS. The module
 * advertises up to VDISC_MAX_DEVS (vdisc.h:136), `vdisc_ndevs' is a
 * module parameter, the config carries `Drives = 1..8', the GUI draws
 * a row per drive - and `struct vdisc_req' has carried a `minor'
 * field from the beginning (vdisc.h). Only the daemon assumed one
 * image, because when it was written there was one.
 *
 * AND IT HAS TO BE ONE DAEMON, not one per drive: /dev/vdiscctl
 * admits a single opener (vdisc_mod.c:2580, "One daemon. A second
 * would race the first for requests"), so the second would get
 * -EBUSY.
 *
 * A DRIVE WITH NO IMAGE IS AN EMPTY DRIVE, not an error. `open' is 0
 * for a slot nobody has attached, and a request for one is answered
 * -ENXIO - which is what a real drive with no disc in it returns.
 */
struct drive {
    struct vdisc_image  img;
    struct vdiscd_child child;      /* its own CD-audio child          */
    int                 open;       /* is an image attached            */
};
/* NO path FIELD - `struct vdisc_image' already carries its own
 * (image.h:57), so a second copy could only disagree with it. */

static struct drive drives[VDISC_MAX_DEVS];

/* ------------------------------------------------------------------ *
 * The drive state - vdiscd_state.h
 * ------------------------------------------------------------------ *
 *
 * `g_state' mirrors the file: the image each drive last held and its
 * autoload flag. Changed by an attach, an eject and `autoload', and
 * written straight away - never at shutdown, when every drive is
 * detached because the DAEMON is stopping, not because a disc came
 * out; the record has to outlive that to be any use at the next load.
 */
static const char          *g_state_file;     /* -s, or NULL       */
static struct vdiscd_state  g_state;

static void
state_save(int quiet)
{
    if (g_state_file == NULL)
        return;
    if (vdiscd_state_write(g_state_file, &g_state) != 0 && !quiet)
        printf("vdiscd: could not write the drive state %s - %s\n",
               g_state_file, strerror(errno));
}

/* A drive now holds `path' (NULL: it is empty). */
static void
state_drive(int d, const char *path, int quiet)
{
    if (d < 0 || d >= VDISC_MAX_DEVS || g_state_file == NULL)
        return;
    if (path == NULL)
        g_state.path[d][0] = '\0';
    else {
        strncpy(g_state.path[d], path, VDISCD_STATE_PATH - 1);
        g_state.path[d][VDISCD_STATE_PATH - 1] = '\0';
    }
    state_save(quiet);
}

/* The drive a request names, or NULL if the minor is out of range or
 * empty. NEVER indexes the array before checking - a minor arrives
 * from the kernel, and vdisc.h:93 warns the kernel reads whatever
 * minor the device NODE carries, with no bound check. */
static struct drive *
drive_for(__u32 minor)
{
    if (minor >= VDISC_MAX_DEVS)
        return NULL;
    if (!drives[minor].open)
        return NULL;
    return &drives[minor];
}

/* ------------------------------------------------------------------ *
 * The control channel
 * ------------------------------------------------------------------ *
 *
 * design/33 section 3f. The GUI cannot attach a disc itself -
 * VDISC_IOC_ATTACH wants the parsed TOC, which is this program's
 * ~1500 lines, and /dev/vdiscctl admits one opener. So the GUI asks
 * and we do the work we already know how to do.
 *
 * THE DRAIN RUNS IN THE EINTR BRANCH of the main loop, because that
 * is the only place this daemon can be reached: it blocks in
 * ioctl(VDISC_IOC_GET_REQ) and there is no select() anywhere in it.
 * A writer signals SIGUSR1, GET_REQ returns -EINTR, and the branch
 * that already existed for SIGTERM now also looks at the channel.
 */

static volatile sig_atomic_t ctl_poked;

static void
on_usr1(int sig)
{
    (void) sig;
    ctl_poked = 1;
}

/* attach <drive> <path> */
static void
cmd_attach(int ctl_fd, const char *sdrive, const char *path, int quiet)
{
    char reply[VDISCD_CTL_LINE];
    int  d;

    if (sdrive == NULL || path == NULL) {
        vdiscd_ctl_reply("err usage: attach <drive> <path>");
        return;
    }

    d = atoi(sdrive);
    if (d < 0 || d >= VDISC_MAX_DEVS) {
        sprintf(reply, "err drive %d is out of range 0..%d",
                d, VDISC_MAX_DEVS - 1);
        vdiscd_ctl_reply(reply);
        return;
    }

    /* SWAPPING A DISC IS A DETACH THEN AN ATTACH, and the detach has
     * to reach the kernel first: the module keeps per-minor state
     * that the new TOC replaces, and attaching over it would leave
     * isofs caching sectors from the old image. */
    if (drives[d].open) {
        vdiscd_play_stop(&drives[d].child);

        /* AND IF THE KERNEL REFUSES, THE SWAP DOES NOT HAPPEN. The
         * same bug as cmd_detach's and worse in its effect: carrying
         * on would attach a new image over a disc the kernel still
         * has open, so a reader would get the old TOC and the new
         * sectors. */
        if (do_detach(ctl_fd, d, 0) != 0) {
            sprintf(reply, "err drive %d is in use - close the program"
                           " using it", d);
            vdiscd_ctl_reply(reply);
            if (!quiet)
                printf("vdiscd: drive %d NOT swapped - the kernel says"
                       " it is in use\n", d);
            return;
        }

        vdisc_image_close(&drives[d].img);
        drives[d].open = 0;
    }

    /*
     * CAN THIS PROCESS READ IT AT ALL - asked first, and the answer
     * named in the reply (2026-10-05, the G01 documentation review,
     * confirmed on 86Box the same evening). An installed vdiscd runs
     * as the `vlhe' account, so an image root can read - anything in
     * /root - is one IT cannot; the GUI, running as root, found the
     * file there and blamed a daemon that was running. access() asks
     * with the REAL uid, which is the account the daemon runs as.
     *
     * The replies the GUI tells apart (vlhe_attach_reply()):
     *   "err denied PATH"       no permission - the file or a folder
     *   "err missing PATH"      no such file
     *   "err cannot open PATH"  read, but not an image it understands
     */
    if (access(path, R_OK) != 0) {
        if (errno == EACCES || errno == EPERM)
            sprintf(reply, "err denied %.240s", path);
        else if (errno == ENOENT || errno == ENOTDIR)
            sprintf(reply, "err missing %.240s", path);
        else
            sprintf(reply, "err cannot open %.200s: %.30s", path,
                    strerror(errno));
        vdiscd_ctl_reply(reply);
        if (!quiet)
            printf("vdiscd: drive %d: cannot read %s: %s\n", d, path,
                   strerror(errno));
        return;
    }

    errno = 0;
    if (vdisc_image_open(&drives[d].img, path) < 0) {
        /* vdisc_image_open has already said why on stderr. A track
         * file the cue names can be unreadable when the cue is not -
         * the same permission answer, then. Otherwise the reply is
         * deliberately vaguer: the daemon's log has the detail. */
        if (errno == EACCES || errno == EPERM)
            sprintf(reply, "err denied %.240s", path);
        else
            sprintf(reply, "err cannot open %.240s", path);
        vdiscd_ctl_reply(reply);
        return;
    }

    if (do_attach(ctl_fd, &drives[d].img, d) < 0) {
        vdisc_image_close(&drives[d].img);
        sprintf(reply, "err the kernel refused drive %d", d);
        vdiscd_ctl_reply(reply);
        return;
    }

    drives[d].open = 1;
    state_drive(d, path, quiet);
    sprintf(reply, "ok attached %d", d);
    vdiscd_ctl_reply(reply);

    if (!quiet)
        printf("vdiscd: drive %d <- %s (control channel)\n", d, path);
}

/* detach <drive> */
static void
cmd_detach(int ctl_fd, const char *sdrive, int quiet)
{
    char reply[VDISCD_CTL_LINE];
    int  d;

    if (sdrive == NULL) {
        vdiscd_ctl_reply("err usage: detach <drive>");
        return;
    }

    d = atoi(sdrive);
    if (d < 0 || d >= VDISC_MAX_DEVS) {
        sprintf(reply, "err drive %d is out of range 0..%d",
                d, VDISC_MAX_DEVS - 1);
        vdiscd_ctl_reply(reply);
        return;
    }

    /* AN EMPTY DRIVE IS NOT AN ERROR. Ejecting nothing is what the
     * user asked for and they got it - reporting a failure would put
     * a dialog in front of someone who did no wrong. */
    if (!drives[d].open) {
        sprintf(reply, "ok drive %d was already empty", d);
        vdiscd_ctl_reply(reply);
        return;
    }

    vdiscd_play_stop(&drives[d].child);

    /*
     * THE KERNEL DECIDES, AND WE KEEP OUR STATE IN STEP WITH IT.
     * Closing the image after a refused detach is how the daemon and
     * the kernel came to disagree about whether a disc was present.
     */
    if (do_detach(ctl_fd, d, 0) != 0) {
        char where[VLHE_MOUNT_MAX];

        /*
         * SAY THE TRUE THING. "close the program using it" sends a
         * user hunting for a program when the drive is MOUNTED, and
         * the fix is umount - a different action entirely. Measured
         * 2026-09-20: the user pressed Eject on a mounted drive, got
         * that message, and the drive was not open in anything.
         */
        if (mounted_at(d, where, sizeof where) == 0)
            sprintf(reply, "err drive %d is mounted at %.200s -"
                           " unmount it first", d, where);
        else
            sprintf(reply, "err drive %d is in use - close the program"
                           " using it", d);
        vdiscd_ctl_reply(reply);
        if (!quiet)
            printf("vdiscd: control-channel detach REFUSED for drive"
                   " %d - %s\n", d,
                   where[0] ? "mounted" : "the kernel says it is in use");
        return;
    }

    vdisc_image_close(&drives[d].img);
    drives[d].open = 0;
    state_drive(d, NULL, quiet);

    sprintf(reply, "ok detached %d", d);
    vdiscd_ctl_reply(reply);

    if (!quiet)
        printf("vdiscd: drive %d ejected (control channel)\n", d);
}

/*
 * THE DRIVE /dev/cdrom REACHES, AMONG THE DRIVES LOADED - design/54
 * D20. The recorded `Cdrom = N' outlives a lowered Drives count, so it
 * is checked against what the module advertises now (/proc/vdisc); a
 * drive past that gives 0 and the record stays, so a larger count
 * brings it back. No entry (an older module) is the maximum, as before.
 */
static int
cdrom_effective(void)
{
    return vdiscd_state_cdrom_in(&g_state, vlhe_vdisc_proc_drives());
}

/* status - one line, because the protocol is one line each way */
static void
cmd_status(void)
{
    char reply[VDISCD_CTL_LINE];
    int  n = 0;
    int  i;

    /*
     * THE AUTOLOAD FLAGS FIRST - `a=' and one digit per drive, 2026-10-03.
     * Before every ` N=' token, so the path parser (which ends a path
     * at the next ` <digit>=') never sees it; a reader that does not
     * know it skips it. The CD page's "Load at startup" box draws from
     * it, on the same request - no second SIGUSR1 a tick.
     */
    n = sprintf(reply, "ok a=");
    for (i = 0; i < VDISC_MAX_DEVS; i++)
        reply[n++] = g_state.autoload[i] ? '1' : '0';
    reply[n] = '\0';
    /* AND WHICH DRIVE /dev/cdrom REACHES - ` c=N', 2026-10-03, for the
     * CD page's "/dev/cdrom" radio. Also before any ` N=' token. */
    n += sprintf(reply + n, " c=%d", cdrom_effective());
    for (i = 0; i < VDISC_MAX_DEVS; i++) {
        int t, audio = 0;

        if (!drives[i].open)
            continue;
        /*
         * THE TRACK COUNTS RIDE ALONG - design/47 C5, 2026-10-01. The
         * CD page asked `status' and then `tracks' every second, each
         * a SIGUSR1 into this daemon's GET_REQ wait - the wait that
         * feeds audio. One reply answers both: `N=T/A:path', where
         * the old form was `N=path'. The reader takes either, so an
         * older daemon still parses; `tracks' stays for the CLI.
         */
        for (t = 0; t < drives[i].img.n_tracks; t++)
            if (!drives[i].img.track[t].is_data)
                audio++;
        /* THE PATH, TRUNCATED IF IT MUST BE. A status line naming
         * eight long paths would not fit, and the GUI wants to know
         * WHICH image is in a drive more than it wants the full
         * path - it has the path it asked us to attach. */
        n += sprintf(reply + n, " %d=%d/%d:%.28s", i,
                     drives[i].img.n_tracks, audio,
                     drives[i].img.n_files ? drives[i].img.file[0].path
                                           : drives[i].img.desc);
        if (n > (int) sizeof reply - 56)
            break;
    }
    if (n == 5 + VDISC_MAX_DEVS + 4)
        strcat(reply, " no drives loaded");

    vdiscd_ctl_reply(reply);
}

/*
 * autoload <drive> <0|1> - the CD page's "Load at startup" box.
 *
 * OURS, NOT THE KERNEL'S: whether this drive's image comes back at the
 * next load. Recorded in the drive state and nowhere else - the GUI
 * asks, the daemon writes, as design/33 section 3i has it for the
 * attachments. An EMPTY drive may be flagged; the flag is the drive's,
 * so whatever goes in next comes back.
 */
static void
cmd_autoload(const char *sdrive, const char *son, int quiet)
{
    char reply[VDISCD_CTL_LINE];
    int  d;

    if (sdrive == NULL || son == NULL
        || (strcmp(son, "0") != 0 && strcmp(son, "1") != 0)) {
        vdiscd_ctl_reply("err usage: autoload <drive> <0|1>");
        return;
    }
    d = atoi(sdrive);
    if (d < 0 || d >= VDISC_MAX_DEVS) {
        sprintf(reply, "err drive %d is out of range 0..%d",
                d, VDISC_MAX_DEVS - 1);
        vdiscd_ctl_reply(reply);
        return;
    }
    if (g_state_file == NULL) {
        /* NO FILE TO KEEP IT IN - a daemon started by hand. Said, not
         * pretended: the box would otherwise tick and mean nothing. */
        vdiscd_ctl_reply("err this vdiscd keeps no drive state (no -s)");
        return;
    }
    g_state.autoload[d] = (son[0] == '1');
    if (vdiscd_state_write(g_state_file, &g_state) != 0) {
        sprintf(reply, "err could not write %.200s", g_state_file);
        vdiscd_ctl_reply(reply);
        return;
    }
    sprintf(reply, "ok autoload %d %d", d, g_state.autoload[d]);
    vdiscd_ctl_reply(reply);
    if (!quiet)
        printf("vdiscd: drive %d %s at the next load\n", d,
               g_state.autoload[d] ? "comes back" : "does not come back");
}

/*
 * cdrom <drive> - POINT /dev/cdrom AT ANOTHER DRIVE, LIVE. 2026-10-03,
 * the user's design: /dev/cdrom is made once at Load, as root, to
 * point at the inner link `vdiscd_ctl_cdrom()' in our run directory;
 * this moves the inner link, which the daemons' account may do. A
 * program holding the old drive open keeps it until it reopens - KsCD
 * at its second Eject press - and the next open gets this one.
 *
 * Recorded in the drive state, so the next load points there again.
 * Any drive in range may be chosen, empty or not: the link names the
 * drive, as a real /dev/cdrom names a drive whatever is in it.
 */
static void
cmd_cdrom(const char *sdrive, int quiet)
{
    char reply[VDISCD_CTL_LINE];
    int  d;

    if (sdrive == NULL) {
        vdiscd_ctl_reply("err usage: cdrom <drive>");
        return;
    }
    d = atoi(sdrive);
    if (d < 0 || d >= VDISC_MAX_DEVS) {
        sprintf(reply, "err drive %d is out of range 0..%d",
                d, VDISC_MAX_DEVS - 1);
        vdiscd_ctl_reply(reply);
        return;
    }
    /* AND ONLY A DRIVE THAT IS LOADED (D20) - a link to a node the
     * module does not advertise reaches nothing. */
    {
        int loaded = vlhe_vdisc_proc_drives();

        if (loaded > 0 && d >= loaded) {
            sprintf(reply, "err drive %d is not loaded - %d drive%s",
                    d, loaded, loaded == 1 ? "" : "s");
            vdiscd_ctl_reply(reply);
            return;
        }
    }
    if (vdiscd_ctl_set_cdrom(d) != 0) {
        sprintf(reply, "err could not point %.200s at drive %d - %.60s",
                vdiscd_ctl_cdrom(), d, strerror(errno));
        vdiscd_ctl_reply(reply);
        return;
    }
    g_state.cdrom = d;
    state_save(quiet);
    sprintf(reply, "ok cdrom %d", d);
    vdiscd_ctl_reply(reply);
    if (!quiet)
        printf("vdiscd: /dev/cdrom now reaches drive %d (%s)\n", d,
               vdiscd_ctl_cdrom());
}

/*
 * HOW MANY TRACKS EACH DRIVE HAS - `ok <i>=<total>/<audio> ...'.
 *
 * A SEPARATE VERB RATHER THAN MORE FIELDS ON `status', and the reason
 * is that status's reply is parsed by finding " <i>=" and reading to
 * the NEXT SPACE (vlhe_backend.c). Appending anything after the path
 * would work only for paths without spaces, and the parser's own
 * comment already admits that case is broken. A second verb leaves
 * the fragile one alone.
 *
 * WHY IT IS WANTED: the CD page has always shown "0 tracks" because
 * nothing ever told it otherwise - design/35 names it as the gap.
 * The daemon is the only thing that knows; it parsed the image.
 *
 * AN EMPTY DRIVE IS OMITTED, like `status' - a drive with no disc has
 * no tracks to report, and the caller already learns it is empty from
 * status.
 */
/*
 * PLAY, STOP AND POSITION - the CD-player verbs, design/34 6e2.
 *
 * WHY THE DAEMON AND NOT THE GUI. Nothing in userspace plays CD
 * audio except this process: `vdiscd_audio.c' does it, and until now
 * it was only ever reached through the kernel's CDROMPLAYMSF. So the
 * control centre's Play button is a REQUEST to us, in the same way
 * `attach' is.
 *
 * THIN BY DESIGN. Every one of these is a wrapper over a function
 * that already exists and is already used by the block path -
 * `vdiscd_play()', `_stop()', `_pause()', `_resume()'. Nothing new
 * happens here; the verbs only give a second caller a way in.
 *
 * AND `pos' IS WHAT THE CD+G VIEWER NEEDS. It answers an LBA, which
 * is what `struct vdisc_pos' already carries - the viewer decodes
 * the subchannel to that position itself (design/34 6e2), so the
 * only thing crossing this channel is a number.
 */
static void
cmd_play(int a1, int a2, int a3)
{
    struct drive *dr;
    int           t, lba = 0, end = 0;

    /* `play <drive> <track> [seconds]'. Track numbers are 1-based,
     * as they are on the disc and in every other message we print;
     * the optional third argument is an offset INTO that track, and
     * is how seeking is done - see below. */
    if (a1 < 0 || a1 >= VDISC_MAX_DEVS || !drives[a1].open) {
        vdiscd_ctl_reply("err no such drive");
        return;
    }
    dr = &drives[a1];

    t = a2;
    if (t < 1 || t > dr->img.n_tracks) {
        vdiscd_ctl_reply("err no such track");
        return;
    }
    if (dr->img.track[t - 1].is_data) {
        /* NAMED, NOT REFUSED SILENTLY. Asking to play track 1 of a
         * mixed-mode disc is an ordinary mistake and the answer is
         * worth reading. */
        vdiscd_ctl_reply("err that track is data");
        return;
    }

    lba = dr->img.track[t - 1].start_lba;

    /*
     * SEEKING IS PLAY WITH AN OFFSET, WHICH IS HOW KsCD DOES IT.
     *
     * There is no seek command here and there does not need to be.
     * `kscd.cpp:786' is the whole mechanism:
     *
     *     tmppos = cur_pos_rel + 30;
     *     if (tmppos < thiscd.trk[cur_track - 1].length)
     *             play_cd(cur_track, tmppos, ...);
     *
     * - it re-issues PLAY from a different point in the same track.
     * A period player had no other way, and neither do we: the
     * kernel path is CDROMPLAYMSF, which takes a start address.
     *
     * BOUNDED AT BOTH ENDS, as KsCD bounds it. Forward past the
     * track's end is REFUSED rather than clamped (`:787' simply
     * does nothing), because running into the next track is not
     * what the button means; backward is CLAMPED to the track start
     * (`:804', `tmppos > 0 ? tmppos : 0'), because the beginning is
     * a sensible place to land.
     *
     * 75 SECTORS A SECOND is CDDA by definition, not a guess.
     */
    if (a3 > 0) {
        int off = a3 * 75;

        if (off >= dr->img.track[t - 1].length) {
            vdiscd_ctl_reply("err past the end of the track");
            return;
        }
        lba += off;
    }
    /*
     * TO THE END OF THE DISC, NOT THE END OF THE TRACK.
     *
     * That is what a CD player does - press play on track 3 and it
     * runs on into 4 - and it is what `vdiscd_audio.c' expects: it
     * looks the track number up per progress line as the LBA
     * advances, and logs the transition (`track %d -> %d ... same
     * channel'). Ending at the track boundary would stop playback
     * where a real drive keeps going.
     *
     * A caller that wants one track only can send `stop'.
     */
    end = dr->img.leadout_lba;

    if (vdiscd_play(&dr->child, &dr->img, lba, end, t) < 0) {
        vdiscd_ctl_reply("err could not start playback");
        return;
    }
    {
        char reply[VDISCD_CTL_LINE];
        sprintf(reply, "ok play %d %d %d", a1, t, lba);
        vdiscd_ctl_reply(reply);
    }
}

static void
cmd_stop(int a1)
{
    if (a1 < 0 || a1 >= VDISC_MAX_DEVS || !drives[a1].open) {
        vdiscd_ctl_reply("err no such drive");
        return;
    }
    vdiscd_play_stop(&drives[a1].child);
    vdiscd_ctl_reply("ok stop");
}

static void
cmd_pause(int a1, int resume)
{
    if (a1 < 0 || a1 >= VDISC_MAX_DEVS || !drives[a1].open) {
        vdiscd_ctl_reply("err no such drive");
        return;
    }
    if (resume)
        vdiscd_play_resume(&drives[a1].child);
    else
        vdiscd_play_pause(&drives[a1].child);
    vdiscd_ctl_reply(resume ? "ok resume" : "ok pause");
}

/*
 * THERE IS NO `pos' VERB, AND THAT IS DELIBERATE - THE PARENT DOES
 * NOT HAVE THE POSITION.
 *
 * The CDDA child pushes VDISC_IOC_PUT_POS to the KERNEL itself
 * rather than relaying through here, and `vdiscd_play.c:45' gives
 * the reason: "the parent must never have to read from a child that
 * may be wedged. A child that writes its own position cannot block
 * the block server by failing to." Adding a relay to answer a GUI
 * poll would undo exactly that.
 *
 * SO THE VIEWER ASKS THE KERNEL. `CDROMSUBCHNL' on the drive node
 * returns the cached position - which is what the module keeps
 * PUT_POS for - and any process that can open /dev/vdiscN can read
 * it without going through this daemon at all. design/34 6e2.
 */

static void
cmd_tracks(void)
{
    char reply[VDISCD_CTL_LINE];
    int  n = 0;
    int  i;

    n = sprintf(reply, "ok");
    for (i = 0; i < VDISC_MAX_DEVS; i++) {
        int t, audio = 0;

        if (!drives[i].open)
            continue;
        for (t = 0; t < drives[i].img.n_tracks; t++)
            if (!drives[i].img.track[t].is_data)
                audio++;
        n += sprintf(reply + n, " %d=%d/%d", i,
                     drives[i].img.n_tracks, audio);
        if (n > (int) sizeof reply - 24)
            break;
    }
    if (n == 2)
        strcpy(reply, "ok no drives loaded");

    vdiscd_ctl_reply(reply);
}

/*
 * ONE DRIVE'S TOC - start LBA and length per track.
 *
 * WHY A SEPARATE VERB FROM `tracks'. That one is a whole-machine
 * summary (`0=11/10 1=0/0'), one field per drive, and a caller asking
 * "what is on this disc" would have to parse a line about every other
 * drive to find out. Different question, different verb.
 *
 * WHAT IT IS FOR: THE CLIENT HOLDS THE TOC, AS KsCD DOES. `cdrom.c:431'
 * computes a track-relative position as
 *
 *     cur_pos_rel = (cur_frame - cd->trk[cur_track-1].start) / 75;
 *
 * from a TOC it read once and keeps in memory, and `CDROMSUBCHNL' for
 * the frame. It never asks a daemon. With this verb our GUI can do the
 * same: read the TOC at attach, and every seek and every track lookup
 * afterwards is local arithmetic.
 *
 * THAT REMOVED A DESIGN PROBLEM RATHER THAN SOLVING ONE. A `seek' verb
 * was written here first, and it could not work: the parent does not
 * know where playback has REACHED - the CDDA child reports straight to
 * the kernel so that a wedged child cannot block the block server
 * (`vdiscd_play.c:45'). Three fixes were considered, including a new
 * ioctl; reading KsCD showed that the client holding the TOC makes all
 * three unnecessary.
 *
 * ONE LINE, SO IT IS BOUNDED. Eight tracks of "n=start/len" fit
 * comfortably; a disc with more is truncated at the buffer and the
 * caller gets what fitted, which is better than a reply it cannot
 * parse. 99 tracks is the CD maximum and would not fit - recorded as a
 * limit rather than handled, because no disc in ExampleCDs/ approaches
 * it and a second verb to page it would be machinery for nothing.
 */
static void
cmd_toc(int a1)
{
    char reply[VDISCD_CTL_LINE];
    struct drive *dr;
    int n, t;

    if (a1 < 0 || a1 >= VDISC_MAX_DEVS || !drives[a1].open) {
        vdiscd_ctl_reply("err no such drive");
        return;
    }
    dr = &drives[a1];

    n = sprintf(reply, "ok toc %d %d", a1, dr->img.n_tracks);
    for (t = 0; t < dr->img.n_tracks; t++) {
        if (n > (int) sizeof reply - 32)
            break;                      /* truncated - see above */
        n += sprintf(reply + n, " %d=%d/%d/%d",
                     dr->img.track[t].num,
                     dr->img.track[t].start_lba,
                     dr->img.track[t].length,
                     dr->img.track[t].is_data ? 1 : 0);
    }
    vdiscd_ctl_reply(reply);
}

/*
 * ONE DRIVE'S IMAGE PATH, IN FULL - `ok <path>'.
 *
 * WHY THIS EXISTS: `status' TRUNCATES TO 28 CHARACTERS and cannot
 * stop. It names every drive in one 320-byte reply, so eight long
 * paths do not fit and `%.28s' is the compromise. Its comment says
 * the GUI "has the path it asked us to attach" - which was true
 * while that field was only DISPLAYED.
 *
 * IT STOPPED BEING TRUE when the CD+G viewer began OPENING the
 * image itself (design/34 6e2). Measured on 86Box 2026-09-27:
 * /mnt/discs/infosoc/infosoc.ccd is 30 characters and arrived as
 * `/mnt/discs/infosoc/infosoc.i', so vdisc_image_open() failed,
 * and the page showed no picture, no track list and no CD-TEXT
 * while the daemon played the disc perfectly from the real path it
 * holds internally. A truncated path is fine to show and useless
 * to fopen().
 *
 * ONE DRIVE PER CALL, WHICH IS WHAT REMOVES THE CLIFF rather than
 * moving it. Widening `status' would still overflow at some number
 * of drives; asking for one path can always fit. That is the same
 * reasoning that split `toc' out of `tracks' - see cmd_tracks().
 *
 * THE PATH RUNS TO END OF LINE, so a path containing SPACES
 * survives - the caller takes everything after "ok ". `status'
 * cannot do that, because its fields are space-separated.
 */
static void
cmd_image(int a1)
{
    char reply[VDISCD_CTL_LINE];

    if (a1 < 0 || a1 >= VDISC_MAX_DEVS || !drives[a1].open) {
        vdiscd_ctl_reply("err no such drive");
        return;
    }

    /*
     * THE DESCRIPTION FILE FIRST - THE .ccd OR .cue, NOT THE DATA
     * FILE. Caught by the user, 2026-09-27: *"Is the path passing
     * the correct thing. I noticed it is a .i file that would be
     * the raw.img and not the ccd file"*.
     *
     * AND THEY WERE RIGHT. This asked for `img.file[0].path', which
     * is the BYTES - `infosoc.img', `smash.raw' - while the thing
     * the caller must reopen is the descriptor that has the TOC.
     * `vdisc_image_open()' on a bare .img would have failed or
     * misparsed, so the truncation fix would have swapped one
     * unopenable path for another and looked like the same bug.
     *
     * AND THE TRUNCATION ALMOST HID IT - BUT DID NOT, BY ONE
     * CHARACTER. `/mnt/discs/infosoc/infosoc.ccd' and `...img' are
     * both 30 characters, and 28 cuts one PAST the dot:
     *
     *     ...ccd  ->  /mnt/discs/infosoc/infosoc.c
     *     ...img  ->  /mnt/discs/infosoc/infosoc.i
     *
     * So the surviving `.i' NAMED the wrong file, which is how the
     * user caught it. One character longer in the directory and
     * both would have ended `infosoc.' with nothing to tell them
     * apart. The evidence was luck of length, not design - which is
     * the argument for this verb returning a whole path rather than
     * for trusting a truncated one to be diagnosable.
     *
     * `desc' IS EMPTY FOR A BARE .iso, which needs no descriptor -
     * there the data file IS the thing to open, so fall through to
     * it rather than refusing.
     */
    if (drives[a1].img.desc[0] != '\0') {
        sprintf(reply, "ok %.255s", drives[a1].img.desc);
        vdiscd_ctl_reply(reply);
        return;
    }

    if (drives[a1].img.n_files == 0) {
        vdiscd_ctl_reply("err no image file");
        return;
    }

    /*
     * 255, NOT 300: the caller's buffer is VLHE_PATH_MAX (256), so a
     * longer path would be cut there instead - silently, and in the
     * place that can least afford it. Bounding it here means the
     * limit is stated once, at the end that knows the protocol.
     */
    sprintf(reply, "ok %.255s", drives[a1].img.file[0].path);
    vdiscd_ctl_reply(reply);
}

/*
 * DRAIN AND ACT. Called from the EINTR branch, so it must be cheap
 * when there is nothing there - SIGTERM lands in the same place and
 * most wakeups are that.
 */
static void ctl_one(int ctl_fd, char *line, int quiet);

/*
 * EVERY QUEUED LINE, NOT ONE - 2026-10-03. One line per wakeup was
 * right while each caller sent its own SIGUSR1; with the waker
 * (vdiscd_ctl_watch) a line already read into the channel's carry is
 * invisible to its select(), so leaving it there would leave it
 * unanswered until somebody else asked something. Bounded, so a
 * flood cannot starve the block reads this daemon exists to serve.
 */
static void
ctl_service(int ctl_fd, int chan_fd, int quiet)
{
    char line[VDISCD_CTL_LINE];
    int  k;

    if (chan_fd < 0)
        return;
    for (k = 0; k < 16; k++) {
        if (vdiscd_ctl_read(chan_fd, line, sizeof line) != 1)
            return;
        ctl_one(ctl_fd, line, quiet);
    }
}

static void
ctl_one(int ctl_fd, char *line, int quiet)
{
    char *verb, *a1, *a2;

    if (vdiscd_ctl_parse(line, &verb, &a1, &a2) < 1)
        return;

    if (strcmp(verb, "attach") == 0)
        cmd_attach(ctl_fd, a1, a2, quiet);
    else if (strcmp(verb, "detach") == 0)
        cmd_detach(ctl_fd, a1, quiet);
    else if (strcmp(verb, "status") == 0)
        cmd_status();
    else if (strcmp(verb, "tracks") == 0)
        cmd_tracks();
    else if (strcmp(verb, "toc") == 0)
        cmd_toc(a1 ? atoi(a1) : -1);
    else if (strcmp(verb, "image") == 0)
        cmd_image(a1 ? atoi(a1) : -1);
    else if (strcmp(verb, "play") == 0)
        /*
         * `a2' IS THE REST OF THE LINE, NOT ONE FIELD - the parser
         * stops splitting at the third slot so a path with spaces
         * survives (`vdiscd_ctl.c:404', and /mnt/My Discs/foo.ccd
         * is an ordinary name). So `play 0 3 30' arrives as
         * a2 = "3 30" and the offset is read from inside it.
         *
         * sscanf RATHER THAN A SECOND SPLIT, because it gives the
         * count: one field means no offset, two means a seek, and
         * anything else is a malformed line we can refuse.
         */
        {
            int trk = -1, off = 0;

            if (a2 != NULL)
                (void) sscanf(a2, "%d %d", &trk, &off);
            cmd_play(a1 ? atoi(a1) : -1, trk, off);
        }
    else if (strcmp(verb, "stop") == 0)
        cmd_stop(a1 ? atoi(a1) : -1);
    else if (strcmp(verb, "pause") == 0)
        cmd_pause(a1 ? atoi(a1) : -1, 0);
    else if (strcmp(verb, "resume") == 0)
        cmd_pause(a1 ? atoi(a1) : -1, 1);
    else if (strcmp(verb, "autoload") == 0)
        cmd_autoload(a1, a2, quiet);
    else if (strcmp(verb, "cdrom") == 0)
        cmd_cdrom(a1, quiet);
    else if (strcmp(verb, "ping") == 0)
        vdiscd_ctl_reply("ok");
    else
        /* NAMED RATHER THAN IGNORED - a newer caller asking an older
         * daemon for something gets a clear refusal, which is the
         * whole compatibility story a text protocol needs. */
        vdiscd_ctl_reply("err unknown command");
}

int
main(int argc, char **argv)
{
    struct vdisc_req req;
    struct drive    *dr;
    struct reply_buf *rb;
    struct sigaction sa;
    const char *path = NULL;
    int ctl_fd, chan_fd = -1, minor = 0, quiet = 0, i, rc = 0;
    int reattach = 0;
    /*
     * LINE BUFFERING, AND IT IS NOT COSMETIC.
     *
     * load.sh redirects this daemon's stdout to DAEMON.LOG, and libc
     * block-buffers a FILE - so everything printed sits in a 4 KB
     * buffer until the process exits. On 86Box 2026-09-19 that meant
     * DAEMON.LOG held vsoundd and vmidid output and NOTHING from
     * vdiscd: it prints a handful of lines and never fills a block,
     * so its attach banner, its swap messages and the control
     * channel's own logging were all invisible while the daemon ran.
     *
     * THE OTHER TWO ONLY LOOK FINE BY ACCIDENT - vsoundd prints a
     * progress line every 500 writes and so crosses 4 KB early.
     * Neither flushed either.
     *
     * A LOG YOU CANNOT READ UNTIL THE PROGRAM EXITS IS NOT A LOG for
     * a daemon, and it is worse than useless when diagnosing a hang:
     * the evidence is in the address space of the process that will
     * not exit.
     */
    setvbuf(stdout, (char *) 0, _IOLBF, 0);


    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            minor = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            g_state_file = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0) {
            reattach = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            /* THE OUTPUT NODE - vsound's, which is the node the user
             * picked rather than always /dev/dsp (design/38). `-d' was
             * taken for the drive minor long before this was needed. */
            vdiscd_audio_set_device(argv[++i]);
        } else if (argv[i][0] == '-') {
            usage();
        } else if (!path) {
            path = argv[i];
        } else {
            usage();
        }
    }
    /*
     * NO IMAGE IS NOW ALLOWED, and `vlhe apply' is why - measured on
     * target 2026-09-21. The plan starts the daemon with no arguments
     * and lets the GUI attach over the control channel, which is the
     * model design/33 section 3f describes; this `usage()' refused
     * that and the disc server never came up.
     *
     * THE DAEMON ALREADY HANDLES AN EMPTY TABLE EVERYWHERE ELSE -
     * cmd_status() answers "ok no drives loaded" and the main loop
     * skips drives whose `open' is 0. Startup was the only place that
     * insisted on one, left over from when the command line was the
     * only way to attach anything.
     *
     * A BARE `vdiscd' IS STILL AN ODD THING TO TYPE BY HAND, so it
     * says what it is doing rather than sitting silent - the same
     * reasoning as the build stamp below.
     */
    if (!path && !quiet)
        fprintf(stderr, "vdiscd: no image given - waiting for one on "
                        "the control channel\n");

    /* THE COMMAND LINE STILL ATTACHES ONE IMAGE TO ONE DRIVE, which
     * is every existing script and every staged test. The others fill
     * in over the control channel. */
    if (minor < 0 || minor >= VDISC_MAX_DEVS) {
        fprintf(stderr, "vdiscd: drive %d is out of range 0..%d\n",
                minor, VDISC_MAX_DEVS - 1);
        return 1;
    }

    /* THE BUILD STAMP, ONCE, AT STARTUP - a stale binary is
     * indistinguishable from a current one in a log otherwise (see
     * vsound_dev.c). It used to live in do_attach(), which reprinted
     * it on every disc swap: one run of three attaches put three
     * identical "built" lines in DAEMON.LOG, seen on 86Box
     * 2026-09-19. */
    if (!quiet)
        printf("vdiscd: built %s %s\n", __DATE__, __TIME__);

    /* THE PID FILE IS NOT THE CHANNEL'S. vdiscd_ctl_open() writes one
     * too, but only if the channel opens - and the Status page must
     * be able to see a daemon that is running with no channel at
     * all. Written here, unconditionally, and removed at the clean
     * exit below. */
    /*
     * AND IT CAN REFUSE - the return is no longer ignored.
     *
     * -2 MEANS ONE IS ALREADY RUNNING. Starting a second would take
     * the device the first holds, fail, exit, and leave the pidfile
     * naming a dead process - which is exactly what happened on
     * 86Box 2026-09-23 when the user pressed Load twice: the orphaned
     * first daemon then kept the module loaded through an Unload that
     * reported success.
     *
     * A WRITE FAILURE IS NOT FATAL. The pidfile is how the Status
     * page and the teardown FIND us; a daemon that cannot write one
     * still works, and refusing to start over it would be worse than
     * being hard to stop.
     */
    {
        int prc = vlhe_status_write_pidfile("vdiscd");

        if (prc == -2) {
            fprintf(stderr, "vdiscd: already running - not starting a"
                            " second one\n");
            return 1;
        }
        if (prc != 0)
            fprintf(stderr, "vdiscd: warning: could not write the pid"
                            " file; the Status page will not see"
                            " this daemon\n");
    }

    rb = (struct reply_buf *) malloc(sizeof *rb);
    if (!rb) {
        fprintf(stderr, "vdiscd: out of memory\n");
        return 1;
    }

    /*
     * THE IMAGE IS OPTIONAL NOW - see the note at the argument loop.
     * With no path the daemon comes up with an empty drive table and
     * waits; `attach' over the control channel fills it, which is
     * what the GUI does.
     *
     * THE CONTROL DEVICE IS NOT OPTIONAL either way. Without it there
     * is no channel to be told about an image on, so a daemon that
     * could not open it would sit doing nothing at all - the failure
     * is worth reporting rather than surviving.
     */
    if (path != NULL) {
        if (vdisc_image_open(&drives[minor].img, path) < 0) {
            free(rb);
            return 1;
        }
        drives[minor].open = 1;
    }

    ctl_fd = open(CTL_DEVICE, O_RDWR);
    if (ctl_fd < 0) {
        fprintf(stderr, "vdiscd: %s: %s\n", CTL_DEVICE, strerror(errno));
        fprintf(stderr, "        is vdisc.o loaded?\n");
        if (path != NULL)
            vdisc_image_close(&drives[minor].img);
        free(rb);
        return 1;
    }

    if (path != NULL && do_attach(ctl_fd, &drives[minor].img, minor) < 0) {
        close(ctl_fd);
        vdisc_image_close(&drives[minor].img);
        free(rb);
        return 1;
    }

    /*
     * SIGTERM and SIGINT set a flag rather than exiting, so the detach
     * below always runs. Without it the kernel keeps a drive attached to
     * a daemon that no longer exists, and every read fails until the
     * control device is reopened.
     *
     * No SA_RESTART: GET_REQ must return EINTR so the loop can see the
     * flag. With restart it would block in the kernel until a request
     * arrived, and an idle daemon would ignore the signal entirely.
     */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* SIGUSR1 SETS A DIFFERENT FLAG - it means "look at the channel",
     * not "stop". Sharing on_signal would end the daemon every time
     * the GUI asked it anything. */
    sa.sa_handler = on_usr1;

    /* A wedged child is killed, not waited for - so a write to a dead
     * one must not take the daemon down with SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /*
     * THE CONTROL CHANNEL. SIGUSR1 is what brings us out of GET_REQ;
     * no SA_RESTART, for the same reason SIGTERM has none above.
     *
     * A CHANNEL THAT WILL NOT OPEN IS NOT FATAL. The daemon's job is
     * serving the drive it was given on the command line, and it can
     * do that with no channel at all - which is exactly what every
     * existing test script expects. Say so and carry on.
     */
    sigaction(SIGUSR1, &sa, NULL);
    chan_fd = vdiscd_ctl_open();
    if (chan_fd < 0) {
        if (!quiet)
            printf("vdiscd: no control channel (%s) - command line only\n",
                   strerror(errno));
    } else {
        /* THE WAKER - so a caller that may not signal us (a user's
         * GUI, with this daemon running as `vlhe') is still heard.
         * vdiscd_ctl.h has why. Without it, callers that can signal
         * still work, so a failed fork is said and survived. */
        pid_t w = vdiscd_ctl_watch(chan_fd);

        if (!quiet)
            printf("vdiscd: control channel on %s%s\n", vdiscd_ctl_fifo(),
                   w > 0 ? "" : " (no waker - only root can reach it)");
    }

    /*
     * A PLAYER PER DRIVE - design/52 AR1, 2026-10-03.
     *
     * THIS WAS ONE PLAYER, FOR THE DRIVE THE COMMAND LINE NAMED, and a
     * loop here was a 2026-09-19 regression: vdiscd_play_init() wrote
     * four FILE-SCOPE STATICS in vdiscd_play.c, so the last call won
     * and drive 0's audio reported against drive 7. Those statics are
     * now fields of each drive's `struct vdiscd_child', so the loop is
     * what it always looked like - each drive its own child, its own
     * vsound channel, its own level - and two discs play at once,
     * mixed. The user, 2026-10-01: "I would like 2 drives play at once
     * each getting their own vsound channel".
     *
     * Still LAZY: a drive holds no child and no channel until it plays
     * (section 3.1a), so eight drives cost nothing until used.
     */
    for (i = 0; i < VDISC_MAX_DEVS; i++)
        vdiscd_play_init(&drives[i].child, ctl_fd, i);

    /*
     * THE DRIVE STATE, AND WHAT COMES BACK - design/33 sections 3c and
     * 3i. Read whatever -r says, so `autoload' and the next attach
     * keep the flags already recorded. Then, with -r (the plan adds it
     * when [CD Settings] DrivesAutoLoad = 1), every drive marked
     * Autoload with an image gets it again - at boot on an installed
     * machine, at Load for a portable folder, after a restart from the
     * Status page either way.
     *
     * A DRIVE THAT CANNOT COME BACK IS LEFT EMPTY AND SAID, never
     * fatal: the image's disk may not be mounted yet, and a daemon
     * that refused to start over one disc would take every other
     * drive with it. Its record is KEPT, so the next load tries again.
     * A drive the command line already filled is not touched.
     */
    if (g_state_file != NULL) {
        int got = vdiscd_state_read(g_state_file, &g_state);

        if (!quiet)
            printf("vdiscd: drive state %s%s\n", g_state_file,
                   got == 0 ? "" : " (none yet)");
        for (i = 0; reattach && i < VDISC_MAX_DEVS; i++) {
            if (!vdiscd_state_wanted(&g_state, i) || drives[i].open)
                continue;
            if (vdisc_image_open(&drives[i].img, g_state.path[i]) < 0) {
                printf("vdiscd: drive %d: %s could not be opened -"
                       " left empty\n", i, g_state.path[i]);
                continue;
            }
            if (do_attach(ctl_fd, &drives[i].img, i) < 0) {
                vdisc_image_close(&drives[i].img);
                printf("vdiscd: drive %d: the kernel refused %s -"
                       " left empty\n", i, g_state.path[i]);
                continue;
            }
            drives[i].open = 1;
            if (!quiet)
                printf("vdiscd: drive %d <- %s (reattached)\n", i,
                       g_state.path[i]);
        }
        /* THE COMMAND LINE'S IMAGE IS AN ATTACH LIKE ANY OTHER. */
        if (path != NULL && drives[minor].open)
            state_drive(minor, path, quiet);

        /* /dev/cdrom's INNER LINK, AT THE RECORDED DRIVE - the plan
         * made it at Load; this keeps it right after a restart. Made
         * whether or not /dev/cdrom points here: unused, it is a link
         * in our own directory and costs nothing. */
        if (vdiscd_ctl_set_cdrom(cdrom_effective()) != 0
            && !quiet)
            printf("vdiscd: could not make %s - %s\n",
                   vdiscd_ctl_cdrom(), strerror(errno));
    }

    while (!stop_requested) {
        int needs_reply = 1;

        if (ioctl(ctl_fd, VDISC_IOC_GET_REQ, &req) < 0) {
            if (errno == EINTR) {
                /* THE ONE PLACE THIS DAEMON CAN BE REACHED. It
                 * blocks in the ioctl above and has no select(), so
                 * a control command arrives as a signal that breaks
                 * it and is picked up here. */
                if (ctl_poked) {
                    ctl_poked = 0;
                    ctl_service(ctl_fd, chan_fd, quiet);
                }
                continue;       /* the loop re-checks stop_requested */
            }
            fprintf(stderr, "vdiscd: get_req: %s\n", strerror(errno));
            rc = 1;
            break;
        }

        memset(&rb->hdr, 0, sizeof rb->hdr);
        rb->hdr.handle = req.handle;

        /*
         * ROUTE BY DRIVE. Until 2026-09-19 this loop ignored
         * `req.minor' and served every request from one image - so a
         * read of /dev/vdisc1 returned drive 0's data, silently.
         *
         * AN EMPTY DRIVE ANSWERS -ENXIO, which is what a real drive
         * with no disc returns, rather than an error that reads like
         * a fault. The audio opcodes below fall through to their own
         * NULL checks because they send no reply.
         */
        dr = drive_for(req.minor);
        if (dr == NULL) {
            switch (req.opcode) {
            case VDISC_OP_PLAY:
            case VDISC_OP_PAUSE:
            case VDISC_OP_RESUME:
            case VDISC_OP_STOP:
            case VDISC_OP_VOLCTRL:
            case VDISC_OP_STALL:
            case VDISC_OP_EJECT:
                /* Fire-and-forget: nothing is waiting for a reply,
                 * so an empty drive is simply silence. */
                continue;
            default:
                rb->hdr.status = -ENXIO;
                rb->hdr.length = 0;
                if (ioctl(ctl_fd, VDISC_IOC_PUT_REPLY, rb) < 0)
                    fprintf(stderr, "vdiscd: put_reply: %s\n",
                            strerror(errno));
                continue;
            }
        }

        /* EVERY DRIVE PLAYS ITS OWN AUDIO - design/52 AR1. The refusal
         * that stood here ("CD audio asked of drive N, but only drive
         * 0 can play it") is gone with the single player it guarded. */
        switch (req.opcode) {
        case VDISC_OP_READ:
            serve_read(&dr->img, &req, rb);
            break;

        case VDISC_OP_READ_AUDIO:
            serve_raw(&dr->img, &req, rb, 1);       /* audio tracks only */
            break;

        case VDISC_OP_READ_SUB:
            serve_sub(&dr->img, &req, rb);
            break;

        case VDISC_OP_READ_RAW:
            serve_raw(&dr->img, &req, rb, 0);       /* any track */
            break;

        case VDISC_OP_READ_CD:
            serve_read_cd(&dr->img, &req, rb);
            break;

        case VDISC_OP_READ_MODE2:
            serve_read_mode2(&dr->img, &req, rb);
            break;

        /*
         * Audio: fire-and-forget. No handle to reply to, and nothing in
         * the kernel is waiting - see the file header.
         */
        case VDISC_OP_PLAY:
            if (!quiet)
                printf("vdiscd: play %lu..%lu (track %lu)\n",
                       (unsigned long) req.lba, (unsigned long) req.arg0,
                       (unsigned long) req.arg1);
            vdiscd_play(&dr->child, &dr->img, (int) req.lba,
                        (int) req.arg0, (int) req.arg1);
            needs_reply = 0;
            break;
        case VDISC_OP_PAUSE:
            vdiscd_play_pause(&dr->child);
            needs_reply = 0;
            break;
        case VDISC_OP_RESUME:
            vdiscd_play_resume(&dr->child);
            needs_reply = 0;
            break;
        case VDISC_OP_VOLCTRL:
            /*
             * arg1 carries the 0-255 level. The kernel already refused
             * unequal channels, so one value is the whole request.
             */
            if (!quiet)
                printf("vdiscd: volume %lu/255\n", (unsigned long) req.arg1);
            vdiscd_play_volume(&dr->child, (int) req.arg1);
            needs_reply = 0;
            break;

        case VDISC_OP_STOP:
            if (!quiet)
                printf("vdiscd: stop\n");
            vdiscd_play_stop(&dr->child);
            needs_reply = 0;
            break;

        /*
         * A CLIENT PRESSED EJECT, AND THE DRIVER ALREADY AGREED.
         *
         * The lock is checked in the DRIVER (vdisc_mod.c's tray_move),
         * because that is what the uniform layer calls and it has the
         * per-drive flag. By the time the request reaches here the
         * decision is made, so this does the same teardown as
         * cmd_detach(): stop the music, tell the kernel, close the
         * image.
         *
         * THE DETACH CAN STILL BE REFUSED - -EBUSY while a filesystem
         * is mounted on the drive, which the lock says nothing about.
         * The image stays open in that case, exactly as cmd_detach()
         * learned to do on 2026-09-19; the alternative is the daemon
         * and the kernel disagreeing about whether a disc is there.
         *
         * NOBODY IS WAITING FOR A REPLY. The request came from
         * tray_move(), which cannot wait - so a refusal is reported
         * in the log and through the drive still having a disc in it,
         * not up the stack.
         */
        case VDISC_OP_EJECT:
            vdiscd_play_stop(&dr->child);
            if (do_detach(ctl_fd, (int) req.minor, 1) != 0) {
                /* NAMED DIFFERENTLY FROM cmd_detach's REFUSAL ON
                 * PURPOSE. Both used to print nearly the same
                 * sentence, and on 2026-09-20 that cost a diagnosis:
                 * two refusals in the log were read as this path when
                 * they were the control channel's, and the trace had
                 * to be cross-checked for `tray OPEN' to tell them
                 * apart. The log should not need a second file to be
                 * unambiguous. */
                if (!quiet)
                    printf("vdiscd: CLIENT eject REFUSED for drive %lu"
                           " - detach failed even with force\n",
                           (unsigned long) req.minor);
            } else {
                vdisc_image_close(&dr->img);
                dr->open = 0;
                state_drive((int) req.minor, NULL, quiet);
                if (!quiet)
                    printf("vdiscd: drive %lu ejected (client eject)\n",
                           (unsigned long) req.minor);
            }
            needs_reply = 0;
            break;

        /*
         * The kernel's stale-report rule fired: the child stopped
         * reporting mid-play. lba is where the drive was left.
         */
        case VDISC_OP_STALL:
            if (!quiet)
                printf("vdiscd: stall at lba %lu - the audio child"
                       " stopped reporting\n", (unsigned long) req.lba);
            vdiscd_play_stalled(&dr->child);
            needs_reply = 0;
            break;

        default:
            rb->hdr.status = -ENOSYS;
            rb->hdr.length = 0;
            break;
        }

        if (!needs_reply)
            continue;

        if (ioctl(ctl_fd, VDISC_IOC_PUT_REPLY, rb) < 0) {
            if (errno == EINTR)
                continue;
            /*
             * A LATE REPLY IS NOT A REASON TO QUIT - design/54 D68,
             * 2026-10-04. ENOENT is the module's one answer for "no
             * request has that handle any more" (vdisc_mod.c, the end
             * of vdisc_do_put_reply): the reader gave up waiting - the
             * raw read's 10 s timeout, D67 - and the request is gone.
             * Breaking out here detached EVERY drive over one stale
             * answer. Say it and carry on; every other failure still
             * stops the daemon as before.
             */
            if (errno == ENOENT) {
                fprintf(stderr, "vdiscd: put_reply: request %lu was given"
                                " up by the kernel before this reply"
                                " (too slow?) - dropped, carrying on\n",
                        (unsigned long) rb->hdr.handle);
                continue;
            }
            fprintf(stderr, "vdiscd: put_reply: %s\n", strerror(errno));
            rc = 1;
            break;
        }
    }

    /*
     * Shutdown. Stop the music first, then detach.
     *
     * vcdd.c:2419-2420 did kill() then a BLOCKING waitpid(), which hangs
     * forever on a child stuck in D state - the exact case the fork
     * exists to contain. vdiscd_child_stop() escalates and then gives
     * up, so this always returns.
     */
    /* EVERY DRIVE'S CHILD - each must be stopped before the parent
     * leaves, or a child outlives the daemon holding a vsound channel.
     * Every drive was initialised above (AR1), so a drive that never
     * played has pid -1 and its stop returns at once. */
    for (i = 0; i < VDISC_MAX_DEVS; i++)
        vdiscd_play_stop(&drives[i].child);

    /* AND EVERY ATTACHED DRIVE IS DETACHED. vdisc_mod.c:2596 detaches
     * them anyway when the control device closes - "Every disc goes
     * away with the daemon that served it" - but doing it explicitly
     * keeps the kernel's view and ours in step while we are still
     * running, and closes each image cleanly. */
    for (i = 0; i < VDISC_MAX_DEVS; i++) {
        if (!drives[i].open)
            continue;
        do_detach(ctl_fd, i, 0);
        vdisc_image_close(&drives[i].img);
        drives[i].open = 0;
    }

    vdiscd_ctl_close(chan_fd);
    vlhe_status_remove_pidfile("vdiscd");
    close(ctl_fd);
    free(rb);

    if (!quiet)
        printf("vdiscd: stopped\n");
    return rc;
}
