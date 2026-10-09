/*
 * image.h - disc image abstraction.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * A backend turns some on-disk description (.ccd, .cue, plain .iso) plus
 * its data file into a uniform track table, and serves sectors from it.
 *
 * The kernel module never sees any of this - it receives only the
 * flattened struct vdisc_attach at attach time, and block requests
 * afterwards. All format knowledge stays here in userspace.
 */

#ifndef _VDISC_IMAGE_H
#define _VDISC_IMAGE_H

#include <stdio.h>
#include "vdisc.h"
#include "cdtext.h"

struct vdisc_track {
    int     num;            /* track number as printed on the disc      */
    int     start_lba;      /* file-relative first sector               */
    int     length;         /* sectors                                  */
    int     is_data;
    int     control;        /* VDISC_CTRL_* except DATA; see vdisc.h    */
    int     sector_form;    /* VDISC_FORM_*                               */
    int     session;        /* 1-based; every track is 1 on a single-session disc */
};

/*
 * THE FILE TABLE, 2026-09-15 (design/29 deliverable 3).
 *
 * An image used to be ONE data file: `fp', `path', `file_size'. That
 * is a CloneCD set and a plain ISO, and it is NOT a Redump-style CUE,
 * which keeps one .bin per track - both real Mode 2 discs in
 * ExampleCDs/ are that shape, and design/26 B2 measured what the one
 * file did to them: every FILE line overwrote the last, and the whole
 * disc was served from the final .bin.
 *
 * NOT A FILE PER TRACK EITHER. Measured on the Video CD: track 1 is
 * 3375 sectors long in the TOC but its .bin holds 3226; sectors 3226
 * to 3374 are track 2's INDEX 00 pregap and they live in TRACK 2's
 * file. A lookup from track to file reads the wrong file for the tail
 * of every track. So the lookup is by DISC LBA, through a table of
 * extents: each extent is a run of disc sectors backed by a run of one
 * file, or a VIRTUAL run with no bytes behind it - a CUE PREGAP or
 * POSTGAP command, which describes sectors the rip did not store. A
 * read inside a virtual run returns zeros (silence).
 *
 * Every file has its own SECTOR STRIDE: 2352 for raw, 2336 for
 * MODE2/2336 (no sync or header), 2048 for a cooked ISO track, 2448
 * for a CDG rip that appends 96 bytes of subchannel to each frame.
 * The stride is a property of the file; what the bytes MEAN is the
 * track's sector_form. Both are needed to find the user data, and
 * neither reaches the kernel.
 */
struct vdisc_file {
    FILE   *fp;
    char    path[1024];
    long    size;           /* bytes                                    */
    int     secsize;        /* stride: 2048, 2336, 2352 or 2448         */
    int     n_sectors;      /* size / secsize                           */
};

struct vdisc_extent {
    int     file;           /* index into file[], or -1 for a VIRTUAL run */
    int     disc_lba;       /* first disc sector of the run             */
    int     rel;            /* first file-relative sector (file >= 0)   */
    int     n;              /* sectors                                  */
};

#define VDISC_MAX_FILES    VDISC_MAX_TRACKS
/* per track: a file run, a POSTGAP and a PREGAP; per file: its tail */
#define VDISC_MAX_EXTENTS  (VDISC_MAX_TRACKS * 4 + 2)

struct vdisc_image {
    struct vdisc_file   file[VDISC_MAX_FILES];
    int                 n_files;
    struct vdisc_extent extent[VDISC_MAX_EXTENTS];
    int                 n_extents;

    char    desc[1024];     /* the description file, if any             */

