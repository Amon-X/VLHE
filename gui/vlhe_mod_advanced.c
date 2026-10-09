/*
 * vlhe_mod_advanced.c - Advanced Settings: what most machines never
 * change.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * THE SETUP TUI HAD THIS MENU FIRST (setup-vlhe, "Advanced") and the
 * control centre did not - design/54 G22, built 2026-10-08 at the
 * user's direction: a SIDEBAR PAGE directly after CD Settings, because
 * everything on it is machine configuration (the system file) and the
 * module pages already have the dirty marks, Apply and Modify it
 * needs. File > Preferences keeps the program's own behaviour.
 *
 * WHAT IS HERE, and where each setting goes:
 *
 *   Startup tab (2026-10-08, the user: "add a load on startup option")
 *     [Sound Settings] LoadAtBoot      what `vlhe apply --boot' - the
 *     [Midi Settings]  LoadAtBoot      init script - brings up. ONE
 *     [CD Settings]    LoadAtBoot      PLACE, chosen over a frame on
 *                               each page's Options tab: those tabs
 *                               are the fullest, and the note telling
 *                               these from Status's "Include in load"
 *                               would have been repeated three times
 *   Recovery tab
 *     [Boot] FinishLeftover     the init script, `vlhe apply --boot'
 *     [Load] BaselineDrift      every Load - MOVED HERE from File >
 *                               Preferences, the one machine setting
 *                               that dialog held (the user: "move it")
 *   Midi tab
 *     [Midi Settings] ChannelRelease   vmidid -R, at its next start
 *     [Midi Settings] AutoVoice*       vmidid -a, and `set autovoice'
 *                               through the Status page's Apply
 *                               settings - the live path these numbers
 *                               are meant for
 *   Debugging tab (2026-10-08)
 *     [Tracing] Enabled         the modules' trace switches and
 *                               vmidid -v, at the next load
 *     [Tracing] Output          kernel log, the modules' rings, both
 *     [Tracing] Capture         the run folder - the setup TUI's
 *                               Debugging menu, as a tab here rather
 *                               than a page: three controls do not
 *                               warrant one (the user)
 *
 * FOUR TABS. The plan said one page unless it needed two; the Recovery
 * and Midi frames alone add up to about 600 px at the target's 12 px
 * font, more than the pane has under 800x600, so the voice tuning has
 * a tab of its own with the channel release, which is the synth's too.
 * Startup and Debugging came later and are each a tab for the same
 * reason.
 *
 * TRACING WAS KEPT OFF THIS PAGE UNTIL 2026-10-08. The user, 2026-10-05:
 * "leave out. It will live in that modview but we need to handle it
 * properly first. Right now we take over syslogd." design/54 section 8
 * is the rework, built that day (the modules' own rings, read without
 * touching syslogd); with the kernel-log path still selectable for
 * the comparison, the tab offers both and says what each costs.
 *
 * C89, GCC 2.95.2, GTK 1.2.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_layout.h"
#include "vlhe_tip.h"
#include "vlhe_priv.h"       /* vlhe_priv_can_act, vlhe_priv_can_unlock */
#include "vlhe_self.h"       /* vlhe_self_is_trial - the Startup note */
#include "vlhe_mod_advanced.h"

#define ADV_TAB_STARTUP    0
#define ADV_TAB_RECOVERY   1
#define ADV_TAB_MIDI       2
#define ADV_TAB_DEBUGGING  3
#define ADV_TABS           4

static GtkWidget   *g_notebook;
static int          g_tab_dirty[ADV_TABS];
static int          g_loading;      /* set while writing widgets   */
static void       (*g_dirty_cb)(void);
static void       (*g_report)(const char *);
static void       (*g_page_cb)(int);

/* The tab names, clean and with the dirty mark, in tab order. Set in
 * advanced_build() - the strings are _() calls, not constants. */
static const char  *g_tab_name[ADV_TABS];
static const char  *g_tab_name_dirty[ADV_TABS];

static GtkWidget   *g_boot[3];      /* [*] LoadAtBoot, VLHE_ENABLE_* order */
static GtkWidget   *g_trace_level;  /* [Tracing] Enabled, an option menu */
static GtkWidget   *g_capture;      /* [Tracing] Capture            */
static GtkWidget   *g_finish;       /* [Boot] FinishLeftover        */
static GtkWidget   *g_bdrift[3];    /* [Load] BaselineDrift, VLHE_BDRIFT_* order */
static GtkWidget   *g_release;      /* [Midi Settings] ChannelRelease, ms */

