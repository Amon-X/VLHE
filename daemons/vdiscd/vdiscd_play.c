/*
 * vdiscd_play.c - the disc daemon's audio commands.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * The glue between the main loop's opcodes and the child that actually
 * plays. Separate from vdiscd_child.c, which knows only how to start,
 * feed and kill a child, and from vdiscd_audio.c, which is the child.
 *
 * WHY A LAYER AT ALL: the child is started LAZILY, on the first play,
 * and reaped on stop. An idle disc holds no child and no vsound channel
 * (section 3.1a), so a disc sitting in a drive costs nothing and does
 * not consume one of four channels.
 *
 * C89 / GCC 2.95 clean.
 */

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "vdisc.h"
#include "vsound.h"
#include "image.h"
#include "vdiscd_child.h"
#include "vdiscd_play.h"

/* The command layout and the opcodes are in vdiscd_play.h, shared
 * with the child - design/26 B11. */
static int send_cmd(struct vdiscd_child *c, int op, unsigned int lba,
                    unsigned int end, int arg);

/*
 * ONE PLAYER PER DRIVE - design/52 AR1, 2026-10-03. The image, the
 * control fd, the drive and the remembered level used to be four
 * file-scope statics here (`play_img', `play_ctl_fd', `play_minor',
 * `play_vol'), so the daemon had ONE player, belonging to the drive
 * the command line named; a play on any other drive was refused on
 * the kernel path and misdirected on the control channel's. They are
 * fields of the `struct vdiscd_child' every caller already passes,
 * and THAT STRUCT IS THE CHILD'S ctx: after the fork the child has its
 * own copy, so its callbacks read its own drive's image and report as
 * its own minor. Each child opens its own vsound channel, so two
 * drives playing at once are mixed - the user: "I would like 2 drives
 * play at once each getting their own vsound channel".
 *
 * WHY THE CHILD REPORTS ITSELF rather than relaying through the
 * parent, over a second pipe: section 3.1b - the parent must never
 * have to read from a child that may be wedged. A child that writes
 * its own position (it inherits the control fd) cannot block the
 * block server by failing to.
 *
 * THE REMEMBERED LEVEL (`vol', design/26 B5): KsCD sends VOLCTRL
 * immediately before PLAYMSF, every play, and between plays there is
 * no child - so it is kept and sent the moment one starts.
 */

/*
 * The read callback handed to the child.
 *
 * Runs in the CHILD after the fork, so it has its own copy of the FILE*
 * and its own file position. That is exactly why the child is a fork
 * rather than a thread: a wedged read here blocks only this process.
 */
static int
play_read_frames(void *ctx, unsigned int lba, unsigned int nframes,
                 void *buf)
{
    struct vdiscd_child *c = (struct vdiscd_child *) ctx;
    struct vdisc_image  *img = c ? c->img : NULL;

    if (!img || !img->backend || !img->backend->read_audio)
        return -1;
    return img->backend->read_audio(img, (int) lba, (int) nframes, buf);
}

/*
 * Push the position down to the kernel. Runs in the CHILD.
 *
 * Failures are ignored deliberately. This is a status update on a path
 * that must not block or abort playback: a detached drive, a closed
 * control device or a rejected value all mean the kernel keeps its last
 * known position, which is a stale reading rather than a broken one.
 * There is nothing useful for the child to do about it, and stopping
 * the music because a position report failed would be worse than the
 * stale value.
 */
static void
play_report(void *ctx, unsigned int lba, int status, int track)
{
    struct vdiscd_child *c = (struct vdiscd_child *) ctx;
    struct vdisc_pos pos;

    if (c == NULL || c->ctl_fd < 0)
        return;

    memset(&pos, 0, sizeof pos);
    pos.minor  = (__u32) c->minor;
    pos.lba    = (__u32) lba;
    pos.status = (__u32) status;
    pos.track  = (__u32) track;

    (void) ioctl(c->ctl_fd, VDISC_IOC_PUT_POS, &pos);
}

