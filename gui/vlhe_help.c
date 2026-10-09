/*
 * vlhe_help.c - the Help window and the About box (design/54 G01).
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * vlhe_help.h has the shape. Four choices worth knowing:
 *
 * PLAIN TEXT, AS THE FILE HAS IT. GTK 1.2 has no HTML widget, and the
 * user's call was "thats fine we can keep text". A GtkText in a fixed
 * width font keeps the file's own columns; the "== ... ==" headings
 * are drawn bold by inserting them in a second font, so the file
 * carries no markup and still reads in a terminal.
 *
 * A FLAT LIST LIKE THE MAIN WINDOW'S SIDEBAR, NOT A TREE - the user,
 * 2026-10-05, after seeing the first build's GtkCTree: a GtkCList with
 * the sidebar's own icons on the page rows, the general topics under
 * the VLHE icon, and each tab as a "-" row beneath its page. Every
 * section is one click away and nothing opens or closes.
 *
 * THE LIST IS BUILT FROM THE HEADINGS, so it cannot drift from the
 * text: a new "== Page / Tab ==" section appears without a code change.
 *
 * THE FONT IS THE PREFERENCES FONT, NOTHING HARDCODED - the user,
 * 2026-10-05: "It should match the font settings in the preferences".
 * The text is drawn in this window's own style font, which is what
 * File > Preferences > Font sets for every widget (vlhe_cc.c,
 * vlhe_apply_font()); the headings in the bold of the same font when
 * the X server has one, else the same font. A font changed while the
 * window is open arrives as a style change and the text is redrawn in
 * it. A PROPORTIONAL font is honoured too, at a cost the user accepted:
 * the file's hand-aligned second columns stop lining up.
 *
 * SIZED FROM THE FONT, 800x600 AT MOST - the target's screen and the
 * main window's size. The text area is as wide as the file's widest
 * line measured in that font; whatever does not fit is wrapped.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>          /* XA_FONT, reading the font's name   */
#include <gdk/gdkx.h>
#include <stdio.h>
#include <string.h>

#include <gtk/gtk.h>

#include "vlhe_help.h"
#include "vlhe_helptext.h"
#include "vlhe_self.h"
#include "vlhe_strings.h"

#ifndef VLHE_VERSION
#define VLHE_VERSION "unknown"
#endif
/* THE BUILD STAMP, as stagelib.sh's vlhe_stamp() writes one - three
 * defines because host/corelcc cannot pass a space inside one (the
 * Makefile's GUIDEF has the detail). */
#if defined(VLHE_BUILD_DATE) && defined(VLHE_BUILD_TIME) \
    && defined(VLHE_BUILD_COMMIT)
#define VLHE_BUILD VLHE_BUILD_DATE " " VLHE_BUILD_TIME " " VLHE_BUILD_COMMIT
#else
#define VLHE_BUILD "unknown"
#endif

#define HELP_MAX_W  800     /* the target's screen, the main window */
#define HELP_MAX_H  600
#define HELP_EXTRA  50      /* scrollbar, borders, the pane divider */
#define LIST_W      170     /* the list's width before it is measured */

static const char *const *g_pages;
static char **const      *g_xpms;
static char             **g_topic_xpm;
static int                g_npages;

static GtkWidget    *g_win;
static GtkWidget    *g_list;
static GtkWidget    *g_text;
static GtkWidget    *g_sw;                        /* the list's scroller */
static int           g_row[VLHE_HELP_MAXSECT];    /* per section, or -1 */
static int           g_cur = -1;                  /* the row on show  */
static GdkFont      *g_font;      /* the style font, referenced       */
static GdkFont      *g_bold;      /* its bold, or the same font       */
static struct vlhe_help g_help;
static int           g_loaded;                    /* 1 found, 0 not   */
static char          g_tried[2048];

