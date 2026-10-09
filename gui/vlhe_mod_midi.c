/*
 * vlhe_mod_midi.c - Midi Settings: the fonts, the synth, the module.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * THREE TABS, AND THE THIRD EXISTS BECAUSE OF PRIVILEGE rather than
 * because of subject matter:
 *
 *   Sound Fonts  the stack vmidid plays. A list, because fonts STACK -
 *                "-s gm.sf2 -s song.sf2@1", later fonts searched
 *                first - so order is meaningful.
 *   Options      the synth's runtime settings. vmidid's, none of them
 *                needing root.
 *   Driver       vmidi.o's one user-facing parameter, and the restart
 *                control. Needs root, and a module reload.
 *
 * WHY DRIVER IS SEPARATE. Mixing privilege levels on one page greys
 * one control in six, which reads as broken rather than as a
 * distinction. CD Settings avoided that by accident - everything on
 * its Options tab needs root, so the whole tab greys together with one
 * sentence explaining it. Three tabs makes that deliberate here.
 *
 * AND THE SPLIT IS THE OPPOSITE OF THE CD's. vmidi.o has ONE
 * user-facing parameter; vmidid has a dozen. So MIDI Options is mostly
 * the DAEMON where CD Options was mostly the MODULE.
 *
 * C89, GCC 2.95.2, GTK 1.2.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_layout.h"
#include "vlhe_rates.h"
#include "vlhe_tip.h"
#include "vlhe_filter.h"   /* vlhe_filesel_fit() */
#include "vlhe_priv.h"       /* vlhe_priv_can_act */
#include "vlhe_fontscan.h"   /* VLHE_N_FONTDIR_DEFAULTS, the defaults */
#include "vlhe_mod_midi.h"

#define PAGE_FONTS      0
#define PAGE_OPTIONS    1
#define PAGE_DRIVER     2

static int              g_page;
static GtkWidget       *g_notebook;
static void           (*g_report)(const char *);
static void           (*g_page_cb)(int);

/* Options */
static GtkWidget       *g_voices;
static GtkWidget       *g_gain;
static GtkWidget       *g_law;
static GtkWidget       *g_filter;
static GtkWidget       *g_reverb;      /* -E, apart since 2026-10-05 */
static GtkWidget       *g_chorus;
static GtkWidget       *g_autovoices;  /* -A, design/21 16 */
static GtkWidget       *g_modenv;      /* -M, design/54 D28 and D70 */
static GtkWidget       *g_rate;
static GtkWidget       *g_synth_status;
static GtkWidget       *g_restart_btn;      /* "Restart Synth" - M6   */

/* Driver */
static GtkWidget       *g_minor;
static GtkWidget       *g_root_note;   /* "needs root", hidden after Modify */
static GtkWidget       *g_driver_status;

/* UNSAVED EDITS. This page had NO dirty tracking at all until
 * 2026-09-19 - the user found Apply reporting nothing to save after
 * changing the voice count, and the sidebar never marking the page
 * (design/32). Volume and Sound had it; MIDI and CD did not.
 *
 * g_loading guards the handlers while WE set a widget - reload() and
 * the initial build both do, and neither is a user edit.
 *
 * KNOWN: changing a value and putting it back leaves this set. It
 * records THAT something was edited, not whether the result differs
 * from what is saved. The user has accepted that for now; the fix is
 * a saved copy per control, and it is worth doing when a spurious
 * "unsaved changes" prompt actually costs someone something. */
static int              g_dirty;
static int              g_loading;
static void           (*g_dirty_cb)(void);

/* THE TAB THE EDIT WAS MADE ON GETS A "*", like the Sound and CD
 * pages' - the user, 2026-10-01: "The Tab should also get the *". An
 * edit comes from the tab on screen, so that is the one marked; all
 * three are cleared when the page is collected or reloaded. */
static int g_tab_dirty[3];

static void tab_mark(int tab, int dirty)
{
    static const char *name[3] = { STR_MID_TAB_SOUND_FONTS, STR_MID_TAB_OPTIONS, STR_MID_TAB_DRIVER };
    GtkWidget *page;
    char lab[24];

    if (g_notebook == NULL || tab < 0 || tab > 2)
        return;
    page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), tab);
    if (page == NULL)
        return;
    sprintf(lab, "%s%s", name[tab], dirty ? " *" : "");
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(g_notebook), page, lab);
    g_tab_dirty[tab] = dirty;
}

static void tabs_clear(void)
{
    int t;

    for (t = 0; t < 3; t++)
        if (g_tab_dirty[t])
            tab_mark(t, 0);
}

static void mark_dirty(void)
{
    if (g_loading)
        return;
    if (g_notebook != NULL) {
        int t = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));

        if (t >= 0 && t <= 2 && !g_tab_dirty[t])
            tab_mark(t, 1);
    }
    if (g_dirty)
        return;
    g_dirty = 1;
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

/* One handler for every control on both tabs. An adjustment sends
 * "value_changed"; a check button and a menu item send different
 * signals, so this takes the widget it is given and ignores it. */
static void on_midi_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    mark_dirty();
}

static void report(const char *m)
{
    if (g_report != NULL)
        g_report(m);
}

/* `wide' decides whether the control fills the row or takes its
 * natural size, AND IT HAS TO BE PER ROW.
 *
 * A REGRESSION THE USER CAUGHT: expansion was set on every row at once
 * to make the two font dropdowns line up, which also stretched Volume
 * curve and Velocity filter across the whole pane to hold strings like
 * "AWE32 (awe)". Right for one pair, absurd for the other.
 *
 * WIDE for the font menus, which hold real filenames - the project's
 * own sf2/ folder has a 41-character one - and where two adjacent
 * dropdowns of different natural widths look misaligned.
 *
 * NARROW for a menu of short fixed strings, where the natural width
 * IS the right width and stretching leaves 300px of empty button. */
static GtkWidget *labelled_row(GtkWidget *vbox, const char *text,
                               GtkWidget *control, const char *after,
                               int wide)
{
    GtkWidget *hbox;
    GtkWidget *lab;

    hbox = gtk_hbox_new(FALSE, 6);

    lab = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
    /* THE COLUMN IS ITS WIDEST LABEL, MEASURED (vlhe_layout.c). It was
     * a guessed 150 after "Song font (bank 1):" was clipped at 130. */
    vlhe_layout_column("midi", lab);
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    /* THE ARROWS NEED A SHADOW OR THEY FLOAT ON BARE BACKGROUND.
     *
     * The user's screenshot, 2026-09-19, showed the arrows drawn
     * outside the entry's frame with no relief, on every page and not
     * only this one - which is what said the cause was shared rather
     * than per-module.
     *
     * IT IS A GTK 1.2 DEFAULT, read from its own source rather than
     * guessed at. gtkspinbutton.c:324 initialises
     * `shadow_type = GTK_SHADOW_NONE', and its paint function
     * (:508) draws the panel's box ONLY when the shadow is not NONE -
     * otherwise it merely clears the background and stamps the two
     * arrows on it. So out of the box the arrow column has no button
     * around it.
     *
     * The arrow panel is a SEPARATE GdkWindow sized
     * ARROW_SIZE + 2 * xthickness and placed at the widget's right
     * edge (:395, :413), with the entry allocated what is left. GTK
     * gets that arithmetic right on its own - an earlier fix here set
     * a 70px usize believing the arrows were being squeezed for
     * space, and that was the wrong diagnosis: size_request already
     * adds ARROW_SIZE. The width is not the problem and the usize is
     * gone.
     *
     * GTK_SHADOW_IN matches the entry beside it, so the two read as
     * one control. */
    if (GTK_IS_SPIN_BUTTON(control))
        gtk_spin_button_set_shadow_type(GTK_SPIN_BUTTON(control),
                                        GTK_SHADOW_IN);

    gtk_box_pack_start(GTK_BOX(hbox), control, wide ? TRUE : FALSE,
                       wide ? TRUE : FALSE, 0);
    gtk_widget_show(control);

    if (after != NULL) {
        lab = gtk_label_new(after);
        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
        gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);
    }

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);
    return hbox;
}

/* ------------------------------------------------------------------ *
 * Sound Fonts                                                         *
 * ------------------------------------------------------------------ */

/* TWO SLOTS, NOT A LIST - reworked 2026-09-17 at the user's call:
 * "Most times a single font is required not two... The confusing part
 * is the last one searched first."
 *
 * THE FIRST VERSION WAS A GtkCList with Add/Remove/Move Up/Move Down
 * and three lines explaining that later fonts are searched first. That
 * explanation was the tell: a list whose semantics need a paragraph is
 * built around an implementation detail rather than around what people
 * do.
 *
 * WHAT THEY DO, from render.h:493-499 and the corpus it cites: "the
 * AWE32-era songs (106 OF 112) are written for exactly that - a GM
 * font in bank 0, the song's own in bank 1." One font usually, two
 * occasionally, never three. And the search-order rule only decides
 * anything when two fonts claim the SAME bank, which that idiom never
 * does.
 *
 * SO: two dropdowns, populated from the font directories. No ordering
 * to explain, no file picker to build, and the folder IS the list. */

static GtkWidget *g_font_gm;
static GtkWidget *g_font_song;
static GtkWidget *g_font_note;

/*
 * THE FONTS THIS PAGE SHOWS - the user's own, or THE MACHINE'S DEFAULT
 * when the user has none. Until 2026-10-08 it read the user's slots
 * alone (vlhe_fonts()), so a font set in setup-vlhe - DefaultFont0 in
 * the system file, which the synth plays at boot and for anyone who
 * has chosen none (design/54 D15) - showed here as "(none)" while
 * Status counted it and the synth played it (the Soyo, as root: the
 * user: "I had a sf set in the setup-vlhe but it wasn't set in the
 * Gui"). The same fallback every other reader uses; the flag says
 * which half the page is showing, for the note and for collect().
 */
static int        g_font_from_machine;

static int page_fonts(struct vlhe_font *fonts, int max)
{
    int from = 0, n;

    n = vlhe_fonts_effective(fonts, max, &from);
    g_font_from_machine = (n > 0 && from) ? 1 : 0;
    return n;
}
static GtkWidget *g_dir_list;
static GtkWidget *g_dir_none;   /* "none yet", shown when the list is empty */
static char       g_avail[VLHE_MAX_AVAIL][VLHE_PATH_MAX];
static int        g_navail;
/* THE FOLDERS THE MENUS WERE SCANNED WITH, one per line, so reload can
 * tell when Cancel has changed them under the menus. */
static char       g_scanned_dirs[VLHE_MAX_FONTDIRS * VLHE_PATH_MAX];

/* THE TWO HALVES OF THE SOUNDFONT FRAME, both built every time and
 * one of them shown - see on_rescan(). */