/*
 * Which track an LBA falls in. Runs in the CHILD, from its own copy of
 * the image after the fork.
 *
 * The kernel has the same lookup (vdisc_track_at) because audio_ioctl
 * must never round-trip to userspace; this one exists so the child can
 * tell the kernel where it has REACHED, which is a different question.
 */
static int
play_track_at(void *ctx, unsigned int lba)
{
    struct vdiscd_child *c = (struct vdiscd_child *) ctx;
    struct vdisc_image  *img = c ? c->img : NULL;
    const struct vdisc_track *t;

    if (!img)
        return 0;
    t = vdisc_image_track_at(img, (int) lba);
    return t ? t->num : 0;
}

/*
 * THE CHILD REPORTS THROUGH ITS OWN OPEN, NOT THE DAEMON'S - design/26
 * B13, design/54 D36, 2026-10-04.
 *
 * The inherited control descriptor IS the daemon's open file, and a
 * child holding it after the daemon was killed kept the drives
 * attached and the module pinned (see vdisc_mod.c's VDISC_CTL_REPORTER).
 * So the child opens the node write-only - the module's reporter, which
 * may only PUT_POS - and drops the inherited one. FD_CLOEXEC could not
 * do this: the child never execs.
 *
 * IF THE OPEN FAILS the inherited descriptor is KEPT: a module from
 * before the reporter answers a second open with EBUSY, and losing
 * every position report would be worse than the old pinning. Only the
 * child's own copy is closed; the daemon's descriptor is untouched.
 *
 * The node is a variable so the host test can point it at a file.
 */
static const char *play_ctl_node = "/dev/vdiscctl";

static void
play_child_reopen(struct vdiscd_child *c)
{
    int fd;

    if (c == NULL || c->ctl_fd < 0)
        return;
    fd = open(play_ctl_node, O_WRONLY);
    if (fd < 0)
        return;                 /* an older module: keep the old way */
    close(c->ctl_fd);
    c->ctl_fd = fd;
}

static int
play_body(int cmd_fd, void *ctx)
{
    play_child_reopen((struct vdiscd_child *) ctx);
    return vdiscd_audio_child(cmd_fd, play_read_frames, play_report,
                              play_track_at, ctx);
}

void
vdiscd_play_init(struct vdiscd_child *c, int ctl_fd, int minor)
{
    vdiscd_child_init(c);
    c->img    = NULL;
    c->ctl_fd = ctl_fd;
    c->minor  = minor;
    c->vol    = -1;
}

/*
 * A WEDGED CHILD IS LEFT ALONE - design/26 B4, fixed 2026-09-14.
 *
 * vdiscd_child_cmd() refuses at once while `wedged' is set, and every
 * command path here answered that refusal with vdiscd_child_stop() -
 * which, because the pid is deliberately kept, sent signals a D-state
 * process cannot receive and waited the full 4.5 s escalation AGAIN,
 * in the block server's loop, on every later command. STOP did it
 * twice: 9 s. The 2026-08-27 collide log shows the give-up block six
 * times in a row for one pid, 27 s of the drive's filesystem frozen.
 *
 * The give-up already happened once; repeating it learns nothing. So
 * a command against a wedged child does the one cheap thing that can
 * change the answer - a non-blocking recheck, in case it has exited
 * since - and returns. If the recheck clears the wedge there is no
 * child either, and the command has nothing to go to.
 *
 * Returns 1 if the caller should give up now.
 */
static int
wedged_skip(struct vdiscd_child *c)
{
    if (!c->wedged)
        return 0;
    vdiscd_child_recheck(c);
    return 1;
}

