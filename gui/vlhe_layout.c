/*
 * vlhe_layout.c - see vlhe_layout.h.
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 * Part of VLHE. See LICENSE for the full license text.
 * C89, GCC 2.95.2.
 */

#include <string.h>
#include <gtk/gtk.h>

#include "vlhe_layout.h"

/*
 * WHY THIS FILE EXISTS - 2026-10-07, measured in a nested X server with
 * the target's own fonts at its 75 dpi (Helvetica 12, ascent 11,
 * descent 3 - the Soyo's 14 px line exactly).
 *
 * THE PARAGRAPHS WERE 580 PX WIDE, WRITTEN DOWN IN 22 PLACES, and the
 * pane holds 636 of content only while it has no vertical scrollbar.
 * When a page grows tall enough for one, the bar takes 15 px and the
 * scrolled window 3 more of spacing; the content no longer fits in
 * what is left and a horizontal bar appears for a couple of pixels.
 * CD Settings did exactly that: 620 wide in 618.
 *
 * SO THE WIDTH IS WORKED OUT, AND THE BAR'S ROOM IS ALWAYS LEFT FOR IT:
 *
 *     text width = pane - viewport frame - scrollbar - spacing
 *                       - 2 x (how deep the label sits)
 *
 * the scrollbar MEASURED (a theme or gtkrc changes it) and the depth
 * WALKED - each frame and notebook between the label and the viewport
 * places its child at border_width + xthickness (gtkframe.c:508,
 * gtknotebook.c's size_allocate in GTK 1.2.8), each other container
 * at its border_width. So a paragraph in a frame in a notebook gets
 * exactly the room it has, wherever it is.
 *
 * Reserving the bar's width costs an empty strip on a page that does
 * not scroll. That is the price of a vertical bar never bringing a
 * horizontal one with it.
 *
 * THE LABEL COLUMNS WERE 150, 130 AND 110 PX, and GTK 1.2's set_usize
 * REPLACES a widget's requested width (gtkwidget.c:2423), so a label
 * longer than its column is cut: "Song font (bank 1):" was at 130, and
 * Render's "Temporary WAV" was at 110 under the target's font. Each
 * column is now the width of its widest label, measured.
 *
 * GTK 1.2 never re-wraps a label when the window is resized, so the
 * pass runs again whenever the pane changes width (on_pane_allocate);
 * the scrolled window is there for a window too narrow for the floor.
 */

#define MAX_NOTES    96
#define MAX_COLUMNS  256

static GtkWidget *g_notes[MAX_NOTES];
static char g_hint[MAX_NOTES];      /* 1: no wider than its own text */
static int g_nnotes;

#define MAX_DIGITS   16
static GtkWidget *g_dig[MAX_DIGITS];
static int g_dign[MAX_DIGITS];
static int g_ndig;

static struct {
    const char *column;
    GtkWidget  *label;
} g_cols[MAX_COLUMNS];
static int g_ncols;

GtkWidget *
vlhe_layout_note(const char *text)
{
    GtkWidget *lab = gtk_label_new(text);

    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    vlhe_layout_wrap(lab);
    return lab;
}

void
vlhe_layout_wrap(GtkWidget *label)
{
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    /* LEFT-JUSTIFIED: a GtkLabel centres its lines by default, which
     * a wrapped paragraph or path must not. */
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
    /* A WIDTH NOW, so a page built before vlhe_layout_finish() runs -
     * or a label made later, by a dialog - still wraps sensibly. */
    gtk_widget_set_usize(label, 560, -1);
    if (g_nnotes < MAX_NOTES)
        g_notes[g_nnotes++] = label;
}

void
vlhe_layout_hint(GtkWidget *label)
{
    vlhe_layout_wrap(label);
    if (g_nnotes > 0 && g_notes[g_nnotes - 1] == label)
        g_hint[g_nnotes - 1] = 1;
}

void
vlhe_layout_digits(GtkWidget *spin, int digits)
{
    if (g_ndig < MAX_DIGITS) {
        g_dig[g_ndig] = spin;
        g_dign[g_ndig] = digits;
        g_ndig++;
    }
}

/*
 * A SPIN BUTTON'S WIDTH FOR N DIGITS, from GTK 1.2.8's own geometry:
 * the entry gets the width less ARROW_SIZE (11) + 2 x xthickness
 * (gtkspinbutton.c:473), and draws its text INNER_BORDER (2) +
 * xthickness in from each side (gtkentry.c:766). Plus 2 for the
 * cursor, so the last digit is not under it.
 */
