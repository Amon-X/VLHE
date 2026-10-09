/*
 * vlhe_picker.c - GtkOptionMenu-style picker backed by a scrolling
 * GtkCList, with the popup height capped at a size YOU choose.
 * GTK 1.2.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * Why not GtkCombo: gtk_combo_popup_list() recomputes the popup height
 * from the space left on screen every time it opens, then forces it
 * onto combo->popwin with set_usize + gdk_window_resize. Nothing you
 * set on the children survives that. Here we own the popup window, so
 * we own its size.
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <stdlib.h>

#include "vlhe_picker.h"

#define POPUP_EVENTS (GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | \
                      GDK_POINTER_MOTION_MASK)

typedef struct {
    GtkWidget    *button;      /* the "option menu" face        */
    GtkWidget    *label;
    GtkWidget    *popwin;      /* GTK_WINDOW_POPUP               */
    GtkWidget    *clist;
    gint          max_height;
    gint          selected;    /* current row, -1 = none         */
    gint          pending_row; /* row released on, for the idle  */
    guint         scroll_idle;
    guint         release_idle;
    VlhePickerChanged changed;
    gpointer      changed_data;
} Picker;

/* ------------------------------------------------------------------ */

static void
picker_popdown(Picker *p)
{
    if (!GTK_WIDGET_VISIBLE(p->popwin))
        return;
    gtk_grab_remove(p->popwin);
    gdk_pointer_ungrab(GDK_CURRENT_TIME);
    gdk_keyboard_ungrab(GDK_CURRENT_TIME);
    gtk_widget_hide(p->popwin);
}

static void
picker_set_row(Picker *p, gint row, gboolean notify)
{
    gchar *text = NULL;

    if (row < 0 || row >= GTK_CLIST(p->clist)->rows)
        return;
    gtk_clist_get_text(GTK_CLIST(p->clist), row, 0, &text);
    p->selected = row;
    gtk_label_set_text(GTK_LABEL(p->label), text ? text : "");
    if (notify && p->changed)
        p->changed(row, text, p->changed_data);
}

/* Runs after the popup has been sized/allocated, so moveto is valid. */
static gint
picker_scroll_idle(gpointer data)
{
    Picker *p = data;

    p->scroll_idle = 0;
    if (GTK_WIDGET_VISIBLE(p->popwin) && p->selected >= 0)
        gtk_clist_moveto(GTK_CLIST(p->clist), p->selected, -1, 0.5, 0.0);
    return FALSE;
}

/* Runs after GtkCList has finished its own button-release handling.
 * CList grabs the pointer for drag-select on press and ungrabs on
 * release, which drops OUR grab - so either pick the row, or take the
 * grab back so outside clicks still close the popup. */