static GtkWidget *g_font_none;      /* the "no SoundFonts found" notice */
static GtkWidget *g_font_have;      /* the two dropdowns and their note */
static GtkWidget *g_font_box;       /* what holds them                  */

/* Build one font dropdown from what was found.
 *
 * "(none)" IS ALWAYS FIRST AND IS ALWAYS SELECTABLE - for the song
 * slot it is the normal state, and for the GM slot it is how a user
 * says "I have not chosen yet" rather than being stuck with whatever
 * happened to sort first. */
/* THE ITEMS ALONE, split out so a RESCAN can replace them without
 * rebuilding the widget around them - gtk_option_menu_set_menu()
 * takes exactly this. `*sel_out' receives which entry to select. */
/* Set by build_font_items() when it had to follow a font to a new path;
 * on_rescan() reads and clears it so the status line can say so. */
static int g_font_moved;

/* The scanner's case-insensitive basename rule, repeated here because
 * it is static there and this is the one other place that needs it. */
static int same_basename(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static GtkWidget *build_font_items(const char *current, int allow_none,
                                   int *sel_out)
{
    GtkWidget *menu;
    GtkWidget *item;
    int i, sel = 0;

    menu = gtk_menu_new();

    /*
     * EVERY ITEM MARKS THE PAGE DIRTY - added 2026-09-21, after the
     * user found a font selection that neither marked the page dirty
     * nor survived a restart. The dropdowns were built without any
     * handler at all, so changing one changed a widget and nothing
     * else; it LOOKED saved because the menu kept showing the choice.
     *
     * The same omission as the one the comment on midi_collect()
     * describes - sound_collect() was written first and the rest
     * followed later - except the FONT half of this page was missed
     * in that pass too.
     */
    if (allow_none) {
        item = gtk_menu_item_new_with_label(STR_MID_MENU_NONE);
        gtk_object_set_user_data(GTK_OBJECT(item), NULL);
        gtk_signal_connect(GTK_OBJECT(item), "activate",
                           GTK_SIGNAL_FUNC(on_midi_changed), NULL);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
    }

    for (i = 0; i < g_navail; i++) {
        /* THE BASENAME IS THE LABEL. A dropdown of full paths would be
         * wider than the window and the name is what identifies a
         * font - the same reasoning as the CD module's recent list. */
        const char *base = strrchr(g_avail[i], '/');

        item = gtk_menu_item_new_with_label(base ? base + 1 : g_avail[i]);
        gtk_object_set_user_data(GTK_OBJECT(item), g_avail[i]);
        gtk_signal_connect(GTK_OBJECT(item), "activate",
                           GTK_SIGNAL_FUNC(on_midi_changed), NULL);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);

        if (current != NULL && strcmp(g_avail[i], current) == 0)
            sel = i + (allow_none ? 1 : 0);
    }

    /*
     * THE CONFIGURED PATH IS GONE BUT ITS NAME IS STILL HERE - FOLLOW
     * IT, 2026-09-24, design/36 row 54.
     *
     * vlhe_fontscan() dedupes BY BASENAME, EARLIEST WINS, and user
     * folders are searched before the defaults - deliberately, so a
     * user's own copy of a font hides the system one. The user copied
     * Roland.SC-55.sf2 to /mnt/xfer, added that folder, and the copy
     * WON: /root/.vlhe/sf2/Roland.SC-55.sf2 - the exact path in the
     * config - vanished from the list. This loop matched by strcmp()
     * on the full path, found nothing, and left `sel' at 0: "(none)",
     * in both dropdowns, with the settings otherwise untouched.
     *
     * By the scanner's own rule the surviving copy IS the same font,
     * so select it. The caller reports the move; the user Applies if
     * they want the new path written.
     */
    if (sel == 0 && current != NULL) {
        const char *cb = strrchr(current, '/');
        cb = cb ? cb + 1 : current;
        for (i = 0; i < g_navail; i++) {
            const char *b2 = strrchr(g_avail[i], '/');
            b2 = b2 ? b2 + 1 : g_avail[i];
            if (same_basename(b2, cb)) {
                sel = i + (allow_none ? 1 : 0);
                g_font_moved = 1;
                break;
            }
        }
    }

    if (sel_out != NULL)
        *sel_out = sel;
    return menu;
}

/* The whole dropdown, for the initial build. */
static GtkWidget *build_font_menu(const char *current, int allow_none)
{
    GtkWidget *opt = gtk_option_menu_new();
    int sel = 0;

    gtk_option_menu_set_menu(GTK_OPTION_MENU(opt),
                             build_font_items(current, allow_none, &sel));
    gtk_option_menu_set_history(GTK_OPTION_MENU(opt), sel);
    return opt;
}

/*
 * THE LIST HOLDS THE USER'S FOLDERS ONLY - the defaults are shown
 * separately, as a label, and cannot be selected.
 *
 * THE USER'S DESIGN, 2026-09-22: "List the default search folders
 * separately - default search folders and then additional search
 * folders which is what it is now that starts empty ... That makes it
 * so a user can not try to delete the set folders."
 *
 * IT REPLACES A BUG RATHER THAN REPORTING ONE. Remove Folder on a
 * default did nothing and said "No change - the same fonts are
 * there", because vlhe_remove_font_dir() returns 0 for a path that
 * was never in the config: filtering it out of the user's Search
 * string succeeds by doing nothing, so the GUI's "that is a default"
 * message could not fire and the page went dirty over a no-op.
 *
 * Making the defaults unselectable means that path is unreachable.
 * The backend's return is still wrong and is worth fixing on its own
 * merits - it is the only caller-visible way to tell "removed" from
 * "not mine to remove" - but no button can reach it from here.
 *
 * AND vlhe_font_dirs() ALREADY ORDERS THEM: the user's first, then
 * exactly the three defaults, so the split needs no backend change
 * and cannot drift from what is actually searched.
 */
static void dirs_redraw(void)
{
    char dirs[VLHE_MAX_FONTDIRS][VLHE_PATH_MAX];
    int n, i;

    if (g_dir_list == NULL)
        return;

    gtk_clist_freeze(GTK_CLIST(g_dir_list));
    gtk_clist_clear(GTK_CLIST(g_dir_list));

    n = vlhe_font_dirs(dirs, VLHE_MAX_FONTDIRS);

    /* THE LAST THREE ARE THE DEFAULTS. Counted from the end rather
     * than compared by name, because vlhe_font_dirs() expands `~/'
     * against $HOME and a string comparison here would have to
     * repeat that. If a default cannot be expanded it is skipped
     * there, so n may be short - hence the guard. */
    n -= VLHE_N_FONTDIR_DEFAULTS;
    if (n < 0)
        n = 0;

    for (i = 0; i < n; i++) {
        char *row[1];
        row[0] = dirs[i];
        gtk_clist_append(GTK_CLIST(g_dir_list), row);
    }

    gtk_clist_thaw(GTK_CLIST(g_dir_list));

    /* AN EMPTY LIST IS THE NORMAL FIRST-RUN STATE and should say so
     * rather than being a blank box the user wonders about. */
    if (g_dir_none != NULL) {
        if (n == 0)
            gtk_widget_show(g_dir_none);
        else
            gtk_widget_hide(g_dir_none);
    }
}

/* ------------------------------------------------------------------ */
/* Adding a search directory                                          */
/* ------------------------------------------------------------------ */

/*
 * GTK 1.2 HAS NO DIRECTORY CHOOSER, so this uses the file chooser and
 * takes the directory - the user's suggestion, 2026-09-22: "would a
 * file picker work and then ignore the file and just record the
 * path?". It would, and it is what everybody did in 1999; there was
 * no GtkDirSelection to reach for.
 *
 * IT DOES NOT QUITE IGNORE THE FILE, and the difference is worth the
 * few lines. Two things can come back:
 *
 *   a DIRECTORY   the user navigated into it and pressed OK without
 *                 selecting anything. Take it as-is.
 *   a FILE        they clicked one inside the directory they meant.
 *                 Take its directory.
 *
 * Handling both means the obvious gesture works either way, where
 * "always strip the last component" would climb one level too far
 * when the selection already IS the directory - and the user would
 * have added the parent without being told.
 *
 * ANYTHING ELSE IS REFUSED RATHER THAN GUESSED AT. A path that stats
 * as neither is a typed name that does not exist, and adding it would
 * put a directory in the config that can never contain a font.
 */
static GtkWidget *g_dir_picker;   /* one at a time */

/* THE RESCAN IS A SIGNAL HANDLER DEFINED BELOW, and adding a folder
 * has to trigger one: a directory whose fonts never appear is a
 * directory the user will add twice. Declared here rather than
 * moved, so the handlers stay in the order they are connected. */
static void on_rescan(GtkWidget *w, gpointer data);
static void fonts_note_refresh(void);
static void synth_status_refresh(void);
static void minor_menu_fill(int current);
static int  minor_menu_value(void);

static void on_dir_picker_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    g_dir_picker = NULL;
}

static void on_dir_picker_cancel(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_dir_picker != NULL)
        gtk_widget_destroy(g_dir_picker);
}

