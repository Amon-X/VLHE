/*
 * vlhe_filter.h - a "Files of type" dropdown for GtkFileSelection.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * GTK 1.2 HAS NO FILTER API. The widget exposes seven functions
 * (gtkfilesel.h) and not one of them filters: the closest,
 * `gtk_file_selection_complete()', SETS THE ENTRY TEXT to a pattern
 * and repopulates once. So the glob shows up in the filename box,
 * and the moment the user walks into another directory it is gone -
 * which is exactly what the user hit on 2026-09-22 and reported as
 * "as soon as you walk a directory the filter vanishes".
 *
 * SO WE BUILD OUR OWN, and design/32 measured how a year earlier:
 *
 *   - `GtkFileSelection' exposes `main_vbox' as a PUBLIC struct
 *     member, so a GtkOptionMenu packs into the dialog's own layout
 *     with no subclassing and looks native.
 *   - ONE PATTERN ONLY. Measured against a seven-file directory:
 *     `*.[ic]*' and `*.?so' work, while `{iso,cue,ccd}' and every
 *     space-, semicolon-, comma-, pipe- and colon-separated form
 *     returns ZERO ROWS. It is fnmatch syntax; the `;' convention
 *     is Qt's and GTK 2's GtkFileFilter, both later.
 *   - `All files' IS THE DEFAULT, and the reason is the failure mode
 *     rather than convention: any filter can hide the file the user
 *     came for, and when it does the list is empty with nothing
 *     saying why, which reads as a broken picker. gcdemu puts `All
 *     files' first for the same reason (ui.py:52-55).
 *
 * ONE EXTENSION PER ENTRY IS ENOUGH IN PRACTICE - the user,
 * 2026-09-22: "It worked for single extentions only which is fine
 * you can just filter mid* since that will pick mid or midi files".
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_FILTER_H
#define VLHE_FILTER_H

#include <gtk/gtk.h>

/* One entry: what the menu says, and the glob it applies. A NULL
 * `pattern' means no filtering - the `All files' row. */
struct vlhe_filter {
    const char *label;          /* "MIDI files (*.mid)"              */
    const char *pattern;        /* "*.mid*", or NULL for everything  */
};

/*
 * ADD A "Files of type" ROW to an open GtkFileSelection, packed into
 * its own vbox above the buttons.
 *
 * `items' is a list terminated by an entry with a NULL label.
 * `deflt' is the index to start on - pass 0 for `All files' when
 * that is first, which design/32 says it should be.
 *
 * THE FILTER SURVIVES NAVIGATION, which is the whole point: it hooks
 * the dialog's own directory list, so walking into a directory
 * re-applies the pattern instead of losing it.
 */
void vlhe_filter_attach(GtkWidget *filesel,
                        const struct vlhe_filter *items,
                        int deflt);

/*
 * A LONG PATH MUST NOT WIDEN THE DIALOG - design/54 D13 (design/32 s12),
 * 2026-10-04. TWO widgets in a GtkFileSelection ask for a path's whole
 * width, unwrapped, and GTK rewrites both on every directory change
 * (gtkfilesel.c): the "Selection: <path>" label and the directory
 * history menu at the top. A deep path stretched the dialog to 1049 px
 * - off an 800x600 screen. MEASURED 2026-10-04 against the target's GTK
 * 1.2: fitting either one alone leaves it over 1500 px for a long path;
 * fitting both brings it to the lists' own width (441 px here). So both
 * get a fixed request; the label still draws across its row (the edge
 * clips the rest), the menu shows the start of the path and its popup
 * still lists them whole, and the full path is always in the entry box.
 * Call it on every GtkFileSelection, after gtk_file_selection_new().
 */
void vlhe_filesel_fit(GtkWidget *filesel);

#endif /* VLHE_FILTER_H */