static gint
picker_release_idle(gpointer data)
{
    Picker *p = data;

    p->release_idle = 0;
    if (!GTK_WIDGET_VISIBLE(p->popwin))
        return FALSE;

    if (p->pending_row >= 0) {
        gint row = p->pending_row, old = p->selected;
        p->pending_row = -1;
        picker_popdown(p);
        picker_set_row(p, row, row != old);
    } else {
        gdk_pointer_grab(p->popwin->window, TRUE, POPUP_EVENTS,
                         NULL, NULL, GDK_CURRENT_TIME);
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */

static void
picker_popup(GtkWidget *button, gpointer data)
{
    Picker *p = data;
    GtkRequisition req;
    gint x, y, w, h, bh, sh, natural;

    gdk_window_get_origin(button->window, &x, &y);
    if (GTK_WIDGET_NO_WINDOW(button)) {
        x += button->allocation.x;
        y += button->allocation.y;
    }
    w  = button->allocation.width;
    bh = button->allocation.height;
    sh = gdk_screen_height();

    /* Natural height of the whole list (+ frame/border slack), capped. */
    gtk_widget_size_request(p->clist, &req);
    natural = req.height + 2 * p->popwin->style->klass->ythickness + 4;
    h = MIN(natural, p->max_height);

    if (y + bh + h <= sh)            /* fits below: normal case */
        y += bh;
    else if (y - h >= 0)             /* fits above */
        y -= h;
    else if (sh - (y + bh) >= y) {   /* neither: use the bigger side */
        h = sh - (y + bh);
        y += bh;
    } else {
        h = y;
        y = 0;
    }

    gtk_widget_set_uposition(p->popwin, x, y);
    gtk_widget_set_usize(p->popwin, w, h);
    gtk_widget_realize(p->popwin);
    gdk_window_resize(p->popwin->window, w, h);

    /* Highlight the current item and put keyboard focus on it. */
    if (p->selected >= 0) {
        gtk_clist_select_row(GTK_CLIST(p->clist), p->selected, 0);
        GTK_CLIST(p->clist)->focus_row = p->selected;
    }

    gtk_widget_show(p->popwin);
    gtk_widget_grab_focus(p->clist);

    gtk_grab_add(p->popwin);
    if (gdk_pointer_grab(p->popwin->window, TRUE, POPUP_EVENTS,
                         NULL, NULL, GDK_CURRENT_TIME) != 0) {
        gtk_grab_remove(p->popwin);
        gtk_widget_hide(p->popwin);
        return;
    }
    gdk_keyboard_grab(p->popwin->window, TRUE, GDK_CURRENT_TIME);

    if (!p->scroll_idle)
        p->scroll_idle = gtk_idle_add(picker_scroll_idle, p);
}

/* Any press that is not inside the popup closes it (like a menu). */
static gint
picker_popwin_press(GtkWidget *popwin, GdkEventButton *ev, gpointer data)
{
    Picker *p = data;
    GtkWidget *child = gtk_get_event_widget((GdkEvent *)ev);

    if (child != popwin) {
        while (child && child != popwin)
            child = child->parent;
        if (child == popwin)
            return FALSE;            /* scrollbar, list: let it through */
    } else if (ev->x >= 0 && ev->y >= 0 &&
               ev->x < popwin->allocation.width &&
               ev->y < popwin->allocation.height) {
        return FALSE;                /* on our own frame border */
    }

    picker_popdown(p);
    return TRUE;
}

static gint
picker_popwin_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    Picker *p = data;
    gint row, old;

    switch (ev->keyval) {
    case GDK_Escape:
        picker_popdown(p);
        break;
    case GDK_Return:
    case GDK_KP_Enter:
    case GDK_space:
        old = p->selected;
        row = GTK_CLIST(p->clist)->focus_row;
        picker_popdown(p);
        picker_set_row(p, row, row != old);
        break;
    default:
        return FALSE;                /* arrows etc. go to the CList */
    }
    /* 1.2: returning TRUE alone doesn't stop the class handler. */
    gtk_signal_emit_stop_by_name(GTK_OBJECT(w), "key_press_event");
    return TRUE;
}

/* Note which row the button came up over; act on it in an idle so
 * CList can finish (and drop its own grab) first. */
static gint
picker_list_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    Picker *p = data;
    GtkCList *cl = GTK_CLIST(w);
    gint row, col, ww, wh;

    p->pending_row = -1;
    if (ev->button == 1 && ev->window == cl->clist_window) {
        gdk_window_get_size(cl->clist_window, &ww, &wh);
        if (ev->x >= 0 && ev->y >= 0 && ev->x < ww && ev->y < wh &&
            gtk_clist_get_selection_info(cl, (gint)ev->x, (gint)ev->y,
                                         &row, &col))
            p->pending_row = row;
    }
    if (!p->release_idle)
        p->release_idle = gtk_idle_add(picker_release_idle, p);
    return FALSE;
}

static void
picker_destroy(GtkWidget *w, gpointer data)
{
    Picker *p = data;
    (void)w;

    if (p->scroll_idle)  gtk_idle_remove(p->scroll_idle);
    if (p->release_idle) gtk_idle_remove(p->release_idle);
    picker_popdown(p);
    gtk_widget_destroy(p->popwin);
    g_free(p);
}

/* ------------------------------------------------------------------ */

