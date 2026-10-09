/*
 * vlhe_mod_sound.c - Sound Settings: the card, the pump, the slot.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * THE SMALLEST OF THE FOUR, and the mockup's version of this tab
 * describes none of what is here. It offers "emulation mode"
 * (OSS-Free / Sound Blaster 16 / AdLib OPL3), a sample rate, a bit
 * depth and a stereo checkbox. **vsound has none of those**: it
 * negotiates format with the real card and mixes to whatever that
 * card accepted, so there is nothing to choose.
 *
 * WHAT IS ACTUALLY HERE, read from the source rather than the mockup:
 *
 *   vsound.o        EIGHT module parameters, ONE user-facing -
 *                   `vsound_midi', the reserved synth channel. The
 *                   rest are trace knobs or tuning with an auto
 *                   default, which design/09 keeps out of the GUI.
 *   vsoundd         -d (the real card), -R (release on idle), and a
 *                   debug capture that is not a setting.
 *
 * AND THE CHANNEL COUNT IS NOT SETTABLE. VSOUND_MAX_CHAN is 4, a
 * compile-time constant in vsound_chan.h. It is SHOWN here because a
 * user wondering why a fifth program got no sound needs the number,
 * but it is a fact rather than a control - and it is a correction to
 * design/vsound.conf.example, which lists "Channels = 4" as though it
 * were a module parameter.
 *
 * C89, GCC 2.95.2, GTK 1.2.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>   /* lstat - which dsp nodes exist */
#include <unistd.h>

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_layout.h"
#include "vlhe_tip.h"
#include "vlhe_priv.h"       /* vlhe_priv_can_act */
#include "vlhe_mod_sound.h"

#define PAGE_DEVICE     0
#define PAGE_OPTIONS    1

static int          g_page;
static GtkWidget   *g_notebook;

/*
 * UNSAVED EDITS, PER TAB - what puts the asterisk on the tab and on
 * the sidebar row.
 *
 * PER WIDGET RATHER THAN ASKED OF THE BACKEND, and that is forced
 * rather than chosen: a page's values do not reach the backend until
 * collect() runs, which is Apply. So while the user is typing the
 * backend knows nothing has changed, and a marker driven from
 * vlhe_dirty() would appear only after the save it is meant to
 * prompt. These flags are set by the controls themselves.
 */
#define SOUND_TAB_DEVICE   0
#define SOUND_TAB_OPTIONS  1
#define SOUND_TABS         2

static int          g_tab_dirty[SOUND_TABS];
static int          g_loading;      /* set while writing widgets   */
static void       (*g_dirty_cb)(void);
static void       (*g_report)(const char *);
static void       (*g_page_cb)(int);

static GtkWidget   *g_card;
/* THE NODE BEHIND EACH "Play through" ENTRY, by menu position - the
 * option menu reports a history index, and this turns it back into
 * the "/dev/dspN" the config stores. */
/* ENTRY 0 IS "(detect)" - an empty node, which the backend reads as
 * "pick the card yourself" (dsp_node_ok() accepts "", and the plan's
 * probe_card() sweeps). design/47 D1, 2026-09-30: without it a saved
 * pick of "" sat on whatever card was listed first, and any Apply here
 * or OK anywhere then WROTE that card - often /dev/dsp, vsound's own
 * once loaded. The user's config silently stopped meaning detect. So
 * the array has one more slot than VLHE_MAX_CARDS and the cards start
 * at 1. */
static char         g_card_node[VLHE_MAX_CARDS + 1][VLHE_PATH_MAX];
static int          g_ncard;
static GtkWidget   *g_proguse;      /* the node programs open       */
/* The path behind each "Programs use" entry, by menu position - the
 * same arrangement as g_card_node above and for the same reason. */
static char         g_use_node[VLHE_DSP_PROBE_MAX][VLHE_PATH_MAX];
static int          g_nuse;
static GtkWidget   *g_release;
static GtkWidget   *g_midi_slot;
static GtkWidget   *g_root_note;   /* "needs root", hidden after Modify */
static GtkWidget   *g_slot_status;
static GtkWidget   *g_limiter;      /* vsound_limit, 0-2            */
static GtkWidget   *g_atten;        /* vsound_atten, by g_atten_pct  */

/* THE ATTENUATION MENU'S VALUES, in percent. Unchanged, about -3 dB
 * and about -6 dB - plus a fourth entry when the config holds some
 * other value, so a hand-edited 80 is shown and kept rather than
 * silently becoming 100 at the next save. */
