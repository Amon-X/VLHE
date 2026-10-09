/*
 * vdiscd_play.h - the disc daemon's audio commands.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/07-vsound.md sections 3.1, 3.1a, 3.1b.
 */

#ifndef _VDISCD_PLAY_H
#define _VDISCD_PLAY_H

struct vdiscd_child;
struct vdisc_image;

/*
 * ONE COMMAND IS ONE WRITE AND ONE READ - design/26 B11, 2026-09-15.
 *
 * A play used to be four write()s (the byte, lba, end, track) that the
 * child read as four non-blocking read()s. If the child woke between
 * the parent's first write and its last it read the byte, found lba
 * missing, dropped the play, and then read the twelve bytes that
 * arrived next AS COMMANDS - 0x73 is STOP, 0x70 PLAY, 0x76 VOLUME.
 * A pipe write below PIPE_BUF (4096 on 2.2) is atomic, so one struct
 * per command cannot tear: the child gets all of it or none of it.
 * Every command is the same size; the fields a command does not use
 * are zero.
 *
 * Both sides include this header; the layout is not repeated anywhere.
 */
struct vdiscd_cmd {
    unsigned char op;           /* CMD_*                              */
    unsigned char pad[3];
    unsigned int  lba;          /* PLAY: first frame                  */
    unsigned int  end;          /* PLAY: one past the last frame      */
    int           arg;          /* PLAY: track; VOLUME: the level     */
};

#define CMD_PLAY        'p'
#define CMD_PAUSE       'z'
#define CMD_RESUME      'r'
#define CMD_STOP        's'
#define CMD_VOLUME      'v'

void vdiscd_play_init(struct vdiscd_child *c, int ctl_fd, int minor);
int  vdiscd_play(struct vdiscd_child *c, struct vdisc_image *img,
                 int lba, int end, int track);
int  vdiscd_play_volume(struct vdiscd_child *c, int vol255);
int  vdiscd_play_pause(struct vdiscd_child *c);
int  vdiscd_play_resume(struct vdiscd_child *c);
int  vdiscd_play_stop(struct vdiscd_child *c);
int  vdiscd_play_stalled(struct vdiscd_child *c);

/* WHERE THE AUDIO CHILD WRITES, when it is not /dev/dsp. vsound takes
 * the node the user picked (design/38), so the plan passes `-o' and
 * this carries it to vdiscd_audio.c. Must be called before the child
 * forks; the default is /dev/dsp. */
void vdiscd_audio_set_device(const char *node);

/* The child's whole life, in vdiscd_audio.c. */
int  vdiscd_audio_child(int cmd_fd,
                        int (*read_frames)(void *ctx, unsigned int lba,
                                           unsigned int nframes,
                                           void *buf),
                        void (*report)(void *ctx, unsigned int lba,
                                       int status, int track),
                        int (*track_at)(void *ctx, unsigned int lba),
                        void *ctx);

#endif /* _VDISCD_PLAY_H */
