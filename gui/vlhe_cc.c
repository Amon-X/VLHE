/*
 * vlhe_cc.c - the VLHE Control Center: window, sidebar and the four
 * module shells.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * vlhe-control-center-spec.md is the layout this implements. THIS FILE
 * OWNS THE SHELL ONLY - the window, the menu bar, the sidebar list, the
 * button row, the status bar, and the show/hide of four notebooks. What
 * goes INSIDE a notebook page is deliberately a stub here, so the
 * container can be looked at and argued with before eight tabs of
 * fields are written against it.
 *
 * IT TALKS TO vlhe_backend.h AND NOTHING ELSE. No ioctl, no /proc, no
 * device node appears in this file. See that header for why.
 *
 * THE WINDOW SIZE IS TWO CONSTANTS, ON PURPOSE. 800x600, and that is
 * MEASURED rather than preferred: KDE's own Control Center was captured
 * on the guest at both sizes, and at 640x480 it loses its whole
 * OK/Apply/Cancel row off the bottom edge. Same arrangement as this
 * window, so the space it needs is the space we need. Changing WIN_W
 * and WIN_H is the whole change - nothing below sizes itself against
 * them, and nothing has a fixed size that would stop the window
 * shrinking. design/32-control-center.md section 4 has the captures.
 *
 * WHY gtk_window_set_default_size AND NOT set_usize: default_size is a
 * REQUEST. The user's window manager can make the window smaller, and
 * the layout survives that because the sidebar is the only fixed-width
 * thing in it. set_usize would impose a floor, and a floor taller than
 * the screen is a window with its buttons off the bottom edge - which
 * is exactly the failure a 640x480 machine would hit.
 *
 * GTK 1.2, C89, GCC 2.95.2. The calls are the 1.0-compatible spellings
 * where they differ, matching vsoundvol_gtk.c and vcdgui.c.
 */

#include <gtk/gtk.h>
#include <gdk/gdkx.h>           /* GDK_FONT_XFONT, GDK_DISPLAY - fonts */
#include <X11/Xatom.h>          /* XA_FONT, for reading a name back    */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>         /* getuid - the sidebar identity line */

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_tip.h"
#include "vlhe_conf.h"
#include "vlhe_conf_template.h"  /* VLHE_TPL_SYSTEM, for the per-file exists test */
#include "vlhe_cli.h"
#include "vlhe_icons.h"
#include "vlhe_help.h"      /* the Help window and About, design/54 G01 */
#include "vlhe_mod_volume.h"
#include "vlhe_mod_cd.h"
#include "vlhe_mod_cdg.h"
#include "vlhe_mod_midi.h"
#include "vlhe_mod_status.h"
#include "vlhe_apply.h"     /* the baseline drift question, 7h Stage 3.3 */
#include "vlhe_priv.h"
#include "vlhe_self.h"
#include "vlhe_mod_render.h"
#include "vlhe_mod_sound.h"
#include "vlhe_filter.h"
#include "vlhe_buttons.h"

/* THE TWO NUMBERS. See the header comment. */
#define WIN_W           800
#define WIN_H           600

/* THE FLOOR, measured against KDE's own (450x200 minimum, 700x600
 * default). Ours is larger because the button row and the status bar
 * are always present; below this the notebook has no usable height. */
/* SMALL, BECAUSE THE PANE SCROLLS. The first version set 480x320 as a
 * floor the layout was supposed to survive, and a capture at exactly
 * that size showed it did not - the drive frames and the explanatory
 * note drew ON TOP of each other and the third drive was off-screen
 * with no way to reach it. A hint the layout cannot honour is a false
 * promise.
 *
 * With the pane in a scrolled window nothing becomes unreachable at any
 * size, so this only has to keep the window from being shrunk to
 * uselessness. */
#define WIN_MIN_W       320
#define WIN_MIN_H       240

/* THE PRODUCT'S NAME, EXPANDED. The user's call, 2026-09-17: the bare
 * "Control Center" says what KIND of program this is and not WHOSE, and
 * a window manager's task list shows only the title. Corel's own
 * Control Center is a different program with the same generic name and
 * may be open at the same time. */
#define WIN_TITLE       STR_SHELL_APP_TITLE     /* vlhe_strings.h */

/* 160, NOT THE SPEC'S 210, AND THE NUMBER COMES FROM THE VIEWER.
 *
 * 800 - 160 leaves 640 for the pane, and a MOD_VIEW module carries no
 * notebook tabs and no button row (see the enum below), which gives
 * back ~70px of height - enough for 480 inside a 600-tall window. So
 * CD+G gets a 640x480 area in the pane at the default window size,
 * which is the point of picking 160 rather than eyeballing it.
 *
 * It still fits the labels: "Sound Settings" is the longest at roughly
 * 130px including its icon and indent. A future grouped list with
 * indented rows under headings would be tighter - check before adding
 * one rather than assuming the slack is there.
 *
 * Fixed WIDTH only; it stretches vertically with the window. */
#define SIDEBAR_W       160

/* How many modules there are. The enum and the table below are the two
 * places a fifth module would be added, and they are adjacent so a
 * mismatch is visible rather than inferred. */
enum {
    MOD_STATUS = 0,     /* FIRST, and deliberately: it is the answer to
                         * "is anything wrong?", which is the question
                         * someone opens this with when sound has
                         * stopped. Every other page assumes its module
                         * is loaded and apologises when it is not. */
    MOD_VOLUME,
    MOD_SOUND,
    MOD_MIDI,
    MOD_CD,
    MOD_CDG,            /* TEMPORARY - proves the MOD_VIEW path        */
    MOD_RENDER,         /* MIDI to a .wav or .mp3 - vlhe_mod_render.c  */
    MOD_COUNT
};

/* WHAT A MODULE IS FOR, and it decides whether it gets a button row.
 *
 * FROM KDE'S OWN kcontrol, read 2026-09-17. Its shell has NO button row
 * at all - toplevel.cpp builds a menu bar, a status bar, a splitter and
 * a tree, and the word "Apply" appears nowhere in it. Every module is a
 * SEPARATE PROGRAM, swallowed into the pane by X reparenting, and each
 * draws its own buttons. So "what does Apply mean against a video
 * display" never arises there: a viewer simply draws none.
 *
 * WE CANNOT SWALLOW and would not want to - four modules in one binary
 * need no process table, no window-add signals and no KWM. But the
 * CONSEQUENCE transfers exactly: the button row belongs to the module,
 * not to the shell. A flag is all that takes. */
enum {
    MOD_SETTINGS = 0,   /* fields to change: Help / OK / Apply / Cancel */
    MOD_VIEW            /* something to look at: Help only             */
};

struct module {
    const char  *label;         /* the sidebar row                      */
    int          kind;          /* MOD_SETTINGS or MOD_VIEW             */
    const char  *status;        /* pushed to the status bar on select   */
    char       **xpm;           /* its sidebar icon, from vlhe_icons.h  */
    GtkWidget   *page;          /* the GtkNotebook, built at startup    */
    gint         row;           /* its sidebar row, for select_module() */
    /*
     * READ MY WIDGETS INTO THE BACKEND. Called by Apply and OK before
     * vlhe_commit(); NULL for a module with nothing to commit.
     *
     * IT DOES NOT WRITE A FILE. The module hands its values to the
     * vlhe_set_* functions, which update memory and set the dirty
     * flag; one vlhe_commit() afterwards writes whatever moved. That
     * split is what lets Apply write ONE file for a whole page of
     * edits instead of rewriting it per control - and what makes a
     * page the user only looked at cost nothing.
     *
     * Returns 0, or -1 if the page holds something it will not
     * accept (a field it can validate itself). It reports its own
     * reason; the shell just stops.
     */
    int        (*collect)(void);
    /*
     * PUT MY WIDGETS BACK TO WHAT THE BACKEND SAYS. Cancel calls
     * this after the backend has re-read its files; NULL for a module
     * with nothing to discard.
     *
     * THE REVERSE OF collect(), and Cancel is the only caller. KDE
     * never needed one: each of its modules is a separate PROCESS, so
     * Cancel discards by exiting and the edits die with it
     * (kcontrol.cpp connects Apply, Help, Default and OK - Cancel is
     * left on Qt's own reject()). We are one binary with four panels,
     * so discarding has to be done rather than got for free.
     */
    void       (*reload)(void);
    /*
     * HAVE I GOT UNSAVED EDITS? Non-zero puts an asterisk on this
     * module's sidebar row. NULL for a module with nothing to save.
     *
     * ASKED OF THE MODULE, NOT OF THE BACKEND, and that is forced:
     * a page's values do not reach the backend until collect() runs,
     * which is Apply - so vlhe_dirty() cannot answer while the user
     * is still typing, which is exactly when the marker is useful.
     */
    int        (*dirty)(void);
    /*
     * THE MACHINE CHANGED - RE-READ ONLY WHAT DEPENDS ON IT. Added
     * 2026-09-24 for design/36 rows 34, 44, 57 and 58, four symptoms
     * of one cause: a label or list computed in a build_*() function
     * and never again. "vmidi is not loaded" after a Load; "vdisc is
     * not loaded" while grip held the device; the Advanced tab's font
     * list needing a GUI restart to show a new font.
     *
     * NOT reload(). That puts every widget back to the backend's
     * values and would wipe an unapplied edit on the page. This one
     * touches nothing the user typed - it re-asks the backend what is
     * loaded, what is running, what fonts exist, and repaints the
     * text and lists that say so.
     *
     * Called when a page is SHOWN, and after Load/Unload has run.
     * NULL for a page with nothing machine-dependent on it.
     */
    void       (*machine)(void);
};

/* THE FOUR MODULES, exactly the spec's table. Built once at startup and
 * shown/hidden on selection - the spec's recommendation, and the reason
 * is that rebuilding pages at runtime loses every widget's state (a
 * half-typed path in an Entry, a scroll position) each time the user
 * clicks away and back. */
static struct module g_mod[MOD_COUNT] = {
    /* A VIEW, not settings: nothing on it is committed, so it takes
     * Help alone rather than the OK/Apply/Cancel trio. */
    { STR_MOD_STATUS,         MOD_VIEW,
      STR_MOD_STATUS_DESC,
      xpm_vlhe,   NULL, 0, NULL, NULL, NULL, NULL },

    /* VOLUME HAS NO collect, AND THAT IS NOT AN OMISSION. Its Levels
     * page is LIVE - every drag is already an ioctl (design/32
     * section 8) - so there is nothing held back to commit. Its
     * Options page will want one when the mixer save/restore setting
     * lands. */
    { STR_MOD_VOLUME,         MOD_SETTINGS,
      STR_MOD_VOLUME_DESC,
      xpm_volume, NULL, 0,
      volume_collect, volume_reload, volume_dirty, volume_machine },
    { STR_MOD_SOUND, MOD_SETTINGS,
      STR_MOD_SOUND_DESC,
      xpm_sound,  NULL, 0,
      sound_collect, sound_reload, sound_dirty, sound_machine },
    { STR_MOD_MIDI,  MOD_SETTINGS,
      STR_MOD_MIDI_DESC,
      xpm_midi,   NULL, 0,
      midi_collect, midi_reload, midi_dirty, midi_machine },
    { STR_MOD_CD,    MOD_SETTINGS,
      STR_MOD_CD_DESC,
      xpm_cd,     NULL, 0,
      cd_collect, cd_reload, cd_dirty, cd_machine },

    /* THE REAL VIEWER, 2026-09-20 - this row was a placeholder proving
     * the MOD_VIEW path and is now the page it was measuring for.
     * design/34 has the subchannel path that will feed it. */
    { STR_MOD_CDG,    MOD_VIEW,
      STR_MOD_CDG_DESC,
      xpm_cd,     NULL, 0, NULL, NULL, NULL, NULL },

    /* RENDER MIDI - a view, because nothing here is committed: the
     * output name and the LAME path belong to one render rather than
     * to the machine. It opens no audio device, so it works while
     * something else is playing and on a machine too slow to play
     * the file live - which 86Box is, at vmidid's defaults. */
    /* MOD_SETTINGS FOR THE ADVANCED TAB'S SAKE. The Render tab holds
     * filenames that belong to one render and are not saved; the
     * Advanced tab is a form whose values persist in the user
     * config, and it wants the OK/Apply/Cancel the shell provides.
     * The tab labels are empty because this module builds its own
     * notebook. */
    { STR_MOD_RENDER,    MOD_SETTINGS,
      STR_MOD_RENDER_DESC,
      xpm_midi,   NULL, 0,
      render_collect, render_reload, render_dirty, render_machine }

    /* THE "Spin Test" ROW WENT HERE until 2026-10-01 - a temporary
     * page of bare spinners for the merged-arrow question, which
     * closed as an 86Box emulation fault (CLAUDE.md section 3). Its
     * file, vlhe_mod_test.c, is in git history at 5505d4a. */
};

static GtkWidget   *g_window;            /* for View / Reset Size   */
static GtkWidget   *g_statusbar;
static guint        g_status_ctx;
static int          g_status_pushed;     /* is there a message to pop?  */
static GtkWidget   *g_pane;              /* holds the four notebooks    */
static GtkWidget   *g_scroll;            /* scrolls it below 800x600    */
static GtkWidget   *g_buttons;           /* the row - ALWAYS occupies space */
static GtkWidget   *g_btn_help;
/* THE MODIFY BUTTON - kcontrol's `SUA'. Shown only in the setuid
 * build, run by a non-root user, with the password not yet given;
 * root never sees it. vlhe_priv.h has the mechanism. */
static GtkWidget   *g_btn_modify;
static GtkWidget   *g_btn_ok;
static GtkWidget   *g_btn_apply;
static GtkWidget   *g_btn_cancel;
static GtkWidget   *g_list;              /* the sidebar, for select_module */
static GtkWidget   *g_noconfig_frame;    /* the "no config" notice, or NULL */
static GtkWidget   *g_noconfig;
static GtkWidget   *g_whoami;            /* "Running as ..." - always shown */

/* ------------------------------------------------------------------ */
/* Status bar                                                         */
/* ------------------------------------------------------------------ */

/* POP BEFORE PUSH, or the stack grows by one message per click and the
 * bar shows the FIRST module selected rather than the current one.
 * gtk_statusbar_pop on an empty stack is harmless, but tracking it is
 * cheaper than relying on that. */
static void status_set(const char *text)
{
    if (g_status_pushed) {
        gtk_statusbar_pop(GTK_STATUSBAR(g_statusbar), g_status_ctx);
        g_status_pushed = 0;
    }

    if (text != NULL) {
        gtk_statusbar_push(GTK_STATUSBAR(g_statusbar), g_status_ctx, text);
        g_status_pushed = 1;
    }
}

/* SHOW OR HIDE THE BUTTONS WITHOUT MOVING ANYTHING.
 *
 * THE ROW ITSELF IS NEVER HIDDEN. gtk_widget_hide() takes a widget out
 * of the layout, so hiding the row would shorten the pane and the
 * whole panel would jump every time the user changed tab. Hiding the
 * four BUTTONS instead leaves their container in place, holding its
 * height, and the user sees a blank strip rather than a resize.
 *
 * THE USER'S CALL, and greying was the alternative: a greyed OK
 * invites "why can't I press it?", where blank space asks nothing.
 *
 * THE RULE, in the user's own terms: "The volume is the main one that
 * acts right away the others are settings to be applied that change
 * the daemons or modules."
 *
 *   A page gets OK/Apply/Cancel only if it holds settings that reach a
 *   CONFIG FILE or a MODULE PARAMETER. A LIVE page gets Help alone.
 *
 * Live pages are the exception and there are two: Volume's Levels,
 * whose sliders write to a running stream through an ioctl that takes
 * effect on the next block, and the CD+G viewer, whose CONTENT updates
 * as the disc plays and whose transport buttons act immediately. The
 * two are live in different senses - controls versus content - but
 * neither has anything to commit.
 *
 * Everything else is a form: Sound, MIDI and CD Settings all
 * reconfigure a daemon or a module, which means writing config and
 * signalling a reload. Apply there is doing real work.
 *
 * So this is per PAGE rather than per MODULE because Volume is both -
 * a live mixer and a form - in one module. */
/* HELP IS SEPARATE FROM THE OTHER THREE, and that is the user's call:
 *
 *   "We dont need the okay on volume but the help button may be handy"
 *
 * Help is not an action on the settings - it EXPLAINS THE PAGE, which
 * is as useful on a live mixer as on a form and arguably more, since
 * Levels is where someone wonders what the MIDI row is or why a
 * channel reads "(free)". OK and Apply are the ones with nothing to do
 * there.
 *
 * WE DELIBERATELY DO NOT COPY KDE HERE. Its read-only pages (the
 * Information group - IO-Ports, Memory, Partitions) show HELP AND OK,
 * and the user identified why that does not transfer: in kcontrol each
 * module is a separate program swallowed into the pane, so OK dismisses
 * THAT PROGRAM and pops back to whatever was underneath - a navigation
 * stack. We are one binary with four panels; there is no stack to
 * unwind, so OK on a page with nothing to commit would only close the
 * window, which the WM button and File/Exit already do. */
/* The page whose row is being shown - so buttons_show() can make the
 * Status page the one exception to "Modify only with the commit trio"
 * (design/49 T2). */
static int g_buttons_for = -1;

static void buttons_show(int help, int commit)
{
    if (g_btn_help == NULL)
        return;

    if (help)
        gtk_widget_show(g_btn_help);
    else
        gtk_widget_hide(g_btn_help);

    if (commit) {
        gtk_widget_show(g_btn_ok);
        gtk_widget_show(g_btn_apply);
        gtk_widget_show(g_btn_cancel);

        /*
         * NO LONGER GREYED UNTIL MODIFY - design/49, the decision of
         * 2026-10-01 ("B"). Three decisions had collided: greying the
         * controls that need root, greying OK/Apply until Modify
         * (below, the user's 2026-09-22 request), and the draft pair
         * of 2026-09-30, which needs a locked user to EDIT machine
         * settings and save them as a draft for root. The third needs
         * the first two false. The user's ruling, in their words: a
         * first-run user "will want to check out the settings and try
         * changing stuff not knowing what is what. Having them greyed
         * out and forcing them to restart pushes them away. Telling
         * someone to run a program as root right away before they
         * have a chance to even look at it scares people away from a
         * security standpoint." So OK and Apply are live for everyone;
         * a commit writes what it may - the user half always, the
         * system half with root - and the status line says which.
         * Modify stays, as what unlocks the save and the Status page's
         * Load/Unload/Restart, which still follow privilege because
         * they act on the machine rather than on a file.
         *
         * THE 2026-09-22 REQUEST, kept for the record - the user,
         * seeing the first run of the setuid build: "is it
         * possible to have the okay and apply greyed out until after
         * the modify has returned successfully?"
         *
         * It is, and it is what kcontrol does: showSUAButton(TRUE)
         * sits beside enableAcceptance(FALSE) in the same branch
         * (kcontrol.cpp:409). I built the button and left out the
         * half that makes it mean something - a Modify beside a live
         * OK offers to unlock what is not locked.
         *
         * CANCEL STAYS LIVE. It abandons edits rather than
         * committing them, so it needs no privilege - and a user who
         * has changed something they cannot save should still be
         * able to put it back.
         *
         * THE ORDINARY BUILD IS UNAFFECTED: vlhe_priv_unlocked()
         * answers TRUE there, because nothing is being held back.
         * What greys its controls is vlhe_can_administer(), which
         * asks whether the config is WRITABLE - a different question
         * and the right one for a machine where an admin group owns
         * the file.
         */
        gtk_widget_set_sensitive(g_btn_ok, TRUE);
        gtk_widget_set_sensitive(g_btn_apply, TRUE);
    } else {
        gtk_widget_hide(g_btn_ok);
        gtk_widget_hide(g_btn_apply);
        gtk_widget_hide(g_btn_cancel);
    }

    /*
     * MODIFY APPEARS BESIDE THE COMMIT TRIO AND ONLY WITH IT.
     *
     * It offers to unlock a commit, so it is meaningless on a page
     * that commits nothing - the Volume mixer and the CD+G viewer
     * apply live and have no OK to ungrey.
     *
     * AND ONLY WHEN THERE IS PRIVILEGE TO REGAIN. kcontrol's
     * showSUAButton is called with TRUE and nothing else, inside the
     * `getuid() != 0' branch, so root never sees one - and neither
     * does anyone running the ordinary build, where it would be a
     * control that cannot work.
     *
     * AND ON THE STATUS PAGE, WHICH COMMITS NOTHING BUT ACTS - design/49
     * T2, the user's decision after the first target test: Load lives
     * there, its refusal said "needs root", and the only Modify was on
     * pages Status is not. A page that ACTS as root is as much a reason
     * for the button as one that commits.
     */
    if (g_btn_modify != NULL) {
        if ((commit || g_buttons_for == MOD_STATUS) && vlhe_priv_can_unlock())
            gtk_widget_show(g_btn_modify);
        else
            gtk_widget_hide(g_btn_modify);
    }
}

/* A module's route to the status bar.
 *
 * PASSED IN RATHER THAN REACHED FOR, so a module never touches the
 * shell's widgets. It is the same reasoning as vlhe_backend.h: the
 * module knows what happened, the shell knows where to show it. */
static void report_to_status(const char *msg)
{
    status_set(msg);
}

/* The Volume module changed tab. Its Levels page applies live and
 * wants no buttons; its Options page is a form and does. */
static void on_volume_page_changed(int wants_buttons)
{
    /* HELP ON BOTH PAGES; the commit trio only where there is
     * something to commit. */
    buttons_show(1, wants_buttons);
}

/* CD Settings changed tab. Its Drive page is LIVE - attach, eject and
 * lock act at once, the way putting a disc in a drive does - so it
 * wants no commit trio. Audio is a form. */
static void on_cd_page_changed(int wants_buttons)
{
    buttons_show(1, wants_buttons);
}

/* Every MIDI page is a form - fonts and synth settings reach the
 * config file, the module parameter needs a reload - so all three
 * want the commit trio. */
static void on_midi_page_changed(int wants_buttons)
{
    buttons_show(1, wants_buttons);
}

static void on_sound_page_changed(int wants_buttons)
{
    buttons_show(1, wants_buttons);
}

/* ------------------------------------------------------------------ */
/* Module pages - STUBS, except Volume                                */
/* ------------------------------------------------------------------ */

/* A placeholder page. The real module_*.c files replace the body of
 * this frame; the frame itself, its shadow and its packing are what the
 * spec pins down and what this proves.
 *
 * GTK_SHADOW_ETCHED_IN is the mockup's groupbox groove, free from the
 * stock 1.2 theme - see the spec's "Free bevels". */
static GtkWidget *stub_page(const char *title, const char *note)
{
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *label;

    frame = gtk_frame_new(title);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    label = gtk_label_new(note);
    /* LEFT-ALIGNED, not centred. Every real page is a column of
     * left-aligned rows, and a centred placeholder makes the frame look
     * like a different shape than it will be. */
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);
    gtk_widget_show(label);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    return frame;
}

/* Build one module's pane.
 *
 * A VIEW GETS NO NOTEBOOK AT ALL. The user's call, 2026-09-17: "the
 * player doesnt need the tabs at the top". A view is a single thing to
 * look at, so a one-tab notebook would be chrome drawn for its own
 * sake - and dropping it gives back ~26px of height, which together
 * with the absent button row (~44px) is what lets a 640x480 viewer fit
 * inside a 600-tall window. See SIDEBAR_W.
 *
 * So the same flag decides three things - buttons, tabs, and the
 * height budget - which is why it is a module property rather than
 * three separate switches. */
