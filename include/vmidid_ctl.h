/*
 * vmidid_ctl.h - the control channel into vmidid.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * WHY THIS EXISTS. Every synth setting reaches vmidid on its command
 * line, parsed once at startup, so changing one means stopping the
 * daemon and starting it again - which drops the channel, takes
 * ~100 ms to reacquire (design/16) and stops whatever was playing.
 *
 * THE USER MEASURED IT, 2026-09-26: playing gmstriving at 22050,
 * changed the setting to 44100, pressed Apply, stopped playmidi,
 * waited past the 3 s release, started again - still 22050. Correct
 * behaviour for a daemon nothing can talk to, and the reason this
 * file exists. design/43 section 6b.
 *
 * SAME MECHANISM AS vdiscd's, DELIBERATELY. `vsound/vdiscd_ctl.c'
 * is a FIFO under /var/run/vlhe, line-based text, opened O_RDWR.
 * Copying that rather than inventing a second shape is the same
 * argument vlhe_restart() makes for the init script: one mechanism
 * that can be understood once.
 *
 * AND ONE DIFFERENCE, WHICH IS A SIMPLIFICATION. vdiscd blocks in
 * ioctl(VDISC_IOC_GET_REQ) with no select() anywhere, so its caller
 * must send SIGUSR1 to break the daemon out - see vdiscd_ctl.h's
 * four-step sequence. vmidid ALREADY SELECTS on its MIDI fd in two
 * places, so the FIFO simply joins the read set and no signal is
 * needed. The caller writes a line and the daemon acts within one
 * pass of its loop.
 *
 * WHAT IT IS NOT. Not a config reader: vmidid learns nothing about
 * vlhe.conf, so there is no second path to any setting and no
 * question of which source wins. The GUI reads the config and sends
 * values; the daemon applies what it is told.
 *
 * ONE LINE OF TEXT EACH WAY, for vdiscd_ctl.h's reason: a line can
 * be written by hand with echo when something is wrong on a machine
 * with no GUI.
 *
 *     set rate 44100
 *     ok rate 44100 deferred
 *
 * SAFE ON THE WORKSTATION, like vdiscd_ctl.c and for the same
 * reason: it makes a directory, a FIFO and a pid file, and opens no
 * device and loads no module. The host test exercises it directly.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VMIDID_CTL_H
#define VMIDID_CTL_H

#include <sys/types.h>

/*
 * A request or reply line, including its newline and terminator.
 * The longest request is a font path - `set font0 <256 bytes>' -
 * and the verb and separators fit in the rest. Same size as
 * vdiscd's for the same reason.
 */
#define VMIDID_CTL_LINE 320

/* WHERE. /var/run rather than /tmp: cleared at boot, which is right
 * for a channel that means nothing without the daemon that made it.
 * VMIDID_CTL_DIR overrides, for the host test only. */
const char *vmidid_ctl_fifo(void);      /* /var/run/vlhe/ctl/vmidid.ctl */
const char *vmidid_ctl_repfifo(void);   /* /var/run/vlhe/ctl/vmidid.rep */

/* ------------------------------------------------------------------ */
/* The daemon's side                                                  */
/* ------------------------------------------------------------------ */

/*
 * CREATE THE CHANNEL. Makes the directory and the FIFO, and opens it
 * O_RDWR|O_NONBLOCK.
 *
 * O_RDWR ON A FIFO IS THE POINT, not an oversight: a reader-only
 * open of a FIFO with no writer returns EOF on every read, so a
 * select() would report it readable forever and the loop would spin.
 * Holding a write end ourselves means it never reports EOF, and a
 * read with nothing in it returns EAGAIN - which is what the loop
 * wants.
 *
 * Returns the fd, or -1 with errno set. A FAILURE IS NOT FATAL to
 * the daemon: it means settings cannot be changed while running,
 * which is exactly where vmidid was before this existed.
 */
int  vmidid_ctl_open(void);

/*
 * READ ONE REQUEST, IF THERE IS ONE. Non-blocking.
 *
 * Returns 1 and fills `line' (newline stripped), 0 when nothing is
 * waiting, -1 on error. The daemon calls this when select() says
 * the fd is readable, and may call it again until it returns 0 -
 * several requests can arrive in one pass.
 */
int  vmidid_ctl_read(int fd, char *line, size_t max);

/* Write one reply line. The newline is added here. */
void vmidid_ctl_reply(int fd, const char *line);

/*
 * REMOVE THE FIFO. Called on the way out, so a dead daemon leaves no
 * channel that would accept a request nobody will ever act on.
 */
void vmidid_ctl_close(int fd);

/* ------------------------------------------------------------------ */
/* The caller's side - the GUI and the CLI                            */
/* ------------------------------------------------------------------ */

/*
 * SEND ONE REQUEST AND READ THE REPLY.
 *
 * `reply' may be NULL if the caller does not want it. Returns 0 when
 * the daemon answered, -1 otherwise (no channel, no daemon, or no
 * answer within the timeout) with errno set.
 *
 * TWO FIFOs, NOT ONE - `vmidid.ctl' in and `vmidid.rep' back, which
 * is what vdiscd does (`vdiscd_ctl.c:76') and the first draft of
 * this header got wrong.
 *
 * ONE WOULD RACE THE CALLER AGAINST ITSELF. Having written a
 * request, a caller reading the same FIFO can read its OWN line
 * back before the daemon gets to it - the data is sitting there and
 * nothing says who it is for. Two channels make the direction part
 * of the structure rather than of the timing.
 */
int  vmidid_ctl_request(const char *req, char *reply, size_t max);

#endif /* VMIDID_CTL_H */
