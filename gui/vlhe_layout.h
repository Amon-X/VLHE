/*
 * vlhe_layout.h - widths the control centre works out instead of
 * hardcoding: the paragraphs' wrap width and the label columns.
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 * Part of VLHE. See LICENSE for the full license text.
 *
 * A module REGISTERS a widget as it builds it; vlhe_layout_finish()
 * sets every registered width once, after the shell has built all
 * the pages and before the window is shown. vlhe_layout.c has the
 * arithmetic and why each part is measured rather than written down.
 *
 * C89, GCC 2.95.2.
 */

#ifndef VLHE_LAYOUT_H
#define VLHE_LAYOUT_H

#include <gtk/gtk.h>

/* A paragraph for a page: a left-aligned label that wraps at the
 * pane's text width. Returns it, not yet packed or shown. */
GtkWidget *vlhe_layout_note(const char *text);

/* Register a label the caller made: it is set to wrap, and gets the
 * same width as a note. For a label whose text changes (a status
 * line, a path) - an unwrapped one makes the whole page that wide. */
void vlhe_layout_wrap(GtkWidget *label);

/* A short hint beside a control ("1 - 2048", "Hz and up"): it keeps
 * its own width when the row has room, and wraps only when it does not
 * - so the control beside it is not squeezed. Static text only: the
 * width is measured once, at vlhe_layout_finish(). */
void vlhe_layout_hint(GtkWidget *label);

/* A label in a page's label column (the "Name:" before a control).
 * Every label registered with the same `column' gets the width of the
 * widest of them, so the controls line up and nothing is clipped. */
void vlhe_layout_column(const char *column, GtkWidget *label);

/* A spin button wide enough for `digits' characters of its own font
 * and no wider - GTK 1.2 gives every spin button one fixed width
 * whatever the font, and a stretched one is mostly empty. The caller
 * packs it without expanding. */
void vlhe_layout_digits(GtkWidget *spin, int digits);

/* Once, after every page is built. `scroll' is the GtkScrolledWindow
 * the pages sit in; `pane_w' the width it is given (the window less
 * the sidebar). */
void vlhe_layout_finish(GtkWidget *scroll, int pane_w);

#endif