static int          g_atten_pct[4];
static int          g_natten;

static void report(const char *m)
{
    if (g_report != NULL)
        g_report(m);
}

/* ------------------------------------------------------------------ */

/*
 * MARK A TAB DIRTY, and retitle it. The asterisk is an editor's
 * convention, needs no artwork, and survives any font size - where a
 * changed icon would need a variant of every 16x16 XPM.
 */
static void mark_dirty(int tab)
{
    GtkWidget *page;
    const char *name;

    /* NOT WHILE WE ARE WRITING THE WIDGETS OURSELVES. reload() and
     * the initial build both set controls, and every one of those
     * fires the same "toggled" signal a user click does. Without this
     * guard a page would be dirty before it had been looked at. */
    if (g_loading)
        return;
    if (tab < 0 || tab >= SOUND_TABS)
        return;
    if (g_tab_dirty[tab])
        return;                 /* already marked */

    g_tab_dirty[tab] = 1;

    name = (tab == SOUND_TAB_DEVICE) ? STR_SND_TAB_DEVICE_DIRTY : STR_SND_TAB_OPTIONS_DIRTY;
    if (g_notebook != NULL) {
        page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), tab);
        if (page != NULL)
            gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(g_notebook),
                                            page, name);
    }

    /* Tell the shell, so the sidebar row gets its asterisk too. */
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

/* Clear every tab - after Apply, OK or Cancel. */
static void clear_dirty(void)
{
    GtkWidget *page;
    int i;

    for (i = 0; i < SOUND_TABS; i++) {
        if (!g_tab_dirty[i])
            continue;
        g_tab_dirty[i] = 0;
        if (g_notebook == NULL)
            continue;
        page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i);
        if (page != NULL)
            gtk_notebook_set_tab_label_text(
                GTK_NOTEBOOK(g_notebook), page,
                (i == SOUND_TAB_DEVICE) ? STR_SND_TAB_DEVICE : STR_SND_TAB_OPTIONS);
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

/* (Re)build the attenuation menu so it holds `cur', and select it. */
static void atten_menu(int cur)
{
    static const int std[3] = { 100, 71, 50 };
    GtkWidget *menu = gtk_menu_new();
    int i, sel = 0;

    g_natten = 0;
    for (i = 0; i < 3; i++)
        g_atten_pct[g_natten++] = std[i];
    for (i = 0; i < g_natten; i++)
        if (g_atten_pct[i] == cur)
            break;
    if (i == g_natten)
        g_atten_pct[g_natten++] = cur;

    for (i = 0; i < g_natten; i++) {
        char lab[64];
        GtkWidget *item;

        if (g_atten_pct[i] == 100)
            strcpy(lab, STR_SND_MENU_ATTEN_NONE);
        else
            sprintf(lab, STR_SND_MENU_ATTEN_PCT, g_atten_pct[i]);
        item = gtk_menu_item_new_with_label(lab);
        gtk_signal_connect(GTK_OBJECT(item), "activate",
                           GTK_SIGNAL_FUNC(on_control_changed),
                           (gpointer)(long) SOUND_TAB_OPTIONS);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        if (g_atten_pct[i] == cur)
            sel = i;
    }
    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_atten), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_atten), sel);
}

/* Which entry an option menu shows, or -1. */
static int menu_index(GtkWidget *w)
{
    GtkOptionMenu *om = GTK_OPTION_MENU(w);

    if (om->menu == NULL || om->menu_item == NULL)
        return -1;
    return g_list_index(GTK_MENU_SHELL(om->menu)->children, om->menu_item);
}

