/*
 * vlhe_buttons.c - see vlhe_buttons.h.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <gtk/gtk.h>

#include "vlhe_buttons.h"
#include "vlhe_strings.h"

/*
 * ONE SIZE FOR EVERY BUTTON IN A ROW, A LITTLE WIDER THAN THE WIDEST
 * ASKS - the user, 2026-10-01, on the first-run dialog: "make these
 * buttons the same sizes and increase the size of Restore backup
 * button so it is slightly larger and then match that size", and on
 * the quit dialog: "the same width and height. Width needs a bit of
 * padding so the t in quit is not against the edge of the button".
 *
 * GTK 1.2 sizes a button to its label, so three labels come out three
 * widths. The widest label's own request plus a margin is the width
 * and the tallest's is the height - measured, not hardcoded, so a
 * different font on the target gets the same proportion. Call it
 * after the buttons are packed and shown.
 *
 * AND THE SAME SIZE FROM ONE DIALOG TO THE NEXT - the user,
 * 2026-10-01: "The could not save okay button needs to be the same
 * size as the other buttons". A lone OK has no sibling to match, so
 * the floor is a reference label - the widest any of these dialogs
 * carries - measured as a throwaway button packed in the SAME row,
 * so it is styled as its siblings are, and destroyed again. Every
 * row is then at least that size, whatever it says.
 */
#define BUTTON_REF_LABEL STR_SHELL_BTN_DISCARD_QUIT   /* vlhe_strings.h */

/* THE ROW MAY HOLD MORE THAN BUTTONS. The Simulate box keeps its
 * "Explain each step" check button in the action area, and a check
 * button IS a GtkButton to the type system - sized as a push button
 * it would come out a blank square with the label squeezed. Only a
 * plain button is a button here. */
static int is_push_button(GtkWidget *w)
{
    return GTK_IS_BUTTON(w) && !GTK_IS_TOGGLE_BUTTON(w);
}

void vlhe_buttons_equalise(GtkWidget *box)
{
    GList *kids = gtk_container_children(GTK_CONTAINER(box));
    GList *l;
    int    widest = 0, tallest = 0;

    /* NO DEFAULT BUTTON, AND THAT IS WHAT MAKES THEM IDENTICAL. A
     * GTK 1.2 button that CAN take the default reserves a ring -
     * 2 * thickness + DEFAULT_SPACING on each axis, in its request
     * (gtkbutton.c:436-441) and off the box it draws (:561-568) - and
     * the one that HAS it paints that ring as a second box around
     * itself. So "Cancel" with the default beside two buttons without
     * came out smaller on the target (2026-10-01), and with the flag
     * on all three it came out the same size WITH A FRAME ROUND IT -
     * the user's capture of the fake. Three identical buttons and a
     * default are not both available.
     *
     * SO THE DEFAULT BECOMES THE FOCUS. With no default widget the
     * window hands Return to the focused one (gtkwindow.c:1196-1200),
     * and space already went there - so a stray keypress still lands
     * on the button the dialog chose, which was the whole point of
     * the default, and no ring is drawn. A dialog whose focus is
     * elsewhere on purpose - the password box - had no default to
     * move and is left alone. */
    {
        GtkWidget *deflt = NULL;

        for (l = kids; l != NULL; l = l->next)
            if (is_push_button(GTK_WIDGET(l->data)) &&
                GTK_WIDGET_HAS_DEFAULT(GTK_WIDGET(l->data)))
                deflt = GTK_WIDGET(l->data);
        if (deflt != NULL) {
            GtkWidget *top = gtk_widget_get_toplevel(deflt);

            if (top != NULL && GTK_IS_WINDOW(top))
                gtk_window_set_default(GTK_WINDOW(top), NULL);
            gtk_widget_grab_focus(deflt);
        }
        for (l = kids; l != NULL; l = l->next)
            if (is_push_button(GTK_WIDGET(l->data)))
                GTK_WIDGET_UNSET_FLAGS(GTK_WIDGET(l->data),
                                       GTK_CAN_DEFAULT);
    }

    {
        GtkWidget     *ref = gtk_button_new_with_label(BUTTON_REF_LABEL);
        GtkRequisition req;

        gtk_box_pack_start(GTK_BOX(box), ref, FALSE, FALSE, 0);
        gtk_widget_size_request(ref, &req);
        widest  = req.width;
        tallest = req.height;
        gtk_widget_destroy(ref);
    }

    for (l = kids; l != NULL; l = l->next) {
        GtkRequisition req;

        if (!is_push_button(GTK_WIDGET(l->data)))
            continue;
        gtk_widget_size_request(GTK_WIDGET(l->data), &req);
        if (req.width > widest)
            widest = req.width;
        if (req.height > tallest)
            tallest = req.height;
    }
    for (l = kids; l != NULL; l = l->next)
        if (is_push_button(GTK_WIDGET(l->data)))
            gtk_widget_set_usize(GTK_WIDGET(l->data), widest + 16, tallest);
    g_list_free(kids);
}
