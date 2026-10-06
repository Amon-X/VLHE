/*
 * vdiscd_ctl.h - the control channel into vdiscd.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHY THIS EXISTS: the GUI cannot attach a disc itself. design/33
 * section 3f - VDISC_IOC_ATTACH wants total_sectors, data_sectors
 * and the whole TOC, which come from opening and parsing the image,
 * and that is ~1500 lines already living in vdiscd. And
 * /dev/vdiscctl admits ONE opener (vdisc_mod.c:2580), so the GUI
 * cannot take it without displacing the daemon that needs it.
 *
 * So the GUI asks and the daemon does the work it already knows.
 *
 * SIGNAL-DRIVEN, AND THE DAEMON'S MAIN LOOP FORCES THAT.
 * vdiscd.c blocks in ioctl(VDISC_IOC_GET_REQ) - there is no select()
 * anywhere in it, and the module sleeps in interruptible_sleep_on().
 * A FIFO alone would therefore be read only when the next disc
 * request happened to arrive, which on an idle machine is never.
 *
 * The sequence:
 *
 *   1. the caller writes one request line to the FIFO
 *   2. it reads the daemon's pid and sends SIGUSR1
 *   3. GET_REQ returns -EINTR; the loop's existing EINTR branch
 *      drains the FIFO, acts, and writes one reply line
 *   4. the caller reads the reply
 *
 * vmidid already takes SIGUSR1/SIGUSR2 to change its voice ceiling
 * while running, so this is a pattern the project has run before.
 *
 * AND STEP 2 IS NO LONGER THE CALLER'S ALONE - 2026-10-03. A signal
 * needs the sender's UID to match the daemon's (2.2's kill_ok test in
 * kernel/signal.c reads uids and CAP_KILL, never groups), so once the
 * daemons run as the `vlhe' account (design/33 section 3k) a GUI
 * running as `nova' is refused with EPERM - and the channel's 0660
 * group mode, which was meant to be the gate, gated nothing. So the
 * daemon keeps a WAKER, vdiscd_ctl_watch(): a child of the same uid
 * that sleeps in select() on the request FIFO and signals its parent
 * while there is something to read. A caller now needs only to be
 * able to WRITE the FIFO - the group's say, as intended. It still
 * signals when it may (root), which costs one more EINTR; a refusal
 * is not a failure.
 *
 * ONE LINE OF TEXT EACH WAY, and that is deliberate. A binary struct
 * would need the version handling design/33 spends a section on; a
 * line can be written by hand with echo when something is wrong at
 * three in the morning on a machine with no GUI.
 *
 * C89, GCC 2.95.2, no toolkit - the GUI and the CLI both link it.
 */

#ifndef VDISCD_CTL_H
#define VDISCD_CTL_H

#include <sys/types.h>

/* A request or reply line, including its newline and terminator. A
 * path is the long field; VLHE_PATH_MAX is 256 and the verb, the
 * drive number and the separators fit in the rest. */
#define VDISCD_CTL_LINE 320

/* WHERE. /var/run rather than /tmp: it is cleared at boot, which is
 * correct for a pid file and a channel that mean nothing without the
 * daemon that made them. Overridable for testing only. */
const char *vdiscd_ctl_fifo(void);      /* /var/run/vlhe/ctl/vdiscd.ctl */
const char *vdiscd_ctl_pidfile(void);   /* /var/run/vlhe/ctl/vdiscd.pid */

/* ------------------------------------------------------------------ */
/* The daemon's side                                                  */
/* ------------------------------------------------------------------ */

/*
 * CREATE THE CHANNEL AND CLAIM IT. Makes the directory, the FIFO and
 * the pid file, and opens the FIFO O_RDWR|O_NONBLOCK.
 *
 * O_RDWR ON A FIFO IS THE POINT, not an oversight: a reader-only
 * open of a FIFO with no writer returns EOF on every read, and the
 * daemon would spin. Holding a write end itself means the FIFO never
 * reports EOF and a read with nothing in it returns EAGAIN, which is
 * what the loop wants.
 *
 * Returns the fd, or -1 with errno set.
 */
int  vdiscd_ctl_open(void);

/* Tear it down: close, unlink both files. Safe to call twice. */
void vdiscd_ctl_close(int fd);