static GtkWidget *build_module(struct module *m)
{
    /* VOLUME IS REAL NOW. The rest are still stubs; each becomes a
     * vlhe_mod_*.c the same way. */
    if (m == &g_mod[MOD_STATUS])
        return status_build(report_to_status);

    if (m == &g_mod[MOD_VOLUME])
        return volume_build(report_to_status, on_volume_page_changed);

    if (m == &g_mod[MOD_CD])
        return cd_build(report_to_status, on_cd_page_changed);

    if (m == &g_mod[MOD_MIDI])
        return midi_build(report_to_status, on_midi_page_changed);

    if (m == &g_mod[MOD_SOUND])
        return sound_build(report_to_status, on_sound_page_changed);

    if (m == &g_mod[MOD_CDG])
        return cdg_build(report_to_status);

    if (m == &g_mod[MOD_RENDER])
        return render_build(report_to_status);

    if (m->kind == MOD_VIEW) {
        /* A VIEW WITH NO BUILDER YET - the pane itself, no notebook
         * wrapping it. MOD_STATUS is the other one and has its own
         * builder above. */
        char note[128];

        sprintf(note, FMT_SHELL_VIEWER_NOT_BUILT_YET, m->label);
        return stub_page(m->label, note);
    }

    /* A SETTINGS MODULE WITH NO BUILDER - none exists today; every row
     * above has one. The stub keeps a future row from crashing the
     * sidebar. The two-tab stub notebook and the `tab[]' names it drew
     * ("Advanced", "Format", "Patches", "Audio") went with design/47
     * Q7: nothing had displayed them since the real pages arrived. */
    {
        char note[128];

        sprintf(note, FMT_SHELL_NOT_BUILT_YET, m->label);
        return stub_page(m->label, note);
    }
}

/* ------------------------------------------------------------------ */
/* Sidebar                                                            */
/* ------------------------------------------------------------------ */

/* Show one module's notebook and hide the rest.
 *
 * SHOW THE NEW ONE BEFORE HIDING THE OLD would flash two pages; hiding
 * first leaves the pane empty for one iteration. Neither is visible at
 * this speed, and doing it in one pass keeps the invariant simple:
 * afterwards exactly one page is visible. */
/* WHICH MODULE IS ON SCREEN. Apply and OK need it: they commit the
 * page the user is looking at, so the shell has to know which that
 * is. show_module() is the one place it changes. */
static int g_current_mod = -1;

/*
 * AFTER LOAD OR UNLOAD, EVERY PAGE RE-READS THE MACHINE. Registered
 * with the Status page, which is the one that runs plans; it does not
 * know the other pages exist and should not.
 */
static int ask_three(const char *title, const char *text, const char *first, const char *second, const char *third);
static void sidebar_mark_dirty(void);
static int do_commit_mask(unsigned long mask);

/*
 * BEFORE A LOAD: IS THERE A PAGE EDITED BUT NOT APPLIED? - the user's
 * request, design/36 2b item 4, RESHAPED 2026-10-01.
 *
 * APPLIED, NOT SAVED - and that word is the whole change. A plan reads
 * the backend's MEMORY (vlhe_modopts() and the rest read cfg_sys in
 * memory; load_config() is a one-time read), and OK/Apply fill memory
 * from the pages. So the one way a Load acts on values the user is no
 * longer looking at is a page edited and never applied - the widgets
 * hold something memory has not seen. That is what this asks about,
 * and "Apply & Load" is the Apply button for those pages followed by
 * the load. Whether memory has been SAVED is a separate matter, kept
 * in view by the sidebar's stars and the quit prompt, and not this
 * dialog's business: a load uses what was applied either way.
 *
 * It used to say "the load will use the settings on disk, not these"
 * and offer Save & Load - true only because OK wrote memory and disk
 * together, and wrong the moment they part (design/49, the File-menu-
 * saves decision).
 *
 * AN IMPORTED DRAFT IS NO LONGER A CASE HERE. Once loaded it sits in
 * memory as applied, reviewed on the pages by the root who imported
 * it; a Load should use it, which is what importing it and pressing
 * Load means. The old branch put the draft back before "loading
 * without saving", because "without saving" was read as "from disk".
 *
 * AND AN UNLOAD ASKS NOTHING - the user, 2026-10-01: "Why does unload
 * worry about a saved config?" It does not read the pages: it stops
 * the daemons and removes the modules by name and restores the nodes
 * from the SESSION RECORDS (design/40), reading the config only as a
 * fallback for a machine loaded by a build that left none. An edited
 * CD major cannot change what an unload removes. The question was
 * inherited from the load side when one callback served both.
 *
 * Returns 0 to cancel the plan.
 */
static int before_plan(int unload)
{
    char text[600];
    unsigned long mask = 0;
    int i, n = 0, ans;

    if (unload)
        return 1;

    strcpy(text, FMT_SHELL_SOME_SETTINGS_HAVE_BEEN);
    for (i = 0; i < MOD_COUNT; i++) {
        if (g_mod[i].dirty == NULL || !g_mod[i].dirty())
            continue;
        if (strlen(text) + strlen(g_mod[i].label) + 80 >= sizeof text)
            break;
        strcat(text, n == 0 ? ":\n\n    " : "\n    ");
        strcat(text, g_mod[i].label);
        mask |= 1UL << i;
        n++;
    }
    if (n == 0)
        return 1;
    strcat(text, FMT_SHELL_LOAD_WILL_USE_APPLIED);

    /* THE LABELS' SHAPE IS THE USER'S, 2026-10-01: "Save & Load" became
     * "Apply & Load" with the reshaping above. The middle one is spelt
     * out - not "w/o", which a screen reader says as "w slash o" - and
     * is not "Discard", because it does not discard: the edits stay on
     * the pages. */
    ans = ask_three(STR_SHELL_TITLE_SETTINGS_NOT_APPLIED, text, STR_SHELL_BTN_APPLY_LOAD, STR_SHELL_BTN_LOAD_WITHOUT_APPLYING, STR_SHELL_BTN_CANCEL);
    if (ans == 1)
        return do_commit_mask(mask) == 0;
    return ans == 2;
}

/*
 * THE SAME QUESTION FOR ONE DAEMON'S Restart - 2026-09-27.
 *
 * WHY NOT before_plan(): THAT ONE ASKS ABOUT EVERY PAGE, and the
 * user's requirement is the opposite - *"if we are resetting vmidid
 * we should only care if vmidid is dirty and not any cd settings
 * etc"*. Nagging about a half-finished CD change while restarting
 * the synth is exactly what this avoids.
 *
 * WHICH PAGES COUNT IS THE BACKEND'S ANSWER, not a strcmp here:
 * `vlhe_restart_wants_page()' knows that Sound Settings feeds all
 * three daemons through `out_token()', which a per-page rule in the
 * GUI would have missed. See vlhe_backend.h.
 *
 * Returns 0 to cancel the restart.
 */
static int before_restart(const char *daemon)
{
    char text[600];
    unsigned long mask = 0;
    int i, n = 0, ans;

    if (daemon == NULL)
        return 1;

    sprintf(text, FMT_SHELL_SOME_SETTINGS_USES_HAVE, daemon);
    for (i = 0; i < MOD_COUNT; i++) {
        if (g_mod[i].dirty == NULL || !g_mod[i].dirty())
            continue;
        if (!vlhe_restart_wants_page(daemon, g_mod[i].label))
            continue;               /* dirty, but nothing to do with it */
        if (strlen(text) + strlen(g_mod[i].label) + 80 >= sizeof text)
            break;
        strcat(text, n == 0 ? ":\n\n    " : "\n    ");
        strcat(text, g_mod[i].label);
        mask |= 1UL << i;
        n++;
    }
    if (n == 0)
        return 1;                   /* nothing relevant is pending */
    strcat(text, FMT_SHELL_RESTART_WILL_USE_APPLIED);

    ans = ask_three(STR_SHELL_TITLE_SETTINGS_NOT_APPLIED, text, STR_SHELL_BTN_APPLY_RESTART, STR_SHELL_BTN_RESTART_WITHOUT_APPLYING, STR_SHELL_BTN_CANCEL);
    if (ans == 1)
        return do_commit_mask(mask) == 0;
    return ans == 2;
}

/*
 * BEFORE A RENDER: THE SAME QUESTION, SCOPED - "limited to
 * midi/renderer settings" (the user). Only Midi Settings and Render
 * MIDI feed the render; a dirty CD Drive page has no bearing on it
 * and must not get in its way.
 */
static int before_render(void)
{
    char text[400];
    unsigned long mask = 0;
    int i, n = 0, ans;

    strcpy(text, FMT_SHELL_SETTINGS_RENDER_USES_HAVE);
    for (i = 0; i < MOD_COUNT; i++) {
        if (i != MOD_MIDI && i != MOD_RENDER)
            continue;
        if (g_mod[i].dirty == NULL || !g_mod[i].dirty())
            continue;
        strcat(text, n == 0 ? ":\n\n    " : "\n    ");
        strcat(text, g_mod[i].label);
        mask |= 1UL << i;
        n++;
    }
    if (n == 0)
        return 1;
    strcat(text, FMT_SHELL_RENDER_WILL_USE_APPLIED);
    ans = ask_three(STR_SHELL_TITLE_SETTINGS_NOT_APPLIED, text, STR_SHELL_BTN_APPLY_RENDER, STR_SHELL_BTN_RENDER_WITHOUT_APPLYING, STR_SHELL_BTN_CANCEL);
    if (ans == 1)
        return do_commit_mask(mask) == 0;
    return ans == 2;
}

static void machine_changed(void)
{
    int i;

    for (i = 0; i < MOD_COUNT; i++)
        if (g_mod[i].machine != NULL)
            g_mod[i].machine();
}

static void show_module(int which)
{
    int i;

    if (which < 0 || which >= MOD_COUNT)
        return;

    g_current_mod = which;

    for (i = 0; i < MOD_COUNT; i++) {
        if (g_mod[i].page == NULL)
            continue;
        if (i == which)
            gtk_widget_show(g_mod[i].page);
        else
            gtk_widget_hide(g_mod[i].page);
    }

    /* A PAGE ASKS THE MACHINE WHEN IT IS SHOWN - see struct module. */
    if (g_mod[which].machine != NULL)
        g_mod[which].machine();

    /*
     * TELL THE SCROLLED WINDOW THE CONTENT CHANGED SIZE.
     *
     * The pane lives in a GtkScrolledWindow with an AUTOMATIC policy
     * so the window can go below 800x600 (see build_window). Showing
     * and hiding pages does NOT re-ask it: the viewport keeps the
     * largest allocation it has seen, so scrollbars raised by one
     * page stay up on every page after it.
     *
     * MEASURED BY THE USER, 2026-09-21, at an UNRESIZED 800x600:
     * Sound Settings' Options tab has text wide enough to raise them,
     * and they then persisted on its Device tab and on every module
     * after - except CD Settings, which happens to rebuild enough
     * widgets to force the recomputation by itself, so the bars
     * vanished there and came back on the next visit to Sound.
     *
     * That "except CD Settings" is what makes it look mysterious and
     * is the tell: nothing was clearing them deliberately.
     *
     * SAME BUG AS THE CD+G DETACH one level up - reparenting there,
     * show/hide here, and in both cases the container was never told.
     */
    if (g_pane != NULL) {
        gtk_widget_queue_resize(g_pane);
        if (g_scroll != NULL)
            gtk_widget_queue_resize(g_scroll);
    }

    /*
     * TELL THE PAGES WHETHER THEY ARE ON SCREEN. The CD page's poll
     * signals vdiscd and opens device nodes once a second; doing that
     * for a page nobody is looking at is waste, and the module says
     * so at its own on_poll(). It refreshes immediately when shown,
     * so nothing is stale.
     */
    cd_set_active(which == MOD_CD);
    cdg_set_active(which == MOD_CDG);
    /* THE TWO POLLING PAGES RUN THEIR TIMERS ONLY WHILE SHOWING -
     * design/47 V2 and T1, 2026-10-01. */
    volume_set_active(which == MOD_VOLUME);
    status_set_active(which == MOD_STATUS);

    /* THE BUTTONS FOLLOW THE PAGE, not just the module. A view never
     * wants them; a settings module may want them on some tabs and not
     * others, so it is asked. */
    /* A VIEW GETS HELP TOO. It has nothing to commit, but it is
     * exactly the kind of page someone wants explained - a CD+G panel
     * more than most. Same reasoning as Levels. */
    g_buttons_for = which;
    if (g_mod[which].kind == MOD_VIEW)
        buttons_show(1, 0);
    else if (which == MOD_VOLUME)
        buttons_show(1, volume_page_wants_buttons());
    else if (which == MOD_CD)
        buttons_show(1, cd_page_wants_buttons());
    else if (which == MOD_MIDI)
        buttons_show(1, midi_page_wants_buttons());
    else if (which == MOD_SOUND)
        buttons_show(1, sound_page_wants_buttons());
    else
        buttons_show(1, 1);

    status_set(g_mod[which].status);
}

/* The GtkCList selection callback.
 *
 * SIMPLER THAN THE TREE'S WAS. There is no root row to guard against
 * and no index offset: every row IS a module, and its row data is the
 * module index directly. */
static void on_module_selected(GtkCList *clist, gint row, gint column,
                               GdkEventButton *event, gpointer user_data)
{
    (void)clist;
    (void)column;
    (void)event;
    (void)user_data;

    show_module((int)row);
}

/* Build one GdkPixmap + mask pair from XPM data.
 *
 * ON FAILURE EVERYTHING STILL WORKS. A NULL pixmap is what the list was
 * given before icons existed and it draws a plain label - so a colour
 * allocation that fails on a loaded 256-colour desktop costs the icon,
 * not the program. That is why this returns rather than checking and
 * exiting: there is nothing here worth refusing to start over.
 */
/*
 * CACHED, AND THAT IS NOT A MICRO-OPTIMISATION.
 *
 * gdk_pixmap_create_from_xpm_d() allocates a new server-side pixmap
 * every call and nothing here frees one. That was harmless while the
 * only caller was sidebar_set_icons(), which runs once - but
 * sidebar_mark_dirty() runs on EVERY CONTROL CHANGE, and six leaked
 * pixmaps per keystroke is a real leak on a 1999 X server with a few
 * megabytes of video memory.
 *
 * Six entries, built on first use, never freed - which is correct
 * here because they live as long as the window does.
 */
static GdkPixmap *icon_load(GtkWidget *w, char **xpm, GdkBitmap **mask)
{
    static GdkPixmap *cache_pix[MOD_COUNT];
    static GdkBitmap *cache_mask[MOD_COUNT];
    int i;

    *mask = NULL;

    if (w == NULL || w->window == NULL || xpm == NULL)
        return NULL;

    /* Keyed on the XPM pointer, which is a distinct static array per
     * module in vlhe_icons.h. */
    for (i = 0; i < MOD_COUNT; i++) {
        if (g_mod[i].xpm != xpm)
            continue;
        if (cache_pix[i] == NULL)
            cache_pix[i] = gdk_pixmap_create_from_xpm_d(w->window,
                                                        &cache_mask[i],
                                                        NULL, xpm);
        *mask = cache_mask[i];
        return cache_pix[i];
    }

    /* Not one of ours - build it uncached rather than refusing. */
    return gdk_pixmap_create_from_xpm_d(w->window, mask, NULL, xpm);
}

static GtkWidget *build_sidebar(void)
{
    GtkWidget *scroll;
    GtkWidget *list;
    GtkWidget *outer;
    GtkWidget *frame;
    int        i;

    scroll = gtk_scrolled_window_new(NULL, NULL);
    /* NEVER a horizontal scrollbar. The column is fixed and a long
     * label is better clipped than given a bar that steals height from
     * a list this short. Vertical AUTOMATIC so a sixth module gets one
     * and five do not. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    /* A FLAT LIST, NOT A TREE - the user's call, 2026-09-17, and it
     * was a GtkCTree only because the mockup's reference (KDE's own
     * kcontrol) uses one.
     *
     * KDE NEEDS A TREE AND WE DO NOT. Its Settings really nest -
     * Desktop has Background, Borders, Colors beneath it. Ours had one
     * root with every module as a leaf, which is a tree in name only:
     * nothing collapses and nothing nests.
     *
     * AND THE STRUCTURE COST REAL WIDTH. Measured after the sidebar
     * was narrowed to 160: "Sound Settings" clipped, though the text
     * is only ~98px. The tree indent, the expander column and the
     * root's nesting were eating ~70px of a 160px pane. A GtkCList has
     * none of those, so the same labels fit with room to spare.
     *
     * Two pieces of bookkeeping went with the root row, which is a
     * fair sign it was carrying nothing: a guard making the root
     * non-selectable, and an index+1 offset that existed only because
     * 0 had to mean "no data set". */
    list = gtk_clist_new(1);
    gtk_clist_set_selection_mode(GTK_CLIST(list), GTK_SELECTION_BROWSE);
    gtk_clist_set_column_width(GTK_CLIST(list), 0, SIDEBAR_W);
    gtk_clist_set_column_auto_resize(GTK_CLIST(list), 0, FALSE);
    gtk_clist_set_shadow_type(GTK_CLIST(list), GTK_SHADOW_IN);

    /* TALLER ROWS THAN THE TEXT NEEDS, so a 16x16 icon sits in one
     * without being cropped. */
    gtk_clist_set_row_height(GTK_CLIST(list), 20);

    for (i = 0; i < MOD_COUNT; i++) {
        char *row[1];
        gint  r;

        /* THE CAST IS THE REASON gtk_clist_append's text argument is
         * not const: 1.2 predates const-correctness here. Casting away
         * const on a string literal we never write is the same thing
         * vsoundvol_gtk.c does. */
        row[0] = (char *)g_mod[i].label;

        /* NO ICONS YET - sidebar_set_icons() attaches them once the
         * window is realized. See that function for why. */
        r = gtk_clist_append(GTK_CLIST(list), row);

        g_mod[i].row = r;
        gtk_clist_set_row_data(GTK_CLIST(list), r, (gpointer)(long)i);
    }

    gtk_signal_connect(GTK_OBJECT(list), "select_row",
                       GTK_SIGNAL_FUNC(on_module_selected), NULL);

    g_list = list;

    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_widget_show(list);

    /* FIXED WIDTH, FREE HEIGHT. -1 means "whatever you were going to
     * be" for that axis, so the sidebar stretches with the window
     * vertically and never horizontally. This is the one fixed size in
     * the whole layout. */
    gtk_widget_set_usize(scroll, SIDEBAR_W, -1);
    gtk_widget_show(scroll);

    /*
     * THE NO-CONFIG NOTICE, UNDER THE LIST.
     *
     * The user's design, 2026-09-19, after Apply reported "Settings
     * saved" on a machine where they had just declined to create a
     * config: "hitting apply everytime and it asking for the config
     * is the wrong approach... having the status bar mention
     * something like not saved or saved is right but easily missed...
     * There is plenty of space in the left bar."
     *
     * AND THE SIDEBAR IS THE RIGHT PLACE because it is visible the
     * WHOLE TIME. A status-bar line appears at the moment of action
     * and scrolls away; this is a standing statement about what kind
     * of session this is, in the 350-odd pixels below six rows.
     *
     * SHOWN ONLY WHEN THERE IS NO CONFIG. A machine with one needs no
     * notice, and a panel that is always there stops being read.
     */
    outer = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    /*
     * WHO THIS PROCESS IS, ALWAYS SHOWN - 2026-09-30, after a target
     * run in which vlhe.gtk started from Corel's "File Manager (root)"
     * came up as nova: that explorer is root but delegates every
     * launch to the desktop user's kfm, so its children are the
     * user's (design/15). Nothing on screen said which it was. This
     * is the same getuid()/geteuid() the notes below already read,
     * printed once where it is always visible.
     */
    {
        GtkWidget *wf = gtk_frame_new(NULL);

        gtk_frame_set_shadow_type(GTK_FRAME(wf), GTK_SHADOW_IN);
        gtk_container_border_width(GTK_CONTAINER(wf), 4);
        g_whoami = gtk_label_new("");
        gtk_label_set_justify(GTK_LABEL(g_whoami), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(g_whoami), 0.0, 0.0);
        gtk_container_add(GTK_CONTAINER(wf), g_whoami);
        gtk_widget_show(g_whoami);
        gtk_box_pack_start(GTK_BOX(outer), wf, FALSE, FALSE, 0);
        gtk_widget_show(wf);
    }

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 4);

    /* WRAPPED BY HAND. GtkLabel in 1.2 has no wrap-to-width that
     * works inside a fixed column - set_line_wrap needs an allocation
     * it does not have yet at build time, so the newlines are
     * written where they belong for SIDEBAR_W. */
    g_noconfig = gtk_label_new(
        STR_SHELL_LABEL_RUNNING_WITHOUT_CONFIGURATION_FILE);
    gtk_label_set_justify(GTK_LABEL(g_noconfig), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(g_noconfig), 0.0, 0.0);
    gtk_container_add(GTK_CONTAINER(frame), g_noconfig);
    gtk_widget_show(g_noconfig);

    g_noconfig_frame = frame;
    /* NOT shown here - sidebar_update_noconfig() decides, and it is
     * called after the backend is up. */

    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);
    gtk_widget_show(outer);

    return outer;
}

/*
 * SHOW OR HIDE THE NOTICE. Called at startup and after any save, so
 * the panel disappears the moment a config exists.
 */
/*
 * DID WE START WITH A CONFIG? Set once, at startup, before anything
 * can change it.
 *
 * THE USER'S QUESTION, 2026-09-19: "Can the program tell if it loaded
 * detected the config file? Maybe change the message to state if you
 * are seeing this after the fact the config file was deleted and file
 * save fixes it."
 *
 * It can, and the two cases genuinely want different words. A fresh
 * install has never had a file and the panel is an offer. A file that
 * was deleted underneath a running program is a surprise, and saying
 * "no configuration file" there reads as though the program lost it.
 */
static int g_had_config_at_start;
/* What vlhe_conf_draft_readback() said at start - shown once the
 * status line exists. */
static char g_readback_note[400];

/* The identity line: the real user, and what the process can do. */
static void sidebar_update_whoami(void)
{
    char name[40];
    char text[120];

    if (g_whoami == NULL)
        return;
    if (vlhe_conf_user_name(name, sizeof name) != 0)
        strcpy(name, "?");
    if (vlhe_priv_can_unlock())
        sprintf(text, FMT_SHELL_RUNNING_AS_ROOT_LOCKED, name);
    else if (vlhe_priv_can_act() && getuid() != 0)
        sprintf(text, FMT_SHELL_RUNNING_AS_UNLOCKED_AS, name);
    else if (vlhe_priv_can_act())
        strcpy(text, FMT_SHELL_RUNNING_AS_ROOT);
    else
        sprintf(text, FMT_SHELL_RUNNING_AS, name);
    gtk_label_set_text(GTK_LABEL(g_whoami), text);
}

