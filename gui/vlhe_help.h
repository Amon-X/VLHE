/*
 * vlhe_help.h - the Help window and the About box (design/54 G01).
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * ONE WINDOW, REUSED, NOT MODAL: a list of the help file's headings on
 * the left, like the main sidebar, and the chosen section's text on
 * the right. Opening it again moves the selection and raises it;
 * closing it hides it. The text is
 * gui/vlhe_helptext.c's, which finds and splits the file.
 */
#ifndef VLHE_HELP_H
#define VLHE_HELP_H

#include <gtk/gtk.h>

/* THE SIDEBAR'S PAGES, IN ITS ORDER, WITH ITS ICONS - the help list
 * shows them in this order after the general topics, which take
 * `topic_xpm'. Called once, before the first vlhe_help_show(); the
 * arrays must stay valid. */
void vlhe_help_set_pages(const char *const *labels, char **const *xpms,
                         int n, char **topic_xpm);

/* OPEN AT A PAGE AND TAB - the Help button. `tab' may be NULL; a tab
 * with no section falls back to the page. `page' NULL opens at the
 * top, which is Help > Contents. */
void vlhe_help_show(const char *page, const char *tab);

/* HELP > ABOUT VLHE - a small modal box over `parent'. */
void vlhe_help_about(GtkWidget *parent);

#endif