void vlhe_help_set_pages(const char *const *labels, char **const *xpms,
                         int n, char **topic_xpm)
{
    g_pages = labels;
    g_xpms = xpms;
    g_npages = n;
    g_topic_xpm = topic_xpm;
}

/* ------------------------------------------------------------------ */
/* Fonts                                                              */
/* ------------------------------------------------------------------ */

/*
 * THE BOLD OF `f' - its X name with the weight field (the third of
 * an XLFD's fourteen) set to "bold". NULL when the font has no name
 * the server will give back, is not an XLFD, or has no bold.
 */
static GdkFont *bold_of(GdkFont *f)
{
    XFontStruct  *xfs;
    unsigned long v;
    char *nm, out[512];
    const char *p;
    int field = 0, i = 0;
    GdkFont *b = NULL;

    if (f == NULL || f->type != GDK_FONT_FONT)
        return NULL;
    xfs = (XFontStruct *) GDK_FONT_XFONT(f);
    if (xfs == NULL || !XGetFontProperty(xfs, XA_FONT, &v))
        return NULL;
    nm = XGetAtomName(GDK_DISPLAY(), (Atom) v);
    if (nm == NULL)
        return NULL;
    if (nm[0] == '-' && strlen(nm) < sizeof out - 8) {
        for (p = nm; *p != '\0'; p++) {
            if (*p == '-') {
                field++;
                out[i++] = '-';
                if (field == 3) {               /* -foundry-family-WEIGHT */
                    strcpy(out + i, "bold");
                    i += 4;
                    while (p[1] != '\0' && p[1] != '-')
                        p++;
                }
                continue;
            }
            out[i++] = *p;
        }
        out[i] = '\0';
        if (field == 14)
            b = gdk_font_load(out);
    }
    XFree(nm);
    return b;
}

/*
 * THE FONTS FROM THE STYLE - called when the window is built and
 * again whenever its style changes (Preferences > Font, applied live).
 */
static void load_fonts(void)
{
    GdkFont *f = g_text->style->font;

    if (g_font != NULL)
        gdk_font_unref(g_font);
    if (g_bold != NULL)
        gdk_font_unref(g_bold);
    g_font = f != NULL ? gdk_font_ref(f) : NULL;
    g_bold = bold_of(f);
    /* NO BOLD: headings in the plain font, still readable */
    if (g_bold == NULL && g_font != NULL)
        g_bold = gdk_font_ref(g_font);
}

/* THE FILE'S WIDEST LINE IN THE TEXT FONT, in pixels */
static int text_width(void)
{
    long pos = 0;
    int widest = 0;

    if (!g_loaded || g_font == NULL)
        return 520;
    while (pos < g_help.size) {
        const char *p = g_help.text + pos;
        const char *nl = memchr(p, '\n', g_help.size - pos);
        long len = nl != NULL ? nl - p : g_help.size - pos;
        int w = gdk_text_width(len >= 7 && strncmp(p, "== ", 3) == 0
                               ? g_bold : g_font, p, (gint) len);
        if (w > widest)
            widest = w;
        pos += len + 1;
    }
    return widest;
}

/* ------------------------------------------------------------------ */
/* The text                                                           */
/* ------------------------------------------------------------------ */

static void text_set(const char *s, long n, int headings)
{
    GtkText *t = GTK_TEXT(g_text);
    long pos = 0;

    gtk_text_freeze(t);
    gtk_text_set_point(t, 0);
    gtk_text_forward_delete(t, gtk_text_get_length(t));

    /* LINE BY LINE, so a heading line can take the bold font. */
    while (pos < n) {
        const char *p = s + pos;
        const char *nl = memchr(p, '\n', n - pos);
        long len = nl != NULL ? nl - p + 1 : n - pos;
        int  head = headings && len >= 7 && strncmp(p, "== ", 3) == 0;

        gtk_text_insert(t, head ? g_bold : g_font, NULL, NULL, p, len);
        pos += len;
    }
    gtk_text_thaw(t);
    /* NO SELECTION LEFT OVER - GtkEditable keeps its selection
     * offsets across a delete, and the new text came up with a stray
     * highlighted run where the old selection had been. */
    gtk_editable_select_region(GTK_EDITABLE(t), 0, 0);
    gtk_adjustment_set_value(t->vadj, 0.0);
}