static void sidebar_update_noconfig(void)
{
    sidebar_update_whoami();
    if (g_noconfig_frame == NULL)
        return;

    /*
     * NOT ROOT COMES FIRST, because it is the more useful thing to
     * say. A user who cannot load modules will find that out by
     * pressing something and being refused; the config notice is
     * about persistence, which they find out later and less
     * painfully.
     *
     * THE CONTROLS GREY WITH NO EXPLANATION TODAY, which is the gap
     * this closes - "why can I not press that" has no answer on the
     * screen, and on a PORTABLE copy the answer is the documented
     * way to use it: run it as root. The portable tarball ships no
     * setuid build deliberately (design/33), so there is no Modify
     * button to reach for and this notice is the whole story.
     *
     * SHOWN ONLY WHEN IT MATTERS. Root sees nothing, and the setuid
     * build's unlocked session sees nothing, because in both cases
     * everything works.
     */
    /* A CONFIG ROOT DID NOT WRITE, seen by someone who cannot act on it
     * - design/49 R1. Its settings are not in use; Load Configuration
     * can review it. Someone who CAN act gets the takeover offer. */
    if (g_noconfig != NULL && !vlhe_priv_can_act()) {
        char why[VLHE_PATH_MAX + 64];

        if (!vlhe_conf_system_trusted(why, (int) sizeof why)) {
            gtk_label_set_text(GTK_LABEL(g_noconfig),
                               STR_SHELL_TEXT_CONFIG_FILE_NOT_ROOT);
            gtk_widget_show(g_noconfig_frame);
            return;
        }
    }
    if (!vlhe_priv_can_act() && vlhe_priv_unlocked() && g_noconfig != NULL) {
        gtk_label_set_text(GTK_LABEL(g_noconfig),
                           STR_SHELL_TEXT_NOT_RUNNING_AS_ROOT);
        gtk_widget_show(g_noconfig_frame);
        return;
    }

    /*
     * SAY WHERE THE USER'S SETTINGS ACTUALLY ARE when it is not the
     * default place - the tree in portable mode. A user who agreed to
     * $HOME, or was told about /tmp, should not have to remember; and
     * the /tmp case carries the one fact that matters most about it.
     */
    if (g_noconfig != NULL) {
        int tier = vlhe_conf_user_tier();

        if (tier == VLHE_USER_TIER_HOME) {
            gtk_label_set_text(GTK_LABEL(g_noconfig),
                               STR_SHELL_TEXT_YOUR_SETTINGS_ARE_SAVED);
            gtk_widget_show(g_noconfig_frame);
            return;
        }
        if (tier == VLHE_USER_TIER_TMP) {
            gtk_label_set_text(GTK_LABEL(g_noconfig),
                               STR_SHELL_TEXT_YOUR_SETTINGS_ARE_TMP);
            gtk_widget_show(g_noconfig_frame);
            return;
        }
    }

    /* A DRAFT IN USE - design/51. The machine settings on screen are
     * this user's saved ones, which the system does not run until
     * root imports them; said for as long as it is true. */
    if (g_noconfig != NULL) {
        char when[40];

        if (vlhe_conf_draft_in_use(NULL, 0, when, (int) sizeof when)) {
            char text[160];

            sprintf(text, FMT_SHELL_MACHINE_SETTINGS_ARE_FROM, when);
            gtk_label_set_text(GTK_LABEL(g_noconfig), text);
            gtk_widget_show(g_noconfig_frame);
            return;
        }
    }

    if (vlhe_conf_exists()) {
        gtk_widget_hide(g_noconfig_frame);
        return;
    }

    /* GONE SINCE WE STARTED, so say that rather than implying it was
     * never there. The settings in the window are still live - only
     * the file is missing - and File / Save Configuration writes them
     * straight back, which is the useful thing to tell someone. */
    if (g_had_config_at_start && g_noconfig != NULL)
        gtk_label_set_text(GTK_LABEL(g_noconfig),
                           STR_SHELL_TEXT_CONFIGURATION_FILE_WAS_REMOVED);

    gtk_widget_show(g_noconfig_frame);
}

/* Attach the sidebar icons. CALLED AFTER THE WINDOW IS REALIZED, and
 * the ordering is the whole point of splitting this out.
 *
 * gdk_pixmap_create_from_xpm_d() needs a real GdkWindow for the visual
 * and colormap. build_sidebar() runs long before anything is on screen,
 * so nothing it creates has one yet, and two attempts to force it early
 * both failed - each worse than the last, which is why this is written
 * down rather than left to be rediscovered:
 *
 *   1. gtk_widget_realize(tree) on a PARENTLESS widget SEGFAULTS.
 *      gtk_widget_get_parent_window() asserts on ->parent and
 *      dereferences it regardless. Core dumped.
 *   2. Parenting it to the scrolled window first stopped the crash but
 *      still warned from gtk_style_attach() - realizing a child before
 *      its ancestors are realized is not something 1.2 supports.
 *
 * So the rule is: let GTK realize the hierarchy in its own order, and
 * hang the pixmaps on afterwards. gtk_clist_set_pixtext() sets an
 * existing row's icon without rebuilding it.
 *
 * A BLANK SIDEBAR AT THIS POINT IS NOT THIS FUNCTION'S FAULT. One
 * appeared while the above was being sorted out and was briefly blamed
 * on realize ordering; the actual cause was a lost
 * gtk_container_add() - the tree had no parent at all, so it was never
 * displayed. If the rows vanish, check the packing before suspecting
 * anything subtle.
 */
/*
 * THE SIDEBAR'S UNSAVED MARKER - an asterisk on the row's label.
 *
 * WHY AN ASTERISK. It is the editor convention, it needs no artwork
 * where a changed ICON would need a variant of every 16x16 XPM, and
 * it survives whatever font the user picked in Preferences.
 *
 * WHY THE SIDEBAR AT ALL, and it is the user's point (2026-09-18):
 * pages are built once and hidden, so a user can edit Sound, move to
 * CD Settings and forget. Until now the only sign was the prompt on
 * quit, which is the worst possible moment to find out. The row says
 * it the whole time.
 *
 * Called whenever a control changes, and after Apply, OK or Cancel.
 */
static void sidebar_mark_dirty(void)
{
    GdkPixmap *pix;
    GdkBitmap *mask;
    char       label[80];
    int        i;

    if (g_list == NULL || g_list->window == NULL)
        return;

    for (i = 0; i < MOD_COUNT; i++) {
        int d = (g_mod[i].dirty != NULL) ? g_mod[i].dirty() : 0;

        sprintf(label, "%.60s%s", g_mod[i].label, d ? " *" : "");

        /* THE ICON HAS TO BE RE-ATTACHED with the text, because
         * set_pixtext sets both at once - passing the label alone
         * through set_text would drop the icon. */
        pix = icon_load(g_list, g_mod[i].xpm, &mask);
        if (pix != NULL)
            gtk_clist_set_pixtext(GTK_CLIST(g_list), g_mod[i].row, 0,
                                  label, 4, pix, mask);
        else
            gtk_clist_set_text(GTK_CLIST(g_list), g_mod[i].row, 0, label);
    }
}

static void sidebar_set_icons(void)
{
    GdkPixmap *pix;
    GdkBitmap *mask;
    int i;

    if (g_list == NULL || g_list->window == NULL)
        return;

    for (i = 0; i < MOD_COUNT; i++) {
        pix = icon_load(g_list, g_mod[i].xpm, &mask);
        if (pix == NULL)
            continue;

        /* SPACING 4 between icon and text, matching what the rows were
         * appended with so attaching an icon does not shift the label. */
        gtk_clist_set_pixtext(GTK_CLIST(g_list), g_mod[i].row, 0,
                              (char *)g_mod[i].label, 4, pix, mask);
    }
}

/* ------------------------------------------------------------------ */
/* Menu bar and buttons - STUBS                                       */
/* ------------------------------------------------------------------ */

/* Records "yes, discard" before the dialog is destroyed. */
static void quit_discard(GtkWidget *w, gpointer data)
{
    (void)w;
    *(int *)data = 1;
}

/* Records "save, then quit" - the saving happens after the dialog is
 * gone, in confirm_quit(), so a failure can keep the window open. */
static void quit_save(GtkWidget *w, gpointer data)
{
    (void)w;
    *(int *)data = 2;
}


/*
 * LEAVING WITH UNSAVED CHANGES - the one moment edits are actually
 * about to be lost, and the only place a prompt belongs.
 *
 * NOT ON NAVIGATION. Switching pages with an edit pending prompts
 * nothing: the pages are built once and hidden, so the widgets keep
 * what was typed and OK commits every page at the end. KDE reaches
 * the same behaviour by a different route - its modules are separate
 * processes that stay ALIVE when you navigate away
 * (configlist.cpp:209, `if (!process)'), so the edits survive there
 * too. The user's call, and prompting per page "would be most
 * annoying with multiple pages".
 *
 * Returns 1 to go ahead and quit, 0 to stay.
 */
/* ------------------------------------------------------------------ */
/* A portable copy's first run                                        */
/* ------------------------------------------------------------------ */

static void seed_yes(GtkWidget *w, gpointer data)
{
    (void)w;
    *(int *)data = 1;
}

/*
 * ASK ONCE, ON THE FIRST RUN OF A PORTABLE COPY ON AN INSTALLED
 * MACHINE - the user's case, 2026-09-22: someone with VLHE installed
 * who unpacks a newer build "to try it out to see if it addresses any
 * issues they have".
 *
 * THEY ALMOST CERTAINLY WANT THEIR OWN SETTINGS in the new copy -
 * testing a build against defaults measures the wrong thing - but it
 * is their call, so this asks. vlhe_portable_offer_seed() decides
 * whether the question arises at all, and its third condition (the
 * tree has no config yet) is what makes this happen once.
 *
 * THE WORDING IS THE USER'S: "[copy old settings] [use new
 * defaults]". Not "trial" anywhere a user can see - that reads as a
 * time-limited demo of paid software, and this is GPL with nothing
 * withheld.
 *
 * NEITHER BUTTON IS DESTRUCTIVE, so unlike confirm_quit() there is no
 * safe default to protect: copying is a COPY, and defaults leave the
 * installed config untouched. Copy takes the focus because it is what
 * the case this exists for wants.
 */
static void
offer_seed(void)
{
    GtkWidget *dlg, *lab, *btn;
    static int answer;
    char       msg[512];

    if (!vlhe_portable_offer_seed())
        return;

    answer = 0;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_START_FROM_YOUR_SETTINGS);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    sprintf(msg,
        FMT_SHELL_COPY_RUNS_FROM_OWN);

    lab = gtk_label_new(msg);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_COPY_OLD_SETTINGS), STR_SHELL_BTN_COPY_OLD_SETTINGS_TIP);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(seed_yes), (gpointer)&answer);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);      /* while we still hold it */

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_USE_NEW_DEFAULTS), STR_SHELL_BTN_USE_NEW_DEFAULTS_TIP);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();

    if (answer) {
        char why[160];
        int  n = vlhe_portable_seed(why, sizeof why);

        if (n < 0) {
            char line[256];

            sprintf(line, FMT_SHELL_COULD_NOT_COPY, why);
            status_set(line);
        } else if (n == 0) {
            status_set(STR_SHELL_MSG_NOTHING_COPY_STARTING_FROM);
        } else {
            /* THE PAGES WERE BUILT FROM THE OLD ANSWER. The backend
             * has re-read the files, so every module's reload() is
             * what makes the window agree with them. */
            int i;

            for (i = 0; i < MOD_COUNT; i++)
                if (g_mod[i].reload != NULL)
                    g_mod[i].reload();
            status_set(STR_SHELL_MSG_YOUR_INSTALLED_SETTINGS_WERE);
        }
    }
}

/*
 * A MODAL QUESTION WITH A SYNCHRONOUS ANSWER - the confirm_quit()
 * pattern, lifted out so it can be asked from more than one place.
 * GTK 1.2 has no gtk_dialog_run(); a nested gtk_main() that the
 * dialog's destroy quits is how it is done, and confirm_quit() below
 * has done it since the beginning.
 *
 * `no' == NULL makes it a NOTICE with one button, returning 1.
 */
static int ask_answer;

static void ask_set_yes(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    ask_answer = 1;
}

/*
 * THREE ANSWERS - 1 for the first button, 2 for the second, 0 for the
 * third or the window close. Save-before-load wants exactly this
 * shape: "Apply & Load" / "Load without Applying" / "Cancel". The
 * middle one exists because loading the SAVED config while a
 * different one is on screen is a legitimate thing to do, and a
 * dialog without it would force a Cancel and a hand reload.
 */
static void ask_set_alt(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    ask_answer = 2;
}

static int ask_three(const char *title, const char *text, const char *first, const char *second, const char *third)
{
    GtkWidget *dlg, *lab, *btn;

    ask_answer = 0;
    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_MOUSE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    lab = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab,
                       TRUE, TRUE, 8);
    gtk_widget_show(lab);

    btn = gtk_button_new_with_label(first);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(ask_set_yes), NULL);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);

    btn = gtk_button_new_with_label(second);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(ask_set_alt), NULL);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);

    btn = gtk_button_new_with_label(third);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);

    /* One size for the three, as every dialog's - the user,
     * 2026-10-01, on the Unsaved settings dialog before a Load. */
    vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();
    return ask_answer;
}

/*
 * DRIFT FROM THE BASELINE, ASKED IN A DIALOG - design/54 7h Stage 3.3,
 * four answers since 2026-10-04 (the user chose "Undo changes" as the way
 * back, and a second agent's parallel labels). Each label says what
 * happens to the changes: Undo changes (put back, then load), Accept
 * changes (they become the baseline), Load anyway (left alone, asked
 * again next Load), Cancel. CANCEL IS THE DEFAULT and the window's close:
 * the other three each change something, and none should happen from a
 * stray Return. UNDO SITS APART ON THE LEFT, Cancel at the right edge, so
 * neither is clicked by habit.
 */
static int g_drift_answer;

static void on_drift_answer(GtkWidget *w, gpointer d)
{
    (void)w;
    g_drift_answer = (int)(long) d;
}

static GtkWidget *drift_button(GtkWidget *box, const char *label,
                               const char *tip, int answer, GtkWidget *dlg)
{
    GtkWidget *btn = vlhe_tipped(gtk_button_new_with_label(label), tip);

    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(on_drift_answer),
                       (gpointer)(long) answer);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    return btn;
}

static int gui_drift_ask(const struct vlhe_drift *d, int n)
{
    GtkWidget *dlg, *lab, *btn;
    char text[2048];
    size_t len;
    int i, held;

    sprintf(text, STR_SHELL_DRIFT_HEAD, n);
    len = strlen(text);
    for (i = 0; i < n && i < 8; i++) {
        char line[3 * VLHE_PATH_MAX];

        sprintf(line, "\n    %.100s: was %.100s, now %.100s",
                d[i].what, d[i].was, d[i].now);
        if (len + strlen(line) + 1 >= sizeof text)
            break;
        strcpy(text + len, line);
        len += strlen(line);
    }
    if (n > 8 && len + 40 < sizeof text) {
        sprintf(text + len, "\n    ... and %d more", n - 8);
        len = strlen(text);
    }
    if (len + strlen(STR_SHELL_DRIFT_TAIL) + 1 < sizeof text)
        strcpy(text + len, STR_SHELL_DRIFT_TAIL);

    g_drift_answer = VLHE_DRIFT_CANCEL;     /* the close, too */
    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_DRIFT_TITLE);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_MOUSE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    lab = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    /* Undo at the left on its own; the rest packed from the right edge,
     * Cancel outermost. */
    btn = drift_button(GTK_DIALOG(dlg)->action_area, STR_SHELL_DRIFT_UNDO,
                       STR_SHELL_DRIFT_UNDO_TIP, VLHE_DRIFT_UNDO, dlg);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    btn = drift_button(GTK_DIALOG(dlg)->action_area, STR_SHELL_DRIFT_CANCEL,
                       STR_SHELL_DRIFT_CANCEL_TIP, VLHE_DRIFT_CANCEL, dlg);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_end(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                     FALSE, FALSE, 0);
    gtk_widget_grab_default(btn);
    btn = drift_button(GTK_DIALOG(dlg)->action_area, STR_SHELL_DRIFT_LOAD,
                       STR_SHELL_DRIFT_LOAD_TIP, VLHE_DRIFT_KEEP, dlg);
    gtk_box_pack_end(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                     FALSE, FALSE, 0);
    btn = drift_button(GTK_DIALOG(dlg)->action_area, STR_SHELL_DRIFT_ACCEPT,
                       STR_SHELL_DRIFT_ACCEPT_TIP, VLHE_DRIFT_UPDATE, dlg);
    gtk_box_pack_end(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                     FALSE, FALSE, 0);
    vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    /* NOT RAISED WHILE IT IS OPEN - design/54 D48, design/55 R6. This
     * is asked from inside the Load's raised window; the nested
     * gtk_main() below would run every handler and timeout as root.
     * Lowered completely for the dialog, raised back for the Load. */
    held = vlhe_priv_suspend();
    gtk_widget_show(dlg);
    gtk_main();
    if (vlhe_priv_resume(held) != 0)
        fprintf(stderr, "vlhe.gtk: root could not be regained after the"
                        " drift question - the Load's root steps will"
                        " fail\n");
    return g_drift_answer;
}

/*
 * THE SAME QUESTION WITH A SCROLLING BODY - design/49 R3. The review
 * of an imported configuration showed the first 12 of each list and
 * accepted the rest unseen; this shows every line, in a viewport the
 * user scrolls, at a size that fits a 768-line screen on the target.
 * Same synchronous shape as ask_yes_no().
 */
static int ask_yes_no_scrolled(const char *title, const char *head, const char *body, const char *tail, const char *yes, const char *no)
{
    GtkWidget *dlg;
    GtkWidget *lab;
    GtkWidget *sw;
    GtkWidget *btn;

    ask_answer = 0;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    lab = gtk_label_new(head);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, FALSE, FALSE, 4);
    gtk_widget_show(lab);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_usize(sw, 560, 300);
    lab = gtk_label_new(body);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_misc_set_padding(GTK_MISC(lab), 6, 4);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(sw), lab);
    gtk_widget_show(lab);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), sw, TRUE, TRUE, 4);
    gtk_widget_show(sw);

    if (tail != NULL && tail[0] != '\0') {
        lab = gtk_label_new(tail);
        gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab,
                           FALSE, FALSE, 4);
        gtk_widget_show(lab);
    }

    btn = gtk_button_new_with_label(yes);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(ask_set_yes), NULL);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);

    if (no != NULL) {
        btn = gtk_button_new_with_label(no);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                           FALSE, FALSE, 0);
        gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                                  GTK_SIGNAL_FUNC(gtk_widget_destroy),
                                  GTK_OBJECT(dlg));
        gtk_widget_show(btn);
    }

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();
    return ask_answer;
}

static int ask_yes_no(const char *title, const char *text,
                      const char *yes, const char *no)
{
    GtkWidget *dlg;
    GtkWidget *lab;
    GtkWidget *btn;

    ask_answer = 0;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    lab = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    btn = gtk_button_new_with_label(yes);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(ask_set_yes), NULL);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);

    if (no != NULL) {
        btn = gtk_button_new_with_label(no);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                           FALSE, FALSE, 0);
        gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                                  GTK_SIGNAL_FUNC(gtk_widget_destroy),
                                  GTK_OBJECT(dlg));
        gtk_widget_show(btn);
    }

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();

    return ask_answer;
}

static int save_config(void);   /* File > Save Configuration, below */

static int confirm_quit(void)
{
    GtkWidget *dlg;
    GtkWidget *lab;
    GtkWidget *btn;
    static int answer;

    /* THE PAGES TOO, NOT THE BACKEND FLAG ALONE - design/47 Q1. Edits
     * reach the backend only in collect(), so an edited page that had
     * not been Applied quit with no prompt while its asterisk was
     * showing. on_revert_saved() (Cancel, until 2026-10-02) has asked
     * the modules since row 55; this is the same question in the same
     * words. */
    {
        int any = vlhe_dirty(), i;

        for (i = 0; i < MOD_COUNT && !any; i++)
            if (g_mod[i].dirty != NULL && g_mod[i].dirty())
                any = 1;
        if (!any)
            return 1;
    }

    answer = 0;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_UNSAVED_CHANGES);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    /*
     * SAY WHICH PAGES - design/36 row 55. The user hit this dialog with
     * the machine fully unloaded and could not tell what it meant: "I
     * have unsaved changes but I don't know what". It was telling the
     * truth (LinkCdrom really had moved) and not saying where. The
     * sidebar already knows - it draws the asterisk from each module's
     * dirty() - so the same list goes in the dialog.
     */
    {
        char text[512];
        int  i, n = 0;

        strcpy(text, FMT_SHELL_SOME_SETTINGS_HAVE_BEEN_2);
        for (i = 0; i < MOD_COUNT; i++) {
            if (g_mod[i].dirty == NULL || !g_mod[i].dirty())
                continue;
            if (strlen(text) + strlen(g_mod[i].label) + 6 >= sizeof text)
                break;
            strcat(text, n == 0 ? " on:\n\n    " : "\n    ");
            strcat(text, g_mod[i].label);
            n++;
        }
        strcat(text, n == 0 ? STR_SHELL_TEXT_LEAVING_DISCARDS_DOT
                            : STR_SHELL_TEXT_LEAVING_DISCARDS);
        lab = gtk_label_new(text);
    }
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    /* THREE WAYS OUT, THE USER'S, 2026-10-01: "It needs a 'Save & Quit'
     * button. 'Discard & Quit', and the 'Cancel'." Save & Quit is the
     * Save Configuration path, so a user who was about to lose edits
     * keeps them in one press rather than Cancel, File, Save, Quit.
     *
     * NEITHER DESTRUCTIVE ONE IS THE DEFAULT. Return should neither
     * throw work away nor write a file, so Cancel takes the focus and
     * a stray keypress is harmless. */
    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_SAVE_QUIT), STR_SHELL_BTN_SAVE_QUIT_TIP);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(quit_save), (gpointer)&answer);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_DISCARD_QUIT), STR_SHELL_BTN_DISCARD_QUIT_TIP);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(quit_discard), (gpointer)&answer);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);

    /* Same width and height for all three - see vlhe_buttons_equalise(). */
    vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

    /* MODAL AND SYNCHRONOUS. gtk_main() nests until the dialog is
     * destroyed, so the answer is known when this returns - GTK 1.2
     * has no gtk_dialog_run(). */
    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();

    /* SAVE & QUIT QUITS ONLY WHEN THE SAVE WAS COMPLETE. A write failure
     * or a validation refusal has already been reported by the save
     * path, and so has the case where the machine half needed root and
     * was kept rather than saved - in each the window stays open, with
     * the status line saying why, instead of taking the message away
     * with it. The user can then Save Draft, or Discard & Quit. */
    if (answer == 2)
        return save_config() == 0 && !vlhe_dirty();
    return answer;
}

static void on_quit(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (!confirm_quit())
        return;
    (void) render_abort();          /* a render stops with the window - D58 */
    gtk_main_quit();
}

/* THE WINDOW MANAGER'S CLOSE BUTTON. It must be `delete_event' and
 * not `destroy': destroy fires when the window is already gone, which
 * is too late to ask anything. Returning TRUE stops the destroy. */
static gint on_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w;
    (void)e;
    (void)data;
    if (!confirm_quit())
        return TRUE;
    (void) render_abort();          /* a render stops with the window - D58 */
    return FALSE;
}

/*
 * GO TO A MODULE - by SELECTING ITS ROW, never by showing the page.
 *
 * gtk_clist_select_row fires "select_row", so show_module() runs from
 * the callback and the sidebar highlight cannot disagree with the
 * page. Calling show_module() directly is what left the first capture
 * of this window with the wrong row highlighted and no way to tell
 * from the screen which module the program thought was current.
 */
static void select_module(int which)
{
    if (which < 0 || which >= MOD_COUNT)
        return;
    if (g_list != NULL)
        gtk_clist_select_row(GTK_CLIST(g_list), g_mod[which].row, 0);
    else
        show_module(which);
}

/* WHERE OK AND CANCEL RETURN TO - the user's chosen start page,
 * bounded, because a config file can hold anything. */
static int default_page(void)
{
    struct vlhe_prefs pf;

    if (vlhe_prefs(&pf) != 0)
        return MOD_STATUS;
    if (pf.start_page < 0 || pf.start_page >= MOD_COUNT)
        return MOD_STATUS;
    return pf.start_page;
}

/* ------------------------------------------------------------------ *
 * The backup, and the forward declarations the dialogs share           *
 * ------------------------------------------------------------------ */

/* Defined with Apply and OK below - a failed restore reports the same
 * way a failed Save does, so there is one dialog for "could not
 * write" rather than two that can drift. */
static void commit_failed(const char *path, const char *why);
static void load_failed(const char *path, const char *why);
static int  review_config(const char *path);
static void offer_takeover(void);
static int  ask_yes_no(const char *title, const char *text,
                       const char *yes, const char *no);

/*
 * FILE > RESTORE BACKUP - what the first-run dialog's "Restore backup"
 * button became when that dialog went, 2026-10-01 (design/51).
 *
 * THE DIALOG WENT BECAUSE ITS QUESTION HAD NO JOB LEFT. "Create a
 * configuration file now?" exists only when something must be written
 * before the program is usable; with Save-only nothing is written
 * until File > Save, so a first start simply says so in the status
 * line and the sidebar. What the dialog ALSO did - offer the `.old'
 * backup when the live file was gone - is worth keeping, and it was
 * always strange as a start-up question only: a user who deletes the
 * file while the program is running had no way to reach it. Now it
 * is a menu item, available whenever a backup exists.
 *
 * The user found `.old' was written and never read (2026-09-19,
 * design/32); this is the reader.
 */