static void on_dir_picker_ok(GtkWidget *w, gpointer d)
{
    const char *sel;
    char        dir[VLHE_PATH_MAX];
    struct stat st;

    (void)w; (void)d;
    if (g_dir_picker == NULL)
        return;

    sel = gtk_file_selection_get_filename(
              GTK_FILE_SELECTION(g_dir_picker));
    if (sel == NULL || *sel == '\0') {
        gtk_widget_destroy(g_dir_picker);
        return;
    }

    if (strlen(sel) >= sizeof dir) {
        report(STR_MID_MSG_PATH_TOO_LONG);
        gtk_widget_destroy(g_dir_picker);
        return;
    }
    strcpy(dir, sel);

    if (stat(dir, &st) != 0) {
        report(STR_MID_MSG_PATH_DOES_NOT_EXIST);
        gtk_widget_destroy(g_dir_picker);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* A FILE: take the directory holding it. */
        char *slash = strrchr(dir, '/');

        if (slash == NULL) {
            report(STR_MID_MSG_CANNOT_TELL_WHICH_FOLDER);
            gtk_widget_destroy(g_dir_picker);
            return;
        }
        /* "/thing" -> "/", not "" - a font in the root is unlikely
         * and an empty path would be added silently. */
        if (slash == dir)
            dir[1] = '\0';
        else
            *slash = '\0';
    }

    /*
     * DROP A TRAILING SLASH - found 2026-09-22 in a target journal,
     * which recorded
     *
     *     start daemon vmidid -s /mnt/xfer//Roland.SC-55.sf2 -p 64
     *
     * after the user added `/mnt/xfer' as a font folder.
     *
     * gtk_file_selection_get_filename() RETURNS A DIRECTORY WITH ONE
     * ON THE END. That is GTK behaving normally - the dialog's entry
     * holds what it would open - and this stored it verbatim, so the
     * config carried `/mnt/xfer/' and vlhe_fontscan.c then appended a
     * separator of its own.
     *
     * HARMLESS AND STILL WRONG. POSIX collapses `//', so every font
     * loaded correctly and nothing failed - but the doubled path went
     * into the config, the change journal and the daemon's argv,
     * where it reads as a defect to anyone who sees it and would send
     * someone looking for a bug that is not there.
     *
     * FIXED AT THE SOURCE rather than only in the join, so the CONFIG
     * is clean too - a path already stored with a slash would
     * otherwise keep producing it. The scanner is guarded as well,
     * because this is not the only writer of that setting.
     *
     * `/' ITSELF IS LEFT ALONE. Stripping it would give the empty
     * string, which names nothing and would be added silently - the
     * same reasoning the file case above already applies.
     */
    {
        size_t dl = strlen(dir);

        while (dl > 1 && dir[dl - 1] == '/')
            dir[--dl] = '\0';
    }

    /*
     * AND RESOLVE IT, which the slash strip above does not do.
     *
     * FOUND ON TARGET 2026-09-23: the folder list held
     *
     *     /mnt/xfer/..
     *     /mnt/xfer/..
     *     /mnt/xfer/.
     *
     * because the picker's DIRECTORY LIST contains `./' and `../' as
     * entries and selecting one builds a path that is perfectly
     * legal, resolves somewhere else, and is not equal to any
     * existing entry - so the duplicate check passes and it is added
     * again. `/mnt/xfer/..' is `/', so the scan then walks the whole
     * root looking for soundfonts.
     *
     * realpath() COLLAPSES `.' AND `..', follows symlinks, and makes
     * the result canonical - which is what the duplicate check needed
     * all along: two spellings of one directory now compare equal.
     *
     * THE BUFFER IS THE TRAP AND IS WHY THIS IS NOT ONE LINE.
     * realpath() writes up to PATH_MAX, which is 4095 on this target,
     * and VLHE_PATH_MAX is 256. Handing it `dir' directly is the
     * textbook stack smash. So it resolves into a full-size buffer
     * and the result is bounds-checked before it comes back.
     *
     * A FAILURE LEAVES `dir' ALONE. realpath() fails when a component
     * does not exist, and the stat() above has already established
     * that this one does - but a race or a permission problem should
     * fall back to the literal path rather than refuse the folder.
     */
    {
        char resolved[4096];

        if (realpath(dir, resolved) != NULL
            && strlen(resolved) < sizeof dir) {
            strcpy(dir, resolved);
        }
    }

    gtk_widget_destroy(g_dir_picker);

    /* THE BACKEND DECIDES WHETHER IT IS ACCEPTABLE - it refuses a
     * duplicate and a list that is already full, and it is the thing
     * that persists it. This only has to say what happened. */
    /*
     * `/' IS NOT A FONT FOLDER, and it is reachable in one click -
     * pick `../' from the picker in a top-level directory and
     * realpath() resolves it there, correctly. Scanning the whole
     * filesystem for .sf2 files would take minutes on a CF card and
     * is never what someone meant.
     *
     * REFUSED RATHER THAN SILENTLY DROPPED, so a user who did mean it
     * learns why instead of wondering where their folder went.
     */
    if (strcmp(dir, "/") == 0) {
        report(STR_MID_MSG_WHOLE_FILESYSTEM_PICK_FOLDER);
        return;
    }

    {
        int rc = vlhe_add_font_dir(dir);

        /* THE BACKEND NOW REFUSES WHAT THIS COMMENT ALWAYS CLAIMED IT
         * DID - row 56. Named separately, because "already listed" and
         * "nothing in it" want different next moves. */
        if (rc == -2) {
            report(STR_MID_MSG_FOLDER_ALREADY_SEARCHED);
            return;
        }
        if (rc == -3) {
            char m[VLHE_PATH_MAX + 64];
            sprintf(m, FMT_MID_NO_SOUNDFONT_IN_FOLDER,
                    dir);
            report(m);
            return;
        }
        if (rc != 0) {
            report(STR_MID_MSG_COULD_NOT_ADD_FOLDER);
            return;
        }
    }

    dirs_redraw();

    /*
     * IT IS A PENDING EDIT LIKE ANY OTHER, so the page is dirty and
     * the sidebar says so - missing until 2026-09-22, when the user
     * found that adding a folder raised no asterisk. The change sits
     * in memory until Apply, which is precisely what the marker is
     * for.
     */
    mark_dirty();

    /*
     * AND RESCAN, SO ITS FONTS APPEAR AT ONCE - a directory whose
     * contents do not show up is one the user adds twice.
     *
     * NO "Folder added" MESSAGE AFTER THIS, deliberately: on_rescan()
     * reports last and says something more useful than we could -
     * "Found more SoundFonts", or "No SoundFonts found" for a folder
     * that has none, which is the case worth knowing about. Saying
     * "Folder added" first would only be overwritten.
     */
    on_rescan(NULL, NULL);
}

/*
 * REMOVE THE SELECTED FOLDER. The list is a GtkCList and its
 * selection is what we act on - there is no second place a folder
 * can be named, which is the point of not having an entry box here.
 */
static void on_dir_remove(GtkWidget *w, gpointer d)
{
    char dirs[VLHE_MAX_FONTDIRS][VLHE_PATH_MAX];
    GList *sel;
    int row, n;

    (void)w; (void)d;

    if (g_dir_list == NULL)
        return;

    sel = GTK_CLIST(g_dir_list)->selection;
    if (sel == NULL) {
        report(STR_MID_MSG_SELECT_FOLDER_REMOVE);
        return;
    }
    row = GPOINTER_TO_INT(sel->data);

    /* THE PATH FROM THE BACKEND, not from the widget's text - the
     * list is drawn from vlhe_font_dirs() and re-reading it keeps
     * the two from drifting if a label is ever shortened for
     * display. */
    n = vlhe_font_dirs(dirs, VLHE_MAX_FONTDIRS);

    /* THE LIST HOLDS THE USER'S FOLDERS ONLY since 2026-09-22, so a
     * row index IS an index into the first part of this array and the
     * defaults at the end are unreachable. The bound is what enforces
     * that here - dirs_redraw() enforces it in the widget. */
    n -= VLHE_N_FONTDIR_DEFAULTS;
    if (row < 0 || row >= n)
        return;

    /*
     * THE RETURN IS NOT CHECKED, AND THAT IS DELIBERATE RATHER THAN
     * AN OVERSIGHT. vlhe_remove_font_dir() returns 0 for a path that
     * was not in the config - filtering out something absent succeeds
     * by doing nothing - so it cannot distinguish "removed" from "not
     * mine to remove", and the old `!= 0' test here never fired.
     *
     * That produced the bug this change replaces: Remove on a default
     * silently marked the page dirty and rescanned, leaving the user
     * with "No change - the same fonts are there" from a DIFFERENT
     * button's handler. The defaults are now unreachable, so the
     * distinction no longer has a caller - but the backend's return
     * is still worth fixing on its own merits, and until it is, a
     * check here would be checking something that cannot vary.
     */
    (void) vlhe_remove_font_dir(dirs[row]);

    dirs_redraw();
    mark_dirty();               /* pending until Apply, like Add */
    on_rescan(NULL, NULL);      /* the fonts it held are gone */
}

static void on_dir_add(GtkWidget *w, gpointer d)
{
    GtkWidget *sel;

    (void)w; (void)d;

    if (g_dir_picker != NULL) {
        gdk_window_raise(g_dir_picker->window);
        return;
    }

    sel = gtk_file_selection_new(
              STR_MID_TITLE_SELECT_FOLDER);
    vlhe_filesel_fit(sel);         /* a long path must not widen it */

    /* NO Create / Rename / Delete - see vlhe_mod_render.c's picker
     * for why. This one is a chooser too. */
    gtk_file_selection_hide_fileop_buttons(GTK_FILE_SELECTION(sel));
    g_dir_picker = sel;

    /* THE FILE LIST IS HIDDEN, because a file is not what is being
     * chosen and showing one invites picking it. The directory list
     * stays, which is what the user navigates with - and a file can
     * still be typed into the entry if someone prefers that route,
     * which on_dir_picker_ok() handles. */
    gtk_widget_hide(GTK_FILE_SELECTION(sel)->file_list->parent);

    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->ok_button),
                       "clicked",
                       GTK_SIGNAL_FUNC(on_dir_picker_ok), NULL);
    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->cancel_button),
                       "clicked",
                       GTK_SIGNAL_FUNC(on_dir_picker_cancel), NULL);
    gtk_signal_connect(GTK_OBJECT(sel), "destroy",
                       GTK_SIGNAL_FUNC(on_dir_picker_destroy), NULL);

    gtk_widget_show(sel);
}

/* SHOW THE HALF THAT APPLIES. Called after every scan. */
static void
fonts_sync_visibility(void)
{
    if (g_font_none != NULL) {
        if (g_navail == 0)
            gtk_widget_show(g_font_none);
        else
            gtk_widget_hide(g_font_none);
    }
    if (g_font_have != NULL) {
        if (g_navail == 0)
            gtk_widget_hide(g_font_have);
        else
            gtk_widget_show(g_font_have);
    }
}

/*
 * RESCAN THE SOUNDFONT FOLDERS.
 *
 * WHY IT IS NEEDED: the scan ran once, at build time, so a .sf2
 * copied in while the program was open did not appear until a
 * restart (the user, 86Box 2026-09-19). Copying a font in IS the
 * common first-run action - nothing on Corel installs one - so
 * needing a restart to see it is exactly the wrong moment to ask for
 * one.
 *
 * IT REBUILDS THE TWO MENUS IN PLACE rather than the page: a
 * GtkOptionMenu's menu can be replaced with
 * gtk_option_menu_set_menu(), which is what build_font_menu()
 * returns the contents for.
 */
/* SCAN THE FOLDERS AND REBUILD BOTH FONT MENUS from the result - the
 * body of Rescan without its report, so Cancel can do the same. It
 * records the folders it scanned with (g_scanned_dirs), one per line,
 * so midi_reload() can tell when they have changed under the menus. */