    /*
     * A SUBCHANNEL SIDECAR - `.cdg' or `.sub' BESIDE THE DATA FILE.
     *
     * WHY IT IS NEEDED: `read_sub()' otherwise slices the subchannel
     * off the end of each sector, which requires a 2448 stride. A
     * CLONECD RIP IS THREE FILES BY DESIGN - `.ccd' the descriptor,
     * `.img' the 2352 data, `.sub' the subchannel - so its subchannel
     * is in a FILE OF ITS OWN and the slice finds nothing. Every CCD
     * rip with graphics was unreadable until this existed.
     *
     * ONE UNIT PER SECTOR, 96 BYTES, INDEXED BY LBA. Both kinds are
     * exactly `leadout_lba * 96' bytes on the discs measured, so the
     * offset is `lba * 96' with no header and no framing. cdemu does
     * the same (`image-ccd/parser.c:1157', size 96, offset
     * `offset*96').
     *
     * `sub_layout' IS THE ONE THING THE FILE NAME CAN TELL US, and
     * only for one of the two:
     *
     *   .cdg  ALWAYS CDG_SUB_RW96 - it is written FOR players, which
     *         consume 24-byte packets, so it is four of them per
     *         sector with no transform.
     *   .sub  NOT KNOWABLE FROM THE NAME. Interleaving is a
     *         CONVERSION FLAG (`2352to2448 -noint'), so the same
     *         extension covers more than one layout - see design/34
     *         section 6f. -1 here means "sniff it".
     *
     * An absent sidecar is `sub_fp == NULL', which is the ordinary
     * case and not an error.
     */
    FILE   *sub_fp;         /* the sidecar, or NULL                     */
    char    sub_path[1024]; /* its path, for reporting                  */
    long    sub_sectors;    /* its length / 96                          */
    int     sub_layout;     /* CDG_SUB_* , or -1 for "sniff it"         */

    int     first_track;
    int     last_track;
    int     n_tracks;
    int     leadout_lba;
    int     n_sessions;     /* highest session number seen, >= 1        */

    struct vdisc_track track[VDISC_MAX_TRACKS];

    /*
     * CD-TEXT, WHEN THE CONTAINER GIVES IT TO US IN PLAIN TEXT.
     *
     * A CUE sheet may carry TITLE / PERFORMER / SONGWRITER / COMPOSER
     * / ARRANGER / MESSAGE at the top for the disc and inside each
     * TRACK for that track - the same seven fields the packs carry,
     * already decoded by whoever wrote the sheet. No subchannel, no
     * unpack, no CRC: the parse is `read the quoted string'.
     *
     * `present' is 0 for a sheet that carries none, which is most of
     * them. Filling this does NOT mean the disc had CD-TEXT in its
     * lead-in - it means the CUE said so, which is a different and
     * usually more reliable thing.
     */
    struct cdtext text;

    const struct vdisc_backend *backend;
};

struct vdisc_backend {
    const char *name;

    /* Return 1 if this backend recognises the given path. */
    int  (*probe)(const char *path);

    /* Populate img from path. Returns 0 on success, -1 on failure
     * (with a message already printed to stderr). */
    int  (*open)(struct vdisc_image *img, const char *path);

    /* Read `n` cooked 2048-byte sectors starting at `lba` into buf. */
    int  (*read_data)(struct vdisc_image *img, int lba, int n, void *buf);

    /* Read `n` raw 2352-byte CDDA frames starting at `lba` into buf. */
    int  (*read_audio)(struct vdisc_image *img, int lba, int n, void *buf);

    void (*close)(struct vdisc_image *img);
};

/* Pick a backend by probing, then open. Returns 0 or -1. */
int  vdisc_image_open(struct vdisc_image *img, const char *path);
void vdisc_image_close(struct vdisc_image *img);

/* Look up the track containing an lba; returns NULL if out of range. */
const struct vdisc_track *vdisc_image_track_at(const struct vdisc_image *img,
                                           int lba);

/* Fill in a struct vdisc_attach from the image. */
void vdisc_image_fill_attach(const struct vdisc_image *img, int minor,
                           struct vdisc_attach *att);

/*
 * The file table. add_file opens and sizes the file and returns its
 * index, or -1 with a message. set_stride fixes the sector size once
 * it is known (a CUE learns it from the file's first TRACK) and
 * derives n_sectors, warning if the size is not a whole number of
 * sectors. add_extent appends a run; file -1 is a virtual run.
 */
int  vdisc_image_add_file(struct vdisc_image *img, const char *path);
int  vdisc_image_set_stride(struct vdisc_image *img, int file, int secsize);
int  vdisc_image_add_extent(struct vdisc_image *img, int file,
                            int disc_lba, int rel, int n);

/*
 * Where is disc sector `lba'? Returns 1 and fills *file and *rel for a
 * sector backed by a file, 0 for a virtual sector, -1 for one outside
 * every extent.
 */
int  vdisc_image_locate(const struct vdisc_image *img, int lba,
                        int *file, int *rel);