static void on_restore_backup(GtkWidget *w, gpointer data)
{
    struct vlhe_commit_err err;
    char   text[VLHE_PATH_MAX + 320];
    char   when[64];
    int    i;

    (void)w; (void)data;

    if (!vlhe_conf_backup_exists()) {
        status_set(STR_SHELL_MSG_THERE_NO_BACKUP_RESTORE);
        return;
    }
    if (!vlhe_can_write_system_conf()) {
        status_set(STR_SHELL_MSG_RESTORING_BACKUP_WRITES_SYSTEM);
        return;
    }
    when[0] = '\0';
    (void)vlhe_conf_backup_when(when, sizeof when);
    /* IT NAMES THE DATE because "restore it" is a different decision
     * for this afternoon than for last year - and it says what is
     * replaced, since unlike the first-run case the live file may
     * well exist. */
    sprintf(text,
            FMT_SHELL_PUT_BACKUP_OLD_BACK,
            VLHE_PATH_MAX - 1, vlhe_system_conf_path(),
            when[0] != '\0' ? "    saved " : "",
            when[0] != '\0' ? when : "",
            when[0] != '\0' ? "\n" : "");
    if (!ask_yes_no(STR_SHELL_TITLE_RESTORE_BACKUP, text, STR_SHELL_BTN_RESTORE, STR_SHELL_BTN_CANCEL))
        return;

    memset(&err, 0, sizeof err);
    if (vlhe_conf_restore_backup(&err) != 0) {
        commit_failed(err.path[0] ? err.path : "(backup)",
                      err.why[0] ? err.why : STR_SHELL_MSG_COULD_NOT_BE_RESTORED);
        return;
    }
    /* THE PAGES HOLD WHAT THEY HELD, so the restored values have to be
     * pushed back into them - otherwise the file and the window
     * disagree until the next start. reload() is Cancel's half and
     * does exactly this. */
    for (i = 0; i < MOD_COUNT; i++)
        if (g_mod[i].reload != NULL)
            g_mod[i].reload();
    sidebar_mark_dirty();
    sidebar_update_noconfig();
    status_set(STR_SHELL_MSG_CONFIGURATION_RESTORED_FROM_BACKUP);
}

/* ------------------------------------------------------------------ *
 * Apply and OK - where a setting stops being a widget                 *
 * ------------------------------------------------------------------ */

/*
 * A MODAL COMPLAINT, for a commit that failed.
 *
 * Not the status bar: a write that did not happen must not scroll
 * away unread, and "needs root" is something the user has to act on.
 */
/*
 * A REFUSED IMPORT HAS ITS OWN DIALOG - design/49 T3. It went through
 * commit_failed() and read "Could not save / Could not write FILE"
 * about a file that was only being read; the user saw it on the first
 * draft test. Same shape, the right words.
 */
static void load_failed(const char *path, const char *why)
{
    GtkWidget *dlg;
    GtkWidget *lab;
    GtkWidget *btn;
    char       text[VLHE_PATH_MAX + 240];

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_COULD_NOT_LOAD);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    sprintf(text, FMT_SHELL_COULD_NOT_LOAD,
            VLHE_PATH_MAX - 1, (path != NULL && *path) ? path : "(unknown)",
            200, (why != NULL && *why) ? why : STR_SHELL_MSG_UNKNOWN_ERROR);
    lab = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);
    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_OK), STR_SHELL_BTN_OK_TIP);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);
    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();
}

static void commit_failed(const char *path, const char *why)
{
    GtkWidget *dlg;
    GtkWidget *lab;
    GtkWidget *btn;
    char       text[VLHE_PATH_MAX + 240];

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_COULD_NOT_SAVE);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    /* THE PATH IS IN THE MESSAGE. "Permission denied" alone leaves a
     * user guessing which of three files it was about. */
    sprintf(text, FMT_SHELL_COULD_NOT_WRITE,
            VLHE_PATH_MAX - 1, (path != NULL && *path) ? path : "(unknown)",
            200, (why != NULL && *why) ? why : STR_SHELL_MSG_UNKNOWN_ERROR);

    lab = gtk_label_new(text);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab, TRUE, TRUE, 8);
    gtk_widget_show(lab);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_OK), STR_SHELL_BTN_OK_TIP);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);

    /* The same size as every other dialog's buttons. */
    vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

    gtk_widget_show(dlg);
}

/*
 * THE COMMIT PATH, shared by Apply and OK.
 *
 * `all' is the difference between them, and it is KDE's distinction
 * read out of kcontrol rather than invented here:
 *
 *   Apply  ONE module - KControlApplication::apply() is a virtual
 *          inside a single swallowed process and cannot reach
 *          another (kcontrol.cpp:247).
 *   OK     EVERY module the user has touched. The module exits
 *          "Accepted", and the SHELL walks the tree writing
 *          "KControlApplication::SaveAndExit" into each other
 *          running child's stdin (configlist.cpp:321, a Corel
 *          modification - BI 242363, Sam Watts, 1998-12-08). The
 *          receiving end is kcontrol.cpp:380: apply(); quit();
 *
 * WHY THAT MATTERS HERE: our pages are built once and hidden, so a
 * user can edit Sound, switch to CD Settings and press OK. Without
 * the `all' pass the Sound edit is silently dropped - the widgets
 * still hold it and nothing ever reads them. That was a real defect
 * in this function until 2026-09-18.
 *
 * Returns 0 on success (INCLUDING a clean no-op), -1 if anything
 * refused.
 */
static int do_commit_mask(unsigned long mask);

/*
 * `all' is every page, otherwise the current one - the two shapes OK
 * and Apply have always used. Both are now a MASK underneath, so that
 * save-before-render can commit exactly the pages a render reads and
 * save-before-load exactly the pages that are dirty (2026-09-24).
 */
static int do_commit(int all)
{
    unsigned long mask = 0;

    if (all)
        mask = ~0UL;
    else if (g_current_mod >= 0 && g_current_mod < MOD_COUNT)
        mask = 1UL << g_current_mod;
    return do_commit_mask(mask);
}

/* THE PAGES HAND THEIR WIDGETS TO THE BACKEND - step 1 of a commit,
 * and all of Save Draft. Returns -1, with the refusing page shown, if
 * a page's values do not validate. */
static int collect_mask(unsigned long mask)
{
    int i;

    for (i = 0; i < MOD_COUNT; i++) {
        if (!(mask & (1UL << i)) || g_mod[i].collect == NULL)
            continue;
        if (g_mod[i].collect() != 0) {
            /* SHOW THE PAGE THAT REFUSED. Reporting a validation
             * failure about a page the user cannot see would be
             * useless - the module names the field, and this puts
             * the field in front of them. (A no-op when it is the
             * page already showing.)
             *
             * AND SAY SO, ALWAYS - 2026-10-01. "The module names the
             * field" was true of one module; the rest returned in
             * silence, so Save Draft "did nothing" with minor 15 on
             * the Midi page. The page's own reason, if it gave one,
             * is already in the status line; this names the page
             * after it, so the user has both. */
            {
                char msg[160];

                select_module(i);
                sprintf(msg, FMT_SHELL_SETTINGS_WERE_NOT_ACCEPTED, g_mod[i].label);
                status_set(msg);
            }
            return -1;
        }
    }
    return 0;
}

static int do_commit_mask(unsigned long mask)
{
    int i;

    /* 1. The pages hand their widgets to the backend. A module with
     *    nothing to commit has no collect and is skipped. */
    if (collect_mask(mask) != 0)
        return -1;

    /*
     * 2. AND THAT IS ALL - OK AND APPLY WRITE NOTHING. The user's
     *    decision, 2026-10-01 (design/51): only the File menu writes
     *    files. "We are one GUI", not KDE's one-process-per-module,
     *    so the convention that OK persists does not carry; what
     *    carries is that a plan, a render and the live font all read
     *    MEMORY, which is what this has just filled. Saving is a
     *    separate act, kept in view by the sidebar's stars and the
     *    quit prompt, and the status line here says where it lives.
     *
     *    This used to run vlhe_commit() with create = 0 and then
     *    explain, in four different sentences, which of the two files
     *    had or had not been written and why. All four are gone.
     */
    if (!vlhe_dirty()) {
        status_set(STR_SHELL_MSG_NO_CHANGES);
        sidebar_mark_dirty();
        return 0;
    }
    status_set(STR_SHELL_MSG_APPLIED_FILE_SAVE_CONFIGURATION);

    /*
     * AND EVERY OTHER PAGE CATCHES UP - added 2026-09-23, after the
     * user applied a soundfont on the Midi page and the Render page
     * went on saying "none set - choose one in Midi Settings and
     * press Apply". They had.
     *
     * A SETTING DISPLAYED ON ONE PAGE AND OWNED BY ANOTHER WAS
     * INVISIBLE UNTIL RESTART. Cancel reloaded everything and Reset
     * reloaded everything; Apply, the one a user presses constantly,
     * reloaded nothing.
     *
     * ONLY THE PAGES IN THE MASK, AND THAT IS NOT A DETAIL. Apply
     * collects THIS PAGE ONLY, so reloading the others would put
     * back what is on DISK over edits a user had made and not yet
     * saved - trading a stale label for lost work, which is the
     * worse bug. OK collects everything, so there is nothing pending
     * to lose. A page is reloaded here only if it was just collected.
     *
     * WHICH LEAVES APPLY'S CASE HALF-FIXED ON PURPOSE. The page's
     * own reload() runs below where it is safe; a cross-page label
     * like Render's soundfont still waits for OK or a page change.
     * Fixing that properly means a change NOTIFICATION rather than a
     * blanket reload - a page saying "the fonts moved" and only the
     * interested pages answering - and that is a bigger change than
     * the bug justifies tonight.
     *
     * AFTER THE STATUS LINE, so a page's reload cannot overwrite the
     * message this function chose.
     */
    for (i = 0; i < MOD_COUNT; i++)
        if ((mask & (1UL << i)) && g_mod[i].reload != NULL)
            g_mod[i].reload();

    sidebar_mark_dirty();
    sidebar_update_noconfig();
    return 0;
}

/* APPLY - save THIS page, stay on it. The one for "change something,
 * hear it, change it again". */
static void on_apply(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    (void)do_commit(0);
}

/*
 * OK - save EVERY page that was touched, then go to the start page.
 *
 * IT DOES NOT CLOSE THE WINDOW, and that is a deliberate break from
 * the dialog convention. OK-then-close assumes the window exists to
 * answer one question; this one hosts a LIVE MIXER and a CD+G viewer,
 * so closing it after a settings change takes away something the user
 * was using. The user's call, 2026-09-18.
 *
 * It is not KDE's OK either. There, OK dismisses a swallowed MODULE
 * and pops a navigation stack; we are one binary with four panels and
 * have no stack to unwind, so closing would just end the program -
 * which File/Exit and the window manager already do.
 */
static void on_ok(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;

    /* ON FAILURE, STAY PUT. do_commit() has already shown the page
     * that refused; navigating away from it now would hide the thing
     * the user has to fix. */
    if (do_commit(1) != 0)
        return;

    /*
     * A PAGE MAY ASK TO KEEP THE USER - the Render page does, and the
     * user asked for it: "This okay should return the Render tab
     * instead of the settings". Someone who has just saved a
     * comparison render's settings wants to RUN one, and OK should
     * put them in front of the thing the settings were for rather
     * than navigating away.
     *
     * Special-cased here rather than given a hook on struct module,
     * because one page wants it and a field nothing else sets would
     * be a hook pretending to be general.
     */
    if (g_current_mod == MOD_RENDER) {
        render_show_first_tab();
        return;
    }

    select_module(default_page());
}

/*
 * CANCEL - THIS PAGE, BACK TO WHAT WAS LAST APPLIED. STAY HERE.
 *
 * NOT EVERYTHING SINCE THE LAST SAVE ANY MORE - the user, 2026-10-02:
 * "if I pick a sound font apply it. then go to renderer mess with
 * that. Dont like a setting and I cancel. It removes the sound font.
 * even though I was changing renderer settings." Under Save-only
 * (design/51) Apply fills memory and the files stay put, so a Cancel
 * that re-read the FILES reached every Applied change in the session,
 * from every page. Before design/51 it reached only un-applied edits,
 * by accident: Apply wrote the file.
 *
 * So Cancel is now kcontrol's Cancel: the page's reload() redraws its
 * widgets from the backend's MEMORY, which is the applied state, and
 * nothing else moves. Undoing APPLIED-but-unsaved changes, across
 * every page, is File > Revert to Saved (on_revert_saved() below) -
 * the old Cancel, with a question in front of it because its reach
 * is wide. Discard & Quit at exit is unchanged.
 *
 * STAYING ON THE PAGE is the point: the user cancelled THIS page's
 * edits, and being moved somewhere else as well is a second thing
 * they did not ask for. It also makes Cancel the only button that
 * does not navigate, which matches it being the only one that does
 * not write.
 */
static void on_cancel(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;

    if (g_current_mod < 0 || g_current_mod >= MOD_COUNT)
        return;

    /* THE PAGE'S OWN dirty() is the test, not the backend's flag:
     * the backend's bit says applied-but-unsaved, which this does
     * not touch; the page's says typed-but-not-applied, which is
     * exactly what goes. */
    if (g_mod[g_current_mod].dirty == NULL
        || !g_mod[g_current_mod].dirty()) {
        status_set(STR_SHELL_MSG_NOTHING_UNDO_PAGE_FILE);
        return;
    }
    if (g_mod[g_current_mod].reload != NULL)
        g_mod[g_current_mod].reload();

    sidebar_mark_dirty();
    status_set(STR_SHELL_MSG_CHANGES_PAGE_UNDONE_BACK);
}

/*
 * FILE / REVERT TO SAVED - everything since the last Save, applied or
 * not, on every page. What Cancel was until 2026-10-02 (above), now
 * named for what it does and asked about first. A draft in use comes
 * back with the files (vlhe_discard()).
 */
static void on_revert_saved(GtkWidget *w, gpointer data)
{
    int i;

    (void)w;
    (void)data;

    /* THE BACKEND'S FLAG IS NOT THE WHOLE ANSWER. A page can hold
     * edits that have not been collected yet, so this asks the
     * MODULES too - otherwise it would refuse to undo exactly the
     * edits the asterisks are advertising. */
    {
        int any = vlhe_dirty();

        for (i = 0; i < MOD_COUNT && !any; i++)
            if (g_mod[i].dirty != NULL && g_mod[i].dirty())
                any = 1;
        if (!any) {
            status_set(STR_SHELL_MSG_NOTHING_REVERT_EVERYTHING_AS);
            return;
        }
    }

    /*
     * THE SAME DIALOG AS "Unsaved changes" - the user, 2026-10-02, on
     * the first version (a paragraph through ask_yes_no(), with
     * "Revert" / "Keep changes" that did not match): "The warning is
     * too verbose. The buttons dont match. It should act like the
     * Unsaved changes dialog box." So: the page list the quit prompt
     * draws, one question, and two equalised buttons with Cancel as
     * the default - their text and their labels.
     */
    {
        GtkWidget *dlg, *lab, *btn;
        char text[512];

        ask_answer = 0;

        dlg = gtk_dialog_new();
        gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_REVERT_SAVED);
        gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
        gtk_container_border_width(GTK_CONTAINER(dlg), 8);

        strcpy(text, FMT_SHELL_FOLLOWING_PAGES_HAVE_UNSAVED);
        for (i = 0; i < MOD_COUNT; i++) {
            if (g_mod[i].dirty == NULL || !g_mod[i].dirty())
                continue;
            if (strlen(text) + strlen(g_mod[i].label) + 6 >= sizeof text)
                break;
            strcat(text, "\n    ");
            strcat(text, g_mod[i].label);
        }
        /* APPLIED-BUT-UNSAVED SETTINGS BELONG TO NO PAGE - their
         * stars are gone, the backend's flag is what is left. Named
         * so the list is never empty when the question is asked. */
        if (vlhe_dirty())
            strcat(text, FMT_SHELL_SETTINGS_ALREADY_APPLIED);
        strcat(text, FMT_SHELL_WOULD_YOU_LIKE_DISCARD);
        lab = gtk_label_new(text);
        gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), lab,
                           TRUE, TRUE, 8);
        gtk_widget_show(lab);

        btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_REVERT_SAVED), STR_SHELL_BTN_REVERT_SAVED_TIP);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                           FALSE, FALSE, 0);
        gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                           GTK_SIGNAL_FUNC(ask_set_yes), NULL);
        gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                                  GTK_SIGNAL_FUNC(gtk_widget_destroy),
                                  GTK_OBJECT(dlg));
        gtk_widget_show(btn);

        /* CANCEL IS THE DEFAULT - Return must not throw work away. */
        btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
        GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                           FALSE, FALSE, 0);
        gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                                  GTK_SIGNAL_FUNC(gtk_widget_destroy),
                                  GTK_OBJECT(dlg));
        gtk_widget_show(btn);
        gtk_widget_grab_default(btn);

        vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

        gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                           GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
        gtk_widget_show(dlg);
        gtk_main();

        if (!ask_answer)
            return;
    }

    /* 1. The backend re-reads its files. */
    vlhe_discard();

    /* 2. EVERY page puts its widgets back, not just this one - the
     *    changes being reverted may be spread across pages the user
     *    visited earlier, exactly as OK's commit is. */
    for (i = 0; i < MOD_COUNT; i++) {
        if (g_mod[i].reload != NULL)
            g_mod[i].reload();
    }

    sidebar_mark_dirty();
    status_set(STR_SHELL_MSG_REVERTED_SAVED_CONFIGURATION);
}

/*
 * FILE / SAVE CONFIGURATION - the only thing that CREATES a file.
 *
 * Apply takes the settings; this is what makes them outlive the
 * process. Separating them is the user's model (design/33 section
 * 1e): the GUI is fully usable with no config at all, and writing
 * one is a deliberate act rather than a side effect of pressing
 * Apply.
 */
/* ------------------------------------------------------------------ *
 * SAVE DRAFT AND LOAD CONFIGURATION - design/49 T0.
 *
 * A normal user cannot save machine settings (the system config is
 * root's), so Save Draft puts the ones on screen in a file of their
 * own. Root opens it with Load Configuration: every value is checked
 * against what the pages offer and arrives as an UNSAVED EDIT, shown
 * on the pages, so root looks before pressing Save. The draft itself
 * is never trusted, and Load never reads it.
 * ------------------------------------------------------------------ */
static GtkWidget *g_conf_sel;       /* the one dialog, or NULL */
static int        g_conf_sel_load;  /* 1 = Load, 0 = Save Draft */

/* Where a draft goes by default: beside the user's own file, or in
 * their home - somewhere that is theirs. */
static void draft_default(char *out, int max)
{
    const char *u = vlhe_user_conf_path();
    const char *h = getenv("HOME");
    char *slash;

    out[0] = '\0';
    if (u != NULL && (int) strlen(u) + 20 < max) {
        strcpy(out, u);
        slash = strrchr(out, '/');
        if (slash != NULL)
            slash[1] = '\0';
        else
            out[0] = '\0';
    }
    if (out[0] == '\0' && h != NULL && (int) strlen(h) + 20 < max)
        sprintf(out, "%s/", h);
    if ((int) strlen(out) + 17 < max)
        strcat(out, "vlhe-draft.conf");
}

static void on_conf_sel_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    g_conf_sel = NULL;
}

static void on_conf_sel_cancel(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (g_conf_sel != NULL)
        gtk_widget_destroy(g_conf_sel);
}

static void on_conf_sel_ok(GtkWidget *w, gpointer data)
{
    char path[VLHE_PATH_MAX];
    char text[VLHE_PATH_MAX + 480];
    int  load = g_conf_sel_load;

    (void)w;
    (void)data;
    if (g_conf_sel == NULL)
        return;
    strncpy(path, gtk_file_selection_get_filename(
                      GTK_FILE_SELECTION(g_conf_sel)), sizeof path - 1);
    path[sizeof path - 1] = '\0';
    gtk_widget_destroy(g_conf_sel);
    if (path[0] == '\0' || path[strlen(path) - 1] == '/') {
        status_set(STR_SHELL_MSG_NO_FILE_CHOSEN);
        return;
    }

    if (!load) {
        struct vlhe_commit_err err;

        /* WHAT IS ON SCREEN, as Save takes it - the pages collect
         * first, and a page that does not validate is shown. */
        if (collect_mask(~0UL) != 0)
            return;
        memset(&err, 0, sizeof err);
        if (vlhe_conf_save_draft(path, &err) != 0) {
            commit_failed(err.path, err.why);
            return;
        }
        sidebar_mark_dirty();
        /* AN EXPORT, NOT A SAVE - design/51: it clears no dirty bit,
         * for root or anyone, so the stars still say what is unsaved. */
        sprintf(text, FMT_SHELL_DRAFT_WRITTEN_ROOT_CAN, path);
        status_set(text);
        return;
    }

    review_config(path);
}

/* The page a config section belongs to, and the label a key has
 * there - so the review reads as the pages do, grouped, rather than
 * as a flat list of section/key pairs. Anything unknown is shown as
 * it is. */
static const char *review_page(const char *sec)
{
    /* THE LEFT SIDE IS A CONFIG SECTION NAME - a key in the file,
     * compared as bytes and never translated; the right side is the
     * page's label from the strings header. They happen to read the
     * same today, which is how the strings pass first turned the
     * keys into labels too (2026-10-02). */
    if (strcmp(sec, "CD Settings") == 0)    return g_mod[MOD_CD].label;
    if (strcmp(sec, "Sound Settings") == 0) return g_mod[MOD_SOUND].label;
    if (strcmp(sec, "Midi Settings") == 0)  return STR_SHELL_TEXT_MIDI_SETTINGS_PAGE;
    return sec;
}

static const char *review_label(const char *sec, const char *key)
{
    static const struct { const char *key, *label; } t[] = {
        { "Major",           STR_SHELL_REVIEW_MAJOR },
        { STR_SHELL_REVIEW_DRIVES,          STR_SHELL_REVIEW_DRIVES },
        { "Packet",          STR_SHELL_REVIEW_PACKET },
        { "LinkCdrom",       STR_SHELL_REVIEW_LINKCDROM },
        { "CardDevice",      STR_SHELL_REVIEW_CARDDEVICE },
        { "ProgramsUse",     STR_SHELL_REVIEW_PROGRAMSUSE },
        { "ReleaseOnIdle",   STR_SHELL_REVIEW_RELEASEONIDLE },
        { "MidiChannel",     STR_SHELL_REVIEW_MIDICHANNEL },
        { "SaveMixerLevels", STR_SHELL_REVIEW_SAVEMIXERLEVELS },
        { STR_SHELL_REVIEW_VOICES,          STR_SHELL_REVIEW_VOICES },
        { "Gain",            STR_SHELL_REVIEW_GAIN },
        { "Reverb",          STR_SHELL_REVIEW_EFFECTS },
        { "Law",             STR_SHELL_REVIEW_LAW },
        { "Filter",          STR_SHELL_REVIEW_FILTER },
        { STR_SHELL_REVIEW_RATE,            STR_SHELL_REVIEW_RATE },
        { "Minor",           STR_SHELL_REVIEW_MINOR },
        { "LoadAtBoot",      STR_SHELL_REVIEW_LOADATBOOT },
        { NULL, NULL }
    };
    int i;

    (void)sec;
    for (i = 0; t[i].key != NULL; i++)
        if (strcmp(key, t[i].key) == 0)
            return t[i].label;
    return key;
}

/*
 * REVIEW A FILE AND, ON ACCEPT, TAKE ITS SETTINGS AS UNSAVED EDITS -
 * design/49 T0, R1 and R3. Called from File > Load Configuration and
 * from the takeover offer for an untrusted live file. Every change is
 * listed, grouped by page with the page's own labels (the user's
 * concern: a first-time setup is 10-15 lines and must read as the
 * pages do, not as a dump); refused and skipped keys follow; the
 * whole thing scrolls. Returns 1 if accepted.
 */