/*
 * DRAIN ONE REQUEST, non-blocking. Returns 1 and fills `line' if one
 * was waiting, 0 if the channel was empty, -1 on a real error.
 *
 * Called from the loop's EINTR branch, so it must never block: an
 * empty channel is the COMMON case, because SIGTERM also lands
 * there.
 */
int  vdiscd_ctl_read(int fd, char *line, size_t len);

/* Write one reply line. The caller is waiting on the reply FIFO. */
int  vdiscd_ctl_reply(const char *line);

/* ------------------------------------------------------------------ */
/* The caller's side - the GUI and the CLI                            */
/* ------------------------------------------------------------------ */

/*
 * IS THE DAEMON THERE? Reads the pid file and checks the process
 * exists. Returns the pid, or 0.
 *
 * kill(pid, 0) is the check, and it is guarded pid > 0 - CLAUDE.md
 * section 1, after a test's kill(c.pid) became kill(-1, SIGKILL)
 * twice and took the X session with it. A pid file is exactly the
 * place a 0 or a -1 can come from.
 */
pid_t vdiscd_ctl_daemon_pid(void);

/*
 * SEND A COMMAND AND WAIT FOR THE ANSWER. Writes the line, signals
 * the daemon, waits up to `timeout_ms' for a reply.
 *
 * Returns 0 and fills `reply' on success; -1 with errno set
 * otherwise, and ETIMEDOUT specifically when the daemon did not
 * answer - which is the case design/33 section 3g calls "vdiscd
 * wedges", where the caller offers a restart.
 */
int  vdiscd_ctl_send(const char *request, char *reply, size_t len,
                     int timeout_ms);

/* ------------------------------------------------------------------ */
/* The protocol                                                       */
/* ------------------------------------------------------------------ */

/*
 * REQUESTS, one line each:
 *
 *   attach <drive> <path>    load an image into a drive
 *   detach <drive>           eject
 *   status                   what is in each drive
 *   ping                     is the daemon answering
 *
 * REPLIES:
 *
 *   ok [text]
 *   err <text>
 *
 * NO VERSION FIELD, DELIBERATELY. An unknown verb gets `err unknown
 * command', which is the whole compatibility story a text protocol
 * needs - a newer caller asking an older daemon for something gets a
 * clear refusal rather than a misparsed struct.
 */

/* Parse a request line into a verb and up to two arguments. Returns
 * the number of fields found, or -1 if the line is malformed.
 * DESTRUCTIVE - it writes terminators into `line'. */
int vdiscd_ctl_parse(char *line, char **verb, char **arg1, char **arg2);

/*
 * THE WAKER - see the header comment. Forks a child that waits in
 * select() on `fd' (the daemon's request FIFO) and sends SIGUSR1 to
 * its parent while the FIFO is readable, every 20 ms until the parent
 * has drained it - which also covers a signal that landed while the
 * parent was not in GET_REQ and so woke nothing. The child closes
 * every other descriptor first: it must never hold /dev/vdiscctl,
 * whose one opener IS the daemon. It ends by itself within 250 ms of
 * its parent going, so nothing ever has to signal it.
 *
 * Returns the child's pid, or -1. Call after SIGUSR1's handler is in.
 */
pid_t vdiscd_ctl_watch(int fd);

/*
 * /dev/cdrom's INNER LINK - 2026-10-03, the user's design. /dev/cdrom
 * is made ONCE at Load, as root, pointing at `<run dir>/cdrom'; that
 * inner link names the drive, /dev/vdiscN, and lives in a directory the
 * daemons' account owns - so vdiscd can move /dev/cdrom to another
 * drive LIVE, without root and without an unload. The kernel follows
 * the chain at open(): a program that already has the drive open
 * keeps the one it opened (KsCD until its Eject-Eject or a restart,
 * kscd.cpp:927); the next open gets the new drive.
 *
 * vdiscd_ctl_cdrom() is the inner link's path. vdiscd_ctl_set_cdrom()
 * points it at /dev/vdisc<drive> ATOMICALLY - a new link under a
 * temporary name, then rename() over the old - so there is never a
 * moment with no /dev/cdrom. 0, or -1 with errno.
 */
const char *vdiscd_ctl_cdrom(void);
int vdiscd_ctl_set_cdrom(int drive);

#endif /* VDISCD_CTL_H */
