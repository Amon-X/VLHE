/*
 * msf.c - MSF <-> LBA conversions. See msf.h for the two conventions.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * C89 / GCC 2.95 clean: no // comments, no declarations after statements.
 */

#include <stdio.h>
#include "msf.h"

/*
 * Absolute disc address: LBA 0 is MSF 00:02:00 because of the 2-second
 * lead-in, so we add VDISC_MSF_OFFSET (150) frames on the way out.
 */
void
vdisc_lba_to_msf(int lba, struct vdisc_msf *msf)
{
    int total;

    total = lba + VDISC_MSF_OFFSET;
    if (total < 0)
        total = 0;

    msf->minute = total / (60 * VDISC_FRAMES_PER_SEC);
    msf->second = (total / VDISC_FRAMES_PER_SEC) % 60;
    msf->frame  = total % VDISC_FRAMES_PER_SEC;
}

int
vdisc_msf_to_lba(const struct vdisc_msf *msf)
{
    int total;

    total = (msf->minute * 60 + msf->second) * VDISC_FRAMES_PER_SEC
          + msf->frame;

    return total - VDISC_MSF_OFFSET;
}

/*
 * CUE INDEX / CCD PLBA position within the image file. This is a plain
 * base-75 decode with NO lead-in offset: "INDEX 01 00:00:00" on track 1
 * means byte 0 of the file.
 *
 * Verified against QUAKE106: read this way, all 11 track boundaries fall
 * inside the image, increase monotonically, and track 11 ends exactly at
 * EOF. The CCD's raw PLBA values agree with it exactly.
 */
int
vdisc_cue_msf_to_sector(int m, int s, int f)
{
    return (m * 60 + s) * VDISC_FRAMES_PER_SEC + f;
}

/*
 * A duration, not an address - no offset applies.
 */
void
vdisc_len_to_msf(int sectors, struct vdisc_msf *msf)
{
    if (sectors < 0)
        sectors = 0;

    msf->minute = sectors / (60 * VDISC_FRAMES_PER_SEC);
    msf->second = (sectors / VDISC_FRAMES_PER_SEC) % 60;
    msf->frame  = sectors % VDISC_FRAMES_PER_SEC;
}

void
vdisc_msf_format(const struct vdisc_msf *msf, char *buf)
{
    sprintf(buf, "%02d:%02d:%02d", msf->minute, msf->second, msf->frame);
}

int
vdisc_msf_valid(const struct vdisc_msf *msf)
{
    if (msf->minute < 0 || msf->second < 0 || msf->frame < 0)
        return 0;
    if (msf->second >= 60)
        return 0;
    if (msf->frame >= VDISC_FRAMES_PER_SEC)
        return 0;
    return 1;
}