static int review_config(const char *path)
{
    static struct vlhe_draft_review r;
    static char body[VLHE_DRAFT_MAX * 3 * 200 + 600];
    char head[VLHE_PATH_MAX + 120];
    char text[VLHE_PATH_MAX + 200];
    char why[160];
    const char *last_page = "";
    int  k, i, any_page = 0;

    /* NO UNSAVED MACHINE SETTINGS - on a page or in the backend. The
     * review has to list the file's changes alone (design/49 T0), and
     * only the system half can mix in (T4): the three pages that
     * carry machine settings are asked, not Volume or Render. MIDI
     * holds both halves, so a pending font change there blocks too -
     * the one imprecision, and it errs towards asking. */
    for (k = 0; k < MOD_COUNT; k++) {
        if (g_mod[k].dirty == NULL || !g_mod[k].dirty())
            continue;
        /* BY MODULE, NOT BY LABEL - this compared the sidebar's
         * text, which the strings header now owns (2026-10-02). */
        if (k == MOD_SOUND || k == MOD_MIDI || k == MOD_CD)
            any_page = 1;
    }
    if (any_page) {
        load_failed(path, STR_SHELL_MSG_THERE_ARE_UNSAVED_MACHINE);
        return 0;
    }
    if (vlhe_conf_draft_review(path, &r, why, (int) sizeof why) != 0) {
        load_failed(path, why);
        return 0;
    }

    body[0] = '\0';
    if (r.nchange == 0) {
        strcat(body, FMT_SHELL_NOTHING_WOULD_CHANGE_MACHINE);
    } else {
        for (k = 0; k < r.nchange && k < VLHE_DRAFT_MAX; k++) {
            char line[200];
            char *f[4];
            char *q;
            int   n = 0;

            strncpy(line, r.change[k], sizeof line - 1);
            line[sizeof line - 1] = '\0';
            f[0] = line;
            for (q = line; *q != '\0' && n < 3; q++)
                if (*q == '\t') {
                    *q = '\0';
                    f[++n] = q + 1;
                }
            if (n < 3)
                continue;
            if (strcmp(review_page(f[0]), last_page) != 0) {
                last_page = review_page(f[0]);
                sprintf(body + strlen(body), "%s%s\n",
                        body[0] ? "\n" : "", last_page);
            }
            sprintf(body + strlen(body), "    %-30.30s  %s  ->  %s\n",
                    review_label(f[0], f[1]), f[2], f[3]);
        }
        if (r.nchange > VLHE_DRAFT_MAX)
            sprintf(body + strlen(body), FMT_SHELL_MORE_NOT_SHOWN,
                    r.nchange - VLHE_DRAFT_MAX);
    }
    if (r.nrefused > 0) {
        strcat(body, FMT_SHELL_REFUSED_NOT_VALUE_THESE);
        for (k = 0; k < r.nrefused && k < VLHE_DRAFT_MAX; k++)
            sprintf(body + strlen(body), "    %s\n", r.refused[k]);
    }
    if (r.nskipped > 0) {
        strcat(body, FMT_SHELL_SKIPPED_NEVER_TAKEN_FROM);
        for (k = 0; k < r.nskipped && k < VLHE_DRAFT_MAX; k++)
            sprintf(body + strlen(body), "    %s\n", r.skipped[k]);
    }
    if (r.nuser > 0)
        sprintf(body + strlen(body), FMT_SHELL_USER_S_OWN_SETTINGS, r.nuser);

    sprintf(head, "%.*s\n\n%s", VLHE_PATH_MAX - 1, path,
            r.nchange == 0 ? "" : STR_SHELL_TEXT_THESE_SETTINGS_WOULD_CHANGE);
    if (r.nchange == 0) {
        vlhe_conf_draft_cancel();
        (void)ask_yes_no_scrolled(STR_SHELL_TITLE_LOAD_CONFIGURATION, head, body, NULL, STR_SHELL_BTN_OK, NULL);
        status_set(STR_SHELL_MSG_NOTHING_WAS_LOADED);
        return 0;
    }
    if (!ask_yes_no_scrolled(STR_SHELL_TITLE_LOAD_CONFIGURATION, head, body, STR_SHELL_BTN_ACCEPT_MAKES_THESE_UNSAVED, STR_SHELL_BTN_ACCEPT, STR_SHELL_BTN_CANCEL)) {
        vlhe_conf_draft_cancel();
        status_set(STR_SHELL_MSG_NOTHING_WAS_LOADED);
        return 0;
    }
    (void)vlhe_conf_draft_accept();
    /* THE PAGES SHOW WHAT ARRIVED, reloaded from the backend. */
    for (i = 0; i < MOD_COUNT; i++)
        if (g_mod[i].reload != NULL)
            g_mod[i].reload();
    sidebar_mark_dirty();
    sprintf(text, FMT_SHELL_CHANGE_S_LOADED_AS, r.nchange,
            vlhe_can_write_system_conf()
                ? STR_SHELL_TEXT_SAVE_TO_MAKE_THEM
                : STR_SHELL_TEXT_SAVING_THEM_NEEDS_ROOT);
    status_set(text);
    return 1;
}

/*
 * A LIVE CONFIG ROOT DID NOT WRITE - design/49 R1. Its values are not
 * loaded (the backend starts from the defaults and keeps the verdict),
 * so someone who can act is offered the review once per session: the
 * same review Load Configuration runs, on that file. Accept makes the
 * differences unsaved edits and Save writes root's own copy over it,
 * which the backend then judges again. NO "take over without review"
 * - accepting a file by its ownership alone is what the refusal
 * exists to prevent. A normal user gets the sidebar note instead.
 */
static int g_takeover_asked;

static void offer_takeover(void)
{
    char why[VLHE_PATH_MAX + 64];
    char text[VLHE_PATH_MAX + 400];

    if (g_takeover_asked || !vlhe_priv_can_act())
        return;
    if (vlhe_conf_system_trusted(why, (int) sizeof why))
        return;
    g_takeover_asked = 1;
    sprintf(text, FMT_SHELL_SETTINGS_ARE_NOT_USE, VLHE_PATH_MAX + 60, why);
    if (ask_yes_no(STR_SHELL_TITLE_CONFIGURATION_NOT_USE, text, STR_SHELL_BTN_REVIEW, STR_SHELL_BTN_NOT_NOW))
        (void)review_config(vlhe_system_conf_path());
}

static void open_conf_sel(int load)
{
    char deflt[VLHE_PATH_MAX];
    static const struct vlhe_filter conf_types[] = {
        { STR_SHELL_FILTER_CONF, "*.conf" },
        { STR_SHELL_FILTER_ALL,                NULL     },
        { NULL, NULL }
    };

    if (g_conf_sel != NULL) {
        gdk_window_raise(g_conf_sel->window);
        return;                 /* one at a time */
    }
    g_conf_sel_load = load;
    g_conf_sel = gtk_file_selection_new(load ? STR_SHELL_TITLE_LOAD_CONFIGURATION
                                             : STR_SHELL_TITLE_SAVE_DRAFT);
    vlhe_filesel_fit(g_conf_sel);  /* a long path must not widen it */
    /* A chooser, not a file manager - no Create / Rename / Delete. */
    gtk_file_selection_hide_fileop_buttons(GTK_FILE_SELECTION(g_conf_sel));
    if (load)
        draft_default(deflt, (int) sizeof deflt);
    else {
        /* THE SAME PLACE A NON-ROOT SAVE PUTS THE MACHINE HALF - so
         * the default name offered is the file the next start would
         * read back, and a user who takes it has one draft, not two. */
        strncpy(deflt, vlhe_draft_default_path(), sizeof deflt - 1);
        deflt[sizeof deflt - 1] = '\0';
        if (deflt[0] == '\0')
            draft_default(deflt, (int) sizeof deflt);
    }
    gtk_file_selection_set_filename(GTK_FILE_SELECTION(g_conf_sel), deflt);
    vlhe_filter_attach(g_conf_sel, conf_types, 0);

    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(g_conf_sel)->ok_button),
                       "clicked", GTK_SIGNAL_FUNC(on_conf_sel_ok), NULL);
    gtk_signal_connect(
        GTK_OBJECT(GTK_FILE_SELECTION(g_conf_sel)->cancel_button),
        "clicked", GTK_SIGNAL_FUNC(on_conf_sel_cancel), NULL);
    gtk_signal_connect(GTK_OBJECT(g_conf_sel), "destroy",
                       GTK_SIGNAL_FUNC(on_conf_sel_destroy), NULL);
    gtk_widget_show(g_conf_sel);
}

static void on_save_draft(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    open_conf_sel(0);
}

static void on_load_config(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    open_conf_sel(1);
}

/* THE SAVE ITSELF, shared by File > Save Configuration and the quit
 * dialog's Save & Quit. Returns 0 when everything was written, -1 when
 * something was not - each already reported in the status line or a
 * dialog, so the caller decides only whether to go on. A save that
 * kept the machine half for want of root returns 0 and leaves
 * vlhe_dirty() set; the caller reads that if it matters. */
static int save_config(void)
{
    struct vlhe_commit_err err;
    int existed;

    existed = vlhe_conf_exists();

    /* EVERY PAGE FIRST, as OK does - saving captures what is on
     * screen, not only what has been applied. A page that refuses
     * has been shown (collect_mask()); nothing is written. */
    if (collect_mask(~0UL) != 0)
        return -1;

    /*
     * THE ONE PLACE A SETTING REACHES A FILE - design/51, 2026-10-01.
     * create = 1: a missing file is made. The backend decides where
     * the machine half goes - the system file for root, the draft at
     * its default path for anyone else - and says so in err.drafted.
     */
    memset(&err, 0, sizeof err);
    if (vlhe_commit(&err, 1) != 0) {
        /*
         * THE USER FILE COULD NOT BE WRITTEN WHERE IT BELONGS - OFFER
         * THE NEXT PLACE. The user's chain, 2026-09-24 (design/36 row
         * 60): the tree, then ASK about $HOME, then NOTIFY and use
         * /tmp. Asked rather than assumed, because a silent fallback
         * would land settings somewhere unexpected under a success
         * message - which is the row-59 shape this is meant to end.
         *
         * Each step re-commits; a failure at the last tier falls
         * through to the ordinary "Could not save" dialog with the
         * last path tried, and the settings stay in memory, dirty.
         */
        while (err.user_tier >= 0 && err.alt[0] != '\0') {
            const char *home = vlhe_conf_user_candidate(VLHE_USER_TIER_HOME);
            int  to_home = (home != NULL && strcmp(err.alt, home) == 0);
            int  next    = to_home ? VLHE_USER_TIER_HOME : VLHE_USER_TIER_TMP;
            char text[2 * VLHE_PATH_MAX + 320];

            if (to_home) {
                sprintf(text,
                        FMT_SHELL_COULD_NOT_WRITE_YOUR,
                        VLHE_PATH_MAX - 1, err.path, 160, err.why,
                        VLHE_PATH_MAX - 1, err.alt);
                if (!ask_yes_no(STR_SHELL_TITLE_SETTINGS_COULD_NOT_SAVED, text, STR_SHELL_BTN_YES_USE_MY_HOME, STR_SHELL_BTN_NO))
                    break;
            } else {
                sprintf(text,
                        FMT_SHELL_COULD_NOT_WRITE_YOUR_2,
                        VLHE_PATH_MAX - 1, err.path, 160, err.why,
                        VLHE_PATH_MAX - 1, err.alt);
                (void)ask_yes_no(STR_SHELL_TITLE_SETTINGS_KEPT_TMP, text, STR_SHELL_BTN_OK, NULL);
            }

            if (vlhe_conf_user_redirect(next) != 0)
                break;
            memset(&err, 0, sizeof err);
            if (vlhe_commit(&err, 1) == 0)
                goto written;
        }
        commit_failed(err.path, err.why);
        return -1;
    }

written:
    /*
     * A FILE THAT IS STILL ABSENT IS MADE WITH THE DEFAULTS. The
     * commit writes only what is dirty, so a Save with nothing
     * changed on one half leaves that half's file unmade - and
     * "either file exists" was the wrong test for it (design/47 B1,
     * and the 2026-10-01 case where the user file came first and the
     * system file was never created). Each file is asked about on
     * its own; the system one only where this run could write it,
     * since for anyone else that file is root's to make.
     */
    {
        struct vlhe_commit_err cerr;
        int want_user = !vlhe_conf_file_exists(VLHE_TPL_USER);
        int want_sys  = !vlhe_conf_file_exists(VLHE_TPL_SYSTEM)
                        && vlhe_can_write_system_conf();

        memset(&cerr, 0, sizeof cerr);
        if ((want_user || want_sys) && vlhe_conf_create(&cerr) < 0) {
            commit_failed(cerr.path[0] ? cerr.path : STR_SHELL_MSG_CONFIGURATION_PLACEHOLDER,
                          cerr.why[0] ? cerr.why : STR_SHELL_MSG_NOTHING_COULD_BE_WRITTEN);
            return -1;
        }
        /* vlhe_conf_create() reports the system file it could not
         * make for a non-root user; that is the expected state here,
         * not a failure, so only another kind of error is shown. */
        if (cerr.path[0] != '\0' && !cerr.needs_root) {
            commit_failed(cerr.path, cerr.why);
            return -1;
        }
    }

    sidebar_update_noconfig();
    if (err.drafted[0] != '\0') {
        char msg[VLHE_PATH_MAX + 120];

        sprintf(msg, FMT_SHELL_SAVED_YOUR_MACHINE_SETTINGS,
                VLHE_PATH_MAX - 1, err.drafted);
        status_set(msg);
    } else {
        status_set(existed ? STR_SHELL_MSG_CONFIGURATION_SAVED
                           : STR_SHELL_MSG_CONFIGURATION_FILE_CREATED);
    }
    return 0;
}

static void on_save_config(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    (void)save_config();
}

/* ------------------------------------------------------------------ *
 * File / Preferences - the program's own settings                     *
 * ------------------------------------------------------------------ */

/* THE ONE DIALOG THAT IS ABOUT THE PROGRAM rather than the emulated
 * hardware, which is why it is in the File menu and not a fifth
 * sidebar entry: the sidebar is the machine, and a font is not part
 * of it.
 *
 * IT APPLIES IMMEDIATELY SINCE 2026-09-29, AND THIS COMMENT SAID THE
 * OPPOSITE. It read: "APPLIES AT NEXT START, and that is a deliberate
 * choice rather than a shortcut ... gtk_widget_reset_rc_styles() over
 * the whole tree, which GTK 1.2 does imperfectly - some widgets keep
 * their old metrics until rebuilt".
 *
 * THAT IS NOT WHAT THE TOOLKIT DOES, and the source settles it.
 * `gtk_widget_set_style_internal()' (gtkwidget.c), which every widget
 * reached by reset_rc_styles goes through, re-runs
 * `gtk_widget_size_request()' and queues a RESIZE when the requisition
 * changed - a clear only when it did not. Metrics are renegotiated,
 * not kept. The whole chain is GTK's own: gtkwindow.c:1393-1408 does
 * exactly this on a `_GTK_READ_RCFILES' client message, which is how
 * the KDE control centre restyles every running GTK program.
 *
 * AND RE-PARSING THE SAME STYLE NAME UPDATES IT IN PLACE rather than
 * accumulating definitions - gtkrc.c:1364-1370 sets `insert' only when
 * `gtk_rc_style_find()' returns NULL. That was the one thing worth
 * checking before relying on this.
 *
 * WHY IT MATTERS BEYOND CONVENIENCE: the restart requirement was the
 * stated reason `vlhe_set_prefs()' wrote the user config immediately -
 * "there is no later commit point to carry it". With the font live
 * there is one, so Preferences becomes a form behind Apply like every
 * other page, and design/36 row 93 closes with it.
 */

/*
 * APPLY THE INTERFACE FONT - at startup, or to a running GUI.
 *
 * `live' 0 is the startup call: parse the rc string and stop, because
 * no widget exists yet. `live' 1 adds the restyle pass over every
 * toplevel, which is what makes an already-built window adopt it.
 *
 * A SIZE WITH NO FAMILY IS "the same font, bigger" AND IS THE COMMON
 * CASE - 2026-09-29, and the dialog used to throw it away.
 *
 * `on_prefs_ok()' zeroed the size whenever the family was
 * "(desktop default)", on the belief that GTK 1.2 needs a full XLFD
 * so the two cannot be separated. **MEASURED, THAT IS WRONG**: a
 * wildcard family resolves - `-*-*-medium-r-normal--10-*-*-*-*-*-
 * iso8859-1' matched 43 fonts on this server - and GTK accepts it.
 * So the family stays optional and only the size need be set, which
 * is what a user asking for bigger text actually wants. The user:
 * "someone will change just the font size not knowing you need to
 * change the family as well. They wont change the family just to
 * change the font size."
 *
 * A WILDCARD FAMILY LETS X CHOOSE, so the result may not be the
 * desktop's own typeface at a new size - it is whatever matches
 * first. That is a real imprecision and it is preferred to
 * discarding the setting: "bigger, possibly a different face" is
 * the request honoured imperfectly, where the old behaviour was the
 * request ignored silently.
 *
 * VLHE_FONT_GTK_DEFAULT IS HOW "Use Default" WORKS AT ALL. GTK
 * offers no way to REMOVE a parsed style - `gtk_rc_clear_styles()'
 * is static in gtkrc.c - so a reset cannot un-parse `vlhe-font'.
 * What it can do is OVERWRITE it, and gtkstyle.c:409 gives the
 * string to overwrite it with: GTK's own built-in default. Same
 * mechanism as any other change, and the widgets go back
 * immediately rather than at the next start.
 *
 * RETURNS 1 IF IT PARSED A FONT, 0 IF THERE WAS NOTHING TO DO, so
 * the caller can say which happened instead of claiming success
 * unconditionally.
 *
 * THE RESTYLE IS GTK'S OWN, copied from gtkwindow.c:1400-1408:
 * reset_rc_styles recurses each toplevel and re-attaches any widget
 * flagged RC_STYLE, and each of those re-measures (see the comment
 * above this section).
 *
 * AND THE TOPLEVEL'S OWN REQUISITION IS THE ONE GTK SKIPS.
 * set_style_internal guards its re-measure with `if (widget->parent
 * ...)', so a GtkWindow - which has no parent - is restyled but never
 * re-measured. Its children are, so the layout inside is right and
 * the window itself can be left too small. The explicit queue_resize
 * is for that case and that case only.
 */

/* GTK's own, gtkstyle.c:409 - what a widget gets with no rc override.
 * NOT necessarily what the DESKTOP set: a themed gtk.rc may say
 * otherwise, and nothing here can read it back. See on_prefs_reset(). */
#define VLHE_FONT_GTK_DEFAULT \
    "-adobe-helvetica-medium-r-normal--*-120-*-*-*-*-iso8859-1"




/*
 * CAN THIS NAME GO INSIDE AN RC STRING? design/49 N2.
 *
 * The font is pasted into `font = "..."' and handed to
 * gtk_rc_parse_string(). A `"' ends the string early and a `\' is an
 * escape, so either lets the rest of the value be read as rc syntax
 * - and the rc grammar includes `engine', `include' and
 * `module_path', the first of which loads a shared object
 * (gtkthemes.c:89). Config values keep quote characters, so a saved
 * font can carry one.
 *
 * IN THE ORDINARY BUILD IT IS A CORRECTNESS BUG - one quote in the
 * value and the style silently does not apply. In the setuid build
 * the rc text is parsed in a process with a recoverable root.
 *
 * PRINTABLE ASCII WITHOUT `"' OR `\' covers every XLFD - they are
 * letters, digits, `-', `*', `?', spaces in family names, and a few
 * others - and every name GtkFontSelection or `xlsfonts' produces.
 */
static int
font_safe(const char *s)
{
    if (s == NULL || *s == '\0')
        return 0;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char) *s;

        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
            return 0;
    }
    return 1;
}

static int
vlhe_apply_font(const struct vlhe_prefs *pf, int live)
{
    int did = 0;

    if (pf != NULL && pf->font[0] != '\0' && font_safe(pf->font)) {
        char rc[512];

        /* THE XLFD GOES STRAIGHT IN. It came from GtkFontSelection
         * and is already complete - no pattern to assemble, no
         * foundry or weight to guess at, which is the whole reason
         * the picker replaced the hand-built menus. */
        sprintf(rc,
                "style \"vlhe-font\" { font = \"%.300s\" }\n"
                "widget_class \"*\" style \"vlhe-font\"\n", pf->font);
        gtk_rc_parse_string(rc);
        did = 1;
    }

    if (live) {
        GList *tops = gtk_container_get_toplevels();

        while (tops != NULL) {
            GtkWidget *w = GTK_WIDGET(tops->data);

            gtk_widget_reset_rc_styles(w);
            gtk_widget_queue_resize(w);         /* the window itself */
            tops = tops->next;
        }
    }
    return did;
}

static GtkWidget *g_prefs_dlg;
static GtkWidget *g_prefs_font;         /* GtkFontSelection      */
static char       g_prefs_font_xlfd[VLHE_FONT_MAX];  /* pending   */
/* 1 when the picker was seeded from the RUNNING X font because no font
 * was saved - design/47 Q4. OK then stores a font only if the user
 * moved off that seed; otherwise it pins a font nobody chose, dirties
 * the user config and says "Font applied". */
static int        g_prefs_font_from_x;
static GtkWidget *g_prefs_start;
static GtkWidget *g_prefs_result;
static GtkWidget *g_prefs_note;
/* [Load] BaselineDrift's three choices, in VLHE_BDRIFT_* order - a
 * MACHINE setting in this dialog, greyed unless running as root
 * (design/54 7h Stage 4; the user's frame and wording, 2026-10-04). */
static GtkWidget *g_prefs_bdrift[3];

static void prefs_close(void)
{
    if (g_prefs_dlg != NULL) {
        gtk_widget_destroy(g_prefs_dlg);
        g_prefs_dlg = NULL;
    }
}




static void on_prefs_cancel(GtkWidget *w, gpointer data)
{
    (void)w; (void)data;
    prefs_close();
}

static void on_prefs_reset(GtkWidget *w, gpointer data)
{
    struct vlhe_prefs pf;

    (void)w; (void)data;

    /* THE FONT ONLY - design/47 Q3. This zeroed the whole struct, so
     * "Use Default" on the font also set the start page to 0 and
     * show_apply_result to an explicit 0, where unset is -1 and means
     * "the mode's default" - which flipped a portable copy's. */
    memset(&pf, 0, sizeof pf);
    (void) vlhe_prefs(&pf);
    pf.font[0] = '\0';
    if (vlhe_set_prefs(&pf) == 0) {
        /*
         * OVERWRITE THE STYLE, DO NOT TRY TO REMOVE IT - and that is
         * the whole fix, 2026-09-29. The user: "the use defaults
         * button which is supposed to reset it to the gtk.rc or
         * whatever defaults didnt change anything."
         *
         * IT DID NOTHING BECAUSE NOTHING CAN UN-PARSE A STYLE.
         * `gtk_rc_clear_styles()' is static in gtkrc.c, so once
         * `vlhe-font' is in GTK's table it stays. Clearing the
         * struct and calling apply() parsed nothing, so the widgets
         * kept the font the user was trying to get rid of - and an
         * earlier version of this comment described exactly that and
         * called it an acceptable limit. It is not: the button has
         * one job.
         *
         * SO PARSE THE DEFAULT OVER IT. Re-parsing the same style
         * name updates the entry in place (gtkrc.c:1364-1370), so
         * this is the same mechanism every other change uses.
         *
         * IT IS GTK'S DEFAULT, NOT NECESSARILY THE DESKTOP'S. A
         * themed gtk.rc may have set something else and nothing here
         * can read that back, so the message says "the interface
         * font" rather than promising the desktop's exact choice.
         */
        gtk_rc_parse_string(
            "style \"vlhe-font\" { font = \""
            VLHE_FONT_GTK_DEFAULT "\" }\n"
            "widget_class \"*\" style \"vlhe-font\"\n");
        vlhe_apply_font(&pf, 1);        /* parses nothing; restyles */
        status_set(STR_SHELL_MSG_FONT_RESET_INTERFACE_DEFAULT);
    }

    prefs_close();
}

