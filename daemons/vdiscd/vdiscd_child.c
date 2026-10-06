/*
 * vdiscd_child.c - starting, commanding and giving up on the CDDA child.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/07-vsound.md section 3.1b. The child exists for CONTAINMENT:
 * the image is read with blocking, uninterruptible I/O, and if that
 * hangs on failing media the process is in D state. Forked, the parent
 * keeps serving block reads.
 *
 * THIS FILE IS THE HALF THAT MAKES CONTAINMENT REAL. A fork that the
 * parent then blocks on has isolated nothing.
 *
 * The old tree got this wrong and it is worth naming, because the code
 * looks careful:
 *
 *     kill(audio_pid, SIGTERM);
 *     waitpid(audio_pid, NULL, 0);        vcdd.c:2419-2420
 *
 * A D-state child does not receive SIGTERM - the signal is deferred
 * until the syscall returns, and it never does. So the parent blocks in
 * waitpid forever, and the block server it was protecting stops. The
 * fork bought nothing in exactly the case it was there for.
 *
 * C89: GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "vdiscd_child.h"

/*
 * How long to spend trying to collect a child before giving up on it.
 *
 * Deliberately short. This is not "how long might it take to exit" - a
 * healthy child exits at once on EOF. It is "how long are we prepared
 * to make the guest's filesystem wait", and the answer is: barely at
 * all.
 */
/*
 * HOW LONG TO WAIT, and why 500 ms was not enough.
 *
 * The child spends most of its life blocked in write() to /dev/dsp: it
 * writes CHUNK_FRAMES (8) raw frames - 18816 bytes - into a channel
 * whose buffer is 4096, so the write sleeps until the mixer has drained
 * it. At 44100 Hz stereo 16-bit that is over 100 ms per chunk, and more
 * when other clients are competing for the mix.
 *
 * A write blocked in the sound driver does not return on SIGTERM, so a
 * perfectly healthy child looks unresponsive for the length of one
 * write. On 2026-08-25 a child was declared wedged, permanently, while
 * doing exactly that - the trace shows it releasing its channel
 * normally 359 lines later.
 *
 * 3 seconds is many times one buffer drain, and still short enough that
 * a genuinely stuck child does not delay a shutdown noticeably. The
 * child also closes the dsp on SIGTERM now (vdiscd_audio.c), which
 * unblocks the write, so this is the belt to that braces.
 */
#define REAP_TRIES      120             /* x 25 ms = 3 s total */
#define REAP_SLEEP_US   25000

/*
 * Put a handle into the "no child" state.
 *
 * Needed because callers test c->pid < 0 to decide whether to start
 * one, and a handle that has never been started would otherwise hold
 * whatever was on the stack. vdiscd_child_start() resets the same
 * four fields, so this is about the state BEFORE the first start -
 * and it clears the player's fields too, which start leaves alone.
 */
void
vdiscd_child_init(struct vdiscd_child *c)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    c->pid    = -1;
    c->cmd_fd = -1;
}