static void
fonts_rescan_menus(void)
{
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    const char *gm = NULL, *song = NULL;
    int n, i;

    g_navail = vlhe_available_fonts(g_avail, VLHE_MAX_AVAIL);
    {
        char dirs[VLHE_MAX_FONTDIRS][VLHE_PATH_MAX];
        int  nd = vlhe_font_dirs(dirs, VLHE_MAX_FONTDIRS), k;

        g_scanned_dirs[0] = '\0';
        for (k = 0; k < nd; k++) {
            strcat(g_scanned_dirs, dirs[k]);
            strcat(g_scanned_dirs, "\n");
        }
    }

    /* KEEP WHAT IS SELECTED. A rescan must not silently change which
     * font the synth would be given - the user pressed it to find a
     * new file, not to lose their choice. */
    n = page_fonts(fonts, VLHE_MAX_FONTS);
    for (i = 0; i < n; i++) {
        if (fonts[i].bank == 0 && gm == NULL)
            gm = fonts[i].path;
        else if (fonts[i].bank == 1 && song == NULL)
            song = fonts[i].path;
    }

    if (g_font_gm != NULL) {
        int sel = 0;
        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_font_gm),
                                 build_font_items(gm, 1, &sel));
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_font_gm), sel);
    }
    if (g_font_song != NULL) {
        int sel = 0;
        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_font_song),
                                 build_font_items(song, 1, &sel));
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_font_song), sel);
    }

    fonts_sync_visibility();
    fonts_note_refresh();
}

static void
on_rescan(GtkWidget *w, gpointer data)
{
    int before = g_navail;

    (void) w;
    (void) data;

    fonts_rescan_menus();

    /*
     * "No change - the same fonts are there" WAS PRINTED WHILE THE
     * SELECTION WAS BEING LOST - row 54. The count had not moved (one
     * copy replaced another), so the status line said success under
     * the same action that emptied both dropdowns. The moved case now
     * says what happened; the user Applies if they want it kept.
     */
    if (g_font_moved) {
        g_font_moved = 0;
        report(STR_MID_MSG_FONT_NOW_FOUND_DIFFERENT);
    } else if (g_navail == before)
        report(g_navail == 0 ? STR_MID_MSG_NO_SOUNDFONTS_FOUND
                             : STR_MID_MSG_NO_CHANGE_SAME_FONTS);
    else if (g_navail > before)
        report(STR_MID_MSG_FOUND_MORE_SOUNDFONTS);
    else
        report(STR_MID_MSG_SOME_SOUNDFONTS_ARE_GONE);
}

/*
 * RE-APPLY THE SOUNDFONT FRAME'S VISIBILITY.
 *
 * The shell calls gtk_widget_show_all(), which is recursive and
 * re-shows the half this page deliberately hid - so without this the
 * window opens with BOTH the "no SoundFonts found" notice and the
 * two dropdowns. Same contract as volume_sync_visibility().
 */
void midi_sync_visibility(void)
{
    fonts_sync_visibility();
    /* AND THE FOLDER LIST'S "(none yet)" LINE - the user, 2026-10-01,
     * with a saved folder in the list AND the line above it saying
     * there were none: dirs_redraw() hid the label at build and the
     * shell's show_all() showed it again. Redrawn here, after. */
    dirs_redraw();
}

/*
 * WHAT THE NOTE UNDER THE DROPDOWNS SAYS - FROM THE FILE, NOT FROM `ok'.
 *
 * It used to print "<font> could not be loaded." whenever font.ok was
 * 0 - and the backend sets ok to 0 ALWAYS, on purpose: "`ok' is the
 * daemon's answer and we do not have it" (vlhe_backend.c). So every
 * configured font was reported as broken from the moment the page was
 * built, and the user read the line as a consequence of the folder
 * they had just added (design/36 row 54). Unknown is not failure.
 *
 * What can honestly be said here is whether the file is THERE. It is
 * checked directly, and re-checked on rescan and when the machine
 * changes.
 */
static void fonts_note_refresh(void)
{
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    struct stat st;
    int n, i;

    if (g_font_note == NULL)
        return;
    n = page_fonts(fonts, VLHE_MAX_FONTS);
    for (i = 0; i < n; i++) {
        if (stat(fonts[i].path, &st) != 0) {
            char m[VLHE_PATH_MAX + 64];

            /* ONE LINE, THE PATH ALONE - the user, 2026-10-07; it was
             * the name and then the path again, two lines at 17 px. */
            sprintf(m, FMT_MID_FONT_MISSING, fonts[i].path);
            gtk_label_set_text(GTK_LABEL(g_font_note), m);
            return;
        }
    }
    /* THE MACHINE'S DEFAULT IS SAID, AND NAMED, so a slot showing a
     * font the user never chose is not read as their own choice, and
     * a default outside the searched folders - which the dropdown
     * cannot show, so it reads "(none)" - is still visible here
     * (page_fonts). The file's name, not its path: a path made this
     * page 1083 px wide once (the Missing line above). */
    if ((g_font_from_machine || vlhe_fonts_as_root()) && n > 0) {
        char m[VLHE_PATH_MAX + 200];
        const char *base = strrchr(fonts[0].path, '/');

        /* TWO SENTENCES, BY WHO IS ASKING - the user, 2026-10-08:
         * "Doesnt root set the default soundfonts so those two should
         * match." They are one setting: root reads and writes
         * DefaultFont* (vlhe_backend.c), so whatever root's slots
         * show IS the machine's default, and the line says so every
         * time root has a font. For anyone else the default is what
         * they get until they choose, and the line says that. */
        sprintf(m, vlhe_fonts_as_root() ? FMT_MID_LABEL_FONT_MACHINE_ROOT
                                        : FMT_MID_LABEL_FONT_FROM_MACHINE,
                base != NULL ? base + 1 : fonts[0].path);
        gtk_label_set_text(GTK_LABEL(g_font_note), m);
        return;
    }
    gtk_label_set_text(GTK_LABEL(g_font_note), "");
}

static GtkWidget *build_fonts(void)
{
    struct vlhe_font fonts[VLHE_MAX_FONTS];
    const char *gm = NULL;
    const char *song = NULL;
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *scroll;
    GtkWidget *hbox;
    GtkWidget *b;
    GtkWidget *note;
    char *titles[1];
    int n, i;

    g_navail = vlhe_available_fonts(g_avail, VLHE_MAX_AVAIL);

    /* WHICH FONT IS IN WHICH SLOT, from the stack vmidid was given. */
    n = page_fonts(fonts, VLHE_MAX_FONTS);
    for (i = 0; i < n; i++) {
        if (fonts[i].bank == 0 && gm == NULL)
            gm = fonts[i].path;
        else if (fonts[i].bank == 1 && song == NULL)
            song = fonts[i].path;
    }

    outer = gtk_vbox_new(FALSE, 0);

    /* ---- the two slots -------------------------------------------- */

    frame = gtk_frame_new(STR_MID_FRAME_SOUNDFONTS);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * BOTH HALVES ARE BUILT, AND ONE IS SHOWN. The page used to
     * branch here and build only the half that applied, which meant
     * a font copied in after startup needed the program restarted to
     * appear (the user, 86Box 2026-09-19). Building both lets
     * on_rescan() swap them with two calls and no rebuild.
     *
     * g_font_box holds them; see the Rescan button below.
     */
    g_font_box = vbox;

    {
        /* SAY WHERE TO PUT ONE. Nothing on Corel installs a SoundFont -
         * no package ships one and awesfx only says where to look - so
         * an empty list is the FIRST thing a new user sees, and an
         * empty dropdown tells them nothing. */
        note = gtk_label_new(
            STR_MID_LABEL_NO_SOUNDFONTS_FOUND_SYNTH);
        gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
        /* FLOWED - one paragraph, wrapped at the pane's text width
         * (vlhe_layout.c). Hand breaks made it look fixed. */
        vlhe_layout_wrap(note);
        gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
        g_font_none = note;
    }

    {
        GtkWidget *have = gtk_vbox_new(FALSE, 6);

        gtk_box_pack_start(GTK_BOX(vbox), have, FALSE, FALSE, 0);
        g_font_have = have;
        vbox = have;            /* the rows below go inside it */

        g_font_gm = build_font_menu(gm, 1);
        labelled_row(vbox, STR_MID_LABEL_GENERAL_MIDI, g_font_gm, NULL, 1);

        g_font_song = build_font_menu(song, 1);
        labelled_row(vbox, STR_MID_LABEL_SONG_FONT, g_font_song, NULL, 1);

        /* NOT CARRIED INTO THE FIRST RESCAN - design/47 M6. The build
         * may have followed a moved font (row 54) and set the flag;
         * nobody reports at build, and a Rescan minutes later would
         * then announce a move it did not find. */
        g_font_moved = 0;

        /* THE SECOND SLOT EXPLAINED IN ONE LINE, where the list needed
         * three. */
        note = gtk_label_new(
            STR_MID_LABEL_MOST_MUSIC_NEEDS_ONLY);
        gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
        /* FLOWED - one paragraph, wrapped at the pane's text width
         * (vlhe_layout.c). Hand breaks made it look fixed. */
        vlhe_layout_wrap(note);
        gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
        gtk_widget_show(note);

        /* A FONT THAT DID NOT LOAD IS SAID. Otherwise the user sees it
         * selected and hears nothing from it. */
        g_font_note = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(g_font_note), 0.0, 0.5);
        /* WRAPPED: it names a file and its path - unwrapped, a long
         * one made this whole page 1083 px wide (2026-10-07). */
        vlhe_layout_wrap(g_font_note);
        gtk_box_pack_start(GTK_BOX(vbox), g_font_note, FALSE, FALSE, 4);
        gtk_widget_show(g_font_note);

        fonts_note_refresh();
    }

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- where they are looked for --------------------------------- */

    /*
     * TWO LISTS, NOT ONE - the user's design, 2026-09-22. The
     * defaults are always searched and cannot be removed, so they are
     * shown as text; only the folders a user added go in a list with
     * a Remove button beside it. dirs_redraw() has the reasoning.
     *
     * THE DEFAULTS ARE A LABEL RATHER THAN A SECOND CLIST because
     * they are three fixed strings that never change - a scrolling,
     * selectable widget would invite exactly the click this change
     * exists to prevent.
     */
    frame = gtk_frame_new(STR_MID_FRAME_ALWAYS_SEARCHED);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 2);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    {
        /*
         * ONE LINE, COMMA-SEPARATED, WITH ~ AS WRITTEN - the user,
         * 2026-10-07: "/usr/share/sounds/sf2, /usr/local/share/sounds/sf2,
         * ~/.vlhe/sf2". It was a label per folder with `~/' expanded to
         * the home directory; three lines became one, and `~' is the
         * shorter and familiar spelling of the folder that is searched.
         * Wrapped, so a narrow window still shows all of it.
         */
        char line[3 * VLHE_PATH_MAX];
        GtkWidget *lab;
        int i;

        line[0] = '\0';
        for (i = 0; i < VLHE_N_FONTDIR_DEFAULTS; i++) {
            const char *d = vlhe_fontscan_defaults[i];

            if (strlen(line) + strlen(d) + 3 >= sizeof line)
                break;
            if (i > 0)
                strcat(line, ", ");
            strcat(line, d);
        }
        lab = gtk_label_new(line);
        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
        vlhe_layout_wrap(lab);
        gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);
    }

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    frame = gtk_frame_new(STR_MID_FRAME_ALSO_SEARCHED);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, TRUE, TRUE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* SHOWN ONLY WHEN THE LIST IS EMPTY, which is the first-run
     * state - a blank box says nothing about whether it is working. */
    g_dir_none = gtk_label_new(STR_MID_LABEL_NONE_YET_USE_ADD);
    gtk_misc_set_alignment(GTK_MISC(g_dir_none), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_dir_none, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_usize(scroll, -1, 70);

    titles[0] = STR_MID_LABEL_FOLDER_COLUMN;
    g_dir_list = gtk_clist_new_with_titles(1, titles);
    gtk_clist_set_selection_mode(GTK_CLIST(g_dir_list),
                                 GTK_SELECTION_SINGLE);
    gtk_clist_set_column_width(GTK_CLIST(g_dir_list), 0, 400);
    gtk_clist_column_titles_passive(GTK_CLIST(g_dir_list));
    /* NO "Folder" HEADING - a one-column list of folders under a frame
     * titled "Also Searched" says it already (the user, 2026-10-07). */
    gtk_clist_column_titles_hide(GTK_CLIST(g_dir_list));
    gtk_container_add(GTK_CONTAINER(scroll), g_dir_list);
    gtk_widget_show(g_dir_list);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    gtk_widget_show(scroll);

    hbox = gtk_hbox_new(FALSE, 6);
    b = vlhe_tipped(gtk_button_new_with_label(STR_MID_BTN_ADD_FOLDER), STR_MID_BTN_ADD_FOLDER_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_dir_add), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    gtk_widget_show(b);

    /*
     * REMOVE, BESIDE ADD - missing until 2026-09-22, when the user
     * added `/dev/input' while testing the picker and found there
     * was no way to take it out again. The backend half
     * (vlhe_remove_font_dir) had existed all along; only the button
     * was never built.
     *
     * IT REFUSES THE THREE DEFAULTS, and so does the backend - they
     * are not in the config string at all, so removing one is a
     * no-op there. Saying so is better than a button that appears to
     * work and does nothing.
     */
    b = vlhe_tipped(gtk_button_new_with_label(STR_MID_BTN_REMOVE_FOLDER), STR_MID_BTN_REMOVE_FOLDER_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_dir_remove), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    gtk_widget_show(b);

    /* RESCAN SITS BESIDE THE FOLDER LIST, not beside the dropdowns,
     * because this is where a user has just been told which folders
     * are searched - so it reads as "look in those again". */
    b = vlhe_tipped(gtk_button_new_with_label(STR_MID_BTN_RESCAN), STR_MID_BTN_RESCAN_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_rescan), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    gtk_widget_show(b);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    gtk_widget_show(outer);

    dirs_redraw();
    return outer;
}