static void on_prefs_ok(GtkWidget *w, gpointer data)
{
    struct vlhe_prefs pf;
    GtkWidget *menu;
    GtkWidget *active;
    int drift_was, drift_now;

    (void)w; (void)data;

    /* FROM THE CURRENT VALUES, NOT ZERO - design/47 Q3. Every field
     * below is read from a widget, so this matters only for a field a
     * future dialog forgets; the reset paths are where zeroing bit. */
    memset(&pf, 0, sizeof pf);
    (void) vlhe_prefs(&pf);
    pf.font[0] = '\0';

    {
        gchar *nm = gtk_font_selection_get_font_name(
                        GTK_FONT_SELECTION(g_prefs_font));

        /* NULL WHEN NOTHING IS SELECTED, which is a real state - the
         * lists start empty if no font matched. Leaving `font' empty
         * then is right: it means "no override".
         *
         * AND SO IS A SEED THE USER DID NOT TOUCH - design/47 Q4. With
         * no saved font the picker opens on the RUNNING X font so it
         * does not open on nothing; OK on that unchanged selection
         * used to store it, which turned "no override" into a pinned
         * XLFD the user never chose. Unchanged seed, empty font. */
        if (nm != NULL) {
            if (!(g_prefs_font_from_x
                  && strcmp(nm, g_prefs_font_xlfd) == 0)) {
                strncpy(pf.font, nm, sizeof pf.font - 1);
                pf.font[sizeof pf.font - 1] = '\0';
            }
            g_free(nm);
        }
    }

    /* WHICH PAGE TO OPEN ON - and unlike the font, this one is read
     * back immediately, because OK and Cancel navigate to it. */
    menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_prefs_start));
    active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
    if (active != NULL)
        pf.start_page = (int)(long)
            gtk_object_get_user_data(GTK_OBJECT(active));

    /*
     * WRITTEN AS AN EXPLICIT 0 OR 1, never left unset - once a user
     * has opened this dialog and pressed OK they have made a choice,
     * even if it matches the mode's default. The unset state stays
     * reachable by hand-editing, which the template's comment
     * describes.
     */
    if (g_prefs_result != NULL)
        pf.show_apply_result =
            GTK_TOGGLE_BUTTON(g_prefs_result)->active ? 1 : 0;

    /* THE MACHINE SETTING - only when it could be changed here. In
     * memory until File > Save Configuration, like everything else
     * (design/51: only the File menu writes files). */
    drift_was = vlhe_baseline_drift();
    drift_now = drift_was;
    if (g_prefs_bdrift[0] != NULL && GTK_WIDGET_IS_SENSITIVE(g_prefs_bdrift[0])) {
        int m;

        for (m = 0; m < 3; m++)
            if (GTK_TOGGLE_BUTTON(g_prefs_bdrift[m])->active
                && vlhe_set_baseline_drift(m) == 0)
                drift_now = m;
    }

    if (vlhe_set_prefs(&pf) == 0) {
        /*
         * SAY WHICH HAPPENED. The font changes on screen now; the
         * SETTING is memory-only until Apply, like every other page
         * - which is the whole point of the change (row 93).
         *
         * AND THE RETURN VALUE IS CHECKED, 2026-09-29. This printed
         * "Font applied" unconditionally, so a dialog that changed
         * nothing - both menus left at their defaults - still
         * claimed it had. The user saw exactly that and reasonably
         * read it as the restyle failing.
         */
        /* SAY WHAT CHANGED - 2026-10-04, the user: OK after changing
         * only System Baseline said "No font set", which was true and
         * said nothing about the setting that did change. */
        int font_set = vlhe_apply_font(&pf, 1);
        char said[256];
        const char *dname = drift_now == VLHE_BDRIFT_WARN
                                ? STR_SHELL_DRIFT_NAME_WARN
                          : drift_now == VLHE_BDRIFT_REFUSE
                                ? STR_SHELL_DRIFT_NAME_REFUSE
                          : STR_SHELL_DRIFT_NAME_ASK;

        if (drift_now != drift_was && font_set) {
            sprintf(said, FMT_SHELL_MSG_FONT_AND_BASELINE_SET, dname);
            status_set(said);
        } else if (drift_now != drift_was) {
            sprintf(said, FMT_SHELL_MSG_BASELINE_SET, dname);
            status_set(said);
        } else if (font_set)
            status_set(STR_SHELL_MSG_FONT_APPLIED_FILE_SAVE);
        else
            status_set(STR_SHELL_MSG_NO_FONT_SET_INTERFACE);
    } else {
        status_set(STR_SHELL_MSG_COULD_NOT_APPLY);
    }

    prefs_close();
}

/* ------------------------------------------------------------------ *
 * Reset Settings - File menu                                          *
 * ------------------------------------------------------------------ *
 *
 * IN THE FILE MENU, NOT AN EDIT MENU. The user raised the question
 * and half-answered it, 2026-09-22: "sometimes Edit menus are just
 * undo redo cut copy paste so there is no one standard right?"
 *
 * There is more of a standard than that suggests. An Edit menu in
 * this era is overwhelmingly undo/redo/cut/copy/paste - it is a
 * DOCUMENT menu, and this is a control panel with no document. A
 * user opening Edit expecting cut and paste and finding a
 * destructive reset would be worse served than by File, which
 * already holds Save Configuration and Preferences. And a whole menu
 * bar entry for one item earns nothing.
 */
static GtkWidget *g_reset_dlg;
static GtkWidget *g_reset_scope;
static int        g_reset_choice;

static void on_reset_scope(GtkWidget *w, gpointer data)
{
    (void)w;
    g_reset_choice = (int)(long) data;
}

/* Each line the backend reports, into the status bar and the log. */
/* Each line the backend reports, into the status bar - and REMEMBERED,
 * because a backend that explains itself should not have its
 * explanation overwritten by a guess. See on_reset_confirmed(). */
static int  g_reset_said;

static void reset_report(const char *line)
{
    g_reset_said = 1;
    status_set(line);
}

static void on_reset_confirmed(GtkWidget *w, gpointer data)
{
    GtkWidget *dlg = (GtkWidget *) data;
    int scopes = g_reset_choice;
    int n;

    (void)w;

    gtk_widget_destroy(dlg);

    /*
     * AND THE CHOOSER BEHIND IT, because the job is finished - the
     * user, 2026-09-22, seeing both stacked over the main window:
     * "if a user selected reset then the reset is done. Should they
     * still return to that other window".
     *
     * No, and Cancel is the case that differs: cancelling means pick
     * something else or close, so it leaves the chooser up. Three
     * reasons this one does not:
     *
     *   - THE CHOOSER IS NOW STALE. It still offers the scope that
     *     was just reset, with nothing to say it happened, so a user
     *     could press Reset again on a file that is already default
     *     and read the same message as though nothing had worked.
     *   - THE RESULT GOES TO THE STATUS BAR, which is behind both
     *     dialogs until they close.
     *   - RESETTING IS NOT A LOOP. Someone wanting several scopes
     *     picks "All settings" rather than going round three times.
     */
    if (g_reset_dlg != NULL)
        gtk_widget_destroy(g_reset_dlg);

    g_reset_said = 0;
    n = vlhe_reset_settings(scopes, reset_report);
    if (n < 0) {
        status_set(STR_SHELL_MSG_NEEDS_ROOT_RUN_CONTROL);
        return;
    }
    if (n == 0) {
        /*
         * ONLY GUESS WHEN NOTHING EXPLAINED ITSELF. The fake backend
         * returns 0 after reporting "this is the demonstration
         * backend - nothing was reset", and a blanket "no settings
         * file was found" replaced that with something untrue -
         * found by the user running it, 2026-09-22.
         *
         * A real backend reports per file too, so this line is now
         * only reached when there was genuinely nothing to say.
         */
        /*
         * "NOTHING WAS RESET" IS NOT A FAILURE, and the wording used
         * to read like one - the user hit it on 2026-09-22 and took
         * it for an error. reset_one() returns 0 both when it FAILED
         * and when there was nothing to do, so a scope that found no
         * files is indistinguishable from one that could not write.
         *
         * THE REPORTS ARE WHAT DISTINGUISH THEM. A real failure
         * report()s its reason per file and sets g_reset_said, so
         * reaching here silently means the scopes genuinely held
         * nothing - which happens easily with All users on a machine
         * where only root has settings.
         */
        if (!g_reset_said)
            status_set(STR_SHELL_MSG_NOTHING_NEEDED_RESETTING_NO);
        return;
    }

    /* THE PAGES ARE NOW SHOWING VALUES THAT ARE NO LONGER ON DISK.
     * Every module's reload() reads the backend, which has just
     * re-read the files, so this is what makes the window agree with
     * them without a restart. */
    {
        int i;
        for (i = 0; i < MOD_COUNT; i++)
            if (g_mod[i].reload != NULL)
                g_mod[i].reload();
    }
    sidebar_mark_dirty();
}

/*
 * THE SECOND PROMPT, and it names the backup rather than warning
 * that nothing can be undone - the user asked for both, and having
 * both means the warning can be true AND calm. The backup is made by
 * the backend, so this describes what WILL happen; a failure to back
 * up aborts that file and is reported per file.
 */
static void open_reset_confirm(GtkWidget *w, gpointer data)
{
    GtkWidget *dlg, *vbox, *lab, *hbox, *b;
    char msg[512];
    const char *what;

    (void)w; (void)data;

    /* No "choose first" test here - design/47 Q8: the dialog opens with
     * a radio selected (open_reset sets VLHE_RESET_USER) and a radio
     * group always has one, so 0 could never be seen. */
    switch (g_reset_choice) {
    case VLHE_RESET_USER:
        what = STR_SHELL_TEXT_RESET_YOUR_OWN;
        break;
    case VLHE_RESET_SYSTEM:
        what = STR_SHELL_TEXT_RESET_SYSTEM;
        break;
    case VLHE_RESET_ALLUSERS:
        what = STR_SHELL_TEXT_RESET_EVERY_USER;
        break;
    default:
        what = STR_SHELL_TEXT_RESET_ALL;
        break;
    }

    dlg = gtk_window_new(GTK_WINDOW_DIALOG);
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_RESET_SETTINGS);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    vbox = gtk_vbox_new(FALSE, 8);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);

    sprintf(msg,
        FMT_SHELL_RESET_DEFAULTS_EACH_FILE,
        what,
        vlhe_seed_exists()
          ? STR_SHELL_TEXT_DEFAULTS_FROM_SEED
          : STR_SHELL_TEXT_DEFAULTS_SHIPPED);

    lab = gtk_label_new(msg);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    hbox = gtk_hbox_new(TRUE, 8);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    /* CANCEL FIRST AND FOCUSED - the safe one should be where the
     * hand already is, for a dialog that can throw work away. */
    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    gtk_signal_connect_object(GTK_OBJECT(b), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_box_pack_start(GTK_BOX(hbox), b, TRUE, TRUE, 0);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_widget_grab_default(b);
    gtk_widget_show(b);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_RESET), STR_SHELL_BTN_RESET_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_reset_confirmed), dlg);
    gtk_box_pack_start(GTK_BOX(hbox), b, TRUE, TRUE, 0);
    gtk_widget_show(b);

    gtk_widget_show(vbox);
    gtk_widget_show(dlg);
}

static void on_reset_dlg_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    g_reset_dlg = NULL;
}

/* ------------------------------------------------------------------ *
 * Modify - asking for the root password, kcontrol's way              *
 * ------------------------------------------------------------------ *
 *
 * SHOWN ONLY WHEN THERE IS PRIVILEGE TO REGAIN. vlhe_priv_can_unlock()
 * is true in the setuid build, run by a non-root user, before the
 * password has been given - so root never sees this button and
 * neither does anyone running the ordinary build, where it would be
 * a control that cannot work.
 *
 * THE SHAPE IS Corel's own (kdelibs/kcontrol.cpp:425): a password
 * box, and on success seteuid(0) and ungrey. The verification is
 * kcheckpass's, which Corel ships setuid root, so nothing here holds
 * or checks a password - it collects one, hands it over a pipe, and
 * reads an exit status.
 *
 * AND THE BUFFER IS WIPED. A password sitting in the process image
 * after the dialog closes is a thing we can avoid for one memset.
 */
static GtkWidget *g_modify_dlg;
static GtkWidget *g_modify_entry;

static void on_modify_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    g_modify_dlg = NULL;
    g_modify_entry = NULL;
}

static void on_modify_cancel(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_modify_dlg != NULL)
        gtk_widget_destroy(g_modify_dlg);
}

static void on_modify_ok(GtkWidget *w, gpointer d)
{
    char pw[128];
    char why[160];
    const char *text;
    int  rc;

    (void)w; (void)d;
    if (g_modify_entry == NULL)
        return;

    text = gtk_entry_get_text(GTK_ENTRY(g_modify_entry));
    strncpy(pw, text != NULL ? text : "", sizeof pw - 1);
    pw[sizeof pw - 1] = '\0';

    rc = vlhe_priv_unlock(pw, why, sizeof why);

    /* WIPED IN BOTH PLACES - ours and the widget's - before anything
     * else happens. */
    memset(pw, 0, sizeof pw);
    gtk_entry_set_text(GTK_ENTRY(g_modify_entry), "");

    if (rc != 0) {
        /* THE DIALOG STAYS OPEN on a wrong password, so it can be
         * retyped without reopening. It closes on every other
         * failure, because those are not retryable.
         *
         * ON THE RETURN CODE, NOT THE WORDING - design/47 section 7
         * fault 3. This tested strstr(why, "not the root password")
         * against text made in vlhe_priv.c, so the strings header
         * could not take that sentence without ending the retry. */
        status_set(why[0] != '\0' ? why : STR_SHELL_MSG_AUTHENTICATION_FAILED);
        if (rc == VLHE_UNLOCK_WRONG) {
            gtk_widget_grab_focus(g_modify_entry);
            return;
        }
        gtk_widget_destroy(g_modify_dlg);
        return;
    }

    gtk_widget_destroy(g_modify_dlg);

    /* THE BUTTON HAS DONE ITS JOB AND GOES. Leaving it would offer
     * to unlock something already unlocked. */
    if (g_btn_modify != NULL)
        gtk_widget_hide(g_btn_modify);

    /*
     * THE PAGES ARE *NOT* RELOADED, AND A FIRST VERSION DID.
     *
     * THE USER, 2026-09-22: "As soon as the password was accepted
     * and I got Unlocked - settings can now be applied it undid any
     * settings I changed. I had sb selected in the card module. hit
     * modify and it reset the settings."
     *
     * reload() IS CANCEL'S IMPLEMENTATION. It re-reads the backend
     * and overwrites every widget, which is exactly right for Cancel
     * and exactly wrong here: unlocking is what lets a user KEEP the
     * edits they have made, so discarding them is the opposite of
     * what the button is for. It was called to refresh the greying
     * and took the pending work with it.
     *
     * NOTHING NEEDS RELOADING ANYWAY. The controls were live the
     * whole time - only the COMMIT was refused - which is the
     * behaviour design/33 says to take from kcontrol: "controls stay
     * live for a non-root user, so settings can be examined and
     * composed. Only the commit is refused." So the widgets already
     * hold what the user typed and the backend already holds what
     * they applied; the only thing that changed is whether OK works.
     *
     * show_module() below repaints the button row, which is the
     * whole of what this moment requires.
     */
    sidebar_mark_dirty();

    /* RE-SHOW THE CURRENT PAGE rather than forcing the button row on.
     * show_module() is where the per-module and per-tab rules live -
     * a live page still wants no OK after unlocking - and calling
     * buttons_show() directly from here would be a second copy of
     * that decision. */
    /*
     * AND THE MACHINE CONTROLS, which were greyed at BUILD - design/49
     * N7. Those pages decided once, when built, and nothing re-asked
     * after Modify, so they stayed grey for the whole session. Each
     * re-applies sensitivity only; none reloads a value, for the
     * reason above.
     */
    sound_privilege_changed();
    midi_privilege_changed();
    cd_privilege_changed();

    if (g_current_mod >= 0 && g_current_mod < MOD_COUNT)
        show_module(g_current_mod);

    status_set(STR_SHELL_MSG_UNLOCKED_SETTINGS_CAN_NOW);
    sidebar_update_whoami();

    /* NOW ABLE TO ACT: a config root did not write gets its one offer
     * - design/49 R1. After the status line, so the offer's own
     * result is what the user reads last. */
    offer_takeover();
}

static void open_modify(GtkWidget *w, gpointer data)
{
    GtkWidget *dlg, *vbox, *lab, *hbox, *b;

    (void)w; (void)data;

    if (g_modify_dlg != NULL) {
        gdk_window_raise(g_modify_dlg->window);
        return;
    }

    dlg = gtk_window_new(GTK_WINDOW_DIALOG);
    g_modify_dlg = dlg;
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_AUTHENTICATION);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(on_modify_destroy), NULL);

    vbox = gtk_vbox_new(FALSE, 8);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);

    lab = gtk_label_new(STR_SHELL_LABEL_CHANGING_THESE_SETTINGS_REQUIRES);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    hbox = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    lab = gtk_label_new(STR_SHELL_LABEL_PASSWORD);
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    g_modify_entry = gtk_entry_new_with_max_length(127);
    /* GTK 1.2's PASSWORD MODE. There is no separate widget; an entry
     * with visibility off draws the invisible character instead. */
    gtk_entry_set_visibility(GTK_ENTRY(g_modify_entry), FALSE);
    gtk_signal_connect(GTK_OBJECT(g_modify_entry), "activate",
                       GTK_SIGNAL_FUNC(on_modify_ok), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_modify_entry, TRUE, TRUE, 0);
    gtk_widget_show(g_modify_entry);

    /* THE ROW IS SHAPED LIKE A GtkDialog ACTION AREA - homogeneous
     * slots with a fixed-size button centred in each - so the pair
     * come out the size every other dialog's buttons do. They were
     * packed to fill the row, which stretched them to whatever width
     * the label above happened to give the window. */
    hbox = gtk_hbox_new(TRUE, 8);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_modify_cancel), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    gtk_widget_show(b);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_OK), STR_SHELL_BTN_OK_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_modify_ok), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b, FALSE, FALSE, 0);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_widget_show(b);

    /* The same size as every other dialog's buttons - the user,
     * 2026-10-01, on seeing Cancel drawn larger than OK. */
    vlhe_buttons_equalise(hbox);

    gtk_widget_show(vbox);
    gtk_widget_show(dlg);

    /* THE CURSOR STARTS IN THE BOX - there is one field and it is
     * what the dialog is for. */
    gtk_widget_grab_focus(g_modify_entry);
}

static void open_reset(GtkWidget *w, gpointer data)
{
    GtkWidget *dlg, *vbox, *lab, *hbox, *menu, *item, *b;
    int root = vlhe_priv_can_act();  /* offer, not "euid 0 now" */

    (void)w; (void)data;

    if (g_reset_dlg != NULL) {
        gdk_window_raise(g_reset_dlg->window);
        return;
    }

    g_reset_choice = VLHE_RESET_USER;

    dlg = gtk_window_new(GTK_WINDOW_DIALOG);
    g_reset_dlg = dlg;
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_RESET_SETTINGS);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(on_reset_dlg_destroy), NULL);

    vbox = gtk_vbox_new(FALSE, 8);
    gtk_container_add(GTK_CONTAINER(dlg), vbox);

    lab = gtk_label_new(
        STR_SHELL_LABEL_PUT_SETTINGS_BACK_DEFAULTS);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    hbox = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    lab = gtk_label_new(STR_SHELL_LABEL_RESET);
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    /*
     * FOUR SCOPES, AND THREE OF THEM NEED ROOT. They are shown
     * greyed rather than hidden for a user who is not: a control
     * that is missing looks like a feature that does not exist,
     * where a greyed one says "not you, not now" - and the note
     * below says why.
     */
    menu = gtk_menu_new();

    item = gtk_menu_item_new_with_label(STR_SHELL_MENU_CURRENT_USER_SETTINGS);
    gtk_signal_connect(GTK_OBJECT(item), "activate",
                       GTK_SIGNAL_FUNC(on_reset_scope),
                       (gpointer)(long) VLHE_RESET_USER);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);

    item = gtk_menu_item_new_with_label(STR_SHELL_MENU_SYSTEM_SETTINGS);
    gtk_signal_connect(GTK_OBJECT(item), "activate",
                       GTK_SIGNAL_FUNC(on_reset_scope),
                       (gpointer)(long) VLHE_RESET_SYSTEM);
    gtk_widget_set_sensitive(item, root ? TRUE : FALSE);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);

    item = gtk_menu_item_new_with_label(STR_SHELL_MENU_ALL_USERS_SETTINGS);
    gtk_signal_connect(GTK_OBJECT(item), "activate",
                       GTK_SIGNAL_FUNC(on_reset_scope),
                       (gpointer)(long) VLHE_RESET_ALLUSERS);
    gtk_widget_set_sensitive(item, root ? TRUE : FALSE);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);

    item = gtk_menu_item_new_with_label(
               STR_SHELL_MENU_ALL_SETTINGS_SYSTEM_EVERY);
    gtk_signal_connect(GTK_OBJECT(item), "activate",
                       GTK_SIGNAL_FUNC(on_reset_scope),
                       (gpointer)(long)(VLHE_RESET_USER
                                        | VLHE_RESET_SYSTEM
                                        | VLHE_RESET_ALLUSERS));
    gtk_widget_set_sensitive(item, root ? TRUE : FALSE);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);

    g_reset_scope = gtk_option_menu_new();
    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_reset_scope), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_reset_scope), 0);
    gtk_box_pack_start(GTK_BOX(hbox), g_reset_scope, TRUE, TRUE, 0);
    gtk_widget_show(g_reset_scope);

    if (!root) {
        lab = gtk_label_new(
            STR_SHELL_LABEL_ONLY_YOUR_OWN_SETTINGS);
        gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
        gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);
    }

    hbox = gtk_hbox_new(TRUE, 8);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CLOSE), STR_SHELL_BTN_CLOSE_TIP);
    gtk_signal_connect_object(GTK_OBJECT(b), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_box_pack_start(GTK_BOX(hbox), b, TRUE, TRUE, 0);
    gtk_widget_show(b);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_RESET_2), STR_SHELL_BTN_RESET_2_TIP);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(open_reset_confirm), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), b, TRUE, TRUE, 0);
    gtk_widget_show(b);

    gtk_widget_show(vbox);
    gtk_widget_show(dlg);
}

