/*
 * vlhe_filter.c - the "Files of type" dropdown. Read vlhe_filter.h
 * for why GTK 1.2 needs one written by hand.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * C89, GCC 2.95.2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "vlhe_filter.h"
#include "vlhe_strings.h"
#include "vlhe_match.h"   /* name_matches() - host-tested, see vlhe_match.c */

/*
 * ONE FILTER PER DIALOG, tracked here rather than in a global,
 * because two pickers can be open at once - the CD page allows one
 * per drive row. The state rides on the GtkFileSelection itself via
 * object data, so it lives and dies with the dialog and needs no
 * cleanup of ours.
 */
#define FILTER_KEY "vlhe-filter"

struct filter_state {
    const struct vlhe_filter *items;
    int   current;
    int   applying;             /* re-entrancy guard, see below      */
};

static void
filter_free(gpointer data)
{
    g_free(data);
}

/*
 * FILTER THE FILE LIST, AND ONLY THE FILE LIST.
 *
 * A FIRST VERSION USED gtk_file_selection_complete() AND BROKE
 * NAVIGATION - the user, 2026-09-22: "the filter also filters
 * directores so it stayed on while walking a directory but it made
 * navication impossible."
 *
 * That is what the function does, and it cannot be talked out of it:
 * gtk_file_selection_populate() builds ONE completion list already
 * narrowed by the pattern, then splits it into directories and files
 * (gtkfilesel.c:1352-1366 in 1.2.8's source). The pattern is applied
 * BEFORE the directory test, so `*.mid*' hides every directory that
 * does not end in .mid. There is nowhere to intervene.
 *
 * SO GTK POPULATES NORMALLY AND WE REMOVE ROWS AFTERWARDS. `file_list'
 * and `dir_list' are separate public members of GtkFileSelection
 * (gtkfilesel.h:54-55), so taking rows out of one leaves the other
 * exactly as the toolkit built it - which is the whole point.
 */
static void
apply_filter(GtkFileSelection *fs, struct filter_state *st)
{
    const char *pattern;
    GtkCList   *list;
    int         row;

    if (st == NULL || st->applying)
        return;

    pattern = st->items[st->current].pattern;
    if (pattern == NULL)
        return;                 /* "All files" - nothing to remove */

    if (fs->file_list == NULL)
        return;
    list = GTK_CLIST(fs->file_list);

    st->applying = 1;
    gtk_clist_freeze(list);

    /* BACKWARDS, because removing a row renumbers everything after
     * it - the oldest list-editing bug there is. */
    for (row = list->rows - 1; row >= 0; row--) {
        gchar *text = NULL;

        if (gtk_clist_get_text(list, row, 0, &text) && text != NULL
            && !name_matches(text, pattern))
            gtk_clist_remove(list, row);
    }

    gtk_clist_thaw(list);
    st->applying = 0;
}

/* The user picked a different type. */
static void
on_filter_chosen(GtkWidget *item, gpointer data)
{
    GtkFileSelection *fs = GTK_FILE_SELECTION(data);
    struct filter_state *st =
        (struct filter_state *) gtk_object_get_data(GTK_OBJECT(fs),
                                                    FILTER_KEY);
    const char *dir;
    int which;

    if (st == NULL)
        return;

    which = (int)(long) gtk_object_get_data(GTK_OBJECT(item),
                                            "vlhe-filter-index");
    st->current = which;

    /*
     * REPOPULATE, THEN FILTER. Rows this filter removed are gone from
     * the widget, so switching to a wider pattern - or back to "All
     * files" - has to ask GTK to rebuild the list before there is
     * anything to keep.
     *
     * set_filename() WITH THE CURRENT DIRECTORY IS THE REBUILD.
     * complete() would do it too and is what the first version used,
     * but it also narrows the DIRECTORY list, which is the bug this
     * file exists to avoid - see apply_filter().
     */
    dir = gtk_file_selection_get_filename(fs);
    if (dir != NULL) {
        char path[512];
        char *slash;

        strncpy(path, dir, sizeof path - 1);
        path[sizeof path - 1] = '\0';

        /* THE DIRECTORY PART, so the entry is not reset to a file the
         * user had selected - set_filename() on a full path selects
         * it, which would look like we had chosen for them. */
        slash = strrchr(path, '/');
        if (slash != NULL)
            slash[1] = '\0';

        st->applying = 1;
        gtk_file_selection_set_filename(fs, path);
        st->applying = 0;
    }

    apply_filter(fs, st);
}

