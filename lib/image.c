/*
 * image.c - backend dispatch and the shared sector-reading helpers.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * C89 / GCC 2.95 clean.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "image.h"
#include "cdg.h"      /* CDG_SUB_RW96 - a `.cdg' sidecar's layout is known */

static const struct vdisc_backend *backends[] = {
    &vdisc_backend_ccd,   /* preferred: authoritative for CloneCD sets */
    &vdisc_backend_cue,
    &vdisc_backend_iso
};
#define N_BACKENDS ((int)(sizeof backends / sizeof backends[0]))

int
vdisc_image_open(struct vdisc_image *img, const char *path)
{
    int i;

    memset(img, 0, sizeof *img);

    for (i = 0; i < N_BACKENDS; i++) {
        if (backends[i]->probe(path)) {
            img->backend = backends[i];
            if (backends[i]->open(img, path) < 0)
                return -1;
            return 0;
        }
    }

    fprintf(stderr, "vdiscd: %s: unrecognised image type "
                    "(expected .ccd, .cue or .iso)\n", path);
    return -1;
}

void
vdisc_image_close(struct vdisc_image *img)
{
    int i;

    if (img->backend && img->backend->close)
        img->backend->close(img);
    for (i = 0; i < img->n_files; i++) {
        if (img->file[i].fp) {
            fclose(img->file[i].fp);
            img->file[i].fp = NULL;
        }
    }
    img->n_files = 0;

    /* THE SIDECAR IS NOT IN `file[]' - it holds no user data and no
     * extent points at it, so it is closed on its own. */
    if (img->sub_fp) {
        fclose(img->sub_fp);
        img->sub_fp = NULL;
    }
    img->sub_sectors = 0;
    img->sub_layout  = -1;
}


/*
 * THE 2448 DE-INTERLEAVE - SOLVED 2026-09-28, design/34 section 6g.
 *
 * A 2448-byte rip stores 96 bytes of INTERLEAVED P-W after each
 * 2352-byte frame, and CD+G lives in the low six bits (P is bit 7,
 * Q bit 6, R-W bits 5-0). Masking those six bits out is correct and
 * IS NOT ENOUGH: the symbols of one sector's CD+G are spread across
 * the subchannel of the sectors ONE AND TWO BEFORE IT.
 *
 * EIGHT WITHIN-SECTOR HYPOTHESES WERE TRIED AND ALL FAILED - byte
 * permutation, 6-bit repack, positional offset, all 20,160 channel
 * orderings, cdemu's block deinterleave, channel-major blocks, XOR
 * descrambling (0x55 and friends, which DESTROY the command byte
 * that is already correct), and whole-stream bit shifts. The best
 * any reached was 32 of 96 bytes, because every one of them assumed
 * a sector's own 96 bytes hold its own CD+G rearranged. They do not.
 *
 * `sub_deint[k]` says where output symbol `k` comes from: `.d` is a
 * SECTOR DELAY (0, -1 or -2) and `.j` the slot within that sector.
 *
 * THE STRUCTURE IS REAL, NOT A FITTED TABLE. Every `k % 4 == 0`
 * slot maps to ITSELF with only a delay - all 24 of them - and the
 * others fall on +24/+48/+72 offsets, a four-way interleave with a
 * 24-symbol stride. A closed form was sought and does not fit
 * exactly (the obvious `(k%4)*24 + (k//4)` transpose scores 26%), so
 * the table is empirical and says so.
 *
 * DERIVED against byte-exact ground truth: we hold the Jimi Hendrix
 * `Smash Hits` disc BOTH as a 2448 rip and as a `.cdg`, same
 * catalogue, same 205,605 sectors. Derived from sectors 5000-5599
 * and verified on 20000-20499 - fifteen thousand sectors away - at
 * **99.973%**.
 *
 * AND IT IS THE FORMAT, NOT THIS FILE. Applied to Fleetwood Mac's
 * `Behind the Mask`, a different 2448 rip whose `.cdg` we do NOT
 * hold, it gives **100% valid CD+G instructions** where reading the
 * sector directly gives 9.6%.
 */
struct sub_deint_ent { int d; int j; };

static const struct sub_deint_ent sub_deint[96] = {
    {-2,  0}, {-2, 66}, {-1, 29}, {-1, 95}, {-1,  4}, {-2, 50},
    {-1, 54}, {-1, 79}, {-2,  8}, {-2, 33}, {-2, 58}, {-2, 83},
    {-1, 12}, {-1, 37}, {-1, 62}, {-1, 87}, {-2, 16}, {-2, 41},
    {-2, 25}, {-2, 91}, {-1, 20}, {-1, 45}, {-1, 70}, {-2, 75},
    {-2, 24}, {-2, 90}, {-1, 53}, { 0, 23}, {-1, 28}, {-2, 74},
    {-1, 78}, { 0,  7}, {-2, 32}, {-2, 57}, {-2, 82}, {-1, 11},
    {-1, 36}, {-1, 61}, {-1, 86}, { 0, 15}, {-2, 40}, {-2, 65},
    {-2, 49}, {-1, 19}, {-1, 44}, {-1, 69}, {-1, 94}, {-1,  3},
    {-2, 48}, {-1, 18}, {-1, 77}, { 0, 47}, {-1, 52}, {-1,  2},
    { 0,  6}, { 0, 31}, {-2, 56}, {-2, 81}, {-1, 10}, {-1, 35},
    {-1, 60}, {-1, 85}, { 0, 14}, { 0, 39}, {-2, 64}, {-2, 89},
    {-2, 73}, {-1, 43}, {-1, 68}, {-1, 93}, { 0, 22}, {-1, 27},
    {-2, 72}, {-1, 42}, { 0,  5}, { 0, 71}, {-1, 76}, {-1, 26},
    { 0, 30}, { 0, 55}, {-2, 80}, {-1,  9}, {-1, 34}, {-1, 59},
    {-1, 84}, { 0, 13}, { 0, 38}, { 0, 63}, {-2, 88}, {-1, 17},
    {-1,  1}, {-1, 67}, {-1, 92}, { 0, 21}, { 0, 46}, {-1, 51}
};

