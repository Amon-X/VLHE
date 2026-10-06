/*
 * vlhe_helptext.h - the help file, read and split into sections.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * NO GTK HERE, deliberately: this is the part with logic in it - the
 * parse, the page-and-tab lookup, the search for the file - so it is
 * the part a host test can drive. gui/vlhe_help.c is the window that
 * shows what this finds (design/54 G01).
 *
 * THE FILE'S SHAPE (doc/vlhe-help.txt): a preamble, then sections,
 * each starting at a line "== Title ==". A title "Page / Tab" is the
 * help for one tab of one page; anything else is a page or a general
 * topic. The section runs from its heading line to the line before
 * the next heading, heading included.
 */
#ifndef VLHE_HELPTEXT_H
#define VLHE_HELPTEXT_H

#define VLHE_HELP_FILE      "vlhe-help.txt"
#define VLHE_HELP_MAXSECT   64
#define VLHE_HELP_TITLE     64

struct vlhe_help_sect {
    char title[VLHE_HELP_TITLE];  /* "Midi Settings / Options"        */
    char page[VLHE_HELP_TITLE];   /* "Midi Settings"                  */
    char tab[VLHE_HELP_TITLE];    /* "Options", or "" for a page/topic */
    long off;                     /* the heading line's offset        */
    long len;                     /* through the end of the section   */
};

struct vlhe_help {
    char *text;                   /* the whole file, NUL-terminated   */
    long  size;
    long  preamble;               /* bytes before the first heading   */
    int   nsect;
    struct vlhe_help_sect sect[VLHE_HELP_MAXSECT];
};

/*
 * READ AND SPLIT `path'. 0 on success; -1 if it cannot be read, in
 * which case `h' is left empty and vlhe_help_free() is still safe.
 * Headings past VLHE_HELP_MAXSECT are folded into the last section
 * rather than dropped, so no text goes missing.
 */
int  vlhe_help_load(struct vlhe_help *h, const char *path);

/* PARSE TEXT ALREADY IN MEMORY - the load's second half, and the
 * entry point the host test drives. Takes ownership of `text', which
 * must have come from malloc() and be NUL-terminated. */
void vlhe_help_parse(struct vlhe_help *h, char *text, long size);

void vlhe_help_free(struct vlhe_help *h);

/*
 * THE SECTION FOR A PAGE AND TAB: "Page / Tab" if there is one, else
 * "Page" - the Help button's fallback when a tab has no section of its
 * own. `tab' may be NULL or "". A trailing " *" on either (the dirty
 * mark the tabs and sidebar carry) is ignored. -1 when not even the
 * page has a section.
 */
int  vlhe_help_find(const struct vlhe_help *h, const char *page,
                    const char *tab);

/*
 * FIND THE HELP FILE, in design/54 G01's order:
 *
 *   1. $VLHE_HELP - honoured only for an ordinary user running an
 *      ordinary binary: never for root, whose programs take no orders
 *      from the environment (design/55), and never under setuid,
 *      where it would let any user have a root-owned file read out
 *      to them. It exists for `make run-gui-cc'.
 *   2. the build's datadir, compiled in as VLHE_DATADIR (installed
 *      builds only - a portable build has none)
 *   3. /usr/share/vlhe, then /usr/local/share/vlhe - the .deb builds
 *      with /usr/local and installs to /usr
 *   4. beside the program, for a portable folder
 *
 * Returns 1 and fills `out' with the first readable one, or 0. Either
 * way `tried' (if not NULL) gets every path looked at, one per line,
 * so the window can say where it looked.
 */
int  vlhe_help_locate(char *out, int max, char *tried, int tmax);

#endif