static void
apply_digits(void)
{
    static const char zeros[] = "0000000000";
    int i;

    for (i = 0; i < g_ndig; i++) {
        GtkWidget *w = g_dig[i];
        int n = g_dign[i] < 10 ? g_dign[i] : 10;
        int xt = w->style->klass->xthickness;
        int text = gdk_text_width(w->style->font, zeros, n);

        gtk_widget_set_usize(w, text + 2 * (xt + 2) + 2
                                + 11 + 2 * xt, -1);
    }
}

/* A label's own width: its longest line, in its font, plus its pad. */
static int
natural_width(GtkWidget *label)
{
    const char *t = GTK_LABEL(label)->label, *e;
    GdkFont *font = label->style->font;
    int widest = 0, w;

    while (t != NULL && *t != '\0') {
        e = strchr(t, '\n');
        w = gdk_text_width(font, t, e != NULL ? (gint) (e - t)
                                               : (gint) strlen(t));
        if (w > widest)
            widest = w;
        t = e != NULL ? e + 1 : NULL;
    }
    return widest + 2 * GTK_MISC(label)->xpad;
}

void
vlhe_layout_column(const char *column, GtkWidget *label)
{
    if (g_ncols < MAX_COLUMNS) {
        g_cols[g_ncols].column = column;
        g_cols[g_ncols].label = label;
        g_ncols++;
    }
}

/*
 * How far in from the viewport's edges `w' sits. `both' is the inset on
 * EACH side (borders, frames, notebooks - symmetric); `left' is extra
 * on the left only: in a horizontal box, the widgets before it and the
 * box's spacing (a value beside its "Name:" label, say). Called after
 * the columns are sized, so a column label's width is its final one.
 */
static void
depth_of(GtkWidget *w, int *both, int *left)
{
    GtkWidget *c, *p;
    GtkRequisition req;
    GList *l;

    *both = 0;
    *left = 0;
    for (c = w, p = w->parent; p != NULL && !GTK_IS_VIEWPORT(p);
         c = p, p = p->parent) {
        if (GTK_IS_CONTAINER(p))
            *both += GTK_CONTAINER(p)->border_width;
        if (GTK_IS_FRAME(p) || GTK_IS_NOTEBOOK(p))
            *both += p->style->klass->xthickness;
        /* A CHECK BUTTON ASKS FOR ITS LABEL'S WIDTH PLUS THE
         * INDICATOR (indicator_size + 3 x indicator_spacing + 2,
         * gtkcheckbutton.c) PLUS GtkButton's own padding, (CHILD_SPACING
         * + xthickness) on each side - CHILD_SPACING is gtkbutton.c's
         * private 1. Missing that padding left a checkbox 6 px wide. */
        if (GTK_IS_CHECK_BUTTON(p)
            && GTK_TOGGLE_BUTTON(p)->draw_indicator) {
            GtkCheckButtonClass *k =
                GTK_CHECK_BUTTON_CLASS(GTK_OBJECT(p)->klass);

            *left += k->indicator_size + 3 * k->indicator_spacing + 2
                     + 2 * (1 + p->style->klass->xthickness);
        }
        if (GTK_IS_HBOX(p)) {
            for (l = GTK_BOX(p)->children; l != NULL; l = l->next) {
                GtkBoxChild *bc = (GtkBoxChild *) l->data;

                if (bc->widget == c) {
                    *left += bc->padding;
                    break;
                }
                if (!GTK_WIDGET_VISIBLE(bc->widget))
                    continue;
                gtk_widget_size_request(bc->widget, &req);
                *left += req.width + 2 * bc->padding
                         + GTK_BOX(p)->spacing;
            }
        }
    }
}

/* What vlhe_layout_finish() measured once, for every later pass. */
static int g_bar_w, g_spacing, g_frame_w;
static int g_pane_w = -1;       /* the width the notes were last set for */
static int g_want_w = -1;       /* the width they should be set for */
static guint g_idle;
static GtkWidget *g_scroll_w;    /* the pane, for its relayout */

/* Every note at the wrap width a pane `pane_w' wide gives it. */
static void
apply_notes(int pane_w)
{
    int text_w = pane_w - g_frame_w - g_bar_w - g_spacing;
    int i;

    for (i = 0; i < g_nnotes; i++) {
        int both, left, w;

        depth_of(g_notes[i], &both, &left);
        w = text_w - 2 * both - left;
        if (g_hint[i] && natural_width(g_notes[i]) < w)
            w = natural_width(g_notes[i]);
        if (w < 100 && !g_hint[i])
            w = 100;
        gtk_widget_set_usize(g_notes[i], w, -1);
    }
    g_pane_w = pane_w;
}