static GtkWidget *labelled_row(GtkWidget *vbox, const char *text,
                               GtkWidget *control, const char *after,
                               int wide)
{
    GtkWidget *hbox;
    GtkWidget *lab;

    hbox = gtk_hbox_new(FALSE, 6);

    lab = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
    vlhe_layout_column("sound", lab);   /* measured: vlhe_layout.c */
    gtk_box_pack_start(GTK_BOX(hbox), lab, FALSE, FALSE, 0);
    gtk_widget_show(lab);

    /* PER ROW, for the reason the MIDI module records: expansion set
     * on every row at once stretches a menu of short strings across
     * the pane, and a stretched GtkOptionMenu's click target does not
     * match its drawn button. */
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

/* ------------------------------------------------------------------ */
/* Device                                                             */
/* ------------------------------------------------------------------ */

static GtkWidget *build_device(void)
{
    struct vlhe_sound sn;
    struct vlhe_card cards[VLHE_MAX_CARDS];
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *menu;
    GtkWidget *item;
    GtkWidget *note;
    int n, i, sel = 0;

    vlhe_sound(&sn);
    n = vlhe_cards(cards, VLHE_MAX_CARDS);

    outer = gtk_vbox_new(FALSE, 0);

    frame = gtk_frame_new(STR_SND_FRAME_SOUND_CARD);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * WHICH CARD THE PUMP PLAYS TO. On a one-card machine this is a
     * formality; on two it is the setting that decides where sound
     * comes out.
     *
     * SAVED, AND ACTING AT THE NEXT LOAD - corrected 2026-10-03 (this
     * said "display-only today", which stopped being true 2026-09-24).
     * The choice is read by sound_collect() and saved; the plan passes
     * it to the pump as `vsoundd -d DEV' when it starts - see the
     * comment there.
     *
     * WHAT IS STILL MISSING IS THE LIVE HALF: `-d' is a start-up flag,
     * and a running vsoundd has no listener to close one device and
     * open another. design/32 section 12 has the two honest completions
     * (a reopen command, or restarting the pump and saying so first);
     * design/54 G03 tracks it.
     */
    g_card = gtk_option_menu_new();
    menu = gtk_menu_new();

    /* "(detect)" FIRST - see g_card_node. Selected when the saved pick
     * is empty, and ALSO when it names a node not in the list below:
     * the alternative is entry 1 showing as chosen, and the next
     * collect writing it. */
    item = gtk_menu_item_new_with_label(STR_SND_MENU_DETECT);
    gtk_signal_connect(GTK_OBJECT(item), "activate",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) SOUND_TAB_DEVICE);
    gtk_menu_append(GTK_MENU(menu), item);
    gtk_widget_show(item);
    g_card_node[0][0] = '\0';
    sel = 0;

    for (i = 0; i < n; i++) {
        char lab[VLHE_PATH_MAX + 80];

        /* OUR OWN DEVICE IS LISTED AND DISABLED. Choosing it would
         * point the pump at itself - reading from and writing to the
         * same device - and the loop is not obvious from the name
         * "/dev/dsp". Shown rather than hidden so the absence is
         * explained rather than mysterious. */
        /* THE NAME IS USUALLY EMPTY and that is expected, not a
         * failure: /proc/sound lists the legacy path only, so a PCI
         * card has no name to give. A bare " - " with nothing after
         * it reads as a bug, so the separator appears only when there
         * is something to separate. */
        if (cards[i].is_ours && cards[i].name[0] != '\0')
            sprintf(lab, FMT_SND_VSOUND,
                    cards[i].node, cards[i].name);
        else if (cards[i].is_ours)
            sprintf(lab, FMT_SND_VSOUND_2, cards[i].node);
        else if (cards[i].name[0] != '\0')
            sprintf(lab, "%.40s - %.40s", cards[i].node, cards[i].name);
        else
            sprintf(lab, "%.40s", cards[i].node);
        /* A CARD SOMEONE HOLDS IS STILL A CARD - design/47 B4: after a
         * Load the pump holds the real one, and it used to drop out
         * of this menu. Listed, and said. */
        if (cards[i].busy && !cards[i].is_ours
            && strlen(lab) + 10 < sizeof lab)
            strcat(lab, FMT_SND_USE);

        item = gtk_menu_item_new_with_label(lab);
        if (cards[i].is_ours)
            gtk_widget_set_sensitive(item, FALSE);
        /* A CHOICE MARKS THE PAGE DIRTY AND IS SAVED BY APPLY - it
         * does not act at once. The user changed it, pressed Apply
         * and was told there was nothing to save (2026-09-24); the
         * menu had no handler at all. */
        gtk_signal_connect(GTK_OBJECT(item), "activate",
                           GTK_SIGNAL_FUNC(on_control_changed),
                           (gpointer)(long) SOUND_TAB_DEVICE);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
        strncpy(g_card_node[i + 1], cards[i].node, VLHE_PATH_MAX - 1);
        g_card_node[i + 1][VLHE_PATH_MAX - 1] = '\0';

        if (sn.card[0] != '\0' && strcmp(cards[i].node, sn.card) == 0)
            sel = i + 1;
    }
    g_ncard = n + 1;            /* "(detect)" plus the cards */

    /*
     * AN EMPTY MENU SAYS SO, rather than being empty.
     *
     * vlhe_cards() probes /dev/dsp through /dev/dsp3 and returns only
     * the nodes that OPEN, so a machine with no sound card - or one
     * whose card is held by something else - gets zero entries. A
     * dropdown with nothing in it reads as a broken widget; one that
     * says "no sound card found" reads as an answer.
     */
    if (n == 0) {
        item = gtk_menu_item_new_with_label(STR_SND_MENU_NO_SOUND_CARD_FOUND);
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
    }

    gtk_option_menu_set_menu(GTK_OPTION_MENU(g_card), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(g_card), sel);
    labelled_row(vbox, STR_SND_LABEL_PLAY_THROUGH, g_card, NULL, 1);

    /*
     * THE CARD'S MODULE - AND THIS ONE IS LIVE, unlike the menu
     * above it. It is a plain config string that `vlhe apply' reads;
     * no daemon has to be retargeted for it to mean something, so
     * there is nothing to wait for.
     *
     * WHY IT EXISTS. vsound has to register at audio device 0 or it
     * does not get /dev/dsp, and a legacy driver already resident at
     * boot holds that slot. Applying a config therefore has to move
     * that driver aside and put it back - which needs its MODULE
     * NAME, a thing nothing on the machine reliably tells us.
     *
     * A COMBO, NOT A MENU, AND THAT IS THE USER'S POINT. 2026-09-21:
     * "a user may be using a sound module that they built themselves
     * that we wont know about". A fixed list only ever finds drivers
     * we thought to name - vlhe_status.h:62 makes the same argument
     * about detection and answers it by asking soundcore who holds
     * it, which is where the entries below come from. But detection
     * cannot see a module that is not loaded YET, so the field stays
     * typeable: the list is a convenience, not the choice.
     *
     * EMPTY MEANS DETECT, NOT "NO CARD" - the placeholder text says
     * so, because an empty box otherwise reads as a missing setting.
     */
    {
        GtkWidget *umenu;
        int k, usel = 0;

        /*
         * THE NODE PROGRAMS OPEN, AND IT REPLACED "Card module" -
         * 2026-09-25. That control named the driver to rmmod so
         * vsound could take /dev/dsp; nothing displaces the card any
         * more (design/38 section 9), so the field had no reader
         * left.
         *
         * AN OPTION MENU, MATCHING "Play through" ABOVE - and the
         * first version of this was an editable combo, on the
         * reasoning that the node might not exist when the page is
         * drawn. THE USER CORRECTED IT: "Why would a user currently
         * point programs to a node that does not exist yet?" They
         * would not. Software is configured against a device that
         * WORKS, which is the premise of the setting - the user
         * already has /dev/dsp2 in their player because that is
         * where their card is.
         *
         * So the set is closed and present: /dev/dsp and /dev/dsp1
         * come from MAKEDEV on every machine, and a higher one
         * exists precisely when something created it. A list also
         * prevents a typo in a path, which here would mean a silent
         * no-op or a redirect to nothing.
         */
        g_proguse = gtk_option_menu_new();
        umenu = gtk_menu_new();

        for (k = 0; k < VLHE_DSP_PROBE_MAX; k++) {
            struct stat sb;
            char node[VLHE_PATH_MAX];

            if (k == 0)
                strcpy(node, "/dev/dsp");
            else
                sprintf(node, "/dev/dsp%d", k);

            /* PRESENT ONLY. A node nobody made is not one a user's
             * programs are pointed at, and offering it would invite
             * a redirect that does nothing. /dev/dsp is always
             * listed: it is the default and the answer for nearly
             * every machine. */
            if (k > 0 && lstat(node, &sb) != 0)
                continue;

            item = gtk_menu_item_new_with_label(node);
            gtk_signal_connect(GTK_OBJECT(item), "activate",
                               GTK_SIGNAL_FUNC(on_control_changed),
                               (gpointer)(long) SOUND_TAB_DEVICE);
            gtk_menu_append(GTK_MENU(umenu), item);
            gtk_widget_show(item);

            strncpy(g_use_node[g_nuse], node, VLHE_PATH_MAX - 1);
            g_use_node[g_nuse][VLHE_PATH_MAX - 1] = '\0';
            if (strcmp(node, sn.programs_use) == 0)
                usel = g_nuse;
            g_nuse++;
        }

        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_proguse), umenu);
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_proguse), usel);
        labelled_row(vbox, STR_SND_LABEL_PROGRAMS_USE, g_proguse, NULL, 1);
    }

    /* THREE PARAGRAPHS CUT TO TWO LINES, 2026-09-25 - the user called
     * the section verbose, and the Mixing frame beside it had just
     * been cut the same way. The labels now say which control does
     * what, so the note keeps only what a label cannot carry: that
     * the redirect is temporary, and that the cards are left alone. */
    note = gtk_label_new(
        STR_SND_LABEL_YOUR_PROGRAMS_DEVICE_POINTED);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /*
     * WRAPPED TO 565, AND THE NUMBER IS ONE OF FOUR BEING TESTED AT
     * ONCE - 2026-09-29, the user's idea: "for sound settings device
     * tab can you do 565, for the sound options tab do 570, for the
     * cd settings tab try 575 and cd settings options tab 580. That
     * way I can test 560 to 580 on the target with a single build".
     *
     * The Status page uses 560 and is known to fit; 585 is known to
     * overflow. These four bracket the gap, so one boot says which
     * is the largest that still fits instead of four.
     *
     * WHEN THE ANSWER IS KNOWN, ALL FIVE SHOULD BECOME ONE SHARED
     * CONSTANT. Four different widths on four pages is a test rig,
     * not a layout.
     */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- what the mixer can carry --------------------------------- */

    frame = gtk_frame_new(STR_SND_FRAME_MIXING);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * A FACT, NOT A CONTROL - see the file header. Shown because a
     * user wondering why a fifth program got no sound needs the
     * number, and there is nowhere else to learn it.
     *
     * ONE LINE, 2026-09-25, AT THE USER'S INSTRUCTION. It was four
     * paragraphs: the count, whether the synth has a slot of its own,
     * that four is a compile-time constant only the setup program can
     * change, and how the format is chosen. All true, none of it
     * anyone reads, and together they pushed the page into a
     * scrollbar.
     */
    {
        char msg[64];

        sprintf(msg, FMT_SND_PROGRAMS_PLAY_ONCE, sn.channels);
        note = gtk_label_new(msg);
        gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
        gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
        gtk_widget_show(note);
    }

    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */
/* Options                                                            */
/* ------------------------------------------------------------------ */

static void slot_refresh(void)
{
    struct vlhe_sound sn;
    char msg[128];

    if (g_slot_status == NULL)
        return;

    vlhe_sound(&sn);

    if (!sn.loaded)
        strcpy(msg, FMT_SND_VSOUND_NOT_LOADED_APPLIES);
    else if (sn.midi_slot != sn.midi_slot_applied)
        sprintf(msg, FMT_SND_MODULE_RUNNING_SLOT_RELOAD,
                sn.midi_slot_applied ? STR_SND_TEXT_WITH : STR_SND_TEXT_WITHOUT);
    else
        strcpy(msg, FMT_SND_MATCHES_RUNNING_MODULE);

    gtk_label_set_text(GTK_LABEL(g_slot_status), msg);
}

static void on_slot_toggled(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    slot_refresh();
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

static GtkWidget *build_options(void)
{
    struct vlhe_sound sn;
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *note;
    int admin;

    vlhe_sound(&sn);
    admin = page_admin();

    outer = gtk_vbox_new(FALSE, 0);

    /* ---- the pump -------------------------------------------------- */

    frame = gtk_frame_new(STR_SND_FRAME_PUMP);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    g_release = vlhe_tipped(gtk_check_button_new_with_label(
        STR_SND_CHECK_RELEASE_CARD_WHEN_LAST), STR_SND_CHECK_RELEASE_CARD_WHEN_LAST_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_release),
                                 sn.release_on_idle ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_release), "toggled",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) SOUND_TAB_OPTIONS);
    gtk_box_pack_start(GTK_BOX(vbox), g_release, FALSE, FALSE, 0);
    gtk_widget_show(g_release);

    /* WHAT IT DOES, NOT WHY - deliberately, and design/32 has the
     * reason. vsoundd's own usage text says this "fixes a note that
     * hangs after the last client on esssolo1, es1370, es1371, cmpci
     * and sonicvibes", and all three parts of that are unsafe: the
     * symptom was superseded in design/16 (it is a CONSTANT tone,
     * audible from load, not a note and not triggered by a close),
     * and the four other cards are a PREDICTION from reading their
     * source. Only esssolo1 was measured. A GUI repeating an unproven
     * card list is worse than one that says less. */
    note = gtk_label_new(
        /*
         * FOUR LINES TO TWO, AND IT NAMES THE CARD. The user,
         * 2026-09-21: "Needed for Ess Solo1. If an audible tone is
         * played try this option. It should not be needed."
         *
         * Naming the card is worth more than describing the symptom
         * in the abstract - a Solo-1 owner recognises their machine,
         * and everyone else is told not to touch it. The reasoning
         * about the risk of not getting the card back is Help's job,
         * not a panel's.
         */
        STR_SND_LABEL_RELEASE_NOTE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* 570 - see the note on build_device()'s label: four
     * widths across four tabs, tested in one boot. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- the module parameter -------------------------------------- */

    frame = gtk_frame_new(STR_SND_FRAME_MODULE);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    g_midi_slot = vlhe_tipped(gtk_check_button_new_with_label(
        STR_SND_CHECK_RESERVE_CHANNEL_MIDI_SYNTH), STR_SND_CHECK_RESERVE_CHANNEL_MIDI_SYNTH_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_midi_slot),
                                 sn.midi_slot ? TRUE : FALSE);
    /* LIVE FOR EVERYONE - design/49, the decision of 2026-10-01 ("B"):
     * a locked user may EDIT a machine setting; what needs root is
     * SAVING it, and the note at the end of the page says where that
     * leads. Greying the control made File > Save Draft unreachable by
     * the person it exists for. */
    (void) admin;
    gtk_signal_connect(GTK_OBJECT(g_midi_slot), "toggled",
                       GTK_SIGNAL_FUNC(on_slot_toggled), NULL);
    gtk_signal_connect(GTK_OBJECT(g_midi_slot), "toggled",
                       GTK_SIGNAL_FUNC(on_control_changed),
                       (gpointer)(long) SOUND_TAB_OPTIONS);
    gtk_box_pack_start(GTK_BOX(vbox), g_midi_slot, FALSE, FALSE, 0);
    gtk_widget_show(g_midi_slot);

    note = gtk_label_new(
        /* THREE LINES TO TWO. The full argument - that it costs a
         * channel either way - is Help's. */
        STR_SND_LABEL_MIDI_SLOT_NOTE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* 570 - see the note on build_device()'s label: four
     * widths across four tabs, tested in one boot. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    /* NO STATUS LINE HERE ANY MORE - 2026-10-07, the user's design:
     * whether vsound is loaded is the button row's line (vlhe_state.c),
     * and "running with other settings - reload" is on Status. The
     * variable stays NULL; its refresh already checks. */

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- the mix's headroom ---------------------------------------- */

    frame = gtk_frame_new(STR_SND_FRAME_MIX_HEADROOM);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /*
     * BUILT 2026-10-02 - design/09 "MIX HEADROOM". This frame stood
     * empty with a note saying neither control existed; the module
     * now has both (vsound_limit, vsound_atten), so it carries them.
     *
     * THE LIMITER'S SHAPE WAS LEFT OPEN by design/09 - attack and
     * release, or a soft knee. The user, 2026-10-02: both, as a
     * choice with off beside them. Module parameters, so they take
     * effect at the next load, like the MIDI slot above.
     */
    g_limiter = vlhe_tipped(gtk_option_menu_new(), STR_SND_MENU_LIMITER_TIP);
    {
        GtkWidget *menu = gtk_menu_new();
        static const char *names[3];
        int i;

        names[0] = STR_SND_MENU_LIMITER_OFF;
        names[1] = STR_SND_MENU_LIMITER_ATTACK_RELEASE;
        names[2] = STR_SND_MENU_LIMITER_SOFT_KNEE;
        for (i = 0; i < 3; i++) {
            GtkWidget *item = gtk_menu_item_new_with_label(names[i]);

            gtk_signal_connect(GTK_OBJECT(item), "activate",
                               GTK_SIGNAL_FUNC(on_control_changed),
                               (gpointer)(long) SOUND_TAB_OPTIONS);
            gtk_menu_append(GTK_MENU(menu), item);
            gtk_widget_show(item);
        }
        gtk_option_menu_set_menu(GTK_OPTION_MENU(g_limiter), menu);
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_limiter),
                                    sn.limiter);
    }
    labelled_row(vbox, STR_SND_LABEL_LIMITER, g_limiter, NULL, 0);

    g_atten = vlhe_tipped(gtk_option_menu_new(), STR_SND_MENU_ATTEN_TIP);
    atten_menu(sn.attenuation);
    labelled_row(vbox, STR_SND_LABEL_ATTENUATION, g_atten, NULL, 0);

    note = gtk_label_new(STR_SND_LABEL_HEADROOM_NOTE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* 570 - see the note on build_device()'s label: four
     * widths across four tabs, tested in one boot. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* BUILT ALWAYS, SHOWN ONLY WHEN IT IS TRUE - so Modify can hide
     * it without rebuilding the page (sound_privilege_changed). */
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

    slot_refresh();

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
        g_page_cb(1);       /* both pages are forms */
}

