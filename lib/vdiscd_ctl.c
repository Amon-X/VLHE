/*
 * vdiscd_ctl.c - the control channel into vdiscd.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * READ vdiscd_ctl.h FIRST. It has why this is signal-driven rather
 * than a select() loop, and why the FIFO is opened O_RDWR.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sched.h>      /* sched_yield - let the daemon answer first */

#include "vdiscd_ctl.h"

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

/* ------------------------------------------------------------------ */
/* The sequence tag - design/47 K3, 2026-10-01                         */
/* ------------------------------------------------------------------ */

/*
 * A REPLY NAMES THE REQUEST IT ANSWERS. Both FIFOs are shared by
 * every caller and requests queue, so a daemon that was stalled - the
 * case the timeout exists for - wakes up, answers the backlog in
 * order, and each answer went to whoever was listening THEN: a
 * `status' reply read by vlhe_drive_image() became a drive's image
 * path. The caller prefixes each request with `#<n> ', the daemon
 * strips it and puts the same `#<n> ' on its reply, and the caller
 * takes only the line with its own n, discarding the rest with a line
 * on stderr. An untagged request gets an untagged reply (an old
 * caller), and an untagged reply is accepted (an old daemon - which
 * would also answer `err unknown command' to a tagged request, so a
 * daemon from a previous image must be reloaded; the Load does that).
 *
 * THE DAEMON'S SIDE: vdiscd_ctl_read() remembers the tag of the
 * request it just handed out, vdiscd_ctl_reply() uses it once. One
 * request is handed out and answered before the next is read, so one
 * slot is enough. vdiscd.c's thirty-one reply sites are untouched.
 */
static unsigned long ctl_pending_tag;
static int           ctl_have_tag;
static unsigned long ctl_seq;           /* the caller's last tag */

/* `#<n> rest' -> n, and `rest' moved to the front; 0 if untagged. */
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


/* /var/run/vlhe/ctl SINCE 2026-10-04 - design/55 section 14: the
 * daemons' account owns THIS directory and root owns the run directory
 * above it, so the account cannot touch root's lock or state. */
#define CTL_DIR_DEFAULT   "/var/run/vlhe/ctl"

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
 * OVERRIDABLE FOR TESTING ONLY, and unlike the config overrides this
 * one is genuinely safe to exercise on the workstation: nothing here
 * loads a module or opens a device. It is a FIFO and a text file.
 */
static const char *
ctl_dir(void)
{
    const char *v = getenv("VDISCD_CTL_DIR");

    return (v != NULL && *v != '\0') ? v : CTL_DIR_DEFAULT;
}

static const char *
path_in_dir(const char *leaf)
{
    /* STATIC BUFFERS, ONE PER LEAF. Two callers can hold both paths
     * at once - vdiscd_ctl_open() does - so a single shared buffer
     * would hand the second caller the first one's string. */
    static char fifo[512];
    static char pid[512];
    char       *buf = (strcmp(leaf, "vdiscd.ctl") == 0) ? fifo : pid;

    sprintf(buf, "%.480s/%.20s", ctl_dir(), leaf);
    return buf;
}

const char *
vdiscd_ctl_fifo(void)
{
    return path_in_dir("vdiscd.ctl");
}

const char *
vdiscd_ctl_pidfile(void)
{
    return path_in_dir("vdiscd.pid");
}

/* The reply FIFO. One per channel, not per caller - the daemon
 * admits one opener of /dev/vdiscctl and answers one request at a
 * time, so there is never more than one caller waiting. */
static const char *
reply_path(void)
{
    static char buf[512];

    sprintf(buf, "%.480s/vdiscd.rep", ctl_dir());
    return buf;
}

const char *
vdiscd_ctl_cdrom(void)
{
    static char buf[512];

    sprintf(buf, "%.480s/cdrom", ctl_dir());
    return buf;
}