static void open_prefs(GtkWidget *w, gpointer data)
{
    struct vlhe_prefs pf;
    GtkWidget *dlg;
    GtkWidget *notebook;
    GtkWidget *page2;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *lab;
    GtkWidget *b;
    /* the Start Page option menu below still builds by hand - there
     * is no toolkit widget for "pick one of our own pages". */
    GtkWidget *menu;
    GtkWidget *item;
    int        i, sel;

    (void)w; (void)data;

    if (g_prefs_dlg != NULL) {
        gdk_window_raise(g_prefs_dlg->window);
        return;
    }

    vlhe_prefs(&pf);

    /* SEED THE PICKER FROM THE SAVED FONT, or from the one in use if
     * there is none - either is more use than opening on nothing.
     * The running font's name comes from X: GDK has no
     * gdk_font_name(), but the loaded XFontStruct carries its own
     * FONT property, which is the full XLFD. */
    strncpy(g_prefs_font_xlfd, pf.font, sizeof g_prefs_font_xlfd - 1);
    g_prefs_font_xlfd[sizeof g_prefs_font_xlfd - 1] = '\0';
    g_prefs_font_from_x = (g_prefs_font_xlfd[0] == '\0');
    if (g_prefs_font_xlfd[0] == '\0' &&
        g_window != NULL && g_window->style != NULL &&
        g_window->style->font != NULL &&
        g_window->style->font->type == GDK_FONT_FONT) {
        XFontStruct  *xfs =
            (XFontStruct *) GDK_FONT_XFONT(g_window->style->font);
        unsigned long v;

        if (xfs != NULL && XGetFontProperty(xfs, XA_FONT, &v)) {
            char *nm = XGetAtomName(GDK_DISPLAY(), (Atom) v);

            if (nm != NULL) {
                strncpy(g_prefs_font_xlfd, nm,
                        sizeof g_prefs_font_xlfd - 1);
                g_prefs_font_xlfd[sizeof g_prefs_font_xlfd - 1] = '\0';
                XFree(nm);
            }
        }
    }

    dlg = gtk_dialog_new();
    g_prefs_dlg = dlg;
    gtk_window_set_title(GTK_WINDOW(dlg), STR_SHELL_TITLE_PREFERENCES);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    /* CLEARED HOWEVER IT CLOSES - 2026-10-01, the user: "If I open it
     * too many times in the fake gui it no longer opens up". OK, Use
     * Default and Cancel went through prefs_close(), which NULLs the
     * pointer; the window manager's close button destroyed the dialog
     * without it, so the "already open" test above raised a widget
     * that was gone and Preferences never opened again. The conf_sel
     * dialog has always done this. */
    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed), &g_prefs_dlg);

    /*
     * TWO TABS, AND THE FONT PICKER IS EMBEDDED IN THE FIRST -
     * 2026-09-29, the user's suggestion: "Can we do a Tab for the
     * font and embed the window and therefoe it would have a
     * parent?"
     *
     * THE PARENT WAS THE RIGHT DIAGNOSIS AND A TAB IS NOT WHAT
     * SUPPLIES IT. Measured both ways: a notebook page and a plain
     * frame each parent the widget, and either works PROVIDED the
     * pack happens before gtk_font_selection_set_filter(). That call
     * repopulates the lists and touches the preview entry, and
     * realizing an entry asserts without a parent window
     * (gtkentry.c:585). An earlier attempt packed first and still
     * failed, which sent this down the route of a separate dialog;
     * the test says the ordering is sufficient and that attempt was
     * mis-made.
     *
     * SO THE TAB IS FOR THE HEIGHT, NOT THE PARENT. Embedded in the
     * old single-column layout this dialog wanted 561x783 against a
     * 600-high target screen. The picker alone is 525x361; with
     * Startup and After Load on a second page, each page fits.
     */
    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), notebook,
                       TRUE, TRUE, 0);
    gtk_widget_show(notebook);

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_container_border_width(GTK_CONTAINER(frame), 4);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), frame,
                             gtk_label_new(STR_SHELL_LABEL_FONT));

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * THE PICKER ITSELF, EMBEDDED. Packed BEFORE it is filtered -
     * see the notebook comment above for why that order is the
     * whole of it.
     */
    /* INTO THE PAGE'S VBOX, made above - the picker and the note
     * below both live on this page, and GtkFrame takes ONE child.
     * An earlier version added the picker to the FRAME directly,
     * which left it competing with that vbox: the vbox won, the
     * picker was orphaned, and the page showed one line of text. */
    g_prefs_font = gtk_font_selection_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_prefs_font, TRUE, TRUE, 0);

    {
        static gchar *charsets[] = { "iso8859-1", NULL };

        /* BITMAP + iso8859-1: scalable faces have no strike at a
         * fixed pixel size on a 1999 server, and a charset we cannot
         * render is not a choice worth offering. */
        gtk_font_selection_set_filter(GTK_FONT_SELECTION(g_prefs_font),
                                      GTK_FONT_FILTER_BASE,
                                      GTK_FONT_BITMAP,
                                      NULL, NULL, NULL, NULL, NULL,
                                      charsets);
    }

    if (g_prefs_font_xlfd[0] != '\0')
        gtk_font_selection_set_font_name(GTK_FONT_SELECTION(g_prefs_font),
                                         g_prefs_font_xlfd);

    /* show_all, NOT show - this is a container of lists, entries and
     * a preview, and show() would map none of them. */
    gtk_widget_show_all(g_prefs_font);


    /* ---- and when it takes effect ---------------------------------- */

    /* ONE LINE. It said "Applies the next time the Control Center
     * starts", which stopped being true when the font went live, and
     * three more explaining that Corel ships an empty theme - which
     * is a fact about the platform, not something the user needs at
     * the moment of choosing a font. */
    g_prefs_note = gtk_label_new(
        STR_SHELL_LABEL_APPLIES_ONCE_USE_DEFAULT);
    gtk_label_set_justify(GTK_LABEL(g_prefs_note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(g_prefs_note), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), g_prefs_note, FALSE, FALSE, 4);
    gtk_widget_show(g_prefs_note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- which page to open on ------------------------------------- */

    /* ITS OWN FRAME, because it is not a font. Both are about the
     * PROGRAM rather than the emulated hardware, which is why they
     * share this dialog rather than being a fifth sidebar entry. */
    /* PAGE TWO holds everything that is not the font. */
    page2 = gtk_vbox_new(FALSE, 0);
    gtk_container_border_width(GTK_CONTAINER(page2), 4);
    /* PREPENDED, so Behaviour is tab 0 and Font tab 1 - the user's
     * order. The Font page is built first because the picker must
     * be packed before it is filtered; this puts the pages on
     * screen the other way round without moving that code. */
    gtk_notebook_prepend_page(GTK_NOTEBOOK(notebook), page2,
                              gtk_label_new(STR_SHELL_LABEL_BEHAVIOUR));
    gtk_widget_show(page2);

    frame = gtk_frame_new(STR_SHELL_FRAME_STARTUP);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_box_pack_start(GTK_BOX(page2), frame, TRUE, TRUE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    hbox = gtk_hbox_new(FALSE, 6);

    lab = gtk_label_new(STR_SHELL_LABEL_OPEN);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
    gtk_widget_set_usize(lab, 60, -1);
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    g_prefs_start = gtk_option_menu_new();
    menu = gtk_menu_new();
    sel = 0;

    /* EVERY MODULE, BY ITS SIDEBAR NAME, so the menu cannot drift
     * from the sidebar - including the viewer, since someone who
     * mostly plays discs may well want it. */
    for (i = 0; i < MOD_COUNT; i++) {
        item = gtk_menu_item_new_with_label(g_mod[i].label);
        gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)(long) i);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        if (i == pf.start_page)
            sel = i;
    }

    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_prefs_start), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_prefs_start), sel);
    gtk_box_pack_start(GTK_BOX(hbox), g_prefs_start, TRUE, TRUE, 0);
    gtk_widget_show(g_prefs_start);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    lab = gtk_label_new(
        STR_SHELL_LABEL_ALSO_WHERE_OK_RETURNS);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 4);
    gtk_widget_show(lab);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- the apply-result box ----------------------------------- */

    frame = gtk_frame_new(STR_SHELL_FRAME_AFTER_LOAD_UNLOAD);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(page2), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * THE LABEL DOES NOT SAY WHEN, and the user noticed: "It should
     * be something like 'Show Dialog boxes - ' but missing a word but
     * think my wording is better for now."
     *
     * The frame title carries it - "After Load and Unload" - so a
     * sighted reader has the context. A SCREEN READER ANNOUNCING THE
     * CHECKBOX ALONE DOES NOT, which is the case that argues for
     * putting it in the label. Left as-is on the user's call; worth
     * revisiting with whatever accessibility pass the cleanup step
     * brings.
     */
    g_prefs_result = vlhe_tipped(gtk_check_button_new_with_label(
        STR_SHELL_CHECK_SHOW_WHAT_HAPPENED_STEP), STR_SHELL_CHECK_SHOW_WHAT_HAPPENED_STEP_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_prefs_result),
                                 vlhe_show_apply_result() ? TRUE : FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), g_prefs_result, FALSE, FALSE, 0);
    gtk_widget_show(g_prefs_result);

    /*
     * THE LABEL SAYS WHICH WAY THE DEFAULT GOES, because it differs by
     * mode and a checkbox alone cannot show that. A user who has never
     * touched this finds it already ticked on a portable copy and
     * already clear on an installed one, and the sentence explains why
     * rather than leaving it looking arbitrary.
     */
    lab = gtk_label_new(
        STR_SHELL_LABEL_DEFAULT_WHEN_RUNNING_FROM);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 4);
    gtk_widget_show(lab);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- System Baseline - [Load] BaselineDrift ------------------ */

    /*
     * A MACHINE SETTING IN THE PROGRAM'S DIALOG, and the frame says so:
     * it is stored in the system file and changes what every Load on
     * this machine does when a path VLHE manages has drifted. The user
     * placed it here, 2026-10-04 - the sidebar is the hardware, and
     * Status has no room - and asked for it greyed unless root.
     */
    frame = gtk_frame_new(STR_SHELL_FRAME_SYSTEM_BASELINE);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(page2), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    lab = gtk_label_new(STR_SHELL_LABEL_BASELINE_DRIFT);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);
    {
        static const char *const words[3] = {
            STR_SHELL_RADIO_DRIFT_ASK,
            STR_SHELL_RADIO_DRIFT_WARN,
            STR_SHELL_RADIO_DRIFT_REFUSE
        };
        static const char *const tips[3] = {
            STR_SHELL_RADIO_DRIFT_ASK_TIP,
            STR_SHELL_RADIO_DRIFT_WARN_TIP,
            STR_SHELL_RADIO_DRIFT_REFUSE_TIP
        };
        GSList *grp = NULL;
        int     cur = vlhe_baseline_drift(), m;
        gint    can = vlhe_priv_can_act() ? TRUE : FALSE;

        for (m = 0; m < 3; m++) {
            g_prefs_bdrift[m] = vlhe_tipped(
                gtk_radio_button_new_with_label(grp, words[m]), tips[m]);
            grp = gtk_radio_button_group(GTK_RADIO_BUTTON(g_prefs_bdrift[m]));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_prefs_bdrift[m]),
                                         m == cur ? TRUE : FALSE);
            gtk_widget_set_sensitive(g_prefs_bdrift[m], can);
            gtk_box_pack_start(GTK_BOX(vbox), g_prefs_bdrift[m],
                               FALSE, FALSE, 0);
            gtk_widget_show(g_prefs_bdrift[m]);
        }
        if (!can) {
            lab = gtk_label_new(STR_SHELL_LABEL_BASELINE_NEEDS_ROOT);
            gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
            gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
            gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 4);
            gtk_widget_show(lab);
        }
    }

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_OK), STR_SHELL_BTN_OK_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_prefs_ok), NULL);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), b,
                       TRUE, TRUE, 0);
    gtk_widget_show(b);
    gtk_widget_grab_default(b);

    /* RESET IS IN THE DIALOG TOO, for the user who can still read it -
     * --reset is for the one who cannot. */
    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_USE_DEFAULT), STR_SHELL_BTN_USE_DEFAULT_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_prefs_reset), NULL);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), b,
                       TRUE, TRUE, 0);
    gtk_widget_show(b);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_prefs_cancel), NULL);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), b,
                       TRUE, TRUE, 0);
    gtk_widget_show(b);

    gtk_widget_show(dlg);
}

/*
 * BACK TO 800x600 - the user's request, 2026-09-20: "If it gets
 * accidentally resized there is no way to go back without
 * restarting".
 *
 * gtk_window_set_default_size() CANNOT DO THIS. Its whole point is
 * that it applies only before the window is first mapped - the
 * comment at the top of this file says so - which is exactly why the
 * window cannot be talked back down once a user has dragged it.
 *
 * gtk_widget_set_usize() on a GtkWindow sets the size directly and
 * takes effect immediately, but it also sets a MINIMUM: leave it in
 * place and the window can never be made smaller again. So it is
 * applied and then cleared with -1,-1, which resizes without pinning.
 */
static void
on_reset_size(GtkWidget *w, gpointer data)
{
    (void) w; (void) data;

    if (g_window == NULL)
        return;

    /*
     * gdk_window_resize(), NOT gtk_widget_set_usize().
     *
     * THE set_usize PAIR DID NOT WORK, and the reason is worth
     * keeping: set_usize records a size REQUEST which GTK acts on
     * when it next computes geometry, so setting 800x600 and then
     * clearing it to -1,-1 in the same handler cancelled the request
     * before the main loop ever saw it. Both calls were processed
     * together and the net effect was nothing.
     *
     * Clearing it is still necessary - a usize left in place becomes
     * a MINIMUM and the window could never be made smaller again -
     * so the answer is not to use set_usize at all. gdk_window_resize
     * asks the X server directly and takes effect now, leaving the
     * widget's own size request untouched.
     */
    if (g_window->window != NULL)
        gdk_window_resize(g_window->window, WIN_W, WIN_H);

    report_to_status(STR_SHELL_MSG_WINDOW_RESET_800_X);
}

/* ------------------------------------------------------------------ */
/* Help - design/54 G01                                               */
/* ------------------------------------------------------------------ */

/*
 * THE PAGE'S TAB NOTEBOOK - the shallowest GtkNotebook under it, found
 * breadth first so a notebook INSIDE a tab is never mistaken for the
 * page's own. NULL for a page without tabs (Status, the CD+G viewer).
 */
static GtkWidget *
page_notebook(GtkWidget *page)
{
    GList *queue = NULL, *kids;
    GtkWidget *found = NULL;

    if (page == NULL)
        return NULL;
    queue = g_list_append(queue, page);
    while (queue != NULL && found == NULL) {
        GtkWidget *w = GTK_WIDGET(queue->data);
        queue = g_list_remove(queue, w);
        if (GTK_IS_NOTEBOOK(w)) {
            found = w;
            break;
        }
        if (GTK_IS_CONTAINER(w)) {
            kids = gtk_container_children(GTK_CONTAINER(w));
            queue = g_list_concat(queue, kids);
        }
    }
    g_list_free(queue);
    return found;
}

/* THE SIDEBAR'S ORDER AND ICONS, for the help list */
static void
help_pages(void)
{
    static const char *labels[MOD_COUNT];
    static char **xpms[MOD_COUNT];
    static int done;
    int i;

    if (done)
        return;
    for (i = 0; i < MOD_COUNT; i++) {
        labels[i] = g_mod[i].label;
        xpms[i] = g_mod[i].xpm;
    }
    /* THE GENERAL TOPICS TAKE THE VLHE ICON, the Status page's */
    vlhe_help_set_pages(labels, xpms, MOD_COUNT, xpm_vlhe);
    done = 1;
}

/*
 * THE HELP BUTTON: the help for the page on show and its current tab.
 * The tab is read off the notebook's own label, so it is whatever the
 * user is looking at - the " *" an unapplied change adds is stripped
 * by the lookup. A tab with no section falls back to the page's.
 */
static void
on_help(GtkWidget *w, gpointer data)
{
    const char *page = NULL;
    char *tab = NULL;

    (void) w; (void) data;
    help_pages();
    if (g_current_mod >= 0 && g_current_mod < MOD_COUNT) {
        GtkWidget *nb = page_notebook(g_mod[g_current_mod].page);
        page = g_mod[g_current_mod].label;
        if (nb != NULL) {
            int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
            GtkWidget *child = cur >= 0
                ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), cur) : NULL;
            GtkWidget *lab = child != NULL
                ? gtk_notebook_get_tab_label(GTK_NOTEBOOK(nb), child) : NULL;
            if (lab != NULL && GTK_IS_LABEL(lab))
                gtk_label_get(GTK_LABEL(lab), &tab);
        }
    }
    vlhe_help_show(page, tab);
}

static void
on_help_contents(GtkWidget *w, gpointer data)
{
    (void) w; (void) data;
    help_pages();
    vlhe_help_show(NULL, NULL);
}

static void
on_about(GtkWidget *w, gpointer data)
{
    (void) w; (void) data;
    vlhe_help_about(g_window);
}

static GtkWidget *build_menubar(void)
{
    GtkWidget *bar;
    GtkWidget *item;
    GtkWidget *menu;
    GtkWidget *entry;

    bar = gtk_menu_bar_new();

    /* File */
    menu  = gtk_menu_new();

    /* SAVE FIRST, ABOVE Preferences, because it acts on the machine's
     * settings where Preferences acts on the program's. */
    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_SAVE_CONFIGURATION);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_save_config), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    /* THE DRAFT PAIR - design/49 T0. A user saves what they cannot
     * save; root loads it, checked, and saves it as root's. */
    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_SAVE_DRAFT);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_save_draft), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_LOAD_CONFIGURATION);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_load_config), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new(); /* separator */
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    /* RESET BESIDE SAVE - both act on the settings FILES, where
     * Preferences acts on the program itself. */
    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_RESET_SETTINGS);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(open_reset), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    /* THE .old BACKUP, READ BACK - the first-run dialog's offer as a
     * menu item, available whenever there is one (design/51). */
    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_RESTORE_BACKUP);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_restore_backup), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    /* THE OLD CANCEL - everything since the last Save, every page -
     * now that Cancel reaches only its own page's unapplied edits
     * (on_cancel(), 2026-10-02). */
    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_REVERT_SAVED);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_revert_saved), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new(); /* separator */
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_PREFERENCES);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(open_prefs), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new(); /* separator */
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_EXIT);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_quit), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    item = gtk_menu_item_new_with_label(STR_SHELL_MENU_FILE);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_bar_append(GTK_MENU_BAR(bar), item);
    gtk_widget_show(item);

    /*
     * View. One item today, and it exists because the window can be
     * resized into a state with no way back - gtk_window_set_default_size
     * applies only before the first map.
     */
    menu = gtk_menu_new();

    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_RESET_WINDOW_SIZE);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_reset_size), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    item = gtk_menu_item_new_with_label(STR_SHELL_MENU_VIEW);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_bar_append(GTK_MENU_BAR(bar), item);
    gtk_widget_show(item);

    /* Help: the contents and the About box (design/54 G01). The
     * page's own help is the Help button, beside OK. */
    menu = gtk_menu_new();

    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_HELP_CONTENTS);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_help_contents), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    entry = gtk_menu_item_new_with_label(STR_SHELL_MENU_ABOUT);
    gtk_signal_connect(GTK_OBJECT(entry), "activate",
                       GTK_SIGNAL_FUNC(on_about), NULL);
    gtk_menu_append(GTK_MENU(menu), entry);
    gtk_widget_show(entry);

    item = gtk_menu_item_new_with_label(STR_SHELL_BTN_HELP);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    /* RIGHT-JUSTIFIED is the 1.2-era convention for Help and is what
     * the KDE and Motif apps on this desktop do. */
    gtk_menu_item_right_justify(GTK_MENU_ITEM(item));
    gtk_menu_bar_append(GTK_MENU_BAR(bar), item);
    gtk_widget_show(item);

    gtk_widget_show(bar);
    return bar;
}