/* WHAT A ROW SHOWS: row data is section+1; 0 shows the preamble. */
static void show_row(int row)
{
    int v;

    if (!g_loaded || row < 0)
        return;
    g_cur = row;
    v = GPOINTER_TO_INT(gtk_clist_get_row_data(GTK_CLIST(g_list), row));
    if (v > 0 && v <= g_help.nsect)
        text_set(g_help.text + g_help.sect[v - 1].off,
                 g_help.sect[v - 1].len, 1);
    else
        text_set(g_help.text, g_help.preamble, 1);
}

static void on_select(GtkCList *clist, gint row, gint col,
                      GdkEventButton *ev, gpointer data)
{
    (void)clist;
    (void)col;
    (void)ev;
    (void)data;
    show_row(row);
}

/* ------------------------------------------------------------------ */
/* The list                                                           */
/* ------------------------------------------------------------------ */

static int page_index(const char *name)
{
    int i;

    for (i = 0; i < g_npages; i++)
        if (strcmp(g_pages[i], name) == 0)
            return i;
    return -1;
}

/* A ROW WITH AN ICON - a page, or a general topic */
static int add_icon_row(const char *label, char **xpm, int value)
{
    gchar *text[1];
    int row;

    text[0] = (gchar *)label;
    row = gtk_clist_append(GTK_CLIST(g_list), text);
    if (xpm != NULL && g_list->window != NULL) {
        GdkBitmap *mask = NULL;
        GdkPixmap *pix = gdk_pixmap_create_from_xpm_d(g_list->window,
                                                      &mask, NULL, xpm);
        /* SPACING 4, as the main sidebar's rows have */
        if (pix != NULL)
            gtk_clist_set_pixtext(GTK_CLIST(g_list), row, 0,
                                  (gchar *)label, 4, pix, mask);
    }
    gtk_clist_set_row_data(GTK_CLIST(g_list), row, GINT_TO_POINTER(value));
    return row;
}

/*
 * A TAB'S ROW: "-" where a page row has its icon, then the tab's name
 * where the page's name starts - so the tabs read as belonging to the
 * page above them. Lined up by measuring, in the list's own font, how
 * many spaces put the "-" under the middle of an icon `iconw' wide.
 */
static int add_tab_row(const char *tab, int iconw, int value)
{
    GdkFont *f = g_list->style->font;
    char label[VLHE_HELP_TITLE + 64];
    gchar *text[1];
    int pad = 0, row, sp, dash;

    sp = f != NULL ? gdk_char_width(f, ' ') : 4;
    dash = f != NULL ? gdk_char_width(f, '-') : 4;
    if (sp < 1)
        sp = 1;
    /* spaces before the dash, centring it under the icon... */
    while (pad < 40 && (pad + 1) * sp + dash / 2 <= iconw / 2)
        pad++;
    memset(label, ' ', pad);
    label[pad] = '-';
    pad++;
    /* ...and after it, out to where a page's name begins (icon + 4) */
    {
        int used = (pad - 1) * sp + dash;
        while (pad < 60 && used + sp <= iconw + 4) {
            label[pad++] = ' ';
            used += sp;
        }
    }
    label[pad] = '\0';
    strncat(label, tab, VLHE_HELP_TITLE - 1);

    text[0] = label;
    row = gtk_clist_append(GTK_CLIST(g_list), text);
    gtk_clist_set_row_data(GTK_CLIST(g_list), row, GINT_TO_POINTER(value));
    return row;
}

