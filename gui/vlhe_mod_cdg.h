/*
 * vlhe_mod_cdg.h - the CD+G viewer, a MOD_VIEW page.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * A VIEW, NOT A FORM. design/32 section 6: MOD_VIEW drops the
 * notebook tabs and the Help/OK/Apply/Cancel row, which is what makes
 * a 300x216 CD+G frame at 2x fit the measured 622x522 pane. There is
 * nothing here to apply - the transport acts immediately, the way a
 * CD player's buttons do.
 *
 * So it has no collect/reload/dirty. Build it and it runs.
 */

#ifndef VLHE_MOD_CDG_H
#define VLHE_MOD_CDG_H

#include <gtk/gtk.h>

GtkWidget *cdg_build(void (*report_fn)(const char *));

/* The shell calls this when the page is shown or hidden, so a viewer
 * that is not on screen does no work. A 2x blit of 600x432 on a
 * Pentium II is not free. */
void cdg_set_active(int on);

/*
 * RE-APPLY VISIBILITY AFTER THE SHELL'S show_all.
 *
 * gtk_widget_show_all() is recursive and undoes any hide a module
 * made during its build (vlhe_cc.c:2350). Same contract as
 * volume_sync_visibility() and midi_sync_visibility().
 */
void cdg_sync_visibility(void);

/*
 * THE VIEWER'S DRIVE MAY HAVE CHANGED - called by the CD page when its
 * "CD+G viewer" radio or the follow option changes (2026-10-03). The
 * viewer's own poll runs only while it is shown, so without this a
 * swap made on the CD page would stop the old disc only on returning.
 * Works out the drive the viewer will now use and, with StopOnSwap,
 * stops the one the viewer was playing. Touches no widget.
 */
void cdg_drive_chosen(void);

/* THE MAIN WINDOW'S PANE (vlhe_cc.c's g_scroll), so the D60 tracing can
 * say when its scrollbars come and go while this page settles. Only the
 * trace reads it. */
void cdg_set_pane_scroll(GtkWidget *sw);

#endif /* VLHE_MOD_CDG_H */