int
vdiscd_ctl_set_cdrom(int drive)
{
    char tmp[520], node[32];
    const char *link = vdiscd_ctl_cdrom();
    struct stat st;

    if (drive < 0 || drive > 99) {
        errno = EINVAL;
        return -1;
    }
    /* THE DIRECTORY MAY NOT EXIST YET - the plan makes the link before
     * any daemon has run (vdiscd_ctl_open() makes it otherwise). */
    if (stat(ctl_dir(), &st) != 0 && mkdir_up(ctl_dir(), 0755) != 0)
        return -1;
    sprintf(node, "/dev/vdisc%d", drive);
    sprintf(tmp, "%.500s.new", link);
    (void) unlink(tmp);                 /* a leftover from a crash */
    if (symlink(node, tmp) != 0)
        return -1;
    if (rename(tmp, link) != 0) {
        int e = errno;

        (void) unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The daemon's side                                                  */
/* ------------------------------------------------------------------ */

static int
make_dir(void)
{
    struct stat st;
    const char *d = ctl_dir();

    if (stat(d, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;

    /* 0755: anyone may see the pid file; the FIFO's own mode decides
     * who may command the daemon. */
    return mkdir_up(d, 0755);
}

static int
make_fifo(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        /* A STALE FIFO IS REUSED, A STALE FILE IS REPLACED. A crash
         * leaves the FIFO behind and it is still a perfectly good
         * FIFO; something else of the same name is not. */
        if (S_ISFIFO(st.st_mode))
            return 0;
        if (unlink(path) < 0)
            return -1;
    }

    /* 0660 AND THE GROUP DECIDES WHO MAY SWAP A DISC - design/33
     * section 3f: "group ownership on the CHANNEL is what decides who
     * may swap a disc, not permissions on a file". The daemon runs as
     * root and records what it did; the channel is the gate. */
    if (mkfifo(path, 0660) < 0 && errno != EEXIST)
        return -1;

    /* mkfifo is masked by umask, so the mode above is a request. Say
     * it again explicitly or a umask of 077 gives a channel only root
     * can use, silently. */
    (void) chmod(path, 0660);
    return 0;
}

static int
write_pidfile(void)
{
    FILE *fp;

    fp = fopen(vdiscd_ctl_pidfile(), "w");
    if (fp == NULL)
        return -1;

    fprintf(fp, "%lu\n", (unsigned long) getpid());
    if (fclose(fp) != 0)
        return -1;

    return 0;
}

int
vdiscd_ctl_open(void)
{
    int fd;

    if (make_dir() < 0)
        return -1;
    if (make_fifo(vdiscd_ctl_fifo()) < 0)
        return -1;
    if (make_fifo(reply_path()) < 0)
        return -1;

    /*
     * O_RDWR, AND THE HEADER SAYS WHY IT MATTERS. A FIFO opened
     * read-only with no writer returns EOF from every read, so a
     * daemon draining it on each signal would see a stream of
     * zero-length reads rather than "nothing there". Holding a write
     * end ourselves means the FIFO is never at EOF and an empty read
     * gives EAGAIN, which is the answer the loop wants.
     *
     * O_NONBLOCK because the drain happens in the EINTR branch of the
     * main loop, where blocking would stall every disc read.
     */
    fd = ctl_fifo_open(vdiscd_ctl_fifo(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (write_pidfile() < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void
vdiscd_ctl_close(int fd)
{
    if (fd >= 0)
        close(fd);

    /* THE PID FILE GOES FIRST. A caller that reads a pid, then finds
     * no FIFO, can report "the daemon is shutting down"; one that
     * finds a FIFO and no pid has nothing to signal. */
    unlink(vdiscd_ctl_pidfile());
    unlink(vdiscd_ctl_fifo());
    unlink(reply_path());
}

int
vdiscd_ctl_read(int fd, char *line, size_t len)
{
    /*
     * ONE LINE AT A TIME, WITH A CARRY - K3's second half. This read
     * a 319-byte gulp and kept the first line; two requests queued
     * by two callers arrived in one read and the second was consumed
     * unanswered, and that caller timed out. The remainder of a read
     * stays here for the next call. One daemon, one fd, one carry.
     */
    static char carry[VDISCD_CTL_LINE * 2];
    static size_t have;
    char  *nl;
    int    n;

    if (fd < 0 || line == NULL || len == 0)
        return -1;

    line[0] = '\0';

    nl = (have > 0) ? memchr(carry, '\n', have) : NULL;
    if (nl == NULL) {
        if (have >= sizeof carry - 1)
            have = 0;           /* a line longer than two buffers: drop it */
        n = read(fd, carry + have, sizeof carry - 1 - have);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n == 0)
            return 0;
        have += (size_t) n;
        nl = memchr(carry, '\n', have);
        if (nl == NULL)
            return 0;           /* a partial line - the rest comes later */
    }

    {
        size_t linelen = (size_t) (nl - carry);
        size_t rest    = have - linelen - 1;

        if (linelen >= len)
            linelen = len - 1;
        memcpy(line, carry, linelen);
        line[linelen] = '\0';
        memmove(carry, nl + 1, rest);
        have = rest;
    }

    ctl_have_tag = tag_strip(line, &ctl_pending_tag);
    return 1;
}

int
vdiscd_ctl_reply(const char *line)
{
    int fd;
    int n;
    char buf[VDISCD_CTL_LINE];

    if (line == NULL)
        return -1;

    /*
     * NON-BLOCKING, AND A MISSING READER IS NOT AN ERROR. If the
     * caller gave up and went away, opening the reply FIFO
     * write-only returns ENXIO - which means "nobody is listening",
     * not "the command failed". The daemon has already done the
     * work.
     */
    fd = ctl_fifo_open(reply_path(), O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return (errno == ENXIO) ? 0 : -1;

    if (ctl_have_tag) {
        sprintf(buf, "#%lu %.290s\n", ctl_pending_tag, line);
        ctl_have_tag = 0;
    } else {
        sprintf(buf, "%.300s\n", line);
    }
    n = write(fd, buf, strlen(buf));
    close(fd);

    return (n < 0) ? -1 : 0;
}

/*
 * THE WAKER - vdiscd_ctl.h has why. The child's whole life is this
 * loop: wait for the FIFO to have something, signal the parent, give
 * it 20 ms to drain, and look again. A one-second select() would do
 * for waiting; 250 ms is so a parent that died is noticed quickly.
 */
pid_t
vdiscd_ctl_watch(int fd)
{
    pid_t parent = getpid();
    pid_t kid;
    int   i;

    if (fd < 0)
        return -1;
    kid = fork();
    if (kid != 0)
        return kid;             /* the parent, or -1 */

    /* NOTHING BUT THE FIFO. Above all not /dev/vdiscctl: held here, it
     * would keep the module believing a daemon is serving after the
     * real one has gone. stdout and stderr stay - DAEMON.LOG. */
    for (i = 3; i < 256; i++)
        if (i != fd)
            (void) close(i);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_IGN);

    for (;;) {
        fd_set         rs;
        struct timeval tv;
        int            r;

        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        tv.tv_sec = 0;
        tv.tv_usec = 250000L;
        r = select(fd + 1, &rs, NULL, NULL, &tv);

        /* THE PARENT IS GONE - reparented to init. Not a pid to signal. */
        if (getppid() != parent)
            _exit(0);
        if (r < 0 && errno != EINTR)
            _exit(1);
        if (r > 0) {
            /* GUARDED, ON A SAVED PID - CLAUDE.md section 1. `parent'
             * was read before the fork and checked against getppid()
             * just above, so this is our own daemon or nothing. */
            if (parent > 1)
                (void) kill(parent, SIGUSR1);
            usleep(20000);
        }
    }
    /* not reached */
}

/* ------------------------------------------------------------------ */
/* The caller's side                                                  */
/* ------------------------------------------------------------------ */

/*
 * IS THIS PID RUNNING vdiscd? - design/55 recommendation 2 (section 6
 * row 18), 2026-10-04. The pid comes from a file in the `vlhe'
 * account's directory, and root then sends it SIGUSR1, whose default
 * action TERMINATES. Without this, whoever can write that file chooses
 * which process root kills. Read from /proc/<pid>/stat, whose name is
 * bracketed and may contain a space - so the LAST ')' - as
 * vlhe_apply.c's pid_is_named() does for the plan runner. Fails closed.
 */
static int
pid_runs(pid_t pid, const char *name)
{
    char  path[64];
    char  buf[256];
    FILE *fp;
    char *op, *cp;
    size_t n;

    if (pid <= 0)
        return 0;
    sprintf(path, "/proc/%ld/stat", (long) pid);
    fp = fopen(path, "r");
    if (fp == NULL)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    if (n == 0)
        return 0;
    buf[n] = '\0';
    op = strchr(buf, '(');
    cp = strrchr(buf, ')');
    if (op == NULL || cp == NULL || cp <= op)
        return 0;
    *cp = '\0';
    return strcmp(op + 1, name) == 0;
}

pid_t
vdiscd_ctl_daemon_pid(void)
{
    FILE *fp;
    long  v = 0;

    fp = fopen(vdiscd_ctl_pidfile(), "r");
    if (fp == NULL)
        return 0;

    if (fscanf(fp, "%ld", &v) != 1)
        v = 0;
    fclose(fp);

    /*
     * GUARDED, AND THIS IS EXACTLY THE PLACE THAT NEEDS IT.
     * CLAUDE.md section 1: a test's kill(c.pid) became
     * kill(-1, SIGKILL) and took the X session down twice. A pid
     * read from a FILE is the likeliest source of a 0 or a negative
     * there is - a truncated write, a crash mid-write, a stale file
     * someone edited.
     */
    if (v <= 0)
        return 0;

    /* Does it exist? Signal 0 checks without sending. */
    if (kill((pid_t) v, 0) < 0 && errno == ESRCH)
        return 0;

    /* AND IS IT vdiscd - see pid_runs(). Not checked when a test has
     * named its own control directory: the host tests' daemon is a
     * fork of the test binary and carries its name. Root never has
     * that variable (vlhe_self_root_env(), design/55 recommendation
     * 3), so this cannot be used to skip the check where it matters. */
    if (getenv("VDISCD_CTL_DIR") == NULL && !pid_runs((pid_t) v, "vdiscd"))
        return 0;

    return (pid_t) v;
}

int
vdiscd_ctl_send(const char *request, char *reply, size_t len,
                int timeout_ms)
{
    pid_t  pid;
    int    wfd, rfd;
    char   buf[VDISCD_CTL_LINE];
    int    n;

    if (request == NULL || reply == NULL || len == 0) {
        errno = EINVAL;
        return -1;
    }
    reply[0] = '\0';

    pid = vdiscd_ctl_daemon_pid();
    if (pid <= 0) {
        errno = ESRCH;
        return -1;
    }

    /*
     * OPEN THE REPLY FIFO BEFORE SENDING. If we opened it after
     * signalling, a fast daemon could answer and close before we
     * were listening, and the reply would be lost into a FIFO with
     * no reader. O_RDWR so this open does not block waiting for a
     * writer.
     */
    rfd = ctl_fifo_open(reply_path(), O_RDWR | O_NONBLOCK);
    if (rfd < 0)
        return -1;

    wfd = ctl_fifo_open(vdiscd_ctl_fifo(), O_WRONLY | O_NONBLOCK);
    if (wfd < 0) {
        close(rfd);
        return -1;
    }

    /*
     * STALE LINES GO FIRST. Anything in the reply FIFO now answers a
     * request that gave up; with the tag below it would be skipped
     * anyway, but reading it out here costs nothing and keeps the
     * FIFO from filling with answers nobody collected.
     */
    {
        char junk[VDISCD_CTL_LINE];

        while (read(rfd, junk, sizeof junk) > 0)
            ;
    }

    /* THE TAG - unique to this process (seeded from the pid) and to
     * this request; see the section above. */
    if (ctl_seq == 0)
        ctl_seq = ((unsigned long) getpid() << 8) & 0x7fffffffUL;
    ctl_seq++;
    sprintf(buf, "#%lu %.290s\n", ctl_seq, request);
    if (write(wfd, buf, strlen(buf)) < 0) {
        close(wfd);
        close(rfd);
        return -1;
    }
    close(wfd);

    /* WAKE IT. The daemon is asleep in GET_REQ; this brings it out at
     * once when we may signal it. Guarded above: pid > 0. EPERM - a
     * user's GUI and a daemon running as `vlhe' - is not a failure:
     * the daemon's waker saw the line arrive and signals for us. */
    if (kill(pid, SIGUSR1) < 0 && errno != EPERM) {
        close(rfd);
        return -1;
    }

    /*
     * POLL FOR THE REPLY. A 10 ms step rather than select(), because
     * this is a GUI thread waiting on a local daemon that answers in
     * microseconds - the loop almost always runs once.
     *
     * LET THE DAEMON RUN BEFORE THE FIRST READ - design/47 K2. On the
     * uniprocessor target the signal above cannot have been delivered
     * yet when this process reads; the first read found nothing and
     * the loop slept. sched_yield() (2.2 has it) hands the CPU over,
     * and the daemon's reply is usually there on the first read.
     *
     * AND THE TIMEOUT IS COUNTED ON THE CLOCK, NOT BY THE LOOP -
     * design/47 K1. 2.2's sys_nanosleep() sleeps timespec_to_jiffies()
     * + 1, and that conversion rounds UP (include/linux/time.h:37), so
     * at HZ=100 ANY usleep up to 10 ms is 2 jiffies = 20 ms: "waited
     * += 10" counted half of what passed and 400/2000/5000 ms were
     * about 800/4000/10000. Halving the sleep would change nothing
     * (5 ms rounds to the same 2 jiffies); gettimeofday() makes the
     * label true whatever the kernel rounds to.
     */
    {
        struct timeval t0, t1;
        long elapsed_ms = 0;

        char   acc[VDISCD_CTL_LINE * 2];
        size_t have = 0;

        sched_yield();
        gettimeofday(&t0, NULL);
        for (;;) {
            char *nl;

            /* LINE BY LINE, AND ONLY THE LINE THAT IS OURS. Two replies
             * can arrive in one read; each is judged by its tag. */
            n = read(rfd, acc + have, sizeof acc - 1 - have);
            if (n > 0)
                have += (size_t) n;
            while ((nl = memchr(acc, '\n', have)) != NULL) {
                size_t linelen = (size_t) (nl - acc);
                unsigned long tag = 0;
                int tagged;

                if (linelen >= sizeof buf)
                    linelen = sizeof buf - 1;
                memcpy(buf, acc, linelen);
                buf[linelen] = '\0';
                memmove(acc, nl + 1, have - linelen - 1);
                have -= linelen + 1;

                tagged = tag_strip(buf, &tag);
                if (tagged && tag != ctl_seq) {
                    fprintf(stderr, "vdiscd_ctl: a late reply (#%lu) was"
                                    " discarded: %.60s\n", tag, buf);
                    continue;
                }
                strncpy(reply, buf, len - 1);
                reply[len - 1] = '\0';
                close(rfd);
                return 0;
            }
            if (have >= sizeof acc - 1)
                have = 0;       /* no newline in two buffers: drop it */
            if (elapsed_ms >= timeout_ms)
                break;
            usleep(10000);
            gettimeofday(&t1, NULL);
            elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L
                       + (t1.tv_usec - t0.tv_usec) / 1000L;
        }
    }

    close(rfd);
    /* THE WEDGED CASE - design/33 section 3g. The caller offers a
     * restart rather than reporting a generic failure. */
    errno = ETIMEDOUT;
    return -1;
}

/* ------------------------------------------------------------------ */
/* The protocol                                                       */
/* ------------------------------------------------------------------ */

int
vdiscd_ctl_parse(char *line, char **verb, char **arg1, char **arg2)
{
    char *p = line;
    int   n = 0;

    if (line == NULL || verb == NULL || arg1 == NULL || arg2 == NULL)
        return -1;

    *verb = *arg1 = *arg2 = NULL;

    while (*p != '\0' && n < 3) {
        char **slot;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        slot = (n == 0) ? verb : (n == 1) ? arg1 : arg2;
        *slot = p;
        n++;

        /*
         * THE LAST FIELD RUNS TO THE END OF THE LINE, because it is
         * a PATH and a path may contain spaces. Splitting it on
         * whitespace would break /mnt/My Discs/foo.ccd, which is an
         * ordinary name on a machine with a Windows partition
         * mounted.
         */
        if (n == 3)
            break;

        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }

    return n;
}