/*
 * LOOK FOR A SIDECAR BESIDE A DESCRIPTOR AND ATTACH THE FIRST FOUND.
 *
 * `.cdg' IS TRIED FIRST, and that is a decision rather than an
 * ordering accident. Where a disc has both - the Information Society
 * set does - they are INDEPENDENT EXTRACTIONS of the same disc by
 * different tools, not one converted from the other (measured: an
 * unpack of the `.sub' matches the `.cdg' at 0% across 538 sectors
 * at nine offsets). The `.cdg' needs NO TRANSFORM and its layout is
 * known from its name; the `.sub' needs a sniff and may be a layout
 * we do not model. Prefer the one we can read without guessing.
 *
 * BOTH CASES ARE TRIED IN UPPER CASE TOO. These files come off
 * Windows tools and ISO-9660 volumes, where the case is whatever the
 * filesystem felt like.
 *
 * Silent when there is none, which is most discs.
 */
void
vdisc_image_find_sub(struct vdisc_image *img, const char *desc_path)
{
    static const char *const try[] = { "cdg", "CDG", "sub", "SUB" };
    char base[1024];
    int  i, n;

    if (img == NULL || desc_path == NULL || img->sub_fp != NULL)
        return;

    strncpy(base, desc_path, sizeof base - 1);
    base[sizeof base - 1] = '\0';

    /* STRIP THE LAST EXTENSION, whatever it is - this is called with
     * a `.ccd' or a `.cue' and should not have to know which. */
    n = (int) strlen(base);
    while (n > 0 && base[n - 1] != '.' && base[n - 1] != '/')
        n--;
    if (n <= 0 || base[n - 1] != '.')
        return;                         /* no extension to swap */
    base[n - 1] = '\0';

    for (i = 0; i < (int) (sizeof try / sizeof try[0]); i++) {
        char cand[1024];
        int  layout;

        sprintf(cand, "%.1000s.%s", base, try[i]);

        /* `.cdg' IS RW96 BY CONSTRUCTION - it is written for players,
         * which consume 24-byte packets. A `.sub' gets -1: see
         * image.h, and design/34 section 6f for why its name cannot
         * say. */
        layout = (try[i][0] == 'c' || try[i][0] == 'C')
               ? CDG_SUB_RW96 : -1;

        if (vdisc_image_attach_sub(img, cand, layout))
            return;
    }
}

/*
 * ATTACH A SUBCHANNEL SIDECAR - `.cdg' or `.sub' beside the image.
 *
 * CALLED BY A BACKEND once it knows the descriptor's path: swap the
 * extension and look. Absent is the ordinary case and returns 0
 * having done nothing, because most discs have no graphics.
 *
 * `layout' is CDG_SUB_RW96 for a `.cdg' - which is what a player
 * consumes, so it is four 24-byte packets and needs no transform -
 * or -1 for a `.sub', whose layout the NAME CANNOT SAY. See
 * image.h and design/34 section 6f.
 *
 * NOT FATAL ON ANY FAILURE. A disc whose sidecar will not open still
 * plays; it simply shows no picture.
 */
int
vdisc_image_attach_sub(struct vdisc_image *img, const char *path, int layout)
{
    FILE *fp;
    long  bytes;

    if (img == NULL || path == NULL || img->sub_fp != NULL)
        return 0;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return 0;                       /* absent - the normal case */

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    bytes = ftell(fp);

    /*
     * A WHOLE NUMBER OF PACKS OR IT IS NOT ONE. Both kinds measured
     * are exactly `leadout * 96', so a file that does not divide is
     * something else wearing the extension - refuse it rather than
     * read misaligned packs and draw noise.
     */
    if (bytes <= 0 || (bytes % VDISC_SUB_SIZE) != 0) {
        fprintf(stderr, "vdiscd: %s: %ld bytes is not a whole number of "
                        "96-byte packs - ignoring it\n", path, bytes);
        fclose(fp);
        return 0;
    }

    img->sub_fp      = fp;
    img->sub_sectors = bytes / VDISC_SUB_SIZE;
    img->sub_layout  = layout;
    strncpy(img->sub_path, path, sizeof img->sub_path - 1);
    img->sub_path[sizeof img->sub_path - 1] = '\0';

    fprintf(stderr, "vdiscd: subchannel sidecar %s (%ld sectors, %s)\n",
            path, img->sub_sectors,
            layout < 0 ? "layout to be sniffed" : "RW96");
    return 1;
}

const struct vdisc_track *
vdisc_image_track_at(const struct vdisc_image *img, int lba)
{
    int i;

    for (i = 0; i < img->n_tracks; i++) {
        const struct vdisc_track *t = &img->track[i];
        if (lba >= t->start_lba && lba < t->start_lba + t->length)
            return t;
    }
    return NULL;
}