/* EVERY TAB SECTION OF `page', as "-" rows, in the file's order */
static void add_tabs(const char *page, int iconw)
{
    int k;

    for (k = 0; k < g_help.nsect; k++)
        if (g_help.sect[k].tab[0] != '\0'
            && strcmp(g_help.sect[k].page, page) == 0)
            g_row[k] = add_tab_row(g_help.sect[k].tab, iconw, k + 1);
}

/* the width of an icon, for lining the "-" rows up under it */
static int icon_width(char **xpm)
{
    int w = 0, h = 0;

    if (xpm == NULL || sscanf(xpm[0], "%d %d", &w, &h) != 2)
        return 16;
    return w;
}

/*
 * THE ORDER design/54 G01 AGREED: the general topics first - every
 * section that is not a sidebar page, under the VLHE icon - then the
 * pages in the sidebar's order, each with its tabs beneath it.
 */
static void list_fill(void)
{
    int i, k, iconw;

    for (k = 0; k < VLHE_HELP_MAXSECT; k++)
        g_row[k] = -1;
    iconw = icon_width(g_topic_xpm);

    gtk_clist_freeze(GTK_CLIST(g_list));
    gtk_clist_clear(GTK_CLIST(g_list));

    for (k = 0; k < g_help.nsect; k++) {
        if (g_help.sect[k].tab[0] != '\0'
            || page_index(g_help.sect[k].page) >= 0)
            continue;
        g_row[k] = add_icon_row(g_help.sect[k].title, g_topic_xpm, k + 1);
        add_tabs(g_help.sect[k].page, iconw);
    }

    for (i = 0; i < g_npages; i++) {
        char **xpm = g_xpms != NULL ? g_xpms[i] : NULL;
        int value;

        k = vlhe_help_find(&g_help, g_pages[i], NULL);
        if (k >= 0) {
            value = k + 1;
        } else {
            /* NO PAGE SECTION: the row shows its first tab's, if any */
            int t;
            value = 0;
            for (t = 0; t < g_help.nsect && value == 0; t++)
                if (g_help.sect[t].tab[0] != '\0'
                    && strcmp(g_help.sect[t].page, g_pages[i]) == 0)
                    value = t + 1;
            if (value == 0)
                continue;                /* nothing to say about it */
        }
        {
            int row = add_icon_row(g_pages[i], xpm, value);
            if (k >= 0)
                g_row[k] = row;
        }
        add_tabs(g_pages[i], icon_width(xpm));
    }

    /* TABS OF A PAGE NOBODY LISTED - never today; kept visible, last */
    for (k = 0; k < g_help.nsect; k++)
        if (g_row[k] < 0)
            g_row[k] = add_icon_row(g_help.sect[k].title, g_topic_xpm,
                                    k + 1);
    gtk_clist_thaw(GTK_CLIST(g_list));
}

/* ------------------------------------------------------------------ */
/* The window                                                         */
/* ------------------------------------------------------------------ */

/* CLOSING HIDES IT - the next Help shows the same window again, with
 * its size and position kept. */
static gint on_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)e;
    (void)data;
    gtk_widget_hide(w);
    return TRUE;
}

static void on_close(GtkWidget *b, gpointer data)
{
    (void)b;
    (void)data;
    gtk_widget_hide(g_win);
}

/*
 * AS BIG AS THE LIST AND THE WIDEST LINE NEED, 800x600 AT MOST - or
 * the screen, if that is smaller. `live' is set when the window is
 * already on screen (a font change), where a default size no longer
 * applies and the window itself is resized.
 */
