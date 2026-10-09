/*
 * vlhe_buttons.h - one size for every button in a dialog's row, and
 * the same size from one dialog to the next.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * GTK 1.2 sizes a button to its label, so a row of three labels comes
 * out three widths, and the one that holds the default draws a ring
 * round itself that the others lack. vlhe_buttons_equalise() makes a
 * row match: every button the width of the widest label plus a
 * margin, the height of the tallest, no default ring - the default
 * becomes the focus, which Return still reaches. vlhe_buttons.c has
 * the measurements and the user's words behind each step.
 *
 * Shared by the shell (vlhe_cc.c) and any module that owns a dialog
 * of its own (the CD+G viewer's Options).
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_BUTTONS_H
#define VLHE_BUTTONS_H

#include <gtk/gtk.h>

/* Call after the row's buttons are packed and shown. `box' is a
 * GtkDialog's action_area or any GtkBox holding only buttons. */
void vlhe_buttons_equalise(GtkWidget *box);

#endif
