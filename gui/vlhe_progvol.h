/*
 * vlhe_progvol.h - the per-program level table's file format.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * design/33 section 1c, built 2026-10-02. vlhe_backend.c saves
 * vsound's table (VSOUND_IOC_PROGGET) to /var/lib/vlhe/volumes and
 * pushes it back (VSOUND_IOC_PROGSET); these two convert between the
 * ioctl's list and the file's [Program Levels] section, and are apart
 * from the device so the host test can check the format with no
 * vsound loaded. The public calls are in vlhe_backend.h.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_PROGVOL_H
#define VLHE_PROGVOL_H

#include "vsound.h"
#include "vlhe_conf.h"

/* Returns how many entries were written into / read out of. A name
 * the file cannot hold as a key, a value that is not `N' or
 * `N muted', or a level above the boost ceiling is skipped. */
int vlhe_progvol_to_conf(const struct vsound_proglist *pl,
                         struct vlhe_conf *c);
int vlhe_progvol_from_conf(const struct vlhe_conf *c,
                           struct vsound_proglist *pl);

#endif