/*
 * THE SIX AutoVoice* NUMBERS, in autovoice.h's order: drain, emergency,
 * healthy, settle, floor, tails. The first five are spins, tails is a
 * check button.
 *
 * THE RANGES ARE autovoice.c's lo_lim[]/hi_lim[], RESTATED. That table
 * is static in the daemon's file and the demonstration build does not
 * link it, so the page cannot ask; the backend's setter does
 * (autovoice_set(), the one copy), so a range that drifted here would
 * be refused there and reported rather than saved. The header's own
 * comments carry the same numbers.
 */
#define AV_SPINS 5
static GtkWidget   *g_av[AV_SPINS];
static GtkWidget   *g_tails;
static const int    g_av_lo[AV_SPINS]   = {  5,  0,  10,   20,  1 };
static const int    g_av_hi[AV_SPINS]   = { 95, 90, 100, 5000, 64 };
static const int    g_av_step[AV_SPINS] = {  1,  1,   1,   10,  1 };

/* "needs root" - one per tab, since a widget sits in one place only;
 * hidden after Modify (advanced_privilege_changed). */
static GtkWidget   *g_root_note[ADV_TABS];

static void report(const char *m)
{
    if (g_report != NULL)
        g_report(m);
}

/* ------------------------------------------------------------------ */

/* MARK A TAB DIRTY, and retitle it - the Sound page's convention. */
static void mark_dirty(int tab)
{
    GtkWidget *page;
    const char *name;

    if (g_loading)
        return;
    if (tab < 0 || tab >= ADV_TABS)
        return;
    if (g_tab_dirty[tab])
        return;

    g_tab_dirty[tab] = 1;

    name = g_tab_name_dirty[tab];
    if (g_notebook != NULL && name != NULL) {
        page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), tab);
        if (page != NULL)
            gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(g_notebook),
                                            page, name);
    }
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

static void clear_dirty(void)
{
    GtkWidget *page;
    int i;

    for (i = 0; i < ADV_TABS; i++) {
        if (!g_tab_dirty[i])
            continue;
        g_tab_dirty[i] = 0;
        if (g_notebook == NULL)
            continue;
        page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i);
        if (page != NULL && g_tab_name[i] != NULL)
            gtk_notebook_set_tab_label_text(
                GTK_NOTEBOOK(g_notebook), page, g_tab_name[i]);
    }
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

/* The controls' handler. The tab is passed as the user data. */
static void on_control_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    mark_dirty((int)(long) data);
}

/* A label, a control, and an optional unit after it - one row. */
static GtkWidget *labelled_row(GtkWidget *vbox, const char *text,
                               GtkWidget *control, const char *after)
{
    GtkWidget *hbox;
    GtkWidget *lab;

    hbox = gtk_hbox_new(FALSE, 6);

    lab = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
    vlhe_layout_column("advanced", lab);    /* measured: vlhe_layout.c */
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    /* THE ARROWS NEED A SHADOW - vlhe_mod_midi.c has the reading of
     * gtkspinbutton.c behind this. */
    if (GTK_IS_SPIN_BUTTON(control))
        gtk_spin_button_set_shadow_type(GTK_SPIN_BUTTON(control),
                                        GTK_SHADOW_IN);

    gtk_box_pack_start(GTK_BOX(hbox), control, FALSE, FALSE, 0);
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

/* A left-justified, wrapped paragraph under a frame's controls. */
static void note_row(GtkWidget *vbox, const char *text)
{
    GtkWidget *note = gtk_label_new(text);

    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);
}

/* A framed group with an 8 px inner box, returned for packing into. */
static GtkWidget *framed(GtkWidget *outer, const char *title)
{
    GtkWidget *frame, *vbox;

    frame = gtk_frame_new(title);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);
    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    return vbox;
}

/* A spin button over [lo, hi] showing `cur', marking `tab' on change. */
static GtkWidget *spin_new(int cur, int lo, int hi, int step, int digits,
                           int tab)
{
    GtkObject *adj;
    GtkWidget *spin;

    if (cur < lo)
        cur = lo;
    if (cur > hi)
        cur = hi;
    adj = gtk_adjustment_new((gfloat) cur, (gfloat) lo, (gfloat) hi,
                             (gfloat) step, (gfloat) (step * 10), 0.0);
    spin = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 0.0, 0);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(spin), FALSE);
    vlhe_layout_digits(spin, digits);
    gtk_signal_connect(adj, "value_changed",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) tab);
    return spin;
}

