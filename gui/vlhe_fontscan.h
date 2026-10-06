/*
 * vlhe_fontscan.h - finding .sf2 files in the search directories.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * Split out of the backend because it is pure directory reading with
 * no device and no ioctl, so it can be tested here - the same reason
 * vlhe_conf.c is its own file.
 *
 * THE SEARCH PATH AND ITS ORDER are vlhe_backend.h's, in the block
 * above vlhe_font_dirs(): three defaults led by awesfx's own
 * /usr/share/sounds/sf2, user-added directories FIRST so a
 * deliberately placed font wins, and deduplication by BASENAME with
 * the earliest path winning.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_FONTSCAN_H
#define VLHE_FONTSCAN_H

#include "vlhe_backend.h"       /* VLHE_PATH_MAX, VLHE_MAX_* */

/*
 * HOW MANY BUILT-IN DIRECTORIES THERE ARE.
 *
 * NAMED 2026-09-22, when the MIDI page started showing the defaults
 * separately from the user's own and needed to know where one list
 * ends and the other begins. The count was spelt `3' in four places
 * by then; a fifth, in the GUI, would have been the one that drifted
 * - it is the only one that does not live beside the array.
 */
#define VLHE_N_FONTDIR_DEFAULTS 3

/* The three built-in directories, in search order after any the user
 * added. Exposed so the backend can present them as removable-or-not
 * and so the test can assert the list rather than restate it. */
extern const char *const vlhe_fontscan_defaults[VLHE_N_FONTDIR_DEFAULTS];

/* Scan `dirs' in order, writing full paths of every .sf2 found.
 *
 * DEDUPLICATED BY BASENAME, EARLIEST WINS - so a user's own copy of
 * FluidR3_GM.sf2 hides the system one rather than the list showing
 * the same font twice with different paths.
 *
 * A directory that does not exist is skipped silently: all three
 * defaults are empty on a fresh machine, because nothing on Corel
 * installs a SoundFont, and that is the normal case rather than an
 * error.
 *
 * Returns how many were written, never negative. */
int vlhe_fontscan(const char dirs[][VLHE_PATH_MAX], int ndirs,
                  char out[][VLHE_PATH_MAX], int max);

/* Is this name a SoundFont? Case-insensitive on the extension,
 * because a file copied from a Windows machine may be .SF2 - and the
 * corpus in sf2/ came from exactly there. */
int vlhe_fontscan_is_sf2(const char *name);

#endif /* VLHE_FONTSCAN_H */