/*
 * Start playing lba..end.
 *
 * Starts the child if there is not one. A child that has WEDGED is not
 * replaced.
 *
 * WHAT A WEDGED CHILD ACTUALLY HOLDS - and it is not the card. It
 * opened /dev/dsp, which is vsound, so what it has is ONE OF FOUR VCHAN
 * SLOTS and whatever is sitting in that channel's buffer. The hardware
 * belongs to vsoundd; other clients still get channels and keep
 * playing. Under the old tree this was a genuine lockout, because
 * /dev/dsp was the card and OSS makes it exclusive - the mixer is
 * exactly what removed that.
 *
 * So forking a replacement would not contend for anything. It would
 * burn a SECOND slot of four, and both channels would be summed by the
 * mixer - so any audio still buffered in the dead one plays underneath
 * the new one. One stuck process becomes two, and half the table is
 * gone.
 *
 * Note the child wedges on a read from the IMAGE FILE, which is after
 * it has taken its channel - so the slot is consumed while producing
 * nothing.
 *
 * The disc goes quiet and block reads carry on, which is the trade
 * section 3.1b chose.
 */
int
vdiscd_play(struct vdiscd_child *c, struct vdisc_image *img,
            int lba, int end, int track)
{
    unsigned int u_lba, u_end;
    char cmd = CMD_PLAY;

    /*
     * A wedged child may have exited since. Check before refusing -
     * without this the flag is permanent, and one false positive killed
     * CD audio for a whole session on 2026-08-25.
     */
    vdiscd_child_recheck(c);

    /*
     * A REFUSED PLAY IS REPORTED, NOT JUST LOGGED - 86Box run 88,
     * 2026-09-15. The play ioctl is fire-and-forget: by the time this
     * runs the kernel has already set PLAY, and nothing would ever
     * report against it - a wedged child does not, and a child that
     * exits makes no report either - so the drive said "playing"
     * with a frozen position until the 10 s stale rule noticed, and
     * once for ten seconds AFTER the old child had already gone.
     * A drive refuses a play at the ioctl; we cannot, but this
     * process knows it refused and holds the control fd. ERROR now:
     * the kernel is at PLAY so the report is taken, the player sees
     * one ERROR on its next poll and a stopped drive after, and the
     * stale rule has nothing left to fire on.
     */
    if (c->wedged) {
        fprintf(stderr, "vdiscd: not starting audio - a previous child "
                        "is wedged and still holds a mixer channel\n");
        play_report(c, (unsigned int) lba, VDISC_AUDIO_ERROR, track);
        return -1;
    }

    if (lba < 0 || end <= lba)
        return -1;

    /* BEFORE THE START, so the fork copies it into the child - which
     * reads through `c', its ctx (design/52 AR1). A disc swapped while
     * a child plays is stopped first (cmd_attach), so a running child
     * never has its image change under it. */
    c->img = img;

    if (c->pid < 0) {
        if (vdiscd_child_start(c, play_body, c) < 0) {
            fprintf(stderr, "vdiscd: cannot start the audio child\n");
            play_report(c, (unsigned int) lba, VDISC_AUDIO_ERROR, track);
            return -1;
        }
        /* A level that arrived before there was a child to hold it.
         * Sent before the play, so the channel it opens has it. */
        if (c->vol >= 0) {
            if (send_cmd(c, CMD_VOLUME, 0, 0, c->vol) < 0) {
                fprintf(stderr, "vdiscd: audio child stopped listening\n");
                return -1;
            }
        }
    }

    u_lba = (unsigned int) lba;
    u_end = (unsigned int) end;

    /* One write of one struct - the four-write version could tear
     * (design/26 B11, the header's comment). */
    if (send_cmd(c, cmd, u_lba, u_end, track) < 0) {
        fprintf(stderr, "vdiscd: audio child stopped listening\n");
        vdiscd_child_stop(c);
        return -1;
    }
    return 0;
}

/*
 * One struct, one write. vdiscd_child_cmd() fails for a child that is
 * gone (EPIPE - stop it, which collects it at once) or one that is not
 * reading (EAGAIN, design/26 B12 - it has already been abandoned by
 * then, and vdiscd_child_stop() returns at once for a wedged child).
 */