/* ------------------------------------------------------------------ *
 * Options - the synth                                                 *
 * ------------------------------------------------------------------ */

static GtkWidget *build_options(void)
{
    struct vlhe_synth sy;
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *menu;
    GtkWidget *item;
    GtkObject *adj;
    GtkWidget *note;

    vlhe_synth(&sy);

    outer = gtk_vbox_new(FALSE, 0);

    frame = gtk_frame_new(STR_MID_FRAME_SYNTHESIS);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* VOICES - the one that matters on a slow machine. design/p3
     * records the P1 struggling at 16 on D_INTRO. */
    adj = gtk_adjustment_new((gfloat) sy.voices, 1.0,
                             (gfloat) VLHE_MAX_VOICES, 1.0, 8.0, 0.0);
    g_voices = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 0.0, 0);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(g_voices), FALSE);
    vlhe_layout_digits(g_voices, 4);    /* Render Advanced's width */
    gtk_signal_connect(adj, "value_changed",
                       GTK_SIGNAL_FUNC(on_midi_changed), NULL);
    labelled_row(vbox, STR_MID_LABEL_MAXIMUM_VOICES, g_voices,
                 STR_MID_LABEL_VOICES_NOTE, 0);

    /* AUTOMATIC REDUCTION, directly under the number it moves - built
     * 2026-10-02, design/21 section 16. On by default, as TiMidity's
     * is; the maximum above is the most it will ever restore to. */
    g_autovoices = vlhe_tipped(gtk_check_button_new_with_label(
        STR_MID_CHECK_AUTO_VOICES), STR_MID_CHECK_AUTO_VOICES_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_autovoices),
                                 sy.auto_voices ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_autovoices), "toggled",
                       GTK_SIGNAL_FUNC(on_midi_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_autovoices, FALSE, FALSE, 0);
    gtk_widget_show(g_autovoices);

    /* GAIN as a percentage, because 0.001..2.0 as a float is a worse
     * control than 100% as an integer - and design/07 keeps floats out
     * of anything that reaches a config line anyway. */
    adj = gtk_adjustment_new((gfloat)(sy.gain_milli / 10), 1.0, 200.0,
                             5.0, 25.0, 0.0);
    g_gain = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 0.0, 0);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(g_gain), FALSE);
    vlhe_layout_digits(g_gain, 4);
    gtk_signal_connect(adj, "value_changed",
                       GTK_SIGNAL_FUNC(on_midi_changed), NULL);
    labelled_row(vbox, STR_MID_LABEL_MASTER_GAIN, g_gain,
                 STR_MID_LABEL_GAIN_NOTE, 0);

    /* THE VOLUME LAW. design/09 records BOTH names as unsatisfactory -
     * "linear is true of the arithmetic and says nothing to a user" -
     * so the menu describes what each DOES and keeps the flag name in
     * brackets for anyone matching it to the command line. */
    g_law = gtk_option_menu_new();
    menu = gtk_menu_new();
    /*
     * THE VALUE RIDES ON THE ITEM - design/47 M1, fixed 2026-09-30.
     * midi_collect() reads these two menus by user_data, as it does
     * the rate, and these items were built WITHOUT any: both reads
     * came back NULL, so every OK on every page saved law 0 and
     * filter 0 whatever was showing, and `-L linear' could never
     * reach the daemon from the GUI.
     */
    item = gtk_menu_item_new_with_label(
        STR_MID_MENU_SOUNDFONT_STANDARD_SPEC);
    gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) VLHE_LAW_SPEC);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    item = gtk_menu_item_new_with_label(
        STR_MID_MENU_GENTLER_SOUND_BLASTER_ERA);
    gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) VLHE_LAW_LINEAR);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    /* THE SIGNAL GOES ON THE MENU, NOT THE OPTION MENU. GTK 1.2
     * gives GtkOptionMenu no "changed" of its own; the selection
     * arrives as "deactivate" on the menu it holds. */
    gtk_signal_connect(GTK_OBJECT(menu), "deactivate",
                       GTK_SIGNAL_FUNC(on_midi_changed), NULL);
    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_law), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_law),
                                sy.law == VLHE_LAW_LINEAR ? 1 : 0);
    labelled_row(vbox, STR_MID_LABEL_VOLUME_CURVE, g_law, NULL, 0);

    note = gtk_label_new(
        STR_MID_LABEL_GENTLER_CURVE_WHAT_OPL3);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    /* THE VELOCITY FILTER - which spec a font is played against. */
    g_filter = gtk_option_menu_new();
    menu = gtk_menu_new();
    /* user_data on every item - see the law menu above (M1). */
    item = gtk_menu_item_new_with_label(STR_MID_MENU_AWE32_AWE);
    gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) VLHE_VF_AWE);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    item = gtk_menu_item_new_with_label(STR_MID_MENU_SOUNDFONT_2_01);
    gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) VLHE_VF_201);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    item = gtk_menu_item_new_with_label(STR_MID_MENU_SOUNDFONT_2_04);
    gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) VLHE_VF_204);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    item = gtk_menu_item_new_with_label(STR_MID_MENU_NONE_FLUIDSYNTH);
    gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) VLHE_VF_NONE);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    /* THE SIGNAL GOES ON THE MENU, NOT THE OPTION MENU. GTK 1.2
     * gives GtkOptionMenu no "changed" of its own; the selection
     * arrives as "deactivate" on the menu it holds. */
    gtk_signal_connect(GTK_OBJECT(menu), "deactivate",
                       GTK_SIGNAL_FUNC(on_midi_changed), NULL);
    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_filter), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_filter), sy.filter);
    labelled_row(vbox, STR_MID_LABEL_VELOCITY_FILTER, g_filter, NULL, 0);

    /*
     * THE SAMPLE RATE - design/36 row 90, the user's ask.
     *
     * THE MOST EFFECTIVE SETTING ON A SLOW MACHINE, and the page had
     * no way to reach it. It changes the DEADLINE rather than the
     * work: a 256-frame block is 5 ms of wall clock at 44100 and
     * 11 ms at 22050, so halving the rate roughly doubles the time
     * available to render it. Measured on 86Box - 795 xruns at the
     * defaults against 1 at 22050 with 32 voices.
     *
     * A LIST, NOT A SPIN BUTTON. These are the rates the hardware of
     * this era actually does; a free number invites 48000 on a card
     * that clamps, and the clamp is silent. The same argument as the
     * Device page's dropdowns (row 71).
     *
     * THE VALUE IS IN `user_data', NOT THE MENU POSITION - a rate is
     * not an index, and the law and filter menus above can use
     * position only because their enums happen to start at 0.
     */
    {
        static const int rates[] = VLHE_SYNTH_RATES;
        static const char *const names[VLHE_SYNTH_NRATES] = {
            STR_MID_RATE_44100,
            STR_MID_RATE_32000,
            STR_MID_RATE_22050,
            STR_MID_RATE_11025
        };
        int k, sel = 0;

        g_rate = gtk_option_menu_new();
        menu = gtk_menu_new();
        for (k = 0; k < VLHE_SYNTH_NRATES; k++) {
            item = gtk_menu_item_new_with_label(names[k]);
            gtk_object_set_user_data(GTK_OBJECT(item),
                                     (gpointer) (long) rates[k]);
            gtk_menu_append(GTK_MENU(menu), item);
            gtk_widget_show(item);
            if (sy.rate == rates[k])
                sel = k;
        }
        gtk_signal_connect(GTK_OBJECT(menu), "deactivate",
                           GTK_SIGNAL_FUNC(on_midi_changed), NULL);
        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_rate), menu);
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_rate), sel);
        labelled_row(vbox, STR_MID_LABEL_SAMPLE_RATE, g_rate, NULL, 0);
    }

    note = gtk_label_new(
        STR_MID_LABEL_LOWERING_RATE_FIRST_THING);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    /* REVERB AND CHORUS, ONE ROW - two switches since 2026-10-05 (the
     * user: "build the reverb chorus split"), side by side so the page
     * is no taller than with the one box it replaces. */
    {
        GtkWidget *fxrow = gtk_hbox_new(FALSE, 16);

        g_reverb = vlhe_tipped(gtk_check_button_new_with_label(
            STR_MID_CHECK_REVERB), STR_MID_CHECK_REVERB_TIP);
        gtk_signal_connect(GTK_OBJECT(g_reverb), "toggled",
                           GTK_SIGNAL_FUNC(on_midi_changed), NULL);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_reverb),
                                     sy.reverb ? TRUE : FALSE);
        gtk_box_pack_start(GTK_BOX(fxrow), g_reverb, FALSE, FALSE, 0);
        gtk_widget_show(g_reverb);

        g_chorus = vlhe_tipped(gtk_check_button_new_with_label(
            STR_MID_CHECK_CHORUS), STR_MID_CHECK_CHORUS_TIP);
        gtk_signal_connect(GTK_OBJECT(g_chorus), "toggled",
                           GTK_SIGNAL_FUNC(on_midi_changed), NULL);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_chorus),
                                     sy.chorus ? TRUE : FALSE);
        gtk_box_pack_start(GTK_BOX(fxrow), g_chorus, FALSE, FALSE, 0);
        gtk_widget_show(g_chorus);

        gtk_box_pack_start(GTK_BOX(vbox), fxrow, FALSE, FALSE, 0);
        gtk_widget_show(fxrow);
    }

    note = gtk_label_new(
        STR_MID_LABEL_TURNING_EFFECTS_OFF_ESCAPE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    /* THE MODULATION ENVELOPE'S MODE - design/54 D28, and a drop-down
     * rather than a checkbox since D70 (the user, 2026-10-05): Fast
     * first, the default; To the SoundFont spec the other. The tip
     * says what was measured and promises no more. [Midi Settings]
     * ModEnv, vmidid -M. user_data carries the value, as the filter
     * menu above. */
    {
        GtkWidget *mitem;
        GtkWidget *mmenu;

        g_modenv = vlhe_tipped(gtk_option_menu_new(),
                               STR_MID_MENU_MODENV_TIP);
        mmenu = gtk_menu_new();
        mitem = gtk_menu_item_new_with_label(STR_MID_MENU_MODENV_FAST);
        gtk_object_set_user_data(GTK_OBJECT(mitem),
                                 (gpointer)(long) VLHE_MODENV_FAST);
        gtk_menu_append(GTK_MENU(mmenu), mitem);
        gtk_widget_show(mitem);
        mitem = gtk_menu_item_new_with_label(STR_MID_MENU_MODENV_SPEC);
        gtk_object_set_user_data(GTK_OBJECT(mitem),
                                 (gpointer)(long) VLHE_MODENV_REFERENCE);
        gtk_menu_append(GTK_MENU(mmenu), mitem);
        gtk_widget_show(mitem);
        gtk_signal_connect(GTK_OBJECT(mmenu), "deactivate",
                           GTK_SIGNAL_FUNC(on_midi_changed), NULL);
        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_modenv), mmenu);
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_modenv),
                                    sy.modenv == VLHE_MODENV_REFERENCE
                                    ? 1 : 0);
        labelled_row(vbox, STR_MID_LABEL_MODENV, g_modenv, NULL, 0);
    }

    /* NO STATUS LINE HERE ANY MORE - 2026-10-07, the user's design:
     * whether the synth is loaded is the button row's line (vlhe_state.c),
     * and "running with other settings - reload" is on Status. The
     * variable stays NULL; its refresh already checks. */

    synth_status_refresh();

    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ *
 * Driver                                                              *
 * ------------------------------------------------------------------ */