/*
 * READ THE WIDGETS INTO THE BACKEND. Apply and OK call this; it
 * writes no file - vlhe_commit() does that once, afterwards.
 */
int sound_collect(void)
{
    struct vlhe_sound sn;
    int rc;

    /* START FROM WHAT THE BACKEND HAS rather than from zero, so the
     * fields this page does not own - channels, loaded, running, and
     * midi_slot_applied - are carried through untouched instead of
     * being cleared by a page that never displayed them. */
    if (vlhe_sound(&sn) != 0)
        return -1;

    if (g_release != NULL)
        sn.release_on_idle =
            GTK_TOGGLE_BUTTON(g_release)->active ? 1 : 0;

    if (g_midi_slot != NULL)
        sn.midi_slot =
            GTK_TOGGLE_BUTTON(g_midi_slot)->active ? 1 : 0;

    if (g_limiter != NULL) {
        int idx = menu_index(g_limiter);

        if (idx >= 0 && idx <= 2)
            sn.limiter = idx;
    }
    if (g_atten != NULL) {
        int idx = menu_index(g_atten);

        if (idx >= 0 && idx < g_natten)
            sn.attenuation = g_atten_pct[idx];
    }

    /*
     * THE CARD IS READ AND SAVED, AND IT TAKES EFFECT AT THE NEXT
     * LOAD - not at once. This used to say the menu was display-only
     * because a running vsoundd cannot be retargeted; that is still
     * true, and it is not a reason to refuse the setting. The plan
     * passes `-d <card>' from the config when it starts the pump
     * (vlhe_apply.c), so the choice is exactly for the cases the
     * user named (2026-09-24): a machine where detection picked the
     * wrong node, and a machine with more than one card. Between
     * Apply and the next Load the file and the running daemon
     * disagree, which is what the dirty bit and the save-before-load
     * question are for.
     */
    if (g_card != NULL && g_ncard > 0) {
        GtkOptionMenu *om = GTK_OPTION_MENU(g_card);
        int idx = -1;

        if (om->menu != NULL && om->menu_item != NULL)
            idx = g_list_index(GTK_MENU_SHELL(om->menu)->children,
                               om->menu_item);
        if (idx >= 0 && idx < g_ncard) {
            strncpy(sn.card, g_card_node[idx], sizeof sn.card - 1);
            sn.card[sizeof sn.card - 1] = '\0';
        }
    }

    /* THE NODE PROGRAMS OPEN - `vlhe apply' reads it from the config
     * next time it builds a plan, and the runner redirects it.
     *
     * BY MENU POSITION, like the card above. No trimming and no empty
     * case to handle any more: a list cannot produce a stray space or
     * a blank, which is the other half of why it is a list. */
    if (g_proguse != NULL && g_nuse > 0) {
        GtkOptionMenu *om = GTK_OPTION_MENU(g_proguse);
        int idx = -1;

        if (om->menu != NULL && om->menu_item != NULL)
            idx = g_list_index(GTK_MENU_SHELL(om->menu)->children,
                               om->menu_item);
        if (idx >= 0 && idx < g_nuse) {
            strncpy(sn.programs_use, g_use_node[idx],
                    sizeof sn.programs_use - 1);
            sn.programs_use[sizeof sn.programs_use - 1] = '\0';
        }
    }

    rc = vlhe_set_sound(&sn);
    if (rc != 0) {
        /* SAY WHAT - 2026-10-01. vlhe_set_sound() refuses a device
         * that is not /dev/dspN and a switch that is not 0 or 1; the
         * menus cannot produce either, so this is a hand-edited
         * config value or a build mismatch, and it said nothing. */
        report(STR_SND_MSG_SOUND_SETTINGS_DEVICE_NAME);
    }

    /* THE PAGE IS CLEAN ONCE ITS VALUES ARE IN THE BACKEND. Whether
     * the FILE was written is vlhe_commit()'s business and its
     * failure is reported separately; the asterisk tracks "the widget
     * disagrees with the backend", which is no longer true. */
    if (rc == 0)
        clear_dirty();
    return rc;
}