static int
send_cmd(struct vdiscd_child *c, int op, unsigned int lba,
         unsigned int end, int arg)
{
    struct vdiscd_cmd m;

    memset(&m, 0, sizeof m);
    m.op  = (unsigned char) op;
    m.lba = lba;
    m.end = end;
    m.arg = arg;
    if (vdiscd_child_cmd(c, &m, (unsigned int) sizeof m) < 0) {
        vdiscd_child_stop(c);
        return -1;
    }
    return 0;
}

static int
send_simple(struct vdiscd_child *c, char cmd)
{
    if (wedged_skip(c))
        return -1;
    if (c->pid < 0)
        return 0;               /* nothing playing; not an error */
    return send_cmd(c, cmd, 0, 0, 0);
}

/*
 * Set the CD channel's volume.
 *
 * THE SCALE: the CD-ROM API is 0-255, vsound's is 0-VSOUND_VOL_MAX
 * (100) with values above that boosting. 255 maps to unity, so a CD
 * player at full volume is exactly "normal" and the boost range stays
 * the volume tool's alone - a CD player should not be able to make the
 * disc louder than everything else on the machine.
 *
 * FINDING OUR OWN CHANNEL: VSOUND_IOC_VOL addresses a channel by INDEX,
 * and the child does not know which slot it was given. VSOUND_IOC_CHANS
 * lists them with the owning pid, so it matches on its own.
 *
 * Runs in the CHILD, which is the process that holds the channel.
 */
int
vdiscd_play_volume(struct vdiscd_child *c, int vol255)
{
    char cmd = CMD_VOLUME;
    int v;

    if (vol255 < 0)
        vol255 = 0;
    if (vol255 > 255)
        vol255 = 255;

    v = (vol255 * VSOUND_VOL_MAX) / 255;

    /* REMEMBERED WHETHER OR NOT ANYTHING IS PLAYING - design/26 B5.
     * "Nothing playing; remember nothing" was the drop KsCD hit on
     * every play. The child that starts next gets this level. */
    c->vol = v;

    if (wedged_skip(c))
        return -1;
    if (c->pid < 0)
        return 0;               /* no child yet; sent when one starts */

    return send_cmd(c, cmd, 0, 0, v);
}

int
vdiscd_play_pause(struct vdiscd_child *c)
{
    return send_simple(c, CMD_PAUSE);
}

int
vdiscd_play_resume(struct vdiscd_child *c)
{
    return send_simple(c, CMD_RESUME);
}

/*
 * Stop, and let the child go.
 *
 * The child closes /dev/dsp on CMD_STOP (section 3.1a) and exits when
 * the pipe closes, so the channel is released either way. The kill path
 * in vdiscd_child_stop() is the backstop for one that does not.
 */
int
vdiscd_play_stop(struct vdiscd_child *c)
{
    if (wedged_skip(c))
        return 0;               /* B4: not a second 4.5 s, twice */
    if (c->pid < 0)
        return 0;
    send_simple(c, CMD_STOP);
    vdiscd_child_stop(c);
    return 0;
}

/*
 * The kernel says the child's reports stopped - VDISC_OP_STALL, the
 * stale-report rule in vdisc_mod.c. Acer run 85, 2026-09-15: the
 * child wedged in D state on a pulled CF card and this process had no
 * way to know until it was told to shut down; the drive said PLAY
 * with a frozen position for two minutes, and every command in
 * between went into a pipe nobody would read.
 *
 * The kernel has already shown the player one ERROR and stopped the
 * drive. Here: if the child has in fact exited, collect it - that is
 * the ordinary end of a play and needs no message. Otherwise abandon
 * it: close its pipe, mark it wedged, signal nothing
 * (vdiscd_child_abandon). From then on wedged_skip() answers every
 * command and shutdown detaches at once.
 */
int
vdiscd_play_stalled(struct vdiscd_child *c)
{
    if (c->wedged) {
        vdiscd_child_recheck(c);
        return 0;
    }
    if (c->pid < 0)
        return 0;               /* nothing playing; a late message */
    if (vdiscd_child_poll(c))
        return 0;               /* exited on its own; not a wedge */
    vdiscd_child_abandon(c);
    return 0;
}
