/*
 * vlhe_mod_advanced.h - the Advanced Settings module's interface.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Same shape as the other settings modules. Both its tabs are forms -
 * every control reaches the SYSTEM config - so both want the commit
 * trio.
 */

#ifndef VLHE_MOD_ADVANCED_H
#define VLHE_MOD_ADVANCED_H

#include <gtk/gtk.h>

GtkWidget *advanced_build(void (*report_fn)(const char *),
                          void (*page_fn)(int));

int  advanced_collect(void);        /* read the widgets into the backend */
void advanced_reload(void);         /* put them back - Cancel's half     */
int  advanced_dirty(void);          /* bitmask of tabs with unsaved edits */
void advanced_privilege_changed(void); /* Modify pressed - the note only */
void advanced_set_dirty_cb(void (*cb)(void));
int  advanced_page_wants_buttons(void);
void advanced_set_page(int page);

#endif /* VLHE_MOD_ADVANCED_H */