/*
 * PUT THE WIDGETS BACK TO WHAT THE BACKEND SAYS - Cancel's half.
 *
 * Called AFTER the backend has re-read its files, so this reads the
 * restored values rather than the abandoned ones.
 */
void sound_reload(void)
{
    struct vlhe_sound sn;

    if (vlhe_sound(&sn) != 0)
        return;

    /* SET THE CONTROLS WITHOUT MARKING THEM DIRTY. Every
     * set_active() below fires "toggled" exactly as a click does, so
     * without this guard Cancel would leave the page dirtier than it
     * found it. */
    g_loading = 1;

    if (g_release != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_release),
                                     sn.release_on_idle ? TRUE : FALSE);
    if (g_midi_slot != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_midi_slot),
                                     sn.midi_slot ? TRUE : FALSE);
    if (g_limiter != NULL)
        gtk_option_menu_set_history(GTK_OPTION_MENU(g_limiter),
                                    sn.limiter);
    if (g_atten != NULL)
        atten_menu(sn.attenuation);

    /* THE CARD MENU, back to the saved node - editable since
     * 2026-09-24, so Cancel has something to undo here too. */
    if (g_card != NULL) {
        int i, found = 0;

        for (i = 1; i < g_ncard; i++)
            if (sn.card[0] != '\0' && strcmp(g_card_node[i], sn.card) == 0) {
                gtk_option_menu_set_history(GTK_OPTION_MENU(g_card), i);
                found = 1;
                break;
            }
        if (!found)             /* empty, or not in the list: detect */
            gtk_option_menu_set_history(GTK_OPTION_MENU(g_card), 0);
    }

    /* THE MODULE FIELD IS RESTORED, because it IS editable and
     * sound_collect() does read it - so Cancel has something to undo
     * here where it has nothing to undo above. The g_loading guard
     * covers the "changed" signal this fires. */
    if (g_proguse != NULL) {
        int k;

        for (k = 0; k < g_nuse; k++)
            if (strcmp(g_use_node[k], sn.programs_use) == 0) {
                gtk_option_menu_set_history(GTK_OPTION_MENU(g_proguse), k);
                break;
            }
    }

    g_loading = 0;
    clear_dirty();
}