/*
 * CAN THIS SESSION CHANGE A MACHINE SETTING? vlhe_mod_sound.c has why
 * both are asked (design/49 N7: access() tests the real uid).
 */
static int
page_admin(void)
{
    return vlhe_can_administer() || vlhe_priv_can_act();
}

/* The "needs root" paragraph - built always, shown only when true. */
static void root_note(GtkWidget *outer, int tab)
{
    GtkWidget *note = gtk_label_new(vlhe_priv_can_unlock()
        ? STR_SND_TEXT_NEEDS_ROOT_MODIFY
        : STR_SND_TEXT_NEEDS_ROOT_RUN_AS_ROOT);

    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 8);
    g_root_note[tab] = note;
    if (!page_admin())
        gtk_widget_show(note);
}

/* Which item an option menu shows, or -1 - vlhe_mod_sound.c's. */
static int menu_index(GtkWidget *w)
{
    GtkOptionMenu *om = GTK_OPTION_MENU(w);

    if (om->menu == NULL || om->menu_item == NULL)
        return -1;
    return g_list_index(GTK_MENU_SHELL(om->menu)->children, om->menu_item);
}

/* A tipped check button showing `on', marking `tab' on change. */
static GtkWidget *check_new(GtkWidget *vbox, const char *text,
                            const char *tip, int on, int tab)
{
    GtkWidget *w = vlhe_tipped(gtk_check_button_new_with_label(text), tip);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), on ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(w), "toggled",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) tab);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);
    return w;
}

/* ------------------------------------------------------------------ */
/* Startup                                                            */
/* ------------------------------------------------------------------ */

static GtkWidget *build_startup(void)
{
    GtkWidget *outer, *vbox;
    static const char *labels[3], *tips[3];
    int i;

    labels[0] = STR_ADV_CHECK_BOOT_SOUND;
    labels[1] = STR_ADV_CHECK_BOOT_MIDI;
    labels[2] = STR_ADV_CHECK_BOOT_CD;
    tips[0] = STR_ADV_CHECK_BOOT_SOUND_TIP;
    tips[1] = STR_ADV_CHECK_BOOT_MIDI_TIP;
    tips[2] = STR_ADV_CHECK_BOOT_CD_TIP;

    outer = gtk_vbox_new(FALSE, 0);

    /*
     * THE THREE LoadAtBoot KEYS - what the init script's `vlhe apply
     * --boot' puts in its plan (vlhe_apply.c in_plan()). NOT the
     * Status page's Include boxes, which are the Load button's: they
     * were one key until 2026-10-02 ("Status shouldn't touch boot
     * behaviour"), and until this tab the boot key had no control in
     * the GUI at all. The note says which is which, once.
     */
    vbox = framed(outer, STR_ADV_FRAME_AT_BOOT);
    for (i = 0; i < 3; i++)
        g_boot[i] = check_new(vbox, labels[i], tips[i],
                              vlhe_component_at_boot(i), ADV_TAB_STARTUP);
    note_row(vbox, STR_ADV_LABEL_BOOT_NOTE);

    /* "AT BOOT" MEANS NOTHING TO A PORTABLE COPY - the user, 2026-09-22
     * (vlhe_backend.h). Nothing of ours is installed to run at boot
     * there, so the boxes are left live (the file they write is still
     * the machine's) and the tab says so in its own words. The build
     * decides what it is; nothing at run time does (vlhe_self.h). */
    if (vlhe_self_is_trial())
        note_row(vbox, STR_ADV_LABEL_BOOT_PORTABLE_NOTE);

    root_note(outer, ADV_TAB_STARTUP);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */
/* Debugging                                                          */
/* ------------------------------------------------------------------ */

static GtkWidget *build_debugging(void)
{
    GtkWidget *outer, *vbox;
    int i;

    outer = gtk_vbox_new(FALSE, 0);

    /*
     * [Tracing] - the setup TUI's Debugging menu (design/09 "TRACING
     * IS DEBUGGING": off by default, one setting, reachable at run
     * time because a user's module cannot be rebuilt for them). The
     * level is a menu, as on the TUI. Where the trace goes is not a
     * choice: the modules' own rings, read with `vlhe trace' - the
     * kernel-log option lived here for one day (2026-10-08) and went
     * with the path (modules/common/vtrace.h).
     */
    vbox = framed(outer, STR_ADV_FRAME_TRACING);

    g_trace_level = vlhe_tipped(gtk_option_menu_new(), STR_ADV_LABEL_TRACING_TIP);
    {
        GtkWidget *menu = gtk_menu_new();
        static const char *names[3];

        names[0] = STR_ADV_MENU_TRACING_OFF;
        names[1] = STR_ADV_MENU_TRACING_EVENTS;
        names[2] = STR_ADV_MENU_TRACING_EVERYTHING;
        for (i = 0; i < 3; i++) {
            GtkWidget *item = gtk_menu_item_new_with_label(names[i]);

            gtk_signal_connect(GTK_OBJECT(item), "activate",
                               GTK_SIGNAL_FUNC(on_control_changed),
                               (gpointer)(long) ADV_TAB_DEBUGGING);
            gtk_menu_append(GTK_MENU(menu), item);
            gtk_widget_show(item);
        }
        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_trace_level), menu);
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_trace_level),
                                    vlhe_tracing());
    }
    labelled_row(vbox, STR_ADV_LABEL_TRACING, g_trace_level, NULL);

    g_capture = check_new(vbox, STR_ADV_CHECK_TRACE_CAPTURE,
                          STR_ADV_CHECK_TRACE_CAPTURE_TIP,
                          vlhe_trace_capture(), ADV_TAB_DEBUGGING);

    note_row(vbox, STR_ADV_LABEL_TRACING_NOTE);

    root_note(outer, ADV_TAB_DEBUGGING);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */
/* Recovery                                                           */
/* ------------------------------------------------------------------ */

static GtkWidget *build_recovery(void)
{
    GtkWidget *outer, *vbox, *lab;

    outer = gtk_vbox_new(FALSE, 0);

    /* ---- after a power loss or a crash ---------------------------- */

    vbox = framed(outer, STR_ADV_FRAME_AFTER_CRASH);

    /*
     * [Boot] FinishLeftover - design/54 7h decision 2. A load that was
     * never unloaded leaves its changes on record (the session file);
     * ticked, the boot puts back what VLHE itself made and then loads.
     * Installed copies only - a portable folder does not run at boot -
     * but the setting is in the system file either way.
     */
    g_finish = vlhe_tipped(gtk_check_button_new_with_label(
        STR_ADV_CHECK_FINISH_LEFTOVER), STR_ADV_CHECK_FINISH_LEFTOVER_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_finish),
                                 vlhe_boot_finish_leftover() ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_finish), "toggled",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) ADV_TAB_RECOVERY);
    gtk_box_pack_start(GTK_BOX(vbox), g_finish, FALSE, FALSE, 0);
    gtk_widget_show(g_finish);

    note_row(vbox, STR_ADV_LABEL_FINISH_NOTE);

    /* ---- System Baseline - [Load] BaselineDrift ------------------- */

    /*
     * MOVED FROM FILE > PREFERENCES, 2026-10-08. It sat there from
     * 2026-10-04 as the one machine setting in a dialog about the
     * program's own behaviour, greyed unless root. Here it is LIVE FOR
     * EVERYONE like every other machine control (design/49, the
     * decision of 2026-10-01): a locked user may edit it, saving needs
     * root, and the note at the foot of the tab says where that leads.
     */
    vbox = framed(outer, STR_ADV_FRAME_SYSTEM_BASELINE);

    lab = gtk_label_new(STR_ADV_LABEL_BASELINE_DRIFT);
    gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
    vlhe_layout_wrap(lab);
    gtk_box_pack_start(GTK_BOX(vbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);
    {
        static const char *const words[3] = {
            STR_ADV_RADIO_DRIFT_ASK,
            STR_ADV_RADIO_DRIFT_WARN,
            STR_ADV_RADIO_DRIFT_REFUSE
        };
        static const char *const tips[3] = {
            STR_ADV_RADIO_DRIFT_ASK_TIP,
            STR_ADV_RADIO_DRIFT_WARN_TIP,
            STR_ADV_RADIO_DRIFT_REFUSE_TIP
        };
        GSList *grp = NULL;
        int     cur = vlhe_baseline_drift(), m;

        for (m = 0; m < 3; m++) {
            g_bdrift[m] = vlhe_tipped(
                gtk_radio_button_new_with_label(grp, words[m]), tips[m]);
            grp = gtk_radio_button_group(GTK_RADIO_BUTTON(g_bdrift[m]));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_bdrift[m]),
                                         m == cur ? TRUE : FALSE);
            gtk_signal_connect(GTK_OBJECT(g_bdrift[m]), "toggled",
                               GTK_SIGNAL_FUNC(on_control_changed),
                               (gpointer)(long) ADV_TAB_RECOVERY);
            gtk_box_pack_start(GTK_BOX(vbox), g_bdrift[m], FALSE, FALSE, 0);
            gtk_widget_show(g_bdrift[m]);
        }
    }

    note_row(vbox, STR_ADV_LABEL_BASELINE_NOTE);

    root_note(outer, ADV_TAB_RECOVERY);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */
/* Midi                                                               */
/* ------------------------------------------------------------------ */

static GtkWidget *build_midi(void)
{
    GtkWidget *outer, *vbox;
    int av[VLHE_AUTOVOICE_N], i;

    /* A set the check refuses comes back as the DEFAULTS - which is
     * what the daemon runs in that case, so it is what to show. */
    vlhe_autovoice_settings(av, NULL, NULL);

    outer = gtk_vbox_new(FALSE, 0);

    /* ---- the synth's channel -------------------------------------- */

    vbox = framed(outer, STR_ADV_FRAME_SYNTH_CHANNEL);

    /*
     * [Midi Settings] ChannelRelease - vmidid -R. A start-up flag with
     * no live verb (design/54 G22), so the note says it waits for the
     * synth's next start. 0..600000, as the backend clamps it; the
     * step is 100 ms, the page 1 s.
     */
    g_release = vlhe_tipped(spin_new(vlhe_midi_release_ms(), 0, 600000,
                                     100, 6, ADV_TAB_MIDI),
                            STR_ADV_LABEL_CHANNEL_RELEASE_TIP);
    labelled_row(vbox, STR_ADV_LABEL_CHANNEL_RELEASE, g_release,
                 STR_ADV_TEXT_MS);
    note_row(vbox, STR_ADV_LABEL_RELEASE_NOTE);

    /* ---- automatic voice tuning ----------------------------------- */

    /*
     * THE SIX AutoVoice* NUMBERS - design/21 section 16, config-only
     * from 2026-10-02 until this page. They tune "Lower voices
     * automatically" on the Midi Settings page; the three fills must
     * keep Emergency < Drain < Healthy or the daemon refuses the whole
     * set, which collect() checks before anything is saved.
     */
    vbox = framed(outer, STR_ADV_FRAME_AUTO_VOICES);
    {
        static const char *labels[AV_SPINS], *tips[AV_SPINS],
                          *units[AV_SPINS];

        labels[0] = STR_ADV_LABEL_DRAIN;
        labels[1] = STR_ADV_LABEL_EMERGENCY;
        labels[2] = STR_ADV_LABEL_HEALTHY;
        labels[3] = STR_ADV_LABEL_SETTLE;
        labels[4] = STR_ADV_LABEL_FLOOR;
        tips[0] = STR_ADV_LABEL_DRAIN_TIP;
        tips[1] = STR_ADV_LABEL_EMERGENCY_TIP;
        tips[2] = STR_ADV_LABEL_HEALTHY_TIP;
        tips[3] = STR_ADV_LABEL_SETTLE_TIP;
        tips[4] = STR_ADV_LABEL_FLOOR_TIP;
        units[0] = STR_ADV_TEXT_PCT_FULL;
        units[1] = STR_ADV_TEXT_PCT_FULL;
        units[2] = STR_ADV_TEXT_PCT_FULL;
        units[3] = STR_ADV_TEXT_MS;
        units[4] = NULL;

        for (i = 0; i < AV_SPINS; i++) {
            g_av[i] = vlhe_tipped(spin_new(av[i], g_av_lo[i], g_av_hi[i],
                                           g_av_step[i], 4, ADV_TAB_MIDI),
                                  tips[i]);
            labelled_row(vbox, labels[i], g_av[i], units[i]);
        }
    }

    g_tails = vlhe_tipped(gtk_check_button_new_with_label(
        STR_ADV_CHECK_TAILS), STR_ADV_CHECK_TAILS_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_tails),
                                 av[5] ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_tails), "toggled",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) ADV_TAB_MIDI);
    gtk_box_pack_start(GTK_BOX(vbox), g_tails, FALSE, FALSE, 0);
    gtk_widget_show(g_tails);

    note_row(vbox, STR_ADV_LABEL_AUTO_NOTE);

    root_note(outer, ADV_TAB_MIDI);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */

static void on_switch_page(GtkNotebook *nb, GtkNotebookPage *pg,
                           guint page, gpointer data)
{
    (void)nb; (void)pg; (void)page; (void)data;
    if (g_page_cb != NULL)
        g_page_cb(1);       /* every tab is a form */
}

/* The six numbers as the widgets hold them, in autovoice.h's order. */
static void read_av(int av[VLHE_AUTOVOICE_N])
{
    int i;

    for (i = 0; i < AV_SPINS; i++)
        av[i] = g_av[i] != NULL
            ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_av[i]))
            : g_av_lo[i];
    av[5] = (g_tails != NULL && GTK_TOGGLE_BUTTON(g_tails)->active) ? 1 : 0;
}

/*
 * READ THE WIDGETS INTO THE BACKEND. Apply and OK call this; it
 * writes no file - vlhe_commit() does that once, afterwards.
 *
 * CHECKED BEFORE ANYTHING IS SET, so a refusal leaves the backend as
 * it was: the voice numbers are the only control that can be wrong
 * (the spins bound each one, but not their order), and they are
 * tested first.
 */
int advanced_collect(void)
{
    int av[VLHE_AUTOVOICE_N], m, rc = 0;
    const char *why = NULL;

    read_av(av);
    /* THE ORDER THE DAEMON INSISTS ON, said in the page's own words
     * before the setter says it in the daemon's. */
    if (!(av[1] < av[0] && av[0] < av[2])) {
        report(STR_ADV_MSG_ORDER);
        return -1;
    }

    if (vlhe_set_autovoice_settings(av, &why) != 0) {
        char msg[200];

        sprintf(msg, FMT_ADV_MSG_REFUSED, why != NULL ? why : "?");
        report(msg);
        return -1;
    }

    /* THE STARTUP TAB - three switches, nothing to refuse. */
    for (m = 0; m < 3; m++)
        if (g_boot[m] != NULL
            && vlhe_set_component_at_boot(m,
                   GTK_TOGGLE_BUTTON(g_boot[m])->active ? 1 : 0) != 0)
            rc = -1;

    if (g_finish != NULL
        && vlhe_set_boot_finish_leftover(
               GTK_TOGGLE_BUTTON(g_finish)->active ? 1 : 0) != 0)
        rc = -1;

    for (m = 0; m < 3; m++)
        if (g_bdrift[m] != NULL && GTK_TOGGLE_BUTTON(g_bdrift[m])->active
            && vlhe_set_baseline_drift(m) != 0)
            rc = -1;

    /* THE DEBUGGING TAB. The menu offers the getter's three values,
     * so a refusal here is a build mismatch, not a user's doing;
     * counted, not worded. */
    if (g_trace_level != NULL) {
        int idx = menu_index(g_trace_level);

        if (idx < 0 || vlhe_set_tracing(idx) != 0)
            rc = -1;
    }
    if (g_capture != NULL
        && vlhe_set_trace_capture(
               GTK_TOGGLE_BUTTON(g_capture)->active ? 1 : 0) != 0)
        rc = -1;

    if (g_release != NULL
        && vlhe_set_midi_release_ms(gtk_spin_button_get_value_as_int(
               GTK_SPIN_BUTTON(g_release))) != 0) {
        report(STR_ADV_MSG_RELEASE_RANGE);
        rc = -1;
    }

    if (rc == 0)
        clear_dirty();
    return rc;
}

/*
 * PUT THE WIDGETS BACK TO WHAT THE BACKEND SAYS - Cancel's half.
 * Called AFTER the backend has re-read its files.
 */
