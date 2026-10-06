/*
 * vlhe_tip.c - the one place a tooltip is set.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * vlhe_strings.h gives every widget string a `_TIP' twin, blank until
 * somebody fills it, and this is what makes a blank one cost nothing:
 * an empty tip sets no tooltip at all, so there is never an empty
 * yellow box, and a filled one shows. The wiring pass wraps each
 * button, check and radio constructor in vlhe_tipped() so the slot is
 * connected at the site the widget is made, with no variable needed.
 *
 * ONE GtkTooltips FOR THE PROGRAM, made on first use. GTK 1.2 keeps
 * the delay and colours per group; one group means they agree.
 *
 * A GtkLabel IS NOT WRAPPED HERE. A label has no X window, so a tip
 * on it never shows; giving one a tip means a GtkEventBox around it,
 * which changes what the caller holds. That is done by hand at the
 * site when a label's slot is filled, not by this function - which
 * therefore sets a label's tip only if it already sits in an event
 * box (the parent is given the tip), and otherwise leaves it.
 */
#include <gtk/gtk.h>
#include "vlhe_tip.h"

static GtkTooltips *g_tips;

void
vlhe_tip(GtkWidget *w, const char *tip)
{
    if (w == NULL || tip == NULL || tip[0] == '\0')
        return;
    if (g_tips == NULL)
        g_tips = gtk_tooltips_new();
    if (GTK_IS_LABEL(w)) {
        GtkWidget *p = w->parent;

        if (p != NULL && GTK_IS_EVENT_BOX(p))
            gtk_tooltips_set_tip(g_tips, p, tip, NULL);
        return;
    }
    gtk_tooltips_set_tip(g_tips, w, tip, NULL);
}

GtkWidget *
vlhe_tipped(GtkWidget *w, const char *tip)
{
    vlhe_tip(w, tip);
    return w;
}
