/*
 * vlhe_mod_cd.h - the CD Settings module's interface to the shell.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Same three-function shape as vlhe_mod_volume.h: build it, tell it
 * which page wants the button row, give it a way to report a failure.
 */

#ifndef VLHE_MOD_CD_H
#define VLHE_MOD_CD_H

#include <gtk/gtk.h>

GtkWidget *cd_build(void (*report_fn)(const char *),
                    void (*page_fn)(int));

/* Does the page currently showing want OK/Apply/Cancel? Drive is a
 * live page - attach, detach and lock act immediately, the way
 * inserting a disc does - so it wants none. Audio is a form. */
int  cd_collect(void);          /* read the widgets into the backend */
void cd_reload(void);           /* put them back - Cancel's half     */
int  cd_dirty(void);            /* unsaved edits on this page        */
void cd_machine(void);          /* modules changed - repaint what says so */
void cd_privilege_changed(void); /* Modify pressed - regrey, no reload */
void cd_set_dirty_cb(void (*cb)(void));
int  cd_page_wants_buttons(void);

/* Open on a given tab, for captures. See -T in vlhe_cc.c. */
void cd_set_page(int page);

/* ON SCREEN OR NOT. The Drive page polls the daemon and opens device
 * nodes; doing that for a page nobody is looking at is waste. The
 * shell calls this from show_module(), and the page refreshes at once
 * when shown rather than waiting for its next tick. */
void cd_set_active(int on);

#endif /* VLHE_MOD_CD_H */