static gint
on_idle(gpointer data)
{
    (void) data;
    g_idle = 0;
    if (g_want_w > 0 && g_want_w != g_pane_w) {
        apply_notes(g_want_w);
        /*
         * AND MAKE THE PANE LAY ITS CONTENTS OUT AGAIN. Found 2026-10-07
         * (the user): maximise, restore, and the notes rewrapped but the
         * page stayed the maximised width - 838 px in a 640 px pane,
         * with a horizontal bar, until another page was shown. GTK 1.2,
         * when a resize leaves the window its size, re-allocates the
         * changed widgets at the allocations they already had, so the
         * viewport never handed its child the narrower width. Queuing
         * a resize on the viewport makes it allocate its child afresh
         * from the new requisition. Measured: back to 636 px, no bar,
         * on MIDI Options and Status, at 12 and 17 px.
         */
        if (g_scroll_w != NULL && GTK_BIN(g_scroll_w)->child != NULL)
            gtk_widget_queue_resize(GTK_BIN(g_scroll_w)->child);
    }
    return FALSE;
}

/*
 * THE PANE WAS RESIZED - rewrap to the new width. Added 2026-10-07: the
 * wrap was set once at startup, so a window made wider kept its text
 * at 800x600's width (the user: "If the window grows the text stays
 * wrapped and doesnt expand").
 *
 * NOT A LOOP, AND WHY. A handler on a LABEL's allocation would be one -
 * a new width re-requests the label, which reallocates it (the warning
 * beside the Status page's note). This watches the SCROLLED WINDOW,
 * whose width is the window's less the sidebar and never depends on
 * what is inside it: the rewrap reallocates, the width is the same, and
 * nothing changes. A vertical bar appearing does not move it either -
 * the bar's room is always left (apply_notes).
 *
 * DEFERRED TO IDLE because GTK 1.2 is in the middle of laying the
 * window out when this runs; changing sizes then is asking for a
 * second pass the toolkit is not expecting.
 */
static void
on_pane_allocate(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    (void) w;
    (void) data;
    if (a->width == g_pane_w || a->width < 2)
        return;
    g_want_w = a->width;
    if (g_idle == 0)
        g_idle = gtk_idle_add(on_idle, NULL);
}

void
vlhe_layout_finish(GtkWidget *scroll, int pane_w)
{
    GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(scroll);
    GtkRequisition req;
    int i, j;

    g_scroll_w = scroll;

    /* THE SCROLLBAR, AS THIS STYLE DRAWS IT. */
    gtk_widget_size_request(sw->vscrollbar, &req);
    g_bar_w = req.width;
    g_spacing = GTK_SCROLLED_WINDOW_CLASS(GTK_OBJECT(sw)->klass)
                    ->scrollbar_spacing;
    /* The viewport draws a frame round the pages, both sides. */
    g_frame_w = 0;
    if (GTK_BIN(scroll)->child != NULL)
        g_frame_w = 2 * GTK_BIN(scroll)->child->style->klass->xthickness;

    /* THE COLUMNS: each label at its own width first, then all of a
     * column at the widest. -1 clears a usize, so the request is the
     * label's own. Once - they do not depend on the window. */
    for (i = 0; i < g_ncols; i++) {
        int widest = 0;

        if (g_cols[i].label == NULL)
            continue;
        for (j = i; j < g_ncols; j++) {
            if (g_cols[j].label == NULL
                || strcmp(g_cols[j].column, g_cols[i].column) != 0)
                continue;
            gtk_widget_set_usize(g_cols[j].label, -1, -1);
            gtk_widget_size_request(g_cols[j].label, &req);
            if (req.width > widest)
                widest = req.width;
        }
        for (j = i; j < g_ncols; j++) {
            if (g_cols[j].label == NULL
                || strcmp(g_cols[j].column, g_cols[i].column) != 0)
                continue;
            gtk_widget_set_usize(g_cols[j].label, widest, -1);
            g_cols[j].label = NULL;     /* done */
        }
    }

    /* THE SPIN BUTTONS, BEFORE THE NOTES - a hint beside one needs its
     * final width (depth_of's `left'). */
    apply_digits();

    /* THE NOTES, AFTER THE COLUMNS - a note beside a column label needs
     * that label's final width (depth_of's `left') - at the width the
     * window opens at, and again whenever the pane is resized. */
    apply_notes(pane_w);
    gtk_signal_connect_after(GTK_OBJECT(scroll), "size_allocate",
                             GTK_SIGNAL_FUNC(on_pane_allocate), NULL);
}