GtkWidget *
vlhe_picker_new(gchar **items, gint n, gint max_height,
                VlhePickerChanged cb, gpointer cb_data)
{
    Picker *p = g_new0(Picker, 1);
    GtkWidget *hbox, *arrow, *frame, *sw;
    gint i;

    p->max_height   = max_height;
    p->selected     = -1;
    p->pending_row  = -1;
    p->changed      = cb;
    p->changed_data = cb_data;

    /* The face: label + down arrow in a button. */
    p->button = gtk_button_new();
    hbox = gtk_hbox_new(FALSE, 4);
    p->label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(p->label), 0.0, 0.5);
    arrow = gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_OUT);
    gtk_box_pack_start(GTK_BOX(hbox), p->label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), arrow, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(p->button), hbox);
    gtk_widget_show_all(hbox);

    gtk_signal_connect(GTK_OBJECT(p->button), "clicked",
                       GTK_SIGNAL_FUNC(picker_popup), p);
    gtk_signal_connect(GTK_OBJECT(p->button), "destroy",
                       GTK_SIGNAL_FUNC(picker_destroy), p);

    /* The popup: frame > scrolled window > clist. */
    p->popwin = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_widget_set_events(p->popwin, GDK_KEY_PRESS_MASK);
    gtk_signal_connect(GTK_OBJECT(p->popwin), "button_press_event",
                       GTK_SIGNAL_FUNC(picker_popwin_press), p);
    gtk_signal_connect(GTK_OBJECT(p->popwin), "key_press_event",
                       GTK_SIGNAL_FUNC(picker_popwin_key), p);

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    gtk_container_add(GTK_CONTAINER(p->popwin), frame);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(frame), sw);

    p->clist = gtk_clist_new(1);
    gtk_clist_set_selection_mode(GTK_CLIST(p->clist), GTK_SELECTION_BROWSE);
    gtk_clist_set_column_auto_resize(GTK_CLIST(p->clist), 0, TRUE);
    gtk_container_add(GTK_CONTAINER(sw), p->clist);  /* native scroll */
    gtk_signal_connect(GTK_OBJECT(p->clist), "button_release_event",
                       GTK_SIGNAL_FUNC(picker_list_release), p);

    gtk_clist_freeze(GTK_CLIST(p->clist));
    for (i = 0; i < n; i++)
        gtk_clist_append(GTK_CLIST(p->clist), &items[i]);
    gtk_clist_thaw(GTK_CLIST(p->clist));

    gtk_widget_show_all(frame);      /* popwin itself stays hidden */

    if (n > 0)
        picker_set_row(p, 0, FALSE);
    gtk_object_set_data(GTK_OBJECT(p->button), "vlhe-picker", p);
    return p->button;
}

/* ------------------------------------------------------------------ */

/* THE PICKER IS ON THE FACE, as object data - so the caller holds one
 * widget pointer and not a struct, which is how every other control
 * on these pages is used. */
#define PICKER_KEY "vlhe-picker"

static Picker *
picker_of(GtkWidget *face)
{
    if (face == NULL)
        return NULL;
    return gtk_object_get_data(GTK_OBJECT(face), PICKER_KEY);
}

void
vlhe_picker_set_items(GtkWidget *face, gchar **items, gint n)
{
    Picker *p = picker_of(face);
    gint i;

    if (p == NULL)
        return;

    picker_popdown(p);
    gtk_clist_freeze(GTK_CLIST(p->clist));
    gtk_clist_clear(GTK_CLIST(p->clist));
    for (i = 0; i < n; i++)
        gtk_clist_append(GTK_CLIST(p->clist), &items[i]);
    gtk_clist_thaw(GTK_CLIST(p->clist));

    p->selected = -1;
    if (n > 0)
        picker_set_row(p, 0, FALSE);
    else
        gtk_label_set_text(GTK_LABEL(p->label), "");
}

void
vlhe_picker_select(GtkWidget *face, gint row)
{
    Picker *p = picker_of(face);

    if (p != NULL)
        picker_set_row(p, row, FALSE);
}

gint
vlhe_picker_selected(GtkWidget *face)
{
    Picker *p = picker_of(face);

    return (p != NULL) ? p->selected : -1;
}