/*
 * AFTER GTK HAS FINISHED, NOT DURING. The dialog repopulates its own
 * list on the directory signal, and filtering a list it is in the
 * middle of building is the re-entrancy that crashed the CD page's
 * combo. An idle handler runs once the main loop is back in charge.
 */
static gint
apply_filter_idle(gpointer data)
{
    GtkFileSelection *fs = GTK_FILE_SELECTION(data);
    struct filter_state *st;

    /* THE DIALOG MAY BE GONE. A user can double-click a directory and
     * hit Cancel before the idle runs; GTK_IS_FILE_SELECTION is the
     * cheap check that it is still a live widget of the right type. */
    if (fs == NULL || !GTK_IS_FILE_SELECTION(fs))
        return FALSE;

    st = (struct filter_state *) gtk_object_get_data(GTK_OBJECT(fs),
                                                     FILTER_KEY);
    apply_filter(fs, st);
    return FALSE;               /* once, not repeatedly */
}

/* The user walked into a directory - re-apply, which is the whole
 * reason this exists rather than a single complete() at open. */
static void
on_dir_selected(GtkWidget *w, gint row, gint col,
                GdkEvent *ev, gpointer data)
{
    GtkFileSelection *fs = GTK_FILE_SELECTION(data);
    struct filter_state *st =
        (struct filter_state *) gtk_object_get_data(GTK_OBJECT(fs),
                                                    FILTER_KEY);

    (void)w; (void)row; (void)col; (void)ev;

    if (st != NULL && !st->applying)
        gtk_idle_add(apply_filter_idle, fs);
}

void
vlhe_filter_attach(GtkWidget *filesel,
                   const struct vlhe_filter *items,
                   int deflt)
{
    GtkFileSelection *fs;
    struct filter_state *st;
    GtkWidget *hbox, *lab, *opt, *menu, *item;
    int i, n;

    if (filesel == NULL || items == NULL)
        return;
    fs = GTK_FILE_SELECTION(filesel);

    for (n = 0; items[n].label != NULL; n++)
        ;
    if (n == 0)
        return;
    if (deflt < 0 || deflt >= n)
        deflt = 0;

    st = g_malloc(sizeof *st);
    st->items    = items;
    st->current  = deflt;
    st->applying = 0;
    gtk_object_set_data_full(GTK_OBJECT(fs), FILTER_KEY, st,
                             filter_free);

    /*
     * INTO THE DIALOG'S OWN VBOX. `main_vbox' is a public member of
     * GtkFileSelection (gtkfilesel.h:58), which is what lets this
     * look native with no subclassing - design/32 measured it.
     */
    hbox = gtk_hbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(hbox), 6);

    lab = gtk_label_new(STR_SHELL_LABEL_FILES_OF_TYPE);
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    menu = gtk_menu_new();
    for (i = 0; i < n; i++) {
        item = gtk_menu_item_new_with_label(items[i].label);
        gtk_object_set_data(GTK_OBJECT(item), "vlhe-filter-index",
                            (gpointer)(long) i);
        gtk_signal_connect(GTK_OBJECT(item), "activate",
                           GTK_SIGNAL_FUNC(on_filter_chosen), fs);
        gtk_menu_append(GTK_MENU(menu), item);
        /* EVERY ITEM NEEDS ITS OWN show(). design/32 records this as
         * a fourth `show_all' trap of a new shape: not show_all
         * undoing a hide, but show_all failing to reach a subtree -
         * the option menu stayed a 34px stub without these. */
        gtk_widget_show(item);
    }

    opt = gtk_option_menu_new();
    gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(opt), deflt);
    gtk_box_pack_start(GTK_BOX(hbox), opt, TRUE, TRUE, 0);
    gtk_widget_show(opt);

    gtk_box_pack_start(GTK_BOX(fs->main_vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    /* RE-APPLY ON NAVIGATION - what complete() alone cannot do. */
    if (fs->dir_list != NULL)
        gtk_signal_connect(GTK_OBJECT(fs->dir_list), "select_row",
                           GTK_SIGNAL_FUNC(on_dir_selected), fs);

    /* AND ONCE NOW, for the directory it opened on. */
    apply_filter(fs, st);
}

void
vlhe_filesel_fit(GtkWidget *filesel)
{
    if (filesel == NULL || GTK_FILE_SELECTION(filesel)->selection_text == NULL)
        return;
    gtk_widget_set_usize(GTK_FILE_SELECTION(filesel)->selection_text, 40, -1);
    if (GTK_FILE_SELECTION(filesel)->history_pulldown != NULL)
        gtk_widget_set_usize(GTK_FILE_SELECTION(filesel)->history_pulldown,
                             250, -1);
}