int
vdiscd_child_start(struct vdiscd_child *c,
                   int (*body)(int cmd_fd, void *ctx), void *ctx)
{
    int fds[2];

    if (c == NULL)
        return -1;

    /* OUR FOUR FIELDS ONLY - the player's (img, ctl_fd, minor, vol)
     * are set before a start and must survive it (design/52 AR1). */
    c->pid    = -1;
    c->cmd_fd = -1;
    c->reaped = 0;
    c->wedged = 0;

    if (pipe(fds) != 0) {
        fprintf(stderr, "vdiscd: pipe: %s\n", strerror(errno));
        return -1;
    }

    c->pid = fork();
    if (c->pid < 0) {
        fprintf(stderr, "vdiscd: fork: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (c->pid == 0) {
        close(fds[1]);                  /* child reads */
        _exit(body(fds[0], ctx));
    }

    close(fds[0]);                      /* parent writes */
    c->cmd_fd = fds[1];
    /*
     * NON-BLOCKING ON THIS END TOO - design/26 B12. Only the child's
     * end was, so once a stuck child had let the pipe fill (4096
     * bytes on 2.2: ~256 commands, a Quake fade is ~90) the block
     * server sat in write() - the exact failure the fork exists to
     * prevent, reached before any of the containment ran. EAGAIN is
     * handled in vdiscd_child_cmd().
     */
    fcntl(c->cmd_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

/*
 * Send a command. Failure here means the child is gone or its pipe is
 * full, and neither is worth stopping the block path for.
 */
int
vdiscd_child_cmd(struct vdiscd_child *c, const void *buf, unsigned int len)
{
    int n;

    if (c == NULL || c->cmd_fd < 0 || c->wedged)
        return -1;

    n = (int) write(c->cmd_fd, buf, (size_t) len);
    if (n == (int) len)
        return 0;

    /*
     * A command is one struct under PIPE_BUF, so a non-blocking write
     * either takes all of it or none (EAGAIN): there is no partial
     * command to worry about. EAGAIN means the pipe is full - the
     * child has not read a command in ~256 of them, which is a child
     * that is not coming back to the pipe any time soon. Abandon it
     * the way the stall rule does (design/26 B4): close the pipe,
     * mark it wedged, signal nothing. EPIPE means it died.
     */
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        fprintf(stderr, "vdiscd: the audio child is not reading its"
                        " commands - the pipe is full\n");
        vdiscd_child_abandon(c);
        return -1;
    }
    if (errno == EPIPE)
        fprintf(stderr, "vdiscd: the audio child has gone\n");
    return -1;
}

/*
 * Has it exited on its own? Called from the poll loop.
 *
 * WNOHANG, ALWAYS. This must never block: it runs in the same loop that
 * serves block reads, and a wedged child would otherwise stop the guest
 * dead - which is the failure the fork exists to prevent.
 */
static void give_up(struct vdiscd_child *c, const char *how);

int
vdiscd_child_poll(struct vdiscd_child *c)
{
    int st;

    if (c == NULL || c->pid <= 0)
        return 0;

    if (waitpid(c->pid, &st, WNOHANG) == c->pid) {
        c->pid    = -1;
        c->reaped = 1;
        return 1;
    }
    return 0;
}

/*
 * Stop it, and GIVE UP IF IT WILL NOT STOP.
 *
 * The sequence, and every step has a reason:
 *
 *   1. close the pipe - a healthy child sees EOF and exits by itself,
 *      which is the normal path and needs no signal at all
 *   2. SIGTERM, in case it is busy rather than waiting on the pipe
 *   3. poll for up to REAP_TRIES x REAP_SLEEP_US
 *   4. SIGKILL, one attempt
 *   5. poll again, briefly
 *   6. GIVE UP. Mark it wedged, say so, and RETURN.
 *
 * Step 6 is the point of the whole file. A child in D state receives
 * neither SIGTERM nor SIGKILL until its syscall returns, and it may
 * never return. Waiting for it would hand the guest's filesystem to a
 * failing disk - so we leave a zombie-to-be behind and carry on.
 *
 * The cost of giving up is one unreaped process and, until the kernel
 * frees it, the fd it holds. The cost of NOT giving up is the block
 * server stopping. That is not a close call.
 */
void
vdiscd_child_stop(struct vdiscd_child *c)
{
    int i;

    if (c == NULL || c->pid <= 0)
        return;

    /*
     * ALREADY GIVEN UP ON: nothing to do. The escalation below would
     * spend 4.5 s signalling a process that has been abandoned for
     * not answering - which is what design/26 B4 measured at 27 s.
     */
    if (c->wedged)
        return;

    if (c->cmd_fd >= 0) {
        close(c->cmd_fd);               /* EOF: the polite exit */
        c->cmd_fd = -1;
    }

    kill(c->pid, SIGTERM);

    for (i = 0; i < REAP_TRIES; i++) {
        if (vdiscd_child_poll(c))
            return;
        usleep(REAP_SLEEP_US);
    }

    kill(c->pid, SIGKILL);

    for (i = 0; i < REAP_TRIES / 2; i++) {
        if (vdiscd_child_poll(c))
            return;
        usleep(REAP_SLEEP_US);
    }

    /*
     * IT IS NOT COMING BACK. Almost certainly stuck in an
     * uninterruptible read on the image - see section 3.1b.
     */
    give_up(c, "is not responding to signals");
}

/*
 * Say it plainly rather than logging a code: whoever reads this needs
 * to know the disk is the suspect, not the audio. Shared by the two
 * ways of learning it - signals it ignored, or reports that stopped.
 */
static void
give_up(struct vdiscd_child *c, const char *how)
{
    fprintf(stderr,
        "vdiscd: the audio child (pid %d) %s.\n"
        "vdiscd: it may be blocked writing audio, or stuck reading the\n"
        "vdiscd: image - a failing disk or a mount that has gone away.\n"
        "vdiscd: Block reads CONTINUE; only CD audio is affected, and it\n"
        "vdiscd: recovers on its own if the child does eventually exit.\n",
        (int) c->pid, how);

    c->wedged = 1;
    /* pid is deliberately NOT cleared: it is still ours, and a later
     * poll may still collect it if the read ever completes. */
}

/*
 * Give up on it WITHOUT SIGNALLING - the kernel said its reports
 * stopped (VDISC_OP_STALL; Acer run 85, 2026-09-15).
 *
 * No SIGTERM, no SIGKILL, no 4.5 s: a child that has stopped reporting
 * mid-play is either in D state, where no signal reaches it, or merely
 * starved, where a signal would kill a process about to recover. The
 * pipe is closed instead. A live child sees EOF and exits by itself
 * (its normal exit); a stuck one sits until its read fails, and either
 * way vdiscd_child_recheck() collects it later and clears the flag.
 *
 * With `wedged' set from here, every command path skips it at once
 * (vdiscd_play.c, wedged_skip) and shutdown detaches without the
 * escalation - which is what let unload.sh's one-second patience force
 * a kill in run 84 and leave the drive attached.
 */
void
vdiscd_child_abandon(struct vdiscd_child *c)
{
    if (c == NULL || c->pid <= 0 || c->wedged)
        return;

    if (c->cmd_fd >= 0) {
        close(c->cmd_fd);
        c->cmd_fd = -1;
    }
    give_up(c, "has stopped reporting its position");
}

/*
 * Has a previously-wedged child finally exited?
 *
 * WITHOUT THIS, `wedged' IS PERMANENT. vdiscd_play() refuses to start
 * audio while it is set, so one false positive killed CD audio for the
 * rest of the session - which is exactly what happened on 2026-08-25:
 * "not starting audio - a previous child is wedged" repeated for every
 * play attempt afterwards, including from the CD player.
 *
 * The child that provoked it was not stuck at all; it exited normally
 * once its write completed. Nothing was watching, so nothing noticed.
 *
 * Called before each play attempt. Cheap: one non-blocking waitpid.
 */
int
vdiscd_child_recheck(struct vdiscd_child *c)
{
    if (c == NULL || !c->wedged)
        return 0;

    /*
     * NO CHILD LEFT TO WAIT FOR MEANS THE WEDGE IS OVER.
     *
     * vdiscd_child_poll() returns 0 when pid <= 0, which is the case
     * once something has already collected it - so polling alone would
     * leave the flag set forever on a handle whose child is demonstrably
     * gone. Caught by the host test rather than by reading: the first
     * version of this function failed exactly here.
     */
    if (c->pid <= 0) {
        c->wedged = 0;
        return 1;
    }

    if (vdiscd_child_poll(c)) {
        fprintf(stderr, "vdiscd: the audio child exited after all; "
                        "CD audio is available again\n");
        c->wedged = 0;
        return 1;
    }
    return 0;
}
