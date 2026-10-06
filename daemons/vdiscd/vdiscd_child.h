/*
 * vdiscd_child.h - the CDDA child's handle.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * design/07-vsound.md section 3.1b.
 */

#ifndef _VDISCD_CHILD_H
#define _VDISCD_CHILD_H

struct vdisc_image;

struct vdiscd_child {
    int pid;            /* -1 once collected                          */
    int cmd_fd;         /* the write end of the command pipe          */
    int reaped;         /* it exited and we collected it              */
    int wedged;         /* it ignored SIGKILL - see vdiscd_child_stop */

    /*
     * THE PLAYER'S, ONE SET PER DRIVE - design/52 AR1, 2026-10-03.
     * These were four file-scope statics in vdiscd_play.c, so there was
     * ONE player and it belonged to the drive the command line named.
     * vdiscd_play_init() sets them; nothing in vdiscd_child.c touches
     * them, and vdiscd_child_start() no longer zeroes the whole struct,
     * or every start would forget which drive it plays.
     */
    struct vdisc_image *img;    /* what this drive's child reads      */
    int ctl_fd;                 /* /dev/vdiscctl, for PUT_POS         */
    int minor;                  /* which drive it reports as          */
    int vol;                    /* last level asked for, vsound scale,
                                 * -1 none - design/26 B5             */
};

void vdiscd_child_init(struct vdiscd_child *c);
int  vdiscd_child_start(struct vdiscd_child *c,
                        int (*body)(int cmd_fd, void *ctx), void *ctx);
int  vdiscd_child_cmd(struct vdiscd_child *c, const void *buf,
                      unsigned int len);
int  vdiscd_child_poll(struct vdiscd_child *c);
void vdiscd_child_stop(struct vdiscd_child *c);
void vdiscd_child_abandon(struct vdiscd_child *c);
int  vdiscd_child_recheck(struct vdiscd_child *c);

#endif /* _VDISCD_CHILD_H */
