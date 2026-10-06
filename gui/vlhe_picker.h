/*
 * vlhe_picker.h - a drop-down that can hold 99 items.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * WHY THIS EXISTS AND GtkOptionMenu DOES NOT DO. Red Book allows 99
 * audio tracks and real discs use them - Nine Inch Nails' "Broken"
 * (1992) has six tracks, 91 one-second silences, then 98 and 99 as
 * the bonus tracks. A GtkOptionMenu holding 99 items asks for 2360px
 * of menu, and GTK 1.2 MENUS DO NOT SCROLL: gtkmenu.c has no scroll
 * code at all, it clamps the popup to y=0 and the overflow is simply
 * unreachable. On the target's 600px screen that is about 25 tracks
 * of 99 - and the two a user of that disc actually wants are the
 * furthest out of reach.
 *
 * AND GtkCombo CANNOT BE BENT INTO SHAPE. Four attempts, measured:
 * gtk_combo_popup_list() recomputes the popup height from the space
 * left on screen EVERY time it opens and forces it onto
 * combo->popwin with set_usize plus gdk_window_resize, so nothing
 * set on the children survives. Capping the list does nothing (it is
 * inside a viewport); capping the scrolled window scrolls but leaves
 * the window running off the screen; gtk_window_set_policy does not
 * refuse the resize.
 *
 * SO THIS OWNS ITS POPUP WINDOW, which is the whole idea: a
 * GtkCList in a scrolled window in a GTK_WINDOW_POPUP we create, so
 * we decide its height. It behaves like an option menu - the face is
 * a button, so a click anywhere on it opens the list - which is the
 * property the user asked for and GtkCombo lacks without help.
 */
#ifndef VLHE_PICKER_H
#define VLHE_PICKER_H

#include <gtk/gtk.h>

/* row is 0-based; text is the row's label, owned by the picker. */
typedef void (*VlhePickerChanged)(gint row, const gchar *text,
                                  gpointer data);

/*
 * Returns the FACE widget - pack it like any other. `max_height' is
 * the popup's cap in pixels; the list scrolls inside it.
 */
GtkWidget *vlhe_picker_new(gchar **items, gint n, gint max_height,
                           VlhePickerChanged cb, gpointer cb_data);

/* Replace the contents. Selection resets to row 0 and does NOT
 * notify - a reload is not a user's choice. */
void vlhe_picker_set_items(GtkWidget *face, gchar **items, gint n);

/* Move the selection without notifying, for following the transport
 * rather than driving it. */
void vlhe_picker_select(GtkWidget *face, gint row);

/* -1 when nothing is selected. */
gint vlhe_picker_selected(GtkWidget *face);

#endif /* VLHE_PICKER_H */
