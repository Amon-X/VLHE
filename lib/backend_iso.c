/*
 * backend_iso.c - plain ISO-9660 image backend.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * The trivial case: a single cooked data track of 2048-byte sectors, no
 * audio, no description file. A synthetic one-track TOC is built so the
 * kernel side sees the same shape as any other image.
 *
 * C89 / GCC 2.95 clean.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "image.h"

static int
ends_with_ci(const char *s, const char *suffix)
{
    int ls = (int) strlen(s);
    int lx = (int) strlen(suffix);
    int i;

    if (lx > ls)
        return 0;
    for (i = 0; i < lx; i++) {
        if (tolower((unsigned char) s[ls - lx + i]) !=
            tolower((unsigned char) suffix[i]))
            return 0;
    }
    return 1;
}

static int
iso_probe(const char *path)
{
    return ends_with_ci(path, ".iso");
}

static int
iso_open(struct vdisc_image *img, const char *path)
{
    unsigned char pvd[6];
    struct vdisc_file *f;
    int k;

    k = vdisc_image_add_file(img, path);
    if (k < 0)
        return -1;
    f = &img->file[k];
    img->desc[0] = '\0';

    vdisc_image_set_stride(img, k, VDISC_SECTOR_SIZE);

    /* Sanity: "CD001" should sit at sector 16, offset 1. */
    if (fseek(f->fp, 16L * VDISC_SECTOR_SIZE + 1, SEEK_SET) == 0 &&
        fread(pvd, 1, 5, f->fp) == 5) {
        pvd[5] = '\0';
        if (memcmp(pvd, "CD001", 5) != 0) {
            fprintf(stderr, "vdiscd: warning: %s has no ISO-9660 signature at "
                            "sector 16 - is it really a cooked .iso?\n", path);
        }
    }

    img->n_tracks    = 1;
    img->first_track = 1;
    img->last_track  = 1;
    img->leadout_lba = f->n_sectors;

    memset(&img->track[0], 0, sizeof img->track[0]);
    img->track[0].num         = 1;
    img->track[0].start_lba   = 0;
    img->track[0].length      = img->leadout_lba;
    img->track[0].is_data     = 1;
    img->track[0].sector_form = VDISC_FORM_ISO_2048;
    img->track[0].session     = 1;
    img->n_sessions           = 1;

    return vdisc_image_add_extent(img, k, 0, 0, f->n_sectors);
}

static int
iso_read_data(struct vdisc_image *img, int lba, int n, void *buf)
{
    return vdisc_image_read_data(img, lba, n, buf);
}

static int
iso_read_audio(struct vdisc_image *img, int lba, int n, void *buf)
{
    (void) img; (void) lba; (void) n; (void) buf;
    return -1;      /* no audio tracks on a plain ISO */
}

static void
iso_close(struct vdisc_image *img)
{
    (void) img;
}

const struct vdisc_backend vdisc_backend_iso = {
    "iso",
    iso_probe,
    iso_open,
    iso_read_data,
    iso_read_audio,
    iso_close
};
