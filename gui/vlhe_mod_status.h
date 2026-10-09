/*
 * vlhe_mod_status.h - the Status page.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * NO TABS AND NO PAGE CALLBACK, unlike the other modules: it is one
 * page, and it is a VIEW with actions rather than a form - nothing on
 * it is committed, so it wants Help alone.
 */

#ifndef VLHE_MOD_STATUS_H
#define VLHE_MOD_STATUS_H

#include <gtk/gtk.h>

GtkWidget *status_build(void (*report_fn)(const char *));

/* Called by the shell AFTER gtk_widget_show_all(), which is recursive
 * and re-shows the Restart buttons this page hides for modules. */
void status_sync_visibility(void);

/*
 * TELL THE SHELL THE MACHINE CHANGED. Fired after Load or Unload has
 * run a plan - not on the poll, which only reads. The shell uses it
 * to have every page re-read what depends on loaded modules and
 * running daemons (design/36 rows 34, 44, 57, 58).
 */
void status_set_machine_cb(void (*cb)(void));

/*
 * ASK THE SHELL BEFORE A PLAN RUNS - design/36 2b item 4, the user:
 * "The load should check to see if dirtybit is saved and ask a user
 * if they want to save and then load." The Status page cannot see
 * the other pages' unsaved edits; the shell can. The callback returns
 * 1 to proceed and 0 to cancel; `unload' says which was pressed, so
 * the shell can word the question.
 */
void status_set_before_cb(int (*cb)(int unload));
/* The shell says whether this page is the one showing: the 2 s refresh
 * runs only while it is (design/47 T1), with a refresh on each show. */
void status_set_active(int active);

/*
 * AND THE ONE FOR A SINGLE DAEMON'S Restart - 2026-09-27.
 *
 * SEPARATE FROM `before_cb' BECAUSE THE QUESTION IS NARROWER. That
 * one asks "is ANY page unsaved" before a whole-machine plan; this
 * asks only about the pages the named daemon is actually started
 * from, which is the user's requirement: *"if we are resetting
 * vmidid we should only care if vmidid is dirty and not any cd
 * settings etc"*.
 *
 * The shell answers it, because only the shell knows which pages are
 * dirty; `vlhe_restart_wants_page()' says which ones count.
 *
 * Returns 0 to cancel the restart.
 */
void status_set_before_restart_cb(int (*cb)(const char *daemon));

#endif /* VLHE_MOD_STATUS_H */