/* THE SHELL'S UNSAVED-SETTINGS QUESTION, as the Status page's Restart
 * buttons ask it - design/47 M6: this button restarted the synth on the
 * settings on disk while this very page showed unsaved edits to them. */
static int (*g_before_restart_cb)(const char *daemon);

void midi_set_before_restart_cb(int (*cb)(const char *daemon))
{
    g_before_restart_cb = cb;
}

static void on_restart(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;

    if (g_before_restart_cb != NULL && !g_before_restart_cb("vmidid"))
        return;
    if (vlhe_restart_synth() != 0) {
        report(STR_MID_MSG_COULD_NOT_RESTART_SYNTH);
        return;
    }

    report(STR_MID_MSG_SYNTH_RESTARTED);
}

/*
 * WHAT THE DRIVER LINE SAYS, RE-ASKED - "vmidi is not loaded" was
 * computed once in build_driver() and never again, so after a Load
 * the page still said it (design/36 rows 44 and 57 - the same bug on
 * the CD page blocked the "Reload to apply" test). Now called from the
 * constructor AND from midi_machine(), which the shell fires when the
 * page is shown and after Load/Unload.
 */
/*
 * THE MINOR MENU - its six entries, plus the configured value if that
 * is one the backend accepts but the menu does not carry. Rebuilt
 * from scratch on reload so a hand-edited value shows up.
 */
static void minor_menu_fill(int current)
{
    static const struct { int minor; const char *label; } entry[] = {
        {  7, STR_MID_MINOR_7 },
        { 10, STR_MID_MINOR_10 },
        { 11, STR_MID_MINOR_11 },
        { 12, STR_MID_MINOR_12 },
        { 13, STR_MID_MINOR_13 },
        { 14, STR_MID_MINOR_14 },
    };
    GtkWidget *menu, *item;
    int i, sel = 2, n = (int)(sizeof entry / sizeof entry[0]);

    if (g_minor == NULL)
        return;
    menu = gtk_menu_new();
    for (i = 0; i < n; i++) {
        item = gtk_menu_item_new_with_label(entry[i].label);
        gtk_object_set_user_data(GTK_OBJECT(item),
                                 (gpointer)(long) entry[i].minor);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        if (entry[i].minor == current)
            sel = i;
    }
    if (current >= 0 && current <= 255 && sel == 2 && current != 11) {
        char lab[48];

        sprintf(lab, FMT_MID_FROM_CONFIGURATION, current);
        item = gtk_menu_item_new_with_label(lab);
        gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) current);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        sel = n;
    }
    gtk_signal_connect(GTK_OBJECT(menu), "deactivate",
                       GTK_SIGNAL_FUNC(on_midi_changed), NULL);
    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_minor), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_minor), sel);
}

static int minor_menu_value(void)
{
    GtkWidget *menu, *active;

    if (g_minor == NULL)
        return 11;
    menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_minor));
    active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
    if (active == NULL)
        return 11;
    return (int)(long) gtk_object_get_user_data(GTK_OBJECT(active));
}

/* "The synth is running" was set once in build_options() and never
 * again - design/47 M3 - so after a Load, an Unload or a Restart the
 * line said whatever was true when the page was first opened. Asked
 * again from midi_machine(), like the driver line below. */
static void synth_status_refresh(void)
{
    struct vlhe_synth sy;

    if (g_synth_status == NULL || vlhe_synth(&sy) != 0)
        return;
    gtk_label_set_text(GTK_LABEL(g_synth_status),
                       sy.running ? STR_MID_TEXT_SYNTH_RUNNING
                                  : STR_MID_TEXT_SYNTH_NOT_RUNNING);
}

static void driver_refresh(void)
{
    struct vlhe_midiopts mo;
    char msg[160];

    if (g_driver_status == NULL)
        return;
    if (vlhe_midiopts(&mo) != 0)
        return;

    if (!mo.loaded)
        strcpy(msg, FMT_MID_VMIDI_NOT_LOADED_APPLIES);
    else if (mo.minor != mo.minor_applied)
        sprintf(msg, FMT_MID_MODULE_RUNNING_MINOR_RELOAD, mo.minor_applied);
    else
        strcpy(msg, FMT_SND_MATCHES_RUNNING_MODULE);
    gtk_label_set_text(GTK_LABEL(g_driver_status), msg);
}

void midi_machine(void)
{
    driver_refresh();
    synth_status_refresh();
    fonts_note_refresh();
}

/*
 * CAN THIS SESSION CHANGE A MACHINE SETTING? design/49 N7.
 *
 * vlhe_can_administer() rests on access(), which on 2.2 tests the
 * REAL uid (fs/open.c:295). In the setuid build that is never 0, so
 * after Modify the answer stayed "no" and these controls could never
 * be enabled. vlhe_priv_can_act() is the effective question - the
 * ordinary build answers it by euid, the setuid build by whether the
 * password was given. Either suffices.
 */
static int
page_admin(void)
{
    return vlhe_can_administer() || vlhe_priv_can_act();
}

static GtkWidget *build_driver(void)
{
    struct vlhe_midiopts mo;
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *b;
    GtkWidget *note;
    int admin;
    vlhe_midiopts(&mo);
    admin = page_admin();

    outer = gtk_vbox_new(FALSE, 0);

    /* ---- the module parameter ------------------------------------ */

    frame = gtk_frame_new(STR_SND_FRAME_MODULE);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * A MENU OF THE SIX FREE UNITS, NOT A SPIN BUTTON OF 0..255 - the
     * user's design, 2026-10-01, after asking "how do we know 11 is
     * free?" and reading the answer (vlhe_backend.h, the table above
     * vlhe_midi_minor_ok()). The spin button offered 256 values of
     * which 15 were refused or unusable; each entry here names the
     * OSS slot it borrows so the choice means something. 15 is
     * deliberately absent: six that work everywhere against one that
     * stops working the day an ESS or C-Media card is fitted.
     *
     * VALUE BY user_data (design/47 M1), and a hand-edited value the
     * backend accepts but the menu lacks (27, say - unit 11 of slot 1)
     * is added as a seventh entry rather than silently changed.
     */
    g_minor = gtk_option_menu_new();
    minor_menu_fill(mo.minor);
    /* LIVE FOR EVERYONE - the 2026-10-01 decision; see the Sound
     * page's build_device() for the reasoning. */
    (void) admin;
    labelled_row(vbox, STR_MID_LABEL_SOUND_MINOR, g_minor, STR_MID_LABEL_MINOR_NOTE, 0);

    note = gtk_label_new(
        STR_MID_LABEL_11_DEFAULT_THESE_SIX);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    /* NO STATUS LINE HERE ANY MORE - 2026-10-07, the user's design:
     * whether vmidi is loaded is the button row's line (vlhe_state.c),
     * and "running with other settings - reload" is on Status. The
     * variable stays NULL; its refresh already checks. */

    driver_refresh();

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- the restart control ------------------------------------- */

    frame = gtk_frame_new(STR_MID_FRAME_SYNTH_DAEMON);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* WHY THIS EXISTS, from design/09: "If vmidi wedges... the slot
     * stays claimed and MIDI stays dead until the module is unloaded,
     * WHICH TAKES THE WHOLE AUDIO PATH DOWN WITH IT." */
    note = gtk_label_new(
        STR_MID_LABEL_IF_MIDI_STOPS_RESPONDING);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    hbox = gtk_hbox_new(FALSE, 6);
    b = vlhe_tipped(gtk_button_new_with_label(STR_MID_BTN_RESTART_SYNTH), STR_MID_BTN_RESTART_SYNTH_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_restart), NULL);
    /* GREYED WITHOUT ROOT - design/47 M6. A restart is a plan, and a
     * plan needs root; the Status page's Restart buttons already
     * follow vlhe_priv_can_act() (design/49 T1), this one did not. */
    gtk_widget_set_sensitive(b, vlhe_priv_can_act() ? TRUE : FALSE);
    g_restart_btn = b;
    gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    gtk_widget_show(b);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* BUILT ALWAYS, SHOWN ONLY WHEN IT IS TRUE - so Modify can hide
     * it without rebuilding the page (midi_privilege_changed). */
    note = gtk_label_new(vlhe_priv_can_unlock()
        ? STR_SND_TEXT_NEEDS_ROOT_MODIFY
        : STR_SND_TEXT_NEEDS_ROOT_RUN_AS_ROOT);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 8);
    g_root_note = note;
    if (!admin)
        gtk_widget_show(note);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */

