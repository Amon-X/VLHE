/*
 * vdiscd_state.h - the drive state file: what is in each drive, and
 * whether it comes back at the next load.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/33 sections 3c and 3i: "The init script reads
 * /var/lib/vlhe/drives and attaches what was there", and the daemon
 * is that file's writer - the GUI never writes it, it asks vdiscd,
 * which records what it did. Built 2026-10-03; until then nothing
 * wrote it and nothing read `DrivesAutoLoad'.
 *
 * WHERE: /var/lib/vlhe/state/drives on an installed machine (state/
 * since 2026-10-04, design/55 section 14), owned by the
 * `vlhe' account the daemon runs as; `drives' in a portable folder,
 * beside `mixers' and `volumes' (the user: "do it on load similar to
 * the mixer"). The plan passes it as `vdiscd -s FILE'.
 *
 * TWO THINGS PER DRIVE, AND THEY ARE INDEPENDENT:
 *
 *   DriveN    the image last attached, cleared by an eject - the
 *             drive's contents, like a disc left in a real drive
 *   AutoloadN 1 if that drive's image comes back at the next load -
 *             the CD page's "Load at startup" box. It belongs to the
 *             DRIVE and survives an eject, so a new disc in a flagged
 *             drive comes back too. The user's case, 2026-10-03:
 *             "I want Quake in 0, and have that autoloaded but keep a
 *             spare vdisc for that I swap out" - drive 0 flagged,
 *             drive 1 not.
 *
 * AND `[CD Settings] DrivesAutoLoad' IS THE MASTER SWITCH: the plan
 * adds `-r' only when it is 1, and without `-r' nothing is reattached
 * whatever the flags say. The user kept both.
 *
 * THE FORMAT IS vlhe.conf's, so the GUI's fallback (vlhe_drives(),
 * when the daemon does not answer) reads it with the ordinary parser:
 *
 *   [Drives]
 *   Drive0 = /mnt/discs/quake/quake.cue
 *   Autoload0 = 1
 *
 * WRITTEN WHOLE, TO A TEMPORARY NAME, THEN RENAMED - a crash mid-write
 * leaves the old file, never half of a new one. That is why the
 * account needs the DIRECTORY, not just the file.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VDISCD_STATE_H
#define VDISCD_STATE_H

#include "vdisc.h"              /* VDISC_MAX_DEVS */

#define VDISCD_STATE_PATH 300   /* the control channel's attach width */

struct vdiscd_state {
    char path[VDISC_MAX_DEVS][VDISCD_STATE_PATH];
    int  autoload[VDISC_MAX_DEVS];
    /* WHICH DRIVE /dev/cdrom REACHES - the CD page's "/dev/cdrom"
     * radio, `Cdrom = N' in the file, 2026-10-03. -1: never chosen,
     * which means drive 0, where the link has always pointed. */
    int  cdrom;
};

/* The drive /dev/cdrom should reach: the recorded one, else 0. */
int vdiscd_state_cdrom(const struct vdiscd_state *st);
/* The same, among `ndrives' loaded drives: a recorded drive past the
 * count gives 0, and the record itself is kept, so a larger count
 * brings it back (design/54 D20). `ndrives' <= 0 means not known, and
 * is the maximum. */
int vdiscd_state_cdrom_in(const struct vdiscd_state *st, int ndrives);

/* Everything empty, every flag off. */
void vdiscd_state_clear(struct vdiscd_state *st);

/*
 * Read `file' into `st'. Keys it does not know, and drives out of
 * range, are ignored. 0 read; -1 absent or unreadable, with `st'
 * cleared - a first run has no file, and that is not an error.
 */
int vdiscd_state_read(const char *file, struct vdiscd_state *st);

/*
 * Write `st' to `file' via `file.new' and rename. A path that would
 * break the format - a newline in it - is left out rather than
 * written. 0 written, -1 not (errno set); the old file is untouched.
 */
int vdiscd_state_write(const char *file, const struct vdiscd_state *st);

/* The drives `-r' would bring back: a flag AND an image. */
int vdiscd_state_wanted(const struct vdiscd_state *st, int drive);

#endif /* VDISCD_STATE_H */