static GtkWidget *build_buttons(void)
{
    GtkWidget *hbox;
    GtkWidget *bbox;
    GtkWidget *hbb;
    GtkWidget *b;

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_container_border_width(GTK_CONTAINER(hbox), 8);

    /* A HEIGHT THE ROW KEEPS WHEN ITS BUTTONS ARE HIDDEN. Without
     * this an empty hbox collapses to nothing and the panel jumps -
     * which is the whole thing buttons_show() exists to avoid.
     *
     * MEASURED, NOT GUESSED. The first attempt used 26 on the theory
     * that it was a stock button's height; the row came out too short,
     * the buttons were squeezed against the status bar with their
     * lower edge clipped, and the reserved strip was 10px rather than
     * 26 because the container's border is counted inside the request.
     * 42 is the button's own height plus this box's 8px border top and
     * bottom, checked against a capture. */
    gtk_widget_set_usize(hbox, -1, 42);

    /* HELP AT THE FAR LEFT, SIZED TO MATCH THE OTHER THREE.
     *
     * NOT IN ITS OWN GtkHButtonBox, which is what the previous version
     * did and which DREW A VISIBLE EMPTY FRAME between Help and OK: a
     * button box with one child and room to spare renders its
     * remainder as a sunken box. Invisible until the row was given a
     * height, then obvious.
     *
     * So Help is packed directly and given the button box's default
     * child size by hand - BOTH dimensions, which is the part that
     * took two attempts.
     *
     * 85 x 27, MEASURED off a capture rather than guessed. Passing -1
     * for the height does NOT mean "keep your natural height" here:
     * Help came out five pixels shorter than the other three and sat
     * lower in the row, with its label clipped. A button box sizes its
     * children uniformly in both axes, so a button packed outside one
     * has to be told both. */
    /* HELP GOES BACK IN A GtkHButtonBox - and the empty-frame problem
     * that caused it to be taken OUT is solved by packing the box
     * FALSE/FALSE so it has no spare room to draw.
     *
     * THREE WRONG EXPLANATIONS PRECEDED THIS, all from reading rather
     * than measuring, and each produced a build that looked fixed and
     * was not:
     *
     *   1. "Help needs its own button box to match" - true for WIDTH,
     *      and it drew a sunken empty frame beside it.
     *   2. "set_usize(-1) does not mean natural height" - so an
     *      explicit 27 was set. No change: still 5px short.
     *   3. "the hbox stretches its children to the row height" - so
     *      Help was wrapped in a non-stretching vbox. No change.
     *
     * gtkbbox.h has the answer: a button box applies child_min_height
     * AND child_ipadding, so its children are its requested size PLUS
     * internal padding. Nothing done to a button OUTSIDE such a box
     * reproduces that, because the padding is the box's, not the
     * button's. Measured: OK's top edge at y=543, Help's at 548.
     *
     * So the only thing that makes Help match is being in the same
     * kind of container, which is what this does. */
    hbb = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(hbb), GTK_BUTTONBOX_START);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_HELP), STR_SHELL_BTN_HELP_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_help), NULL);
    gtk_container_add(GTK_CONTAINER(hbb), b);
    gtk_widget_show(b);

    /* MODIFY, BESIDE HELP - kcontrol's placement, and the same
     * reasoning as Help's: it is not an action on the settings, it
     * is a route to being allowed to take one. Built always, SHOWN
     * by buttons_show() only where it can work. */
    g_btn_modify = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_MODIFY), STR_SHELL_BTN_MODIFY_TIP);
    GTK_WIDGET_SET_FLAGS(g_btn_modify, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(g_btn_modify), "clicked",
                       GTK_SIGNAL_FUNC(open_modify), NULL);
    gtk_container_add(GTK_CONTAINER(hbb), g_btn_modify);
    /* NOT shown here - buttons_show() decides, and on the ordinary
     * build the answer is never. */

    gtk_box_pack_start(GTK_BOX(hbox), hbb, FALSE, FALSE, 0);
    gtk_widget_show(hbb);
    g_btn_help = b;

    /* OK / Apply / Cancel at the far right, in a button box so they
     * come out the same width as each other.
     *
     * GTK_CAN_DEFAULT ON ALL THREE, AND THAT IS THE FIX FOR A REAL
     * BUG the user spotted on screen: OK came out visibly NARROWER
     * than the other two.
     *
     * Why. A button that can be the default reserves a default-
     * indicator border INSIDE its own allocation. The button box gives
     * every child the same outer size, so the one with the border has
     * less room left for its label and draws smaller. Setting the flag
     * on only OK - which is what "make OK the default" naively looks
     * like - therefore makes OK the odd one out.
     *
     * With the flag on all three they reserve the same border and come
     * out identical. Only OK calls grab_default(), so it is still the
     * button Return activates; the flag says "could be default", not
     * "is". */
    bbox = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
    gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 6);

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_OK), STR_SHELL_BTN_OK_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_ok), NULL);
    gtk_container_add(GTK_CONTAINER(bbox), b);
    gtk_widget_show(b);
    gtk_widget_grab_default(b);
    g_btn_ok = b;

    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_APPLY), STR_SHELL_BTN_APPLY_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_apply), NULL);
    gtk_container_add(GTK_CONTAINER(bbox), b);
    gtk_widget_show(b);
    g_btn_apply = b;

    /* CANCEL NO LONGER QUITS - 2026-09-18. It used to, because there
     * was nothing to discard and closing was the only sensible thing
     * a Cancel could do. Now the panels hold editable fields, so it
     * means what it says: throw away the unsaved edits and stay.
     * Closing the program is File/Exit and the window manager. */
    b = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CANCEL), STR_SHELL_BTN_CANCEL_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_cancel), NULL);
    gtk_container_add(GTK_CONTAINER(bbox), b);
    gtk_widget_show(b);
    g_btn_cancel = b;

    gtk_box_pack_end(GTK_BOX(hbox), bbox, FALSE, FALSE, 0);
    gtk_widget_show(bbox);

    gtk_widget_show(hbox);
    return hbox;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    GtkWidget *win;
    GtkWidget *outer;
    GtkWidget *body;
    GtkWidget *right;
    int        i;
    /*
     * OPEN ON THE USER'S CHOSEN PAGE, defaulting to Status.
     *
     * STATUS IS THE SHIPPED DEFAULT because a fresh install is
     * exactly when something is most likely misconfigured, and that
     * page answers "is anything wrong?" - landing on Volume would
     * show sliders on a machine whose daemon is dead.
     *
     * BUT VOLUME IS WHAT THE WINDOW IS MOSTLY OPENED FOR on an
     * ordinary day (the user, 2026-09-18), so this is a preference
     * rather than an argument either side has to win. -m still
     * overrides it, below.
     */
    int        startup_mod = MOD_STATUS;
    int        startup_tab = 0;     /* -T, for captures - see below */
    int        show_reset = 0;      /* --reset-dialog, likewise     */
    int        show_review = 0;     /* --review-dialog, likewise    */
    int        show_help = 0;       /* --help-dialog, likewise      */
    int        show_about = 0;      /* --about-dialog, likewise     */
    int        mod_from_argv = 0;   /* -m wins over the preference */
    int        start_vertical = 0;
    int        start_page = 0;
    int        reset_font = 0;
    char       title[128];

    /*

     * PRIVILEGE IS DEALT WITH FIRST, BEFORE ANYTHING READS THE

     * ENVIRONMENT. In the setuid build this drops effective root and

     * scrubs GTK_MODULES and friends; in the ordinary build it does

     * nothing at all. vlhe_priv.h has why it must precede

     * gtk_init(): GTK 1.2 g_module_open()s whatever GTK_MODULES

     * names and the target's 1.2.7 imports neither getuid nor

     * geteuid, so it cannot tell that it is setuid.

     *

     * IT ALSO PRECEDES THE SUBCOMMAND CHECK below, because those run

     * in the same process and have the same reason to be

     * unprivileged until asked.

     */

    /*
     * WHERE ARE WE? Resolved before anything asks, and with argv[0]
     * in hand - which the lazy path inside vlhe_self_dir() cannot
     * have. /proc/self/exe answers on any machine with /proc and
     * argv[0] is the fallback, so passing it matters only where /proc
     * is absent - but it costs one line and the alternative is a
     * resolver that silently has less to work with.
     *
     * BEFORE vlhe_priv_init(), because that scrubs the environment
     * and drops privilege, and this reads neither.
     */
    vlhe_self_init(argv[0]);

    /* ROOT TAKES NO OVERRIDES FROM ITS ENVIRONMENT - design/55
     * recommendation 3. Before anything reads one; a no-op for anyone
     * but root, and for the setuid build run by a user, which
     * vlhe_priv_init() rebuilds instead. vlhe_self.h has the list. */
    vlhe_self_root_env();

    vlhe_priv_init();

    /* THE BACKEND RAISES PRIVILEGE AROUND ROOT'S WORK THROUGH THESE -
     * vlhe_backend.h lists what it wraps. In the ordinary build both
     * are no-ops. Registered here, not in the backend, because the
     * CLI and the host tests link the backend without vlhe_priv.c. */
    vlhe_backend_set_priv(vlhe_priv_raise, vlhe_priv_lower);


    /*

     * A SUBCOMMAND MEANS THIS IS NOT A GUI RUN - and the test

     * happens BEFORE gtk_init(), which would otherwise open the

     * display and fail on a machine with no X. That is the whole

     * point of `vlhe volume' existing: it must work where the GUI

     * cannot. design/33 section 1d.

     *

     * The subcommands live in vlhe_cli.c, linked into BOTH this

     * binary and the toolkit-free vlhe.295, so they behave

     * identically whichever one a user reaches.

     */

    if (argc > 1 && vlhe_cli_is_subcommand(argv[1])) {
        /* NO WAY BACK TO ROOT FOR A SUBCOMMAND - design/54 D49. */
        vlhe_priv_drop_for_good();
        return vlhe_cli_main(argc, argv);
    }


    /* BEFORE gtk_init(), which would otherwise load a shared object
     * named by --gtk-module on the command line - design/49 N1. The
     * allow-list covers the environment; this covers argv. */
    vlhe_priv_filter_argv(&argc, argv);

    gtk_init(&argc, &argv);

    /* --reset - discard the saved font and start with the default.
     *
     * THE ESCAPE HATCH, and it exists because of one failure mode the
     * closed font list does not prevent: a size that resolves and is
     * legible in principle but is too small to work with on a
     * 1024x768 panel. The Preferences dialog is then reachable only
     * by squinting at it.
     *
     * A SEPARATE CONFIG FILE WAS THE ALTERNATIVE - keep the font
     * somewhere a bad value cannot take the rest of the settings with
     * it. Rejected: two files are two things to find and back up, the
     * recovery is editing a text file either way, and what actually
     * protects the other settings is that a parse failure in one
     * section does not abandon the others.
     *
     * BEFORE the saved font is read, so it wins. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reset") == 0) {
            struct vlhe_prefs pf;

            /* THE FONT ONLY - design/47 Q3, as on_prefs_reset(). */
            memset(&pf, 0, sizeof pf);
            (void) vlhe_prefs(&pf);
            pf.font[0] = '\0';
            vlhe_set_prefs(&pf);
            /*
             * AND COMMIT IT, WHICH `vlhe_set_prefs()' USED TO DO
             * ITSELF - 2026-09-29, when that became memory-only.
             *
             * THIS ONE MUST REACH THE DISK. It is the escape hatch
             * for a font too small to read the Preferences dialog
             * with; leaving it in memory would clear the font for
             * one run and hand the user the same unreadable window
             * next time, which is the whole failure it exists for.
             *
             * `create' IS 1 AND THAT IS NOT THE ROW 93 CASE. The
             * guard row 88 established is about a dialog making a
             * file the user never asked for; here the user typed
             * `--reset' to change a stored setting, which is the
             * consent that guard wants.
             */
            {
                struct vlhe_commit_err err;

                memset(&err, 0, sizeof err);
                if (vlhe_commit(&err, 1) != 0)
                    fprintf(stderr, "vlhe.gtk: --reset: the font was"
                                    " cleared for this run but not"
                                    " saved: %s - %s\n",
                            err.path[0] ? err.path : STR_SHELL_MSG_CONFIGURATION_PLACEHOLDER,
                            err.why[0] ? err.why : STR_SHELL_MSG_COULD_NOT_WRITE);
            }
            reset_font = 1;
            break;
        }
    }

    /*
     * THE DRAFT READ BACK, BEFORE ANY PAGE IS BUILT - design/51. For a
     * user who cannot write the system file, the draft at its default
     * path holds their machine settings (a non-root Save put them
     * there); taken here, the pages are built showing it. Root takes
     * nothing. The note goes to the status line once there is one.
     */
    (void) vlhe_conf_draft_readback(g_readback_note, sizeof g_readback_note);

    /* THE SAVED FONT, applied before any widget exists - and since
     * 2026-09-29 also applicable to a RUNNING gui, which is why the
     * rc string is built by a function rather than inline here. See
     * vlhe_apply_font(). */
    {
        struct vlhe_prefs pf;

        if (!reset_font && vlhe_prefs(&pf) == 0)
            vlhe_apply_font(&pf, 0);
    }

    /* -F NAME - override the interface font, for looking at one.
     *
     * NOT A SETTING AND NOT INTENDED TO SHIP. The desktop picks the
     * interface font - KDE has a panel for it - and an application
     * that overrides it looks foreign beside everything else. This
     * exists so a font can be SEEN before deciding anything:
     *
     *   ARGS='-F fixed'          the X core fixed font
     *   ARGS='-F 9x15'
     *   ARGS='-F -*-courier-medium-r-normal--12-*'
     *
     * gtk_rc_parse_string() applies it to every widget class, which is
     * the only route GTK 1.2 offers - there is no per-application font
     * API. It must run before any widget is built. */
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-F") == 0) {
            char rc[512];

            if (!font_safe(argv[i + 1])) {
                fprintf(stderr, "vlhe.gtk: -F %s: not a usable font name"
                                " (quotes and backslashes cannot go"
                                " into GTK rc text)\n", argv[i + 1]);
                break;
            }

            sprintf(rc,
                    "style \"vlhe-font\" { font = \"%.400s\" }\n"
                    "widget_class \"*\" style \"vlhe-font\"\n",
                    argv[i + 1]);
            gtk_rc_parse_string(rc);
            break;
        }
    }

    /* -m NAME - open on that module.
     *
     * SO A CAPTURE NEEDS NO MOUSE. The user works in 86Box on this
     * same workstation and there is ONE pointer; driving the window
     * with xdotool fights them for it and can deliver a click into the
     * guest (CLAUDE.md, and design/32 section 2). An argument that
     * selects a module makes any screen reachable with one launch and
     * one `import' - reproducible, scriptable, and it touches no input
     * state at all.
     *
     *   make run-gui-cc ARGS='-m CD+G Viewer'
     *
     * Matched case-insensitively on a prefix, so `-m cd+g' is enough
     * and the shell quoting stays simple. */
    /* THE SAVED SETTING FIRST, THEN THE FLAG. VerticalSliders is a
     * USER setting (~/.vlhe/vlhe.conf) and until 2026-09-19 nothing
     * read it here, so the orientation reset to horizontal on every
     * start however the checkbox was left. -V still wins, because it
     * exists to force the presentation for a capture. */
    start_vertical = vlhe_ui_vertical_sliders();

    /* -V - start with vertical faders, for the same reason as -m: a
     * capture of that presentation should not need the mouse.
     * -T N - open on tab N of the selected module, likewise. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0)
            start_vertical = 1;
        else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc)
            start_page = atoi(argv[i + 1]);
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reset-dialog") == 0)
            show_reset = 1;
        else if (strcmp(argv[i], "--review-dialog") == 0)
            show_review = 1;        /* the fake backend's one line of each */
        else if (strcmp(argv[i], "--help-dialog") == 0)
            show_help = 1;
        else if (strcmp(argv[i], "--about-dialog") == 0)
            show_about = 1;
    }

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-T") == 0)
            startup_tab = atoi(argv[i + 1]);
    }

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            int j;

            for (j = 0; j < MOD_COUNT; j++) {
                if (strncasecmp(argv[i + 1], g_mod[j].label,
                                strlen(argv[i + 1])) == 0) {
                    startup_mod = j;
                    mod_from_argv = 1;
                    break;
                }
            }
            break;
        }
    }

    if (vlhe_backend_init() != 0) {
        fprintf(stderr, "vlhe.gtk: backend init failed\n");
        return 1;
    }

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    /* THE BACKEND IS NAMED IN THE TITLE when it is not the real one, so
     * a screenshot of scripted state cannot be mistaken for a machine.
     * See vlhe_backend.h. */
    if (strcmp(vlhe_backend_name(), "real") == 0)
        strcpy(title, WIN_TITLE);
    else
        sprintf(title, "%s [%s backend]", WIN_TITLE,
                vlhe_backend_name());
    gtk_window_set_title(GTK_WINDOW(win), title);

    g_window = win;
    gtk_window_set_default_size(GTK_WINDOW(win), WIN_W, WIN_H);

    /* A REAL FLOOR, AND IT IS NOT THE DEFAULT SIZE. KDE's kcontrol asks
     * for 700x600 and permits shrinking to 450x200 - so it DOES have a
     * degradation path, and an earlier note here guessing it was simply
     * a fixed size that overflowed was wrong.
     *
     * Ours is the same shape: 800x600 requested, WIN_MIN_W/H allowed.
     * Below the floor the layout stops being usable rather than merely
     * cramped, so a window manager honouring this is doing the user a
     * favour. gdk_geometry is 1.2's route to a size hint the WM reads. */
    {
        GdkGeometry geom;

        geom.min_width  = WIN_MIN_W;
        geom.min_height = WIN_MIN_H;
        gtk_window_set_geometry_hints(GTK_WINDOW(win), NULL, &geom,
                                      GDK_HINT_MIN_SIZE);
    }
    gtk_signal_connect(GTK_OBJECT(win), "delete_event",
                       GTK_SIGNAL_FUNC(on_delete), NULL);
    gtk_signal_connect(GTK_OBJECT(win), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);

    outer = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), outer);

    gtk_box_pack_start(GTK_BOX(outer), build_menubar(), FALSE, FALSE, 0);

    /* THE BODY: SIDEBAR FIXED, RIGHT PANE EXPANDING.
     *
     * A DRAGGABLE GtkHPaned WAS TRIED AND REMOVED, 2026-09-17, because
     * the argument for it did not survive contact with the window.
     *
     * The case I made was KDE's: its kcontrol uses a QSplitter because
     * its tree holds arbitrary NESTED module names of unpredictable
     * length. Ours holds four fixed names we choose ourselves. And the
     * long-label case the fake backend exists to expose - "External
     * MIDI Passthrough" - is a CHANNEL name, which belongs in the
     * Volume panel's rows, not in the sidebar. So the splitter was
     * solving a problem this widget does not have.
     *
     * The user's test showed the cost: dragging it right gave the
     * sidebar 475px of empty space beneath four short labels while
     * squeezing the pane that holds the actual content. Resizable in
     * the direction nobody needs, at the expense of the one everybody
     * does.
     */
    body = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(body), build_sidebar(), FALSE, FALSE, 0);

    right = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), right, TRUE, TRUE, 0);

    /* THE PANE IS ITS OWN BOX so all four notebooks can live in it at
     * once, only one shown. Packing them straight into `right' would
     * work too, but then the button row's position would depend on
     * which page was visible. */
    /* THE PANE SCROLLS BELOW ITS NATURAL SIZE, and that is the answer
     * to the failure the user identified in KDE's own Control Center:
     * captured at 640x480 it is simply cut off on two edges, and the
     * whole OK/Apply/Cancel row is off the bottom with no way to reach
     * it.
     *
     * AUTOMATIC ON BOTH AXES means NO BARS AT 800x600 - the panels fit,
     * and a scrollbar that is always present would waste width on every
     * normal machine - and bars below that, so everything stays
     * REACHABLE rather than merely present.
     *
     * ONLY THE PANE SCROLLS. The menu bar, the button row and the
     * status bar stay pinned, because scrolling those away would put
     * OK out of reach by a different route and reproduce the very
     * failure this fixes. The sidebar keeps its own scroll; nesting it
     * inside this one would give two vertical bars for one list.
     *
     * gtk_scrolled_window_add_with_viewport, not container_add: a
     * GtkVBox has no native scrolling support, so it needs a
     * GtkViewport between it and the scrolled window. */
    g_pane = gtk_vbox_new(FALSE, 0);

    g_scroll = gtk_scrolled_window_new(NULL, NULL);
    /*
     * BOTH AUTOMATIC, AND A `NEVER' HERE WAS THE WRONG FIX.
     *
     * The user found a horizontal bar on pages with nothing to
     * scroll sideways to: a page too TALL raises the vertical bar,
     * that bar takes ~15px of WIDTH, the content no longer fits what
     * is left, and a horizontal bar appears for those 15px.
     *
     * SETTING HORIZONTAL TO NEVER CURED THE SYMPTOM AND BROKE
     * SOMETHING REAL - the user, 2026-09-21: "If the window is
     * resized smaller a horizontal scrollbar is needed." The whole
     * reason this scrolled window exists is that the window may go
     * below 800x600, and NEVER would clip the pane with no way to
     * reach the rest.
     *
     * THE FAULT IS THE PAGE OVERFLOWING VERTICALLY, not the policy.
     * Sound Settings' Options tab was cut off mid-sentence before the
     * horizontal bar was ever the question; shortening its text is
     * the fix, and the detail belongs in Help.
     *
     * (The sidebar above IS horizontal-NEVER, deliberately - a long
     * module name is better clipped than given a bar. Different
     * widget, different argument.)
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_scroll),
                                          g_pane);
    gtk_box_pack_start(GTK_BOX(right), g_scroll, TRUE, TRUE, 0);
    cdg_set_pane_scroll(g_scroll);  /* D60: its bars, in the CD+G trace */
    gtk_widget_show(g_scroll);
    gtk_widget_show(g_pane);

    for (i = 0; i < MOD_COUNT; i++) {
        g_mod[i].page = build_module(&g_mod[i]);
        gtk_box_pack_start(GTK_BOX(g_pane), g_mod[i].page, TRUE, TRUE, 0);
        /* NOT shown: show_module() below reveals exactly one. Every
         * child of a notebook IS shown, so gtk_widget_show on the
         * notebook is all that is needed to reveal a whole page. */
        gtk_widget_show_all(g_mod[i].page);
        gtk_widget_hide(g_mod[i].page);
    }

    /* AFTER show_all, AND THAT ORDER IS THE WHOLE POINT.
     *
     * gtk_widget_show_all() is RECURSIVE: it shows every descendant,
     * including the scale the Volume module deliberately hid. So a
     * module that wants one of two widgets visible cannot arrange it
     * during its own build - the shell's show_all undoes it a moment
     * later.
     *
     * Found on the first capture: both the horizontal AND vertical
     * faders were drawn for every channel, which also made each row
     * ~125px tall and pushed the fifth channel off the panel. The
     * SECOND capture found two more of the same thing - three unused
     * channel rows, and the "module not loaded" notice, both drawn
     * beside the working panel.
     *
     * So this is not a slider fix: it is "let the module re-apply
     * every visibility decision the shell just trampled". */
    volume_set_vertical(start_vertical);
    volume_sync_visibility();
    status_sync_visibility();
    midi_sync_visibility();
    cdg_sync_visibility();

    /* AND THE "NEEDS ROOT" NOTES - design/49 N7 builds them always and
     * shows each only when the page cannot administer, and the
     * show_all above had just shown all three regardless: a note
     * saying "needs root" beside a control that works. The same
     * functions Modify calls re-apply exactly that decision. */
    sound_privilege_changed();
    midi_privilege_changed();
    cd_privilege_changed();
    if (start_page > 0) {
        volume_set_page(start_page);
        cd_set_page(start_page);
        midi_set_page(start_page);
        sound_set_page(start_page);
    }

    g_buttons = build_buttons();
    gtk_box_pack_start(GTK_BOX(right), g_buttons, FALSE, FALSE, 0);
    gtk_widget_show(right);
    gtk_widget_show(body);

    g_statusbar = gtk_statusbar_new();
    g_status_ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(g_statusbar),
                                                "module");
    gtk_box_pack_start(GTK_BOX(outer), g_statusbar, FALSE, FALSE, 0);
    gtk_widget_show(g_statusbar);

    gtk_widget_show(outer);

    /* OPEN ON THE FIRST MODULE. A control centre showing an empty pane
     * until something is clicked looks broken.
     *
     * SELECT THE ROW, DO NOT JUST SHOW THE PAGE. Calling show_module()
     * directly leaves the sidebar highlighting whatever the list chose
     * for itself. The first capture of this window had exactly that:
     * the wrong row highlighted, Volume's page showing, and no way to
     * tell from the screen which one the program thought was current.
     *
     * gtk_clist_select_row fires "select_row", so show_module() runs
     * from the callback and the two can no longer disagree - there is
     * one path into a module, not two. */
    /* THE PREFERENCE, unless -m named a module on the command line.
     * Checked here rather than at the declaration because the backend
     * has to be up first. */
    if (!mod_from_argv)
        startup_mod = default_page();

    /* AND ONLY ONE PATH IS TAKEN - design/47 Q5. The `else' below used
     * to hang off the -T test, so without -T the start page was shown
     * TWICE: once from the row's select_row callback and once directly,
     * and every machine() scan ran twice at startup - the Render font
     * rescan included. show_module() directly is for a build with no
     * sidebar list, which is what the comment above always meant. */
    if (g_list != NULL)
        gtk_clist_select_row(GTK_CLIST(g_list), g_mod[startup_mod].row, 0);
    else
        show_module(startup_mod);

        /*
         * `-T N' OPENS A TAB, and it exists so a capture can be taken
         * of one without clicking. CLAUDE.md is explicit: "PREFER A
         * COMMAND-LINE ARGUMENT OVER DRIVING THE WIDGET... one
         * launch, one import, no input at all, and reproducible" -
         * because xdotool drives the SAME pointer the user is
         * holding, and a click meant for a notebook tab can land in
         * their emulator.
         *
         * Silently ignored when the page has no notebook, and
         * clamped by GTK itself when the number is out of range.
         */
        /* `--reset-dialog' OPENS IT AT STARTUP, for the same reason
         * -T exists: a capture of a dialog otherwise needs a click,
         * and xdotool drives the pointer the user is holding. It
         * opens the chooser, never the confirmation - nothing
         * destructive can be reached from a command line. */
        if (show_review)
            (void)review_config("/tmp/vlhe-fake-draft.conf");
        if (show_reset)
            open_reset(NULL, NULL);

        /* A PORTABLE COPY'S FIRST RUN, asked before the user starts
         * changing things - answering it afterwards would mean
         * offering to overwrite edits they had just made. */
        offer_seed();

        if (startup_tab > 0 && g_mod[startup_mod].page != NULL
            && GTK_IS_NOTEBOOK(g_mod[startup_mod].page))
            gtk_notebook_set_page(
                GTK_NOTEBOOK(g_mod[startup_mod].page), startup_tab);

    gtk_widget_show(win);

    /* AFTER show(), WHICH REALIZES THE HIERARCHY. See the function. */
    sidebar_set_icons();

    /* `--help-dialog' AND `--about-dialog': the Help window (at the
     * page and tab -m and -T chose) and the About box, opened at
     * startup for a capture - the same reason as --reset-dialog. */
    if (show_help)
        on_help(NULL, NULL);
    if (show_about)
        on_about(NULL, NULL);

    /* THE MODULES TELL US when a control changes, rather than the
     * shell polling for it - so the asterisk appears on the keystroke
     * and not a quarter-second later. */
    sound_set_dirty_cb(sidebar_mark_dirty);
    volume_set_dirty_cb(sidebar_mark_dirty);
    /* MIDI AND CD WERE MISSING - the user found both pages accepting
     * edits the sidebar never marked, and Apply reporting nothing to
     * save (2026-09-19). CD had kept the bit internally all along and
     * simply never published it; MIDI had no tracking at all. */
    midi_set_dirty_cb(sidebar_mark_dirty);
    status_set_machine_cb(machine_changed);
    /* SAVE BEFORE LOAD, AND BEFORE RENDER - the user, 2026-09-24. */
    status_set_before_cb(before_plan);
    vlhe_apply_set_drift_ask(gui_drift_ask);        /* 7h Stage 3.3 */
    vlhe_apply_set_frontend(VLHE_FRONT_GUI);        /* its messages */
    status_set_before_restart_cb(before_restart);
    midi_set_before_restart_cb(before_restart);     /* design/47 M6 */
    render_set_before_cb(before_render);
    cd_set_dirty_cb(sidebar_mark_dirty);
    render_set_dirty_cb(sidebar_mark_dirty);

    /* WHETHER A CONFIG EXISTED AT STARTUP. Nothing creates one at
     * start any more (design/51), so this is simply how we started;
     * it is what lets the sidebar say "removed while running" instead
     * of "none found". */
    g_had_config_at_start = vlhe_conf_exists();

    /* THE NO-CONFIG NOTICE, after the window is up so the sidebar is
     * realized and hiding a shown widget does not shift the layout. */
    sidebar_update_noconfig();

    /*
     * A FIRST START SAYS SO AND ASKS NOTHING - design/51, 2026-10-01.
     * The first-run dialog ("Create ... with default settings?") went
     * with Save-only: nothing need be written for the program to work,
     * so there is nothing to ask. The sidebar carries the same fact
     * for as long as it is true; this is the line for the moment.
     *
     * AND THE DRAFT READ BACK, if there was one, says where the machine
     * settings came from - it was taken before the pages were built,
     * so they already show it. */
    if (g_readback_note[0] != '\0')
        status_set(g_readback_note);
    else if (!vlhe_conf_exists())
        status_set(STR_SHELL_MSG_NO_CONFIGURATION_FILE_SETTINGS);
    /* THE OTHER OFFER - a config exists but root did not write it
     * (design/49 R1): asks, once, if we can act. */
    offer_takeover();

    /* AND AGAIN AFTERWARDS, because the takeover offer may have changed
     * what the sidebar should say. One stat(); idempotent. (The
     * first-run dialog this once guarded against is gone.) */
    sidebar_update_noconfig();

    gtk_main();

    vlhe_backend_fini();
    return 0;
}