static void on_switch_page(GtkNotebook *nb, GtkNotebookPage *pg,
                           guint page, gpointer data)
{
    (void)nb; (void)pg; (void)data;

    g_page = (int) page;
    if (g_page_cb != NULL)
        g_page_cb(1);   /* every MIDI page is a form */
}

/*
 * READ THE WIDGETS INTO THE BACKEND. Apply and OK call this.
 *
 * WHY THIS DID NOT EXIST UNTIL 2026-09-19: sound_collect() was
 * written as the first real one and the others were never added, so
 * Apply on this page found nothing dirty and said "No changes to
 * save" however many settings had been changed. The user hit it
 * changing the voice count and again changing the synth driver's
 * minor.
 */
/* THE PATH BEHIND A FONT DROPDOWN'S SELECTION, or NULL for "(none)"
 * (whose item carries no user_data) and for an empty path. */
static const char *
menu_font_path(GtkWidget *opt)
{
    GtkWidget  *menu, *active;
    const char *path;

    if (opt == NULL)
        return NULL;
    menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(opt));
    active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
    path = (active != NULL)
             ? (const char *) gtk_object_get_user_data(GTK_OBJECT(active))
             : NULL;
    return (path != NULL && path[0] != '\0') ? path : NULL;
}

int midi_collect(void)
{
    struct vlhe_synth    sy;
    struct vlhe_midiopts mo;
    GtkWidget           *menu;
    GtkWidget           *active;

    /*
     * A SONG FONT NEEDS A GENERAL MIDI FONT UNDER IT - 2026-10-06, the
     * documentation review's finding 1; the user: "use that check in
     * the gui to ensure sf0 is set". With General MIDI on "(none)" the
     * song font was saved alone, and the synth then took `FILE@1' as
     * its base font and refused - MIDI broke with nothing said. The
     * setup TUI refuses the same choice (font_set()).
     *
     * FIRST, before any setting reaches the backend, so a refusal
     * leaves nothing half-applied. Only when fonts were found
     * (g_navail): with none, both dropdowns read "(none)" and the
     * font slots are not written anyway - see below.
     */
    if (g_navail > 0 && menu_font_path(g_font_gm) == NULL
        && menu_font_path(g_font_song) != NULL) {
        report(STR_MID_MSG_SONG_FONT_NEEDS_GM);
        return -1;
    }

    /* START FROM WHAT THE BACKEND HAS, so fields this page does not
     * own - `running', `rate', the _applied values - are carried
     * through rather than cleared by a page that never showed them. */
    if (vlhe_synth(&sy) != 0)
        return -1;

    if (g_voices != NULL)
        sy.voices = gtk_spin_button_get_value_as_int(
                        GTK_SPIN_BUTTON(g_voices));

    /* THE SPIN BOX IS A PERCENTAGE, THE STRUCT IS THOUSANDTHS.
     * design/07 keeps floats out of anything reaching a config line,
     * so the conversion lives here rather than in the widget. */
    if (g_gain != NULL)
        sy.gain_milli = gtk_spin_button_get_value_as_int(
                            GTK_SPIN_BUTTON(g_gain)) * 10;

    if (g_law != NULL) {
        menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_law));
        active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
        if (active != NULL)
            sy.law = (int)(long)
                gtk_object_get_user_data(GTK_OBJECT(active));
    }

    if (g_filter != NULL) {
        menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_filter));
        active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
        if (active != NULL)
            sy.filter = (int)(long)
                gtk_object_get_user_data(GTK_OBJECT(active));
    }

    if (g_reverb != NULL)
        sy.reverb = GTK_TOGGLE_BUTTON(g_reverb)->active ? 1 : 0;
    if (g_chorus != NULL)
        sy.chorus = GTK_TOGGLE_BUTTON(g_chorus)->active ? 1 : 0;
    if (g_autovoices != NULL)
        sy.auto_voices = GTK_TOGGLE_BUTTON(g_autovoices)->active ? 1 : 0;
    if (g_modenv != NULL) {
        menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_modenv));
        active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
        if (active != NULL)
            sy.modenv = (int)(long)
                gtk_object_get_user_data(GTK_OBJECT(active));
    }

    /* THE RATE, BY user_data - see build_options() for why not by
     * position. A menu with no active item leaves the stored value
     * alone rather than defaulting it. */
    if (g_rate != NULL) {
        menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_rate));
        active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
        if (active != NULL)
            sy.rate = (int) (long)
                gtk_object_get_user_data(GTK_OBJECT(active));
    }

    if (vlhe_set_synth(&sy) != 0)
        return -1;

    /*
     * THE TWO FONT SLOTS - and until 2026-09-21 this page read
     * neither, so a font the user picked was never saved. It looked
     * saved because the dropdown kept showing it; closing and
     * reopening the GUI lost it (the user, 86Box).
     *
     * THEY ARE USER SETTINGS. vlhe_set_fonts() writes Font0 and
     * Font1 under [Midi Settings] in the USER config, not the system
     * one - which font to load is a person's choice, not a
     * machine's. It is memory-only until vlhe_commit().
     *
     * AND ROOT'S ARE ALSO THE MACHINE'S - design/54 D15, 2026-10-03.
     * Boot runs with no user to ask (init's HOME is /), so root's fonts
     * are mirrored to DefaultFont0/1 in the system config, and that is
     * what boot plays. A user's own fonts reach a running synth through
     * the login agent, when it exists (design/54 P01).
     *
     * BANK 0 IS THE GM FONT, BANK 1 THE SONG'S. Both optional: a
     * dropdown left on "(none)" carries a NULL user_data, which
     * leaves that slot empty and makes vlhe_set_fonts() unset the
     * key rather than write a blank one.
     */
    {
        struct vlhe_font fonts[VLHE_MAX_FONTS];
        const char      *path;
        int              nf = 0;

        memset(fonts, 0, sizeof fonts);

        if (g_font_gm != NULL) {
            menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_font_gm));
            active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu))
                                    : NULL;
            path = (active != NULL)
                     ? (const char *) gtk_object_get_user_data(
                           GTK_OBJECT(active))
                     : NULL;
            if (path != NULL && path[0] != '\0') {
                strncpy(fonts[nf].path, path, VLHE_PATH_MAX - 1);
                fonts[nf].bank = 0;
                nf++;
            }
        }

        if (g_font_song != NULL && nf < VLHE_MAX_FONTS) {
            menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_font_song));
            active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu))
                                    : NULL;
            path = (active != NULL)
                     ? (const char *) gtk_object_get_user_data(
                           GTK_OBJECT(active))
                     : NULL;
            if (path != NULL && path[0] != '\0') {
                strncpy(fonts[nf].path, path, VLHE_PATH_MAX - 1);
                fonts[nf].bank = 1;
                nf++;
            }
        }

        /*
         * CALLED EVEN WHEN nf IS 0, deliberately - that is how BOTH
         * slots get cleared. vlhe_set_fonts() unsets any key it is
         * not given, so skipping the call on an empty selection
         * would make "(none)" impossible to save.
         *
         * BUT NOT WHEN NO FONTS WERE FOUND, and the test is
         * g_navail rather than the widgets. BOTH HALVES OF THIS PAGE
         * ARE ALWAYS BUILT - see build_fonts(), which builds the
         * notice AND the dropdowns so Rescan can swap them without a
         * rebuild - so `g_font_gm != NULL' is true even on a machine
         * with no fonts at all and would not have guarded anything.
         *
         * WHAT THAT GUARDS AGAINST: a machine where the config names
         * a font the scan cannot see (an unmounted disc, a path
         * typed by hand). The dropdowns would both read "(none)"
         * because there is nothing to populate them with, and saving
         * that would erase a setting the user never chose to clear.
         */
        /*
         * AND NOT WHEN THE MENUS STILL SHOW THE MACHINE'S DEFAULT,
         * UNTOUCHED - 2026-10-08, the design/47 Q4 rule the font
         * picker already follows: a slot the page SEEDED from the
         * system file is not the user's choice, and writing it into
         * their file would pin today's default against any later
         * change to the machine's. Only a selection that differs
         * from the default reaches vlhe_set_fonts().
         */
        if (g_navail > 0) {
            int untouched = 0;

            if (g_font_from_machine) {
                struct vlhe_font mf[VLHE_MAX_FONTS];
                int from = 0, nm, k;

                nm = vlhe_fonts_effective(mf, VLHE_MAX_FONTS, &from);
                if (from && nm == nf) {
                    untouched = 1;
                    for (k = 0; k < nf; k++)
                        if (mf[k].bank != fonts[k].bank
                            || strcmp(mf[k].path, fonts[k].path) != 0)
                            untouched = 0;
                }
                /* AND AN EMPTY SELECTION IS UNTOUCHED TOO: with no
                 * font of their own there is nothing for "(none)" to
                 * clear, and the slots read "(none)" by themselves
                 * when the machine's font lies outside the searched
                 * folders. Writing that as root would have emptied
                 * the machine's default (vlhe_set_fonts() writes
                 * DefaultFont* for root - D15), which is what
                 * setup-vlhe had just set. Seen in Xephyr, 2026-10-08. */
                if (from && nf == 0)
                    untouched = 1;
            }
            if (!untouched && vlhe_set_fonts(fonts, nf) != 0)
                return -1;
        }
    }

    /* THE DRIVER TAB, which is a module parameter rather than a
     * daemon setting - hence a separate struct and a separate set. */
    if (vlhe_midiopts(&mo) == 0 && g_minor != NULL) {
        char why[160], msg[200];

        mo.minor = minor_menu_value();
        if (vlhe_set_midiopts(&mo) != 0) {
            /* SAY WHY - design/47 M4. The menu offers only values the
             * backend accepts, so this is reached by a hand-edited
             * configuration value or a changed machine (15 with an
             * ESS driver now present); the reason is the backend's. */
            (void) vlhe_midi_minor_ok(mo.minor, why, sizeof why);
            sprintf(msg, FMT_MID_MIDI_SETTINGS_SOUND_MINOR,
                    why[0] ? why : STR_MID_TEXT_PICK_ANOTHER);
            report(msg);
            return -1;
        }
    }

    /* SAVED, so the page is clean. Only on the success path - a
     * collect that failed has not committed anything and the edits
     * are still unsaved. */
    g_dirty = 0;
    tabs_clear();
    return 0;
}

