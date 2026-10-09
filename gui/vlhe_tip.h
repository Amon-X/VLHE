/*
 * vlhe_tip.h - set a tooltip from a vlhe_strings.h `_TIP' slot.
 * vlhe_tip.c says how a blank one is nothing and a label is left.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 */
#ifndef VLHE_TIP_H
#define VLHE_TIP_H

#include <gtk/gtk.h>

/* Sets `tip' on `w' unless the tip is empty. */
void       vlhe_tip(GtkWidget *w, const char *tip);

/* The same, returning `w' - for wrapping a constructor in place:
 *     vlhe_tipped(gtk_button_new_with_label(STR_X), STR_X_TIP)      */
GtkWidget *vlhe_tipped(GtkWidget *w, const char *tip);

#endif
