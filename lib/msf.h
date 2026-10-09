/*
 * msf.h - MSF (minute:second:frame) <-> LBA conversions.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * There are two distinct conventions in play, and conflating them is the
 * classic BIN/CUE emulator bug (every audio track lands 150 sectors late):
 *
 *   FILE-RELATIVE SECTOR INDEX
 *       Used by CUE INDEX statements for the images we care about, and by
 *       CCD "PLBA" values. Sector 0 is the first sector in the file.
 *       No +/-150 involved.
 *
 *   ABSOLUTE DISC MSF
 *       What the CD-ROM API reports through CDROMREADTOCENTRY in
 *       CDROM_MSF format, and what a real drive shows. Sector 0 of the
 *       disc is MSF 00:02:00, i.e. 150 frames in, because of the
 *       2-second lead-in.
 *
 * So: vdisc_lba_to_msf() ADDS 150 and vdisc_msf_to_lba() SUBTRACTS it, while
 * vdisc_cue_msf_to_sector() does NOT - it is a plain base-75 decode.
 *
 * C89 / GCC 2.95 clean.
 */

#ifndef _VDISC_MSF_H
#define _VDISC_MSF_H

#include "vdisc.h"

struct vdisc_msf {
    int minute;
    int second;
    int frame;
};

/* Absolute disc MSF <-> LBA. These apply the 150-frame lead-in offset. */
void vdisc_lba_to_msf(int lba, struct vdisc_msf *msf);
int  vdisc_msf_to_lba(const struct vdisc_msf *msf);

/* CUE/CCD file-relative position: plain base-75 decode, NO 150 offset. */
int  vdisc_cue_msf_to_sector(int m, int s, int f);

/* Sector count -> MSF duration (also no offset; a length is not an address) */
void vdisc_len_to_msf(int sectors, struct vdisc_msf *msf);

/* Format helpers. buf must hold at least 9 bytes ("mm:ss:ff\0"). */
void vdisc_msf_format(const struct vdisc_msf *msf, char *buf);

/* Validity check: frame < 75, second < 60, all non-negative. */
int  vdisc_msf_valid(const struct vdisc_msf *msf);

#endif /* _VDISC_MSF_H */
