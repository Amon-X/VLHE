/*
 * vlhe_mod_volume.h - the Volume module's interface to the shell.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * DELIBERATELY THREE FUNCTIONS. The shell builds the module, tells it
 * which slider orientation to draw, and gives it a way to report a
 * failure into the status bar. Everything else - polling, the driver,
 * the widgets - is the module's own business.
 */

#ifndef VLHE_MOD_VOLUME_H
#define VLHE_MOD_VOLUME_H

#include <gtk/gtk.h>

/* Build the module's notebook. `report_fn' is called with a one-line
 * message when something the user did could not be done; it may be
 * NULL. */
/* `page_fn' is called when the user changes tab, with 1 if that page
 * wants the shell's OK/Apply/Cancel row and 0 if it does not. Levels
 * applies every change immediately and wants none; Options writes a
 * config file and does. May be NULL. */
GtkWidget *volume_build(void (*report_fn)(const char *),
                        void (*page_fn)(int));

/* Does the page currently showing want the button row? The shell asks
 * this when Volume is selected, since the answer depends on the tab
 * rather than on the module. */
int  volume_collect(void);      /* read the Options widgets          */
void volume_reload(void);       /* put them back - Cancel's half     */
int  volume_dirty(void);        /* unsaved Options edits             */
void volume_set_dirty_cb(void (*cb)(void));
int volume_page_wants_buttons(void);

/* Open on a given tab - 0 Levels, 1 Options. For captures, so a
 * screenshot of either needs no mouse. See -T in vlhe_cc.c. */
void volume_set_page(int page);

/* Swap every channel's slider between horizontal and vertical. Both
 * exist for every channel and share one GtkAdjustment, so this shows
 * one and hides the other - no value is copied and nothing is rebuilt. */
void volume_set_vertical(int vertical);
int  volume_get_vertical(void);

/* RE-APPLY EVERY VISIBILITY DECISION THE MODULE HAS MADE.
 *
 * The shell calls this AFTER gtk_widget_show_all(), which is recursive
 * and re-shows widgets the module deliberately hid: the unused slider
 * orientation, rows past the channel count, and the "module not
 * loaded" notice. Without it the panel draws all of them at once. */
void volume_sync_visibility(void);

/* The shell says whether this page is the one showing: the 250 ms poll
 * runs only while it is (design/47 V2). A show refreshes at once. */
void volume_set_active(int active);
/* A plan ran - re-read the machine (the shell's machine hook). */
void volume_machine(void);

#endif /* VLHE_MOD_VOLUME_H */