/*
 * The two readers every backend uses - the table makes them generic.
 *
 * read_data: `n' cooked 2048-byte sectors from `lba'. Each sector is
 * located, read at its file's stride, and its user data extracted by
 * the TRACK's form: offset 0 for a cooked ISO, 16 for Mode 1, and for
 * Mode 2 the sector's OWN subheader decides Form 1 (offset 24, or 8 in
 * a 2336-byte file) or Form 2 - which FAILS, as a drive fails a
 * READ(10) of one. A sector inside an audio track fails too, as a
 * drive fails it; a virtual sector reads as zeros.
 *
 * read_audio: `n' raw 2352-byte frames from `lba'. A 2448-byte CDG
 * frame yields its first 2352 (the subchannel is ignored here); a
 * virtual frame is silence.
 */
int  vdisc_image_read_data(struct vdisc_image *img, int lba, int n, void *buf);
int  vdisc_image_read_audio(struct vdisc_image *img, int lba, int n, void *buf);

/*
 * read_sub: `n' sectors' worth of RAW subchannel, VDISC_SUB_SIZE (96)
 * bytes each, P through W interleaved as the disc carries them.
 *
 * Returns 0, -1 for a bad argument or an unreadable sector, and -2
 * when the track's file has NO subchannel - a 2352-byte stride rather
 * than 2448. Those are different answers and a caller needs both:
 * design/27 section 4a measured QUAKE106.sub as a complete capture
 * whose R-W is all zeros, so zeros mean "recorded and empty" while
 * -2 means "never recorded".
 *
 * NOT UNPACKED. cdtext_rw_unpack() does that in userspace, and CD+G
 * and CD-TEXT share it. design/34 has the scope.
 */
int  vdisc_image_read_sub(struct vdisc_image *img, int lba, int n, void *buf);

/*
 * ATTACH A SUBCHANNEL SIDECAR - a `.cdg' or `.sub' beside the data.
 *
 * For a backend to call once it knows the descriptor's path. Absent
 * is the ordinary case: returns 0 having done nothing, and the disc
 * simply has no graphics. 1 when one was attached.
 *
 * `layout' is CDG_SUB_RW96 for a `.cdg' (already four 24-byte
 * packets) or -1 for a `.sub', whose layout the name cannot say -
 * see the struct field and design/34 section 6f.
 *
 * REFUSES A FILE THAT IS NOT A WHOLE NUMBER OF 96-BYTE PACKS,
 * loudly, rather than reading it misaligned and drawing noise.
 */
int  vdisc_image_attach_sub(struct vdisc_image *img, const char *path,
                            int layout);

/*
 * LOOK FOR A SIDECAR BESIDE A DESCRIPTOR and attach the first found.
 *
 * Tries `<base>.cdg' then `<base>.sub', each in upper case too.
 * **`.cdg' FIRST DELIBERATELY**: where a disc has both they are
 * independent extractions rather than conversions of one another
 * (measured), and the `.cdg' needs no transform and states its own
 * layout. Silent when there is none.
 */
void vdisc_image_find_sub(struct vdisc_image *img, const char *desc_path);

/*
 * read_mode2: `n' sectors of VDISC_MODE2_SECTOR (2336) bytes from
 * `lba', each being the raw frame FROM ITS SUBHEADER ONWARD - sync and
 * header stripped, form NOT consulted, bytes as stored.
 *
 * This is what CDROMREADMODE2 asks for, and it is the path a Video CD
 * player needs: read_data() deliberately FAILS on Form 2 (as a drive
 * fails a READ(10) of one), and Form 2 is exactly what a VCD's MPEG
 * track is. The caller gets the subheader and decides for itself.
 */
int  vdisc_image_read_mode2(struct vdisc_image *img, int lba, int n, void *buf);

/*
 * read_pieces: the GENERAL form of the three readers above. `want' is
 * a mask of VDISC_SEC_*, and the named pieces of each sector are
 * returned concatenated in sector order. design/30.
 *
 * piece_len: how many bytes ONE sector yields for that mask, so a
 * caller can size its buffer before reading. `mode2' and `form2'
 * describe the sector, because the sub-header exists only on Mode 2
 * and the user data is 2324 rather than 2048 on Form 2 - which also
 * has no EDC/ECC.
 */
int  vdisc_image_read_pieces(struct vdisc_image *img, int lba, int n,
                             unsigned int want, void *buf, int *got);
int  vdisc_sector_piece_len(unsigned int want, int mode2, int form2);

extern const struct vdisc_backend vdisc_backend_ccd;
extern const struct vdisc_backend vdisc_backend_cue;
extern const struct vdisc_backend vdisc_backend_iso;

#endif /* _VDISC_IMAGE_H */