void advanced_reload(void)
{
    int av[VLHE_AUTOVOICE_N], i, m;

    g_loading = 1;

    for (m = 0; m < 3; m++)
        if (g_boot[m] != NULL)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_boot[m]),
                                         vlhe_component_at_boot(m) ? TRUE : FALSE);

    if (g_trace_level != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_trace_level),
                                    vlhe_tracing());
    if (g_capture != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_capture),
                                     vlhe_trace_capture() ? TRUE : FALSE);

    if (g_finish != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_finish),
                                     vlhe_boot_finish_leftover() ? TRUE : FALSE);
    m = vlhe_baseline_drift();
    if (m >= 0 && m < 3 && g_bdrift[m] != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_bdrift[m]), TRUE);
    if (g_release != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_release),
                                  (gfloat) vlhe_midi_release_ms());

    vlhe_autovoice_settings(av, NULL, NULL);
    for (i = 0; i < AV_SPINS; i++)
        if (g_av[i] != NULL)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_av[i]),
                                      (gfloat) av[i]);
    if (g_tails != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_tails),
                                     av[5] ? TRUE : FALSE);

    g_loading = 0;
    clear_dirty();
}

int advanced_page_wants_buttons(void)
{
    return 1;
}

void advanced_set_page(int page)
{
    if (g_notebook != NULL)
        gtk_notebook_set_page(GTK_NOTEBOOK(g_notebook), page);
}

GtkWidget *advanced_build(void (*report_fn)(const char *),
                          void (*page_fn)(int))
{
    GtkWidget *nb;
    GtkWidget *tab;

    g_report = report_fn;
    g_page_cb = page_fn;

    /* THE BUILD SETS EVERY CONTROL FROM THE BACKEND, and each of
     * those fires the same signal a user click does. */
    g_loading = 1;

    g_tab_name[ADV_TAB_STARTUP]         = STR_ADV_TAB_STARTUP;
    g_tab_name_dirty[ADV_TAB_STARTUP]   = STR_ADV_TAB_STARTUP_DIRTY;
    g_tab_name[ADV_TAB_RECOVERY]        = STR_ADV_TAB_RECOVERY;
    g_tab_name_dirty[ADV_TAB_RECOVERY]  = STR_ADV_TAB_RECOVERY_DIRTY;
    g_tab_name[ADV_TAB_MIDI]            = STR_ADV_TAB_MIDI;
    g_tab_name_dirty[ADV_TAB_MIDI]      = STR_ADV_TAB_MIDI_DIRTY;
    g_tab_name[ADV_TAB_DEBUGGING]       = STR_ADV_TAB_DEBUGGING;
    g_tab_name_dirty[ADV_TAB_DEBUGGING] = STR_ADV_TAB_DEBUGGING_DIRTY;

    nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);

    tab = gtk_label_new(g_tab_name[ADV_TAB_STARTUP]);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_startup(), tab);

    tab = gtk_label_new(g_tab_name[ADV_TAB_RECOVERY]);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_recovery(), tab);

    tab = gtk_label_new(g_tab_name[ADV_TAB_MIDI]);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_midi(), tab);

    tab = gtk_label_new(g_tab_name[ADV_TAB_DEBUGGING]);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_debugging(), tab);

    gtk_signal_connect(GTK_OBJECT(nb), "switch_page",
                       GTK_SIGNAL_FUNC(on_switch_page), NULL);
    g_notebook = nb;

    g_loading = 0;

    return nb;
}

/* Which tabs hold unsaved edits, as a bitmask - 0 when clean. */
int advanced_dirty(void)
{
    return (g_tab_dirty[ADV_TAB_STARTUP]   ? 1 : 0) |
           (g_tab_dirty[ADV_TAB_RECOVERY]  ? 2 : 0) |
           (g_tab_dirty[ADV_TAB_MIDI]      ? 4 : 0) |
           (g_tab_dirty[ADV_TAB_DEBUGGING] ? 8 : 0);
}

void advanced_set_dirty_cb(void (*cb)(void))
{
    g_dirty_cb = cb;
}

/*
 * MODIFY WAS PRESSED - design/49 N7. The notes only; no value is
 * reloaded, for the reason vlhe_mod_sound.c gives.
 */
void
advanced_privilege_changed(void)
{
    int admin = page_admin(), i;

    for (i = 0; i < ADV_TABS; i++) {
        if (g_root_note[i] == NULL)
            continue;
        if (admin)
            gtk_widget_hide(g_root_note[i]);
        else
            gtk_widget_show(g_root_note[i]);
    }
}