void
vdisc_image_fill_attach(const struct vdisc_image *img, int minor,
                      struct vdisc_attach *att)
{
    int i;

    memset(att, 0, sizeof *att);
    att->proto_version = VDISC_PROTO_VERSION;
    att->minor         = (__u32) minor;
    att->total_sectors = (__u32) img->leadout_lba;
    att->leadout_lba   = (__u32) img->leadout_lba;
    att->first_track   = (__u8)  img->first_track;
    att->last_track    = (__u8)  img->last_track;
    att->n_tracks      = (__u8)  img->n_tracks;

    /*
     * BLOCK LBA EQUALS DISC LBA - protocol 2. data_start is always 0
     * and the device runs to the end of the LAST data track, so that a
     * second session's filesystem sits where isofs will look for it
     * after CDROMMULTISESSION: at its absolute address. vdisc.h has the
     * history of the window this replaces. Audio tracks inside that
     * range are reachable by a block read and the reader refuses them,
     * as a drive does.
     */
    att->data_sectors = 0;
    att->data_start   = 0;
    for (i = 0; i < img->n_tracks; i++) {
        if (img->track[i].is_data) {
            int end = img->track[i].start_lba + img->track[i].length;
            if ((__u32) end > att->data_sectors)
                att->data_sectors = (__u32) end;
        }
    }

    for (i = 0; i < img->n_tracks && i < VDISC_MAX_TRACKS; i++) {
        att->track[i].start_lba    = (__u32) img->track[i].start_lba;
        att->track[i].length       = (__u32) img->track[i].length;
        att->track[i].is_data      = (__u8)  img->track[i].is_data;
        att->track[i].control      = (__u8)  (img->track[i].control &
                                              ~VDISC_CTRL_DATA);
        att->track[i].sector_form  = (__u8)  img->track[i].sector_form;
        att->track[i].session      = (__u8)  (img->track[i].session > 0
                                              ? img->track[i].session : 1);
    }
}

/* ------------------------------------------------------------------ *
 * The file table
 * ------------------------------------------------------------------ */

int
vdisc_image_add_file(struct vdisc_image *img, const char *path)
{
    struct vdisc_file *f;

    if (img->n_files >= VDISC_MAX_FILES) {
        fprintf(stderr, "vdiscd: %s: too many data files (max %d)\n",
                path, VDISC_MAX_FILES);
        return -1;
    }
    if (strlen(path) >= sizeof f->path) {
        fprintf(stderr, "vdiscd: data file path too long\n");
        return -1;
    }
    f = &img->file[img->n_files];
    memset(f, 0, sizeof *f);
    strcpy(f->path, path);

    f->fp = fopen(path, "rb");
    if (!f->fp) {
        fprintf(stderr, "vdiscd: %s: cannot open\n", path);
        return -1;
    }
    if (fseek(f->fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "vdiscd: %s: cannot seek\n", path);
        fclose(f->fp);
        f->fp = NULL;
        return -1;
    }
    f->size = ftell(f->fp);
    if (f->size < 0) {
        fprintf(stderr, "vdiscd: %s: cannot size\n", path);
        fclose(f->fp);
        f->fp = NULL;
        return -1;
    }
    return img->n_files++;
}

int
vdisc_image_set_stride(struct vdisc_image *img, int file, int secsize)
{
    struct vdisc_file *f;

    if (file < 0 || file >= img->n_files || secsize <= 0)
        return -1;
    f = &img->file[file];
    f->secsize   = secsize;
    f->n_sectors = (int) (f->size / secsize);

    /*
     * A 2448 STRIDE MEANS WE WILL SERVE DE-INTERLEAVED R-W.
     *
     * `read_sub()' runs `sub_deint[]' over an embedded subchannel,
     * so what it HANDS BACK is already separated R-W - CDG_SUB_RW96
     * - whatever the bytes on disc look like. Recording that here,
     * where the stride becomes known, rather than in the read loop:
     * a caller asks the layout BEFORE its first read (to decide
     * whether to sniff), and a field filled as a side effect of
     * reading is not there when it looks. That cost a correct
     * decode its first test - 0 packets, because the caller sniffed
     * our own output and chose PW96.
     *
     * A SIDECAR STILL WINS. `find_sub()' runs at open and sets this
     * from the file's extension; the guard keeps that answer.
     */
    if (secsize == VDISC_RAW_SECTOR + VDISC_SUB_SIZE && img->sub_fp == NULL)
        img->sub_layout = CDG_SUB_RW96;
    if (f->size % secsize) {
        fprintf(stderr, "vdiscd: warning: %s size %ld is not a multiple of "
                        "%d - image may be truncated\n",
                f->path, f->size, secsize);
    }
    return 0;
}

int
vdisc_image_add_extent(struct vdisc_image *img, int file,
                       int disc_lba, int rel, int n)
{
    struct vdisc_extent *e;

    if (n <= 0)
        return 0;               /* nothing to describe; not an error */
    if (img->n_extents >= VDISC_MAX_EXTENTS) {
        fprintf(stderr, "vdiscd: image layout too fragmented (max %d runs)\n",
                VDISC_MAX_EXTENTS);
        return -1;
    }
    if (file >= img->n_files)
        return -1;
    e = &img->extent[img->n_extents++];
    e->file     = file < 0 ? -1 : file;
    e->disc_lba = disc_lba;
    e->rel      = rel;
    e->n        = n;
    return 0;
}

