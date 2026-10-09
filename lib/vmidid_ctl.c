/*
 * vmidid_ctl.c - the control channel into vmidid.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * READ vmidid_ctl.h FIRST. It has why there are two FIFOs, why they
 * are opened O_RDWR, and why this needs no signal where vdiscd's
 * equivalent does.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

#include "vmidid_ctl.h"

/*
 * OPEN A FIFO OF OURS, AND ONLY A FIFO - design/55 recommendation 2
 * (R2, section 6 row 17), 2026-10-04. The control directory belongs to
 * the `vlhe' account, while root opens these for every drive and MIDI
 * request; a plain open() would follow a symlink left at the name, or
 * write the request into a regular file in its place. O_NOFOLLOW
 * refuses a link at the last component (ELOOP) - the target's headers
 * define it only under _GNU_SOURCE, so the i386 value is given here,
 * the same on the host - and fstat() refuses anything but a FIFO.
 * errno is the open's own on failure, so ENXIO ("no reader") still
 * means what its callers expect; a wrong type is EINVAL.
 */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0400000
#endif

static int
ctl_fifo_open(const char *path, int flags)
{
    struct stat st;
    int fd = open(path, flags | O_NOFOLLOW);

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

/*
 * THE SEQUENCE TAG - design/47 K3, 2026-10-01, the same as vdiscd's
 * (vdiscd_ctl.c has the full account). The caller prefixes a request
 * with `#<n> ', the daemon strips it and echoes it on the reply, and
 * the caller takes only the line with its own n. An untagged request
 * gets an untagged reply; an untagged reply is accepted.
 */
static unsigned long ctl_pending_tag;
static int           ctl_have_tag;
static unsigned long ctl_seq;

static int
tag_strip(char *line, unsigned long *tag)
{
    char *p = line;
    unsigned long v = 0;

    if (*p != '#')
        return 0;
    p++;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (unsigned long) (*p++ - '0');
    if (*p != ' ')
        return 0;
    p++;
    memmove(line, p, strlen(p) + 1);
    *tag = v;
    return 1;
}

/* /var/run/vlhe/ctl SINCE 2026-10-04 - design/55 section 14, as
 * vdiscd_ctl.c. */
#define CTL_DIR_DEFAULT "/var/run/vlhe/ctl"

/*
 * MAKE A DIRECTORY AND, IF NEED BE, ITS PARENT - design/54 D44. Since
 * 2026-10-04 the control directory is /var/run/vlhe/ctl, a level below
 * the run directory, and on a fresh boot or in a portable copy (no
 * account, so nothing has prepared it) neither exists yet. The parent
 * is made 0755 and left as it is made; an installed machine's daemon
 * start gives it to root (account_dirs()). 0 made or already there.
 */
static int
mkdir_up(const char *d, int mode)
{
    char parent[512];
    const char *slash;

    if (mkdir(d, mode) == 0 || errno == EEXIST)
        return 0;
    if (errno != ENOENT)
        return -1;
    slash = strrchr(d, '/');
    if (slash == NULL || slash == d || (size_t) (slash - d) >= sizeof parent)
        return -1;
    memcpy(parent, d, (size_t) (slash - d));
    parent[slash - d] = '\0';
    if (mkdir(parent, 0755) != 0 && errno != EEXIST)
        return -1;
    return (mkdir(d, mode) == 0 || errno == EEXIST) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Paths                                                              */
/* ------------------------------------------------------------------ */

/*
 * OVERRIDABLE FOR TESTING ONLY. Safe to exercise here, unlike the
 * config overrides: nothing in this file opens a device or loads a
 * module - it is two FIFOs and some text.
 */
static const char *
ctl_dir(void)
{
    const char *v = getenv("VMIDID_CTL_DIR");

    return (v != NULL && *v != '\0') ? v : CTL_DIR_DEFAULT;
}

/* STATIC BUFFERS, ONE PER LEAF, so a caller can hold both paths at
 * once - the same arrangement vdiscd_ctl.c uses and for the same
 * reason. */
const char *
vmidid_ctl_fifo(void)
{
    static char buf[512];

    sprintf(buf, "%.480s/vmidid.ctl", ctl_dir());
    return buf;
}

const char *
vmidid_ctl_repfifo(void)
{
    static char buf[512];

    sprintf(buf, "%.480s/vmidid.rep", ctl_dir());
    return buf;
}

/* ------------------------------------------------------------------ */
/* Shared                                                             */
/* ------------------------------------------------------------------ */

/*
 * MAKE A FIFO IF IT IS NOT THERE. An existing one is fine and is the
 * common case: /var/run is cleared at boot, but a daemon restarted
 * within one boot finds its own.
 *
 * MODE 0660 IS A REQUEST, NOT A GUARANTEE - mkfifo is masked by
 * umask, exactly as vdiscd_ctl.c notes. A caller that needs the
 * mode must chmod it.
 */
static int
make_fifo(const char *path)
{
    if (mkfifo(path, 0660) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * READ ONE LINE FROM AN ALREADY-OPEN NON-BLOCKING FIFO.
 *
 * ONE BYTE AT A TIME, which looks wasteful and is the right shape
 * here: a FIFO read takes whatever is in the pipe, so a bulk read
 * would take the NEXT request too and this function has nowhere to
 * put it. The lines are tens of bytes and arrive when a human
 * presses a button.
 */
static int
read_line(int fd, char *out, size_t max)
{
    size_t n = 0;

    if (out == NULL || max == 0)
        return -1;

    for (;;) {
        char c;
        int  rc = (int) read(fd, &c, 1);

        if (rc == 0)
            break;                      /* EOF: no writer (see .h) */
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* NOTHING MORE. A partial line means the writer is
                 * mid-write; drop it rather than acting on half a
                 * request. It will be rewritten or it was never
                 * finished. */
                return 0;
            }
            return -1;
        }
        if (c == '\n') {
            out[n] = '\0';
            return 1;
        }
        if (n + 1 < max)
            out[n++] = c;
        /* A LINE TOO LONG IS TRUNCATED, NOT DROPPED. The verb is at
         * the front, so a truncated request fails to parse and is
         * answered with an error - which says more than silence. */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The daemon's side                                                  */
/* ------------------------------------------------------------------ */

int
vmidid_ctl_open(void)
{
    const char *p;
    int         fd;

    /* THE DIRECTORY MAY NOT EXIST on a machine where nothing has run
     * yet. EEXIST is success. */
    if (mkdir_up(ctl_dir(), 0770) != 0)
        return -1;

    p = vmidid_ctl_fifo();
    if (make_fifo(p) < 0)
        return -1;

    /* O_RDWR - see the header. A reader-only open with no writer
     * makes select() report readable forever. */
    fd = ctl_fifo_open(p, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return -1;

    /* THE REPLY FIFO IS MADE HERE AND OPENED PER REPLY. Holding it
     * open would mean holding a write end of a pipe nobody is
     * reading, and a caller that never collects would then leave
     * stale bytes in front of the next one. */
    if (make_fifo(vmidid_ctl_repfifo()) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int
vmidid_ctl_read(int fd, char *line, size_t max)
{
    int rc;

    if (fd < 0)
        return -1;
    rc = read_line(fd, line, max);
    if (rc == 1)
        ctl_have_tag = tag_strip(line, &ctl_pending_tag);
    return rc;
}

void
vmidid_ctl_reply(int fd, const char *line)
{
    int  rfd;
    char buf[VMIDID_CTL_LINE];
    int  n;

    (void) fd;                          /* replies go to the other FIFO */

    if (line == NULL)
        return;

    /*
     * O_NONBLOCK ON THE WRITE SIDE, AND THAT IS THE WHOLE POINT.
     *
     * A caller may have given up and gone; opening a FIFO for
     * writing with no reader BLOCKS FOREVER without this flag, and
     * the synth would stop dead answering a question nobody asked.
     * With it, the open fails ENXIO and the reply is dropped -
     * which is correct: there is nobody to tell.
     */
    rfd = ctl_fifo_open(vmidid_ctl_repfifo(), O_WRONLY | O_NONBLOCK);
    if (rfd < 0)
        return;

    if (ctl_have_tag) {
        n = sprintf(buf, "#%lu %.*s\n", ctl_pending_tag,
                    VMIDID_CTL_LINE - 16, line);
        ctl_have_tag = 0;
    } else {
        n = sprintf(buf, "%.*s\n", VMIDID_CTL_LINE - 2, line);
    }
    if (write(rfd, buf, (size_t) n) < 0) {
        /* Nothing useful to do: the caller is gone or the pipe is
         * full. Reported nowhere because this is the reply to a
         * request, not the work itself. */
        (void) 0;
    }
    close(rfd);
}

void
vmidid_ctl_close(int fd)
{
    if (fd >= 0)
        close(fd);
    /*
     * THE FIFOs GO WITH THE DAEMON. A channel left behind would
     * accept a request nobody will act on, and a GUI writing to it
     * would wait out its timeout rather than being told at once
     * that the synth is not running.
     */
    unlink(vmidid_ctl_fifo());
    unlink(vmidid_ctl_repfifo());
}

/* ------------------------------------------------------------------ */
/* The caller's side                                                  */
/* ------------------------------------------------------------------ */

/* How long to wait for the daemon to answer. It replies within one
 * pass of its loop, which is bounded by a block period - under
 * 25 ms at any rate we allow. A second is generous and still short
 * enough that a GUI does not appear hung. */
#define REPLY_TIMEOUT_MS 1000
#define REPLY_POLL_MS    10

int
vmidid_ctl_request(const char *req, char *reply, size_t max)
{
    int  wfd, rfd;
    char buf[VMIDID_CTL_LINE];
    int  n, waited;
    struct timeval t0, t1;      /* the reply wait, on the clock - K1 */

    if (req == NULL)
        return -1;

    /*
     * THE REPLY END IS OPENED FIRST, BEFORE THE REQUEST IS SENT.
     *
     * Otherwise the daemon can answer before we are listening, and
     * a FIFO with no reader discards what is written to it - so the
     * reply would be gone and we would time out having done the
     * work. Opening first is the whole reason this is not two
     * independent steps.
     *
     * O_RDWR again, so this open does not block waiting for the
     * daemon to become a writer.
     */
    rfd = ctl_fifo_open(vmidid_ctl_repfifo(), O_RDWR | O_NONBLOCK);
    if (rfd < 0)
        return -1;                      /* no channel: no daemon */

    wfd = ctl_fifo_open(vmidid_ctl_fifo(), O_WRONLY | O_NONBLOCK);
    if (wfd < 0) {
        close(rfd);
        return -1;
    }

    /* STALE LINES FIRST - answers to requests that gave up. */
    {
        char junk[VMIDID_CTL_LINE];

        while (read_line(rfd, junk, sizeof junk) == 1)
            ;
    }
    if (ctl_seq == 0)
        ctl_seq = ((unsigned long) getpid() << 8) & 0x7fffffffUL;
    ctl_seq++;
    n = sprintf(buf, "#%lu %.*s\n", ctl_seq, VMIDID_CTL_LINE - 16, req);
    if (write(wfd, buf, (size_t) n) < 0) {
        close(wfd);
        close(rfd);
        return -1;
    }
    close(wfd);

    if (reply == NULL || max == 0) {
        close(rfd);
        return 0;                       /* sent, answer not wanted */
    }

    /*
     * POLL RATHER THAN select() ON THE REPLY. The fd is O_RDWR, so
     * it is ALWAYS readable as far as select is concerned - our own
     * write end keeps it from ever reporting EOF, which is the
     * property we wanted on the daemon's side and the one that
     * makes select useless here.
     */
    /*
     * COUNTED ON THE CLOCK, NOT BY THE LOOP - design/47 K1, 2026-10-01.
     * 2.2's select() rounds the timeout UP to a jiffy and adds one
     * (fs/select.c:284, ROUND_UP then + jiffies + 1), so at HZ=100 a
     * 10 ms sleep is 20 ms and "waited += 10" counted half of what
     * passed: REPLY_TIMEOUT_MS was about 2000 real. gettimeofday()
     * makes the label true whatever the kernel rounds to. (vdiscd_
     * ctl.c has the same change; it also yields before the first read
     * - sched_yield() is not declared under this tree's -ansi, and
     * the synth answers from its own loop rather than from a wait a
     * signal must break, so the yield matters less here.)
     */
    gettimeofday(&t0, NULL);
    for (waited = 0; waited < REPLY_TIMEOUT_MS; ) {
        int rc = read_line(rfd, reply, max);

        if (rc == 1) {
            unsigned long tag = 0;

            /* ONLY THE LINE THAT IS OURS - K3. A tagged line for
             * another request is a late answer; skipped, and read
             * again at once. */
            if (tag_strip(reply, &tag) && tag != ctl_seq) {
                fprintf(stderr, "vmidid_ctl: a late reply (#%lu) was"
                                " discarded: %.60s\n", tag, reply);
                continue;
            }
            close(rfd);
            return 0;
        }
        if (rc < 0) {
            close(rfd);
            return -1;
        }
        /*
         * select() AS THE SLEEP, NOT usleep().
         *
         * vdiscd_ctl.c uses usleep here and compiles because that
         * tree is built without -ansi -pedantic. This one IS - the
         * synth's Makefile passes both - and under them usleep has
         * no declaration, so 2.95.2 warns. Any warning from 2.95.2
         * is a defect (CLAUDE.md section 3), and select() with no
         * fds is the portable sleep every 1999 program used.
         */
        {
            struct timeval tv;

            tv.tv_sec  = 0;
            tv.tv_usec = REPLY_POLL_MS * 1000L;
            select(0, NULL, NULL, NULL, &tv);
        }
        gettimeofday(&t1, NULL);
        waited = (int) ((t1.tv_sec - t0.tv_sec) * 1000L
                        + (t1.tv_usec - t0.tv_usec) / 1000L);
    }

    close(rfd);
    errno = ETIMEDOUT;
    return -1;
}