static void size_window(int live)
{
    int listw = LIST_W, maxw, maxh, w, h, ow;

    if (g_loaded) {
        ow = gtk_clist_optimal_column_width(GTK_CLIST(g_list), 0);
        if (ow > 0)
            listw = ow + 24;
    }
    gtk_widget_set_usize(g_sw, listw, -1);

    maxw = gdk_screen_width() - 20;
    if (maxw > HELP_MAX_W)
        maxw = HELP_MAX_W;
    maxh = gdk_screen_height() - 60;
    if (maxh > HELP_MAX_H)
        maxh = HELP_MAX_H;

    w = listw + text_width() + HELP_EXTRA;
    if (w > maxw)
        w = maxw;
    h = maxh < 520 ? maxh : 520;

    if (live && g_win->window != NULL)
        gdk_window_resize(g_win->window, w, h);
    else
        gtk_window_set_default_size(GTK_WINDOW(g_win), w, h);
}

/*
 * PREFERENCES > FONT, APPLIED WHILE THE WINDOW IS OPEN. The rc style
 * reaches every widget (vlhe_apply_font() resets every toplevel), but
 * text already inserted keeps the font it was inserted in - so take
 * the new font and draw the section again.
 */
static void on_style_set(GtkWidget *w, GtkStyle *old, gpointer data)
{
    (void)w;
    (void)data;
    if (old == NULL || g_win == NULL)
        return;                 /* the first style, at realize */
    load_fonts();
    if (g_cur >= 0)
        show_row(g_cur);
    size_window(GTK_WIDGET_MAPPED(g_win));
}

static void build(void)
{
    GtkWidget *vbox, *pane, *sw, *hbox, *sb, *bbox, *btn;
    char path[1024];

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win), STR_HELP_TITLE);
    gtk_window_set_wmclass(GTK_WINDOW(g_win), "vlhe-help", "vlhe.gtk");
    gtk_window_set_policy(GTK_WINDOW(g_win), TRUE, TRUE, FALSE);
    gtk_signal_connect(GTK_OBJECT(g_win), "delete_event",
                       GTK_SIGNAL_FUNC(on_delete), NULL);

    vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_win), vbox);
    gtk_widget_show(vbox);

    pane = gtk_hpaned_new();
    gtk_container_border_width(GTK_CONTAINER(pane), 6);
    gtk_box_pack_start(GTK_BOX(vbox), pane, TRUE, TRUE, 0);
    gtk_widget_show(pane);

    /* THE LIST, in a scrolled window like the main sidebar */
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_paned_add1(GTK_PANED(pane), sw);
    gtk_widget_show(sw);
    g_sw = sw;

    g_list = gtk_clist_new(1);
    gtk_clist_set_selection_mode(GTK_CLIST(g_list), GTK_SELECTION_BROWSE);
    gtk_clist_set_column_auto_resize(GTK_CLIST(g_list), 0, TRUE);
    gtk_signal_connect(GTK_OBJECT(g_list), "select_row",
                       GTK_SIGNAL_FUNC(on_select), NULL);
    gtk_container_add(GTK_CONTAINER(sw), g_list);
    gtk_widget_show(g_list);

    /* THE TEXT. GtkText scrolls through its own adjustments, so it
     * takes a scrollbar beside it rather than a GtkScrolledWindow -
     * the 1.2 idiom (testgtk's text demo does the same). */
    hbox = gtk_hbox_new(FALSE, 0);
    gtk_paned_add2(GTK_PANED(pane), hbox);
    gtk_widget_show(hbox);

    g_text = gtk_text_new(NULL, NULL);
    gtk_text_set_editable(GTK_TEXT(g_text), FALSE);
    gtk_text_set_word_wrap(GTK_TEXT(g_text), TRUE);
    gtk_signal_connect(GTK_OBJECT(g_text), "style_set",
                       GTK_SIGNAL_FUNC(on_style_set), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_text, TRUE, TRUE, 0);
    gtk_widget_show(g_text);

    sb = gtk_vscrollbar_new(GTK_TEXT(g_text)->vadj);
    gtk_box_pack_start(GTK_BOX(hbox), sb, FALSE, FALSE, 0);
    gtk_widget_show(sb);

    bbox = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
    gtk_container_border_width(GTK_CONTAINER(bbox), 8);
    gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);
    gtk_widget_show(bbox);

    btn = gtk_button_new_with_label(STR_SHELL_BTN_CLOSE);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_close), NULL);
    gtk_container_add(GTK_CONTAINER(bbox), btn);
    gtk_widget_grab_default(btn);
    gtk_widget_show(btn);

    /* REALIZED NOW, so the icons have a window to be made against */
    gtk_widget_realize(g_win);
    gtk_widget_realize(g_list);

    /* THE FILE IS READ ONCE, when the window is first made - a help
     * file does not change under a running program. */
    g_loaded = vlhe_help_locate(path, sizeof path, g_tried, sizeof g_tried)
               && vlhe_help_load(&g_help, path) == 0;
    if (g_loaded)
        list_fill();
    load_fonts();
    size_window(0);

    if (!g_loaded) {
        char msg[sizeof g_tried + 128];
        sprintf(msg, "%s%s", STR_HELP_NOT_FOUND, g_tried);
        text_set(msg, strlen(msg), 0);
    }
}