int sound_page_wants_buttons(void)
{
    return 1;
}

void sound_set_page(int page)
{
    if (g_notebook != NULL)
        gtk_notebook_set_page(GTK_NOTEBOOK(g_notebook), page);
}

GtkWidget *sound_build(void (*report_fn)(const char *),
                       void (*page_fn)(int))
{
    GtkWidget *nb;
    GtkWidget *tab;

    g_report = report_fn;
    g_page_cb = page_fn;
    (void) report;      /* reporting lands with the real backend */

    /* THE BUILD SETS EVERY CONTROL FROM THE BACKEND, and each of
     * those fires the same signal a user click does - so the page
     * would be born dirty without this. */
    g_loading = 1;

    nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);

    tab = gtk_label_new(STR_SND_LABEL_DEVICE);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_device(), tab);

    tab = gtk_label_new(STR_VOL_LABEL_OPTIONS);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_options(), tab);

    gtk_signal_connect(GTK_OBJECT(nb), "switch_page",
                       GTK_SIGNAL_FUNC(on_switch_page), NULL);
    g_notebook = nb;

    g_loading = 0;

    return nb;
}

/* Which tabs hold unsaved edits, as a bitmask - 0 when clean. The
 * shell asks so it can put an asterisk on the sidebar row. */
int sound_dirty(void)
{
    return (g_tab_dirty[SOUND_TAB_DEVICE]  ? 1 : 0) |
           (g_tab_dirty[SOUND_TAB_OPTIONS] ? 2 : 0);
}

/* The shell hands over a callback so a widget changing can repaint
 * the sidebar immediately, rather than the shell polling for it. */
void sound_set_dirty_cb(void (*cb)(void))
{
    g_dirty_cb = cb;
}

/*
 * THE MACHINE CHANGED. "vsound is not loaded" was computed by
 * slot_refresh() at build and on the toggle only - never after a Load
 * (design/36 rows 44/57's family). The shell calls this when the page
 * is shown and after Load/Unload.
 */
void sound_machine(void)
{
    slot_refresh();
}

/*
 * MODIFY WAS PRESSED - design/49 N7. Re-apply only what privilege
 * decides: the slot's sensitivity and the note. NOTHING ELSE IS
 * TOUCHED, because on_modify_ok() once reloaded the pages here and
 * threw away the user's pending edits (vlhe_cc.c says so at length).
 */
void
sound_privilege_changed(void)
{
    int admin = page_admin();

    /* Nothing to re-grey since 2026-10-01 - the controls are live for
     * everyone; only the note comes and goes. */
    if (g_root_note != NULL) {
        if (admin)
            gtk_widget_hide(g_root_note);
        else
            gtk_widget_show(g_root_note);
    }
}