int
vdisc_image_locate(const struct vdisc_image *img, int lba,
                   int *file, int *rel)
{
    int i;

    for (i = 0; i < img->n_extents; i++) {
        const struct vdisc_extent *e = &img->extent[i];
        if (lba >= e->disc_lba && lba < e->disc_lba + e->n) {
            if (e->file < 0)
                return 0;
            *file = e->file;
            *rel  = e->rel + (lba - e->disc_lba);
            return 1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Sector readers
 *
 * OFFSETS ARE `long', WHICH IS 32-BIT SIGNED ON i386 - a 2 GB ceiling,
 * and fseek/ftell share it. That is a real limit on this platform and
 * has bitten this project's hardware before, so it is worth stating
 * that NO REAL CD CAN REACH IT:
 *
 *   LONG_MAX / 2352      = 913045 sectors = 202 minutes of CDDA
 *   a Red Book CD        = 360000 sectors =  80 minutes = 847 MB
 *   a 99-minute overburn = 445500 sectors = 1.05 GB
 *
 * So the largest disc that exists is under half the limit. The only
 * thing that would exceed it is a DVD image at 4.7 GB, and DVD support
 * is deliberately out of scope - generic_packet is NULL, see
 * vdisc_mod.c.
 *
 * If that ever changes, these become off_t with _FILE_OFFSET_BITS=64,
 * and the files' size with them.
 * ------------------------------------------------------------------ */

/* The largest stride a file may have; one frame's buffer. */
#define VDISC_MAX_STRIDE 2448

/* Read one file sector into frame[]; returns the stride or -1. */
static int
read_frame(struct vdisc_image *img, int file, int rel, unsigned char *frame)
{
    struct vdisc_file *f = &img->file[file];
    long off = (long) rel * f->secsize;

    if (f->secsize <= 0 || f->secsize > VDISC_MAX_STRIDE)
        return -1;
    if (rel < 0 || rel >= f->n_sectors)
        return -1;
    if (fseek(f->fp, off, SEEK_SET) != 0)
        return -1;
    if (fread(frame, 1, (size_t) f->secsize, f->fp) != (size_t) f->secsize)
        return -1;
    return f->secsize;
}

/*
 * Mode 2 (CD-ROM XA): the form is read from each sector's own
 * subheader, byte 18 bit 5 of a 2352-byte frame - never from the
 * track. design/27 3c measured why: on both real Mode 2 discs in
 * ExampleCDs/ track 1 mixes Form 1 (the ISO-9660 structures, 2048
 * bytes at offset 24) with Form 2 (the content, 2324 bytes, no ECC).
 * The filesystem is all Form 1, so a mount and a directory listing
 * work; a Form 2 sector cannot be returned through a 2048-byte block
 * read and the request FAILS, exactly as ide-cd and sr fail a READ(10)
 * of one. Returning the first 2048 of the 2324 would hand `cp' a
 * silently damaged file.
 *
 * The header's mode byte is NOT consulted. The Sega Video CD's
 * boundary sectors carry a correct MSF and a mode byte with junk in
 * the high bits (0xe2, 0xc2, 0x82 ...) over all-zero user data; the
 * submode bit alone reads them as empty Form 1, which is right.
 *
 * A 2336-byte file is the same sector without sync and header, so the
 * subheader sits at 0 and the data at 8.
 *
 * The messages print ONCE per daemon: a `cp' of a .DAT file would
 * otherwise print one line per sector.
 */
static int
extract_mode2(const unsigned char *frame, int stride, int lba,
              unsigned char *out)
{
    static int warned_form2, warned_subhdr;
    int sub;

    if (stride == VDISC_RAW_SECTOR)
        sub = VDISC_XA_SUBHDR_OFF;
    else if (stride == VDISC_RAW_SECTOR - VDISC_XA_SUBHDR_OFF)   /* 2336 */
        sub = 0;
    else
        return -1;

    if (memcmp(frame + sub, frame + sub + VDISC_XA_SUBHDR_SIZE,
               VDISC_XA_SUBHDR_SIZE) != 0 && !warned_subhdr) {
        fprintf(stderr, "vdiscd: warning: lba %d: the two copies of the XA "
                        "subheader disagree; using the first (reported "
                        "once)\n", lba);
        warned_subhdr = 1;
    }
    if (frame[sub + (VDISC_XA_SUBMODE_OFF - VDISC_XA_SUBHDR_OFF)]
            & VDISC_XA_FORM2) {
        if (!warned_form2) {
            fprintf(stderr, "vdiscd: lba %d is a Mode 2 Form 2 sector (2324 "
                            "bytes) - a 2048-byte block read cannot return "
                            "it; refused, as a real drive refuses it "
                            "(reported once)\n", lba);
            warned_form2 = 1;
        }
        return -1;
    }
    memcpy(out, frame + sub + 2 * VDISC_XA_SUBHDR_SIZE, VDISC_SECTOR_SIZE);
    return 0;
}

int
vdisc_image_read_data(struct vdisc_image *img, int lba, int n, void *buf)
{
    static int warned_audio;
    unsigned char frame[VDISC_MAX_STRIDE];
    unsigned char *out = (unsigned char *) buf;
    int i;

    for (i = 0; i < n; i++, out += VDISC_SECTOR_SIZE) {
        int cur = lba + i;
        const struct vdisc_track *t = vdisc_image_track_at(img, cur);
        int file, rel, where, stride;

        /*
         * A block read into an audio track. A real drive fails it
         * (ILLEGAL MODE FOR THIS TRACK); so do we, since with block
         * LBA equal to disc LBA the block device covers the whole
         * disc and dd would otherwise read Mode 1 "data" out of
         * CDDA frames.
         */
        if (!t || !t->is_data) {
            if (!warned_audio) {
                fprintf(stderr, "vdiscd: lba %d is %s - a data read there "
                                "is refused, as a real drive refuses it "
                                "(reported once)\n", cur,
                        t ? "in an audio track" : "outside every track");
                warned_audio = 1;
            }
            return -1;
        }

        where = vdisc_image_locate(img, cur, &file, &rel);
        if (where < 0)
            return -1;
        if (where == 0) {
            memset(out, 0, VDISC_SECTOR_SIZE);      /* a virtual gap */
            continue;
        }

        stride = read_frame(img, file, rel, frame);
        if (stride < 0)
            return -1;

        switch (t->sector_form) {
        case VDISC_FORM_ISO_2048:
            if (stride != VDISC_SECTOR_SIZE)
                return -1;
            memcpy(out, frame, VDISC_SECTOR_SIZE);
            break;
        case VDISC_FORM_MODE1_2352:
            if (stride != VDISC_RAW_SECTOR)
                return -1;
            memcpy(out, frame + VDISC_MODE1_DATA_OFF, VDISC_SECTOR_SIZE);
            break;
        case VDISC_FORM_MODE2:
            if (extract_mode2(frame, stride, cur, out) < 0)
                return -1;
            break;
        default:
            return -1;
        }
    }
    return 0;
}

/*
 * Read `n' MODE 2 sectors as the uniform layer's CDROMREADMODE2 wants
 * them: 2336 bytes each, the sector from its SUBHEADER onward -
 * subheader, user data and whatever follows - with the 12-byte sync
 * and 4-byte header stripped.
 *
 * WHY THIS IS NOT read_data() WITH A DIFFERENT SIZE. That one extracts
 * USER DATA by form, and deliberately FAILS on Form 2 because a drive
 * fails a READ(10) of one - which is right for a filesystem, and
 * useless here: **Form 2 is exactly what a Video CD's MPEG track is**.
 * The Sega sampler's track 2 is Form 2 throughout (132,969 sectors,
 * 443 of 443 sampled).
 *
 * AND IT IS NOT read_audio() EITHER, which returns 2352 from offset 0.
 * The layer asks for 2336 and expects the subheader first; handing it
 * a sync pattern would shift every byte by 16.
 *
 * THE FORM IS NOT CONSULTED, and that is deliberate. CDROMREADMODE2
 * asks for a fixed 2336 bytes whatever the sector holds (the layer's
 * own `blocksize` is CD_FRAMESIZE_RAW0, a constant), so BOTH forms are
 * returned bytes-as-stored and the CALLER decides what it has from the
 * subheader it now possesses. That is what a drive does; extracting by
 * form is the cooked path's job.
 *
 * A 2336-byte file already IS this layout - the subheader sits at 0 -
 * so it copies whole. A 2448-byte CDG frame yields its first 2352's
 * worth, subchannel ignored, same as the audio path.
 */
/*
 * How many bytes one sector yields for a given field mask.
 *
 * SEPARATE FROM THE ASSEMBLY so the caller can size its buffer before
 * reading anything - the kernel needs this to check the request
 * against cgc->buflen, and the daemon to set the reply length.
 *
 * `mode2' and `form2' describe the sector, because two of the pieces
 * depend on it: the sub-header exists only on Mode 2, and the user
 * data is 2048 bytes on Mode 1 and Mode 2 Form 1 but 2324 on Form 2,
 * which carries no EDC/ECC at all.
 */
int
vdisc_sector_piece_len(unsigned int want, int mode2, int form2)
{
    int n = 0;

    if (want & VDISC_SEC_SYNC)
        n += VDISC_SYNC_SIZE;
    if (want & VDISC_SEC_HEADER)
        n += VDISC_HDR_SIZE;
    if ((want & VDISC_SEC_SUBHDR) && mode2)
        n += 2 * VDISC_XA_SUBHDR_SIZE;
    if (want & VDISC_SEC_DATA)
        n += form2 ? VDISC_FORM2_DATA : VDISC_SECTOR_SIZE;
    if (want & VDISC_SEC_EDC)
        n += form2 ? VDISC_EDC_FORM2
                   : (mode2 ? VDISC_EDC_FORM1 : VDISC_EDC_MODE1);
    /*
     * C2 IS ZERO-FILLED AND THAT IS THE CORRECT ANSWER, not a stub.
     * MMC-2 4.2.5: "If the drive does not support the C2 pointers ...
     * the data returned shall be zero filled." A C2 bit means "the
     * drive could not read this byte reliably"; there was no physical
     * read here, so no byte was unreadable, and zeros say exactly
     * that. cdemu does the same.
     */
    if (want & VDISC_SEC_C2BLOCK)
        n += VDISC_C2_SIZE + 2;     /* C2 + block error byte + pad */
    else if (want & VDISC_SEC_C2)
        n += VDISC_C2_SIZE;

    return n;
}

/*
 * Assemble the requested pieces of `n' sectors from `lba'.
 *
 * THE GENERAL FORM OF THE THREE READERS ABOVE. Each of them is this
 * with a fixed mask:
 *
 *   read_audio  SYNC|HEADER|SUBHDR|DATA|EDC on a 2352 frame  (all of it)
 *   read_mode2  SUBHDR|DATA|EDC                              (2336)
 *   read_data   DATA                                         (2048)
 *
 * and any other combination is equally answerable, which the
 * whole-byte matching it replaces could not do. design/30 has why
 * that matters: `CDROM_SEND_PACKET' passes an application's own
 * command through, and three references all decode the bitfield.
 *
 * THE PIECES COME OUT IN SECTOR ORDER, which is what the standard
 * requires - sync, header, sub-header, data, EDC, then C2.
 */
int
vdisc_image_read_pieces(struct vdisc_image *img, int lba, int n,
                        unsigned int want, void *buf, int *got)
{
    unsigned char frame[VDISC_MAX_STRIDE];
    unsigned char *out = (unsigned char *) buf;
    unsigned char *start = out;
    int i;

    /*
     * `got' RETURNS THE LENGTH ACTUALLY ASSEMBLED, because the caller
     * cannot compute it: every sector's contribution depends on its
     * OWN form, and a Form 2 sector yields 2324 bytes of user data
     * where a Form 1 yields 2048. Asking the image twice - once to
     * read, once to measure - would read the file twice.
     */
    if (got != NULL)
        *got = 0;

    for (i = 0; i < n; i++) {
        const struct vdisc_track *t;
        int file, rel, where, stride, sub, mode2, form2, dataoff;

        t = vdisc_image_track_at(img, lba + i);
        if (t == NULL)
            return -1;

        /*
         * THE POLICY BIT, and it is the run-107 lesson. Only refuse a
         * data track when the caller SAID it wanted CD-DA.
         */
        if ((want & VDISC_SEC_AUDIO) && t->is_data)
            return -1;

        where = vdisc_image_locate(img, lba + i, &file, &rel);
        if (where < 0)
            return -1;

        if (where == 0) {
            /* A virtual sector: silence for audio, zeros for data. */
            int len = vdisc_sector_piece_len(want, 0, 0);
            memset(out, 0, (size_t) len);
            out += len;
            continue;
        }

        stride = read_frame(img, file, rel, frame);
        if (stride < 0)
            return -1;

        /*
         * WHERE THE SECTOR STARTS IN THE FRAME. A 2352 or 2448 file
         * holds the whole thing; a 2336 file begins at the sub-header
         * and has no sync or header to give.
         */
        if (stride == VDISC_RAW_SECTOR || stride == VDISC_CDG_SECTOR) {
            sub = VDISC_XA_SUBHDR_OFF;
        } else if (stride == VDISC_MODE2_SECTOR) {
            sub = 0;
            if (want & (VDISC_SEC_SYNC | VDISC_SEC_HEADER))
                return -1;      /* not stored in this file */
        } else {
            /* A 2048-byte cooked file: only user data exists. */
            if (want & ~(unsigned int)(VDISC_SEC_DATA | VDISC_SEC_AUDIO))
                return -1;
            memcpy(out, frame, VDISC_SECTOR_SIZE);
            out += VDISC_SECTOR_SIZE;
            continue;
        }


        /*
         * Mode and form. The mode byte is at offset 15 of a full
         * frame; a 2336 file has no header, and everything stored
         * that way is Mode 2 by construction.
         */
        mode2 = (sub == 0) ? 1 : (frame[VDISC_MODE_OFF] == 2);
        form2 = mode2 && (frame[sub + (VDISC_XA_SUBMODE_OFF -
                                       VDISC_XA_SUBHDR_OFF)]
                          & VDISC_XA_FORM2) ? 1 : 0;

        if (want & VDISC_SEC_SYNC) {
            memcpy(out, frame, VDISC_SYNC_SIZE);
            out += VDISC_SYNC_SIZE;
        }
        if (want & VDISC_SEC_HEADER) {
            memcpy(out, frame + VDISC_SYNC_SIZE, VDISC_HDR_SIZE);
            out += VDISC_HDR_SIZE;
        }
        if ((want & VDISC_SEC_SUBHDR) && mode2) {
            memcpy(out, frame + sub, 2 * VDISC_XA_SUBHDR_SIZE);
            out += 2 * VDISC_XA_SUBHDR_SIZE;
        }
        if (want & VDISC_SEC_DATA) {
            int len = form2 ? VDISC_FORM2_DATA : VDISC_SECTOR_SIZE;
            dataoff = mode2 ? sub + 2 * VDISC_XA_SUBHDR_SIZE : sub;
            memcpy(out, frame + dataoff, (size_t) len);
            out += len;
        }
        if (want & VDISC_SEC_EDC) {
            int len = form2 ? VDISC_EDC_FORM2
                            : (mode2 ? VDISC_EDC_FORM1 : VDISC_EDC_MODE1);
            int dlen = form2 ? VDISC_FORM2_DATA : VDISC_SECTOR_SIZE;
            dataoff = (mode2 ? sub + 2 * VDISC_XA_SUBHDR_SIZE : sub) + dlen;
            memcpy(out, frame + dataoff, (size_t) len);
            out += len;
        }
        if (want & (VDISC_SEC_C2 | VDISC_SEC_C2BLOCK)) {
            int len = (want & VDISC_SEC_C2BLOCK)
                      ? VDISC_C2_SIZE + 2 : VDISC_C2_SIZE;
            memset(out, 0, (size_t) len);       /* see the comment above */
            out += len;
        }
    }

    if (got != NULL)
        *got = (int) (out - start);
    return 0;
}

int
vdisc_image_read_mode2(struct vdisc_image *img, int lba, int n, void *buf)
{
    unsigned char frame[VDISC_MAX_STRIDE];
    unsigned char *out = (unsigned char *) buf;
    int i;

    for (i = 0; i < n; i++, out += VDISC_MODE2_SECTOR) {
        int file, rel, where, stride;

        where = vdisc_image_locate(img, lba + i, &file, &rel);
        if (where < 0)
            return -1;
        if (where == 0) {
            memset(out, 0, VDISC_MODE2_SECTOR); /* virtual: zeros */
            continue;
        }
        stride = read_frame(img, file, rel, frame);
        if (stride == VDISC_RAW_SECTOR || stride == VDISC_CDG_SECTOR) {
            /* Skip sync (12) + header (4); take the next 2336. */
            memcpy(out, frame + VDISC_XA_SUBHDR_OFF, VDISC_MODE2_SECTOR);
        } else if (stride == VDISC_MODE2_SECTOR) {
            memcpy(out, frame, VDISC_MODE2_SECTOR);
        } else {
            /* A 2048-byte cooked file has no subheader to return. */
            return -1;
        }
    }
    return 0;
}

int
vdisc_image_read_audio(struct vdisc_image *img, int lba, int n, void *buf)
{
    unsigned char frame[VDISC_MAX_STRIDE];
    unsigned char *out = (unsigned char *) buf;
    int i;

    for (i = 0; i < n; i++, out += VDISC_RAW_SECTOR) {
        int file, rel, where, stride;

        where = vdisc_image_locate(img, lba + i, &file, &rel);
        if (where < 0)
            return -1;
        if (where == 0) {
            memset(out, 0, VDISC_RAW_SECTOR);       /* virtual: silence */
            continue;
        }
        stride = read_frame(img, file, rel, frame);
        if (stride < VDISC_RAW_SECTOR)
            return -1;      /* a 2048/2336 data file holds no CDDA */
        /* 2352 is the frame; a 2448-byte CDG frame carries 96 bytes of
         * subchannel after it, which the audio path does not want. */
        memcpy(out, frame, VDISC_RAW_SECTOR);
    }
    return 0;
}

/*
 * READ THE RAW SUBCHANNEL - 96 bytes per sector, P through W,
 * interleaved exactly as the disc carries them.
 *
 * design/34 has the scope. In short: the bytes are already under the
 * read head for a 2448-byte rip and were being discarded
 * (image.h:155, "the subchannel is ignored here"), and BOTH CD+G and
 * CD-TEXT need them - they share cdtext_rw_unpack() and differ only
 * in what the unpacked 72 bytes mean.
 *
 * RAW, NOT UNPACKED, AND THAT IS DELIBERATE. The unpack lives in
 * userspace where it is tested; doing it here too would be two copies
 * to keep in step. And the Q channel is wanted in its own right -
 * design/27 section 4a records it as the authoritative source for the
 * control nibble and for CDROMSUBCHNL, both of which we approximate
 * today.
 *
 * -ENODATA RATHER THAN ZEROS when a track has no subchannel. A caller
 * must be able to tell "this disc carries none" from "this disc
 * carries silence", and QUAKE106.sub is exactly why: design/27
 * section 4a measured it as a COMPLETE capture whose R-W is all
 * zeros. Zeros are a real answer on a disc that has the space and no
 * content; a 2352-byte stride has no space at all.
 */
int
vdisc_image_read_sub(struct vdisc_image *img, int lba, int n, void *buf)
{
    unsigned char frame[VDISC_MAX_STRIDE];
    unsigned char *out = (unsigned char *) buf;
    int i;

    if (img == NULL || buf == NULL || n < 0)
        return -1;

    /*
     * THE SIDECAR WINS WHERE THERE IS ONE - 2026-09-28.
     *
     * It is indexed by LBA at 96 bytes a sector with no framing, so
     * the whole run is ONE seek and ONE read rather than a frame at
     * a time. That also makes it the fast path, which matters: the
     * CD+G viewer walks forward a sector at a time and a 30-second
     * seek replays 2250 of them.
     *
     * PAST THE END IS ZEROS, NOT AN ERROR. A sidecar is sized to the
     * lead-out; a read beyond it is a sector that exists with no
     * subchannel recorded, which is the same answer a virtual sector
     * gets below.
     */
    if (img->sub_fp != NULL) {
        if (lba < 0)
            return -1;
        if (fseek(img->sub_fp, (long) lba * VDISC_SUB_SIZE, SEEK_SET) != 0)
            return -1;
        for (i = 0; i < n; i++, out += VDISC_SUB_SIZE) {
            if (lba + i >= img->sub_sectors
                || fread(out, 1, VDISC_SUB_SIZE, img->sub_fp)
                       != VDISC_SUB_SIZE) {
                memset(out, 0, VDISC_SUB_SIZE);
            }
        }
        return 0;
    }

    /*
     * THE DE-INTERLEAVE WINDOW, CARRIED ACROSS THE LOOP - 2026-09-28.
     *
     * EACH FRAME USED TO BE READ THREE TIMES: once as its own
     * sector, then again as history for each of the two that follow.
     * Walking a track of 175,000 sectors therefore did 525,000
     * `fseek'+`fread' pairs of 2448 bytes each - 1.3 GB through the
     * file layer to produce 17 MB of subchannel.
     *
     * THE COMMENT BELOW PREDICTED IT and assumed the file cache
     * would absorb it. On a 300 MHz emulated Pentium II it does not:
     * the user measured OVER A HUNDRED SECONDS to start one track.
     *
     * THE RUN IS SEQUENTIAL, so sector i's history frames are the
     * ones just read for i-1 and i-2. Shifting them down costs a
     * memcpy and saves two reads, which is exactly 3x.
     *
     * `win_lba' IS WHAT MAKES IT SAFE. The window is only valid if
     * it really holds the preceding sectors, so it records which
     * sector it was last filled for; anything else refills from
     * scratch. Without that a caller passing a non-contiguous run
     * would silently decode against the wrong history - noise that
     * looks like a bad rip.
     */
    {
        unsigned char win[3][VDISC_SUB_SIZE];
        int           win_got[3];
        int           win_lba = -1;     /* which sector win[0] is    */

        memset(win_got, 0, sizeof win_got);

    for (i = 0; i < n; i++, out += VDISC_SUB_SIZE) {
        int file, rel, where, stride;

        where = vdisc_image_locate(img, lba + i, &file, &rel);
        if (where < 0)
            return -1;
        if (where == 0) {
            /* A VIRTUAL SECTOR - a pregap or a gap between files.
             * There is no medium there, so there is no subchannel
             * either; zeros are the honest answer for a sector that
             * exists and holds nothing. */
            memset(out, 0, VDISC_SUB_SIZE);
            continue;
        }

        stride = read_frame(img, file, rel, frame);
        if (stride < VDISC_RAW_SECTOR + VDISC_SUB_SIZE)
            return -2;      /* no subchannel in this file at all */

        /*
         * DE-INTERLEAVE ACROSS SECTORS - see sub_deint[] above.
         *
         * THIS SECTOR'S CD+G IS NOT IN THIS SECTOR. Each output
         * symbol comes from a slot of the sector 0, 1 or 2 EARLIER,
         * so the two preceding frames have to be read as well. The
         * old code copied the 96 bytes straight out, which is what
         * made every karaoke disc in this form decode as noise.
         *
         * THREE READS PER SECTOR IS THE COST, and it is why the
         * SIDECAR path above is preferred where one exists: that
         * one is a single seek for the whole run. Here the reads
         * are of adjacent sectors, so the file cache absorbs most
         * of it.
         *
         * A SECTOR WITH NO PREDECESSOR - the first one or two of a
         * file - cannot be de-interleaved and yields zeros. That is
         * correct rather than an error: a disc's first sectors are
         * lead-in and carry no graphics, and CDG_SNIFF/the viewer
         * both treat an all-zero span as "nothing here yet".
         */
        {
            unsigned char (*hist)[VDISC_SUB_SIZE] = win;
            int *got = win_got;
            int k, dd;

            if (win_lba == lba + i - 1) {
                /*
                 * CONTIGUOUS - SHIFT, DO NOT RE-READ. win[0] becomes
                 * win[1] and so on, and only THIS sector's frame is
                 * read. The flags shift with the data: without that,
                 * a sector after a gap would inherit a stale "we
                 * have it" and de-interleave against the wrong
                 * frame.
                 */
                memcpy(win[2], win[1], VDISC_SUB_SIZE);
                memcpy(win[1], win[0], VDISC_SUB_SIZE);
                win_got[2] = win_got[1];
                win_got[1] = win_got[0];
                win_got[0] = 0;

                {
                    unsigned char fr2[VDISC_MAX_STRIDE];
                    int s2 = read_frame(img, file, rel, fr2);

                    if (s2 >= VDISC_RAW_SECTOR + VDISC_SUB_SIZE) {
                        memcpy(win[0], fr2 + VDISC_RAW_SECTOR,
                               VDISC_SUB_SIZE);
                        win_got[0] = 1;
                    }
                }
            } else {
                /* NOT CONTIGUOUS - the first sector of a call, or a
                 * caller that jumped. Fill all three the long way. */
                for (dd = 0; dd < 3; dd++) {
                    int f2, r2, w2, s2;
                    unsigned char fr2[VDISC_MAX_STRIDE];

                    win_got[dd] = 0;
                    if (lba + i - dd < 0)
                        continue;
                    w2 = vdisc_image_locate(img, lba + i - dd, &f2, &r2);
                    if (w2 <= 0)
                        continue;       /* virtual or out of range */
                    s2 = read_frame(img, f2, r2, fr2);
                    if (s2 < VDISC_RAW_SECTOR + VDISC_SUB_SIZE)
                        continue;
                    memcpy(win[dd], fr2 + VDISC_RAW_SECTOR, VDISC_SUB_SIZE);
                    win_got[dd] = 1;
                }
            }
            win_lba = lba + i;

            for (k = 0; k < VDISC_SUB_SIZE; k++) {
                int d = -sub_deint[k].d;        /* 0, 1 or 2 back */

                /*
                 * MASKED TO SIX BITS. P (bit 7) and Q (bit 6) are
                 * the timing and position channels and are not part
                 * of CD+G; a `.cdg' file has them cleared, so a
                 * comparison against one fails on exactly those two
                 * bits. Caught by diffing this output against the
                 * same disc's `.cdg': ten of twelve bytes matched
                 * and the two that did not differed only by bit 6.
                 */
                out[k] = (d >= 0 && d < 3 && got[d])
                       ? (unsigned char)(hist[d][sub_deint[k].j] & 0x3f)
                       : 0;
            }
        }
    }
    }           /* the de-interleave window */
    return 0;
}