static void select_row(int row)
{
    GtkCList *l = GTK_CLIST(g_list);

    if (row < 0)
        return;
    gtk_clist_select_row(l, row, 0);
    /* ALREADY SELECTED SENDS NO SIGNAL, and the text may have been
     * scrolled since - so show it again either way. */
    show_row(row);
    if (gtk_clist_row_is_visible(l, row) != GTK_VISIBILITY_FULL)
        gtk_clist_moveto(l, row, 0, 0.3, 0.0);
}

void vlhe_help_show(const char *page, const char *tab)
{
    int k = -1;

    if (g_win == NULL)
        build();

    if (g_loaded) {
        if (page != NULL)
            k = vlhe_help_find(&g_help, page, tab);
        /* CONTENTS, or a page with no section: the first row, which is
         * "The window" */
        select_row(k >= 0 && g_row[k] >= 0 ? g_row[k] : 0);
    }

    gtk_widget_show(g_win);
    if (g_win->window != NULL)
        gdk_window_raise(g_win->window);
}

/* ------------------------------------------------------------------ */
/* About                                                              */
/* ------------------------------------------------------------------ */

static void about_line(GtkWidget *box, const char *s)
{
    GtkWidget *l = gtk_label_new(s);

    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(l), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 1);
    gtk_widget_show(l);
}

void vlhe_help_about(GtkWidget *parent)
{
    GtkWidget *dlg, *btn, *box;
    char line[1200], path[1024];
    const char *dir;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_ABOUT_TITLE);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    if (parent != NULL)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(parent));
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    box = gtk_vbox_new(FALSE, 2);
    gtk_container_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), box, TRUE, TRUE, 0);
    gtk_widget_show(box);

    sprintf(line, STR_ABOUT_NAME, VLHE_VERSION);
    about_line(box, line);
    about_line(box, STR_ABOUT_DESC);
    about_line(box, "");
    about_line(box, STR_ABOUT_COPYRIGHT);
    about_line(box, STR_ABOUT_LICENCE);
    about_line(box, "");
    sprintf(line, STR_ABOUT_BUILD, VLHE_BUILD);
    about_line(box, line);

    dir = vlhe_self_dir();
    if (vlhe_self_is_trial()) {
        sprintf(line, STR_ABOUT_PORTABLE,
                dir != NULL && dir[0] != '\0' ? dir : "?");
        about_line(box, line);
    } else {
        about_line(box, STR_ABOUT_INSTALLED);
    }
    if (vlhe_help_locate(path, sizeof path, NULL, 0)) {
        sprintf(line, STR_ABOUT_HELP_FILE, path);
        about_line(box, line);
    } else {
        about_line(box, STR_ABOUT_HELP_NONE);
    }

    btn = gtk_button_new_with_label(STR_SHELL_BTN_OK);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_grab_default(btn);
    gtk_widget_show(btn);
    gtk_widget_show(dlg);
}