/* PUT THEM BACK - Cancel's half. Called after the backend has
 * re-read its files, so this reads the restored values. */
int midi_dirty(void)
{
    return g_dirty;
}

void midi_set_dirty_cb(void (*cb)(void))
{
    g_dirty_cb = cb;
}

/*
 * SELECT THE ITEM WHOSE PATH THIS IS - design/47 M2, 2026-09-30.
 *
 * The font menus carry the path as user_data (build_font_items()),
 * so the saved font is found by comparing paths, never by remembering
 * an index: the list is rebuilt by Rescan and its order is the
 * scanner's. NULL selects "(none)", which is item 0 when the menu
 * allows it. A path no longer in the list leaves the menu where it
 * is - the same choice build_font_items() makes for a vanished font,
 * and the next collect then stores what is showing.
 */
static void
font_menu_select(GtkWidget *opt, const char *path)
{
    GtkWidget *menu;
    GList     *l;
    int        i = 0;

    if (opt == NULL)
        return;
    menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(opt));
    if (menu == NULL)
        return;
    for (l = GTK_MENU_SHELL(menu)->children; l != NULL; l = l->next, i++) {
        const char *d = (const char *) gtk_object_get_user_data(GTK_OBJECT(l->data));

        if ((path == NULL && d == NULL)
            || (path != NULL && d != NULL && strcmp(path, d) == 0)) {
            gtk_option_menu_set_history(GTK_OPTION_MENU(opt), i);
            return;
        }
    }
}

void midi_reload(void)
{
    struct vlhe_synth    sy;
    struct vlhe_midiopts mo;
    struct vlhe_font     fonts[VLHE_MAX_FONTS];
    const char          *gm = NULL, *song = NULL;
    int                  nf, i;

    /* SETTING WIDGETS IS NOT A USER EDIT. Without this every
     * gtk_spin_button_set_value() below fires on_midi_changed and
     * Cancel would leave the page dirtier than it found it. */
    g_loading = 1;

    if (vlhe_synth(&sy) == 0) {
        if (g_voices != NULL)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_voices),
                                      (gfloat) sy.voices);
        if (g_gain != NULL)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_gain),
                                      (gfloat)(sy.gain_milli / 10));
        if (g_law != NULL)
            gtk_option_menu_set_history(GTK_OPTION_MENU(g_law),
                                        sy.law == VLHE_LAW_LINEAR ? 1 : 0);
        if (g_reverb != NULL)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_reverb),
                                         sy.reverb ? TRUE : FALSE);
        if (g_chorus != NULL)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_chorus),
                                         sy.chorus ? TRUE : FALSE);
        if (g_autovoices != NULL)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_autovoices),
                                         sy.auto_voices ? TRUE : FALSE);
        if (g_modenv != NULL)
            gtk_option_menu_set_history(GTK_OPTION_MENU(g_modenv),
                                        sy.modenv == VLHE_MODENV_REFERENCE
                                        ? 1 : 0);

        /* THE RATE, BY POSITION - the menu is built in vlhe_rates.h's
         * order and nothing else may reorder it. Unknown rates fall back to the
         * first entry rather than leaving a stale selection that the
         * next collect would then store. */
        if (g_rate != NULL) {
            static const int rates[] = VLHE_SYNTH_RATES;
            int k, sel = 0;

            for (k = 0; k < VLHE_SYNTH_NRATES; k++)
                if (sy.rate == rates[k])
                    sel = k;
            gtk_option_menu_set_history(GTK_OPTION_MENU(g_rate), sel);
        }

        /* THE FILTER, BY POSITION - the menu is built in VLHE_VF_*
         * order (0..3) and nothing reorders it. Missing until
         * 2026-09-30 (design/47 M2): Cancel and a config change left
         * the menu showing whatever was last picked. */
        if (g_filter != NULL)
            gtk_option_menu_set_history(GTK_OPTION_MENU(g_filter),
                                        (sy.filter >= VLHE_VF_AWE
                                         && sy.filter <= VLHE_VF_NONE)
                                        ? sy.filter : VLHE_VF_AWE);
    }

    if (vlhe_midiopts(&mo) == 0 && g_minor != NULL)
        minor_menu_fill(mo.minor);

    /*
     * AND THE TWO FONT MENUS - design/47 M2. The same bank-0 /
     * bank-1 reading on_rescan() does, so Cancel after changing a
     * font shows the saved one again instead of the cancelled pick,
     * which the next OK would otherwise have written.
     */
    nf = page_fonts(fonts, VLHE_MAX_FONTS);
    for (i = 0; i < nf; i++) {
        if (fonts[i].bank == 0 && gm == NULL)
            gm = fonts[i].path;
        else if (fonts[i].bank == 1 && song == NULL)
            song = fonts[i].path;
    }
    /*
     * THE FONT MENUS FOLLOW THE FOLDERS - the user, 2026-10-02: "It
     * was add a folder for sf2 and cancel. I can still select fonts
     * it found. I did rescan and they went away." Add Folder scans
     * and rebuilds the menus from the new folder list; Cancel put the
     * old list back (vlhe_discard(), dirs_redraw() below) and only
     * RE-SELECTED in menus built from the new one, so fonts from a
     * folder no longer searched stayed on offer until Rescan. If the
     * folders differ from what the menus were scanned with, scan
     * again - quietly, since nothing here is the user's doing.
     */
    {
        char dirs[VLHE_MAX_FONTDIRS][VLHE_PATH_MAX];
        char key[VLHE_MAX_FONTDIRS * VLHE_PATH_MAX];
        int  nd = vlhe_font_dirs(dirs, VLHE_MAX_FONTDIRS);
        int  k;

        key[0] = '\0';
        for (k = 0; k < nd; k++) {
            strcat(key, dirs[k]);
            strcat(key, "\n");
        }
        if (strcmp(key, g_scanned_dirs) != 0)
            fonts_rescan_menus();
    }

    font_menu_select(g_font_gm, gm);
    font_menu_select(g_font_song, song);
    fonts_note_refresh();           /* which half the slots now show */

    /*
     * AND THE FOLDER LIST - missing until 2026-09-22, when the user
     * found that adding a folder "can not be cancelled".
     *
     * vlhe_add_font_dir() is MEMORY ONLY until vlhe_commit() writes,
     * exactly like every other control on this page - so Cancel
     * should discard it. The backend was already doing its half:
     * Cancel re-reads the config, which drops the pending change.
     * What was missing is redrawing the list from it, so the widget
     * kept showing a folder the backend had just forgotten.
     */
    dirs_redraw();

    g_loading = 0;
    g_dirty = 0;
    tabs_clear();
}

int midi_page_wants_buttons(void)
{
    return 1;
}

void midi_set_page(int page)
{
    if (g_notebook != NULL)
        gtk_notebook_set_page(GTK_NOTEBOOK(g_notebook), page);
}

GtkWidget *midi_build(void (*report_fn)(const char *),
                      void (*page_fn)(int))
{
    GtkWidget *nb;
    GtkWidget *tab;

    g_report = report_fn;
    g_page_cb = page_fn;

    /*
     * BUILDING IS NOT EDITING - and this guard was missing until
     * 2026-09-21, when connecting `activate' to the font dropdowns
     * made it matter.
     *
     * gtk_option_menu_set_history() in build_font_menu() fires
     * `activate' on the item it selects, so the page marked ITSELF
     * dirty the moment it was constructed - and every page is built
     * at start, so the star was there before anyone had touched the
     * page. (This said "pages are built LAZILY, on first visit" until
     * 2026-10-01; they never were - design/47 V1 found the sentence
     * was wrong when it sent a diagnosis the wrong way.)
     * sidebar_mark_dirty() refreshes every row, so whichever page it
     * was told about, the MIDI row showed the star it had just
     * earned. That looked like one page's edit marking another
     * dirty; it was not.
     *
     * reload() has had the same guard since it was written, for the
     * same reason (see its comment). This is the other place widgets
     * are set by us rather than by a person.
     */
    g_loading = 1;

    nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);

    tab = gtk_label_new(STR_MID_LABEL_SOUND_FONTS);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_fonts(), tab);

    tab = gtk_label_new(STR_SND_TAB_OPTIONS);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_options(), tab);

    tab = gtk_label_new(STR_MID_LABEL_DRIVER);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_driver(), tab);

    gtk_signal_connect(GTK_OBJECT(nb), "switch_page",
                       GTK_SIGNAL_FUNC(on_switch_page), NULL);
    g_notebook = nb;

    /* CLOSE THE GUARD, and clear anything the build earned. A page
     * that has just appeared has no unsaved edits by definition. */
    g_loading = 0;
    g_dirty = 0;
    tabs_clear();

    return nb;
}

/*
 * MODIFY WAS PRESSED - design/49 N7. Sensitivity and the note only;
 * no values are reloaded (see sound_privilege_changed()).
 */
void
midi_privilege_changed(void)
{
    int admin = page_admin();

    /* The minor stays live (2026-10-01); the Restart button still
     * follows privilege, because a restart acts on the machine. */
    if (g_restart_btn != NULL)          /* M6 */
        gtk_widget_set_sensitive(g_restart_btn,
                                 vlhe_priv_can_act() ? TRUE : FALSE);
    if (g_root_note != NULL) {
        if (admin)
            gtk_widget_hide(g_root_note);
        else
            gtk_widget_show(g_root_note);
    }
}
