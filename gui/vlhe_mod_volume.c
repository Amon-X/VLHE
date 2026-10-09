/*
 * vlhe_mod_volume.c - the Volume module: per-stream sliders, and the
 * options that belong beside them.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * WHAT THIS IS NOT. It is not /dev/mixer. OSS has 25 mixer controls,
 * every one naming a physical signal path, with nothing that means
 * "channel 2" - so the card's mixer stays the REAL CARD'S, where every
 * control keeps its meaning and any applet works, and per-stream volume
 * is ours. vsoundvol_gtk.c's header has the longer version.
 *
 * THE CARD'S PCM IS THE MASTER over everything here: applications and
 * CD audio together, after mixing. The window says so, because someone
 * reaching for a slider expecting per-application control of the card
 * will otherwise be confused.
 *
 * TWO TABS:
 *
 *   Levels   one row per channel - the live controls
 *   Options  preferences that change what happens automatically
 *
 * "OPTIONS", NOT "ADVANCED". Advanced is a warning label, and it would
 * be warning about a checkbox that restores mixer levels. The contents
 * are mundane and a user should not be discouraged from reading them.
 *
 * BOTH SLIDER ORIENTATIONS, SHARING ONE GtkAdjustment. The user wants
 * horizontal and vertical both available (vlhe-control-center-spec.md's
 * closing section). They are not two implementations: one adjustment
 * per channel holds the value, both scales display it, and the View
 * menu swaps which is packed. A handler wired to the adjustment sees
 * changes from either without knowing which exists.
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
#include "vlhe_mod_volume.h"

/* HOW OFTEN TO LOOK. vsound's generation counter makes a poll one
 * ioctl and one integer compare, so this is cheap; 250ms is what
 * vsoundvol_gtk.c settled on and there is no reason to differ. */
#define POLL_MS         250

/* A VERTICAL FADER NEEDS A HEIGHT - a GtkVScale with none collapses to
 * nothing useful. Horizontal ones take their width from the box. */
/* A FLOOR, NOT THE HEIGHT. The faders expand to fill the panel (see
 * build_levels); this is only what they shrink to when the window is
 * small. */
#define VFADER_H        120

/* EVERY VERTICAL COLUMN IS THE SAME WIDTH, and that is the user's
 * requirement rather than a tidiness choice: "they should be evenly
 * spaced so a long program name does not overlap".
 *
 * If a column sized itself to its label, a channel called
 * "quake" and one called "External MIDI Passthrough" would be wildly
 * different widths, the faders would sit at uneven intervals, and the
 * whole row would REFLOW every time a program started or exited -
 * moving the slider under the user's pointer.
 *
 * So the column is fixed and the LABEL is what gives: it is clipped,
 * and the full name is in the tooltip. A fader that stays put matters
 * more than a name that always fits. */
#define VCOL_W          76

/* THE BOOST - design/09, "A BOOST option - per-channel gain above
 * 100%". BUILT 2026-10-02.
 *
 * 200%, NOT pavucontrol's 153, and the arithmetic is OUR case: lxdoom
 * is structurally capped at half scale (its mixer's VOL_MAX 128 x 127
 * = 16256 against 32767), so 2x recovers exactly what it withholds
 * while quake and CD audio arrive at full scale. Measured on
 * 2026-08-31 the user balanced three clients by cutting quake to 17%
 * - throwing 83% of one stream away because the quiet one could not
 * come up. The ceiling is a BUILD setting (`make VOL_BOOST=150'), the
 * user's call, and VLHE_VOL_BOOST carries it here.
 *
 * WHY THIS WAS A DISABLED PLACEHOLDER UNTIL NOW. Above unity the
 * per-channel gain wrapped - a sample at 20000 doubled became 40000
 * and went negative, a crack rather than distortion - and boosting
 * one stream while two others ran at full scale was precisely the
 * case that clips. vsound_mix.c now sums into 32 bits and LIMITS the
 * sum (Sound > Limiter), so neither is true any more.
 *
 * PER ROW, AND NOT SAVED. Ticking Boost extends that channel's slider
 * to the ceiling; unticking brings it back to 100, and a level above
 * 100 comes down with it - a slider whose range no longer holds its
 * value would be lying. A channel already above 100 (set from the
 * command line, say) shows Boost ticked. A new program in the slot
 * starts unticked. */

/* ONE CHANNEL, TWO PRESENTATIONS, ONE VALUE.
 *
 * THE TWO ORIENTATIONS ARE DIFFERENT CONTAINERS, NOT ONE CONTAINER
 * WITH A DIFFERENT WIDGET IN IT - and getting that wrong is what the
 * first attempt did. Swapping an hscale for a vscale inside a
 * horizontal row gives five vertical faders STACKED IN A COLUMN, each
 * still in its own row. A real vertical mixer puts them SIDE BY SIDE,
 * KMix-style, with the name and mute beneath each fader.
 *
 * So there are two widget trees per channel:
 *
 *   hrow    name | ----slider---- | state | mute      packed in a VBox
 *   vcol    fader / name / mute, stacked               packed in an HBox
 *
 * THE HORIZONTAL SCALE SHARES `adj' DIRECTLY. The vertical one CANNOT,
 * and that is a GTK 1.2 limitation rather than a choice:
 *
 * GtkVScale PUTS THE ADJUSTMENT'S LOWER BOUND AT THE TOP. On a 0..100
 * volume that draws 0 at the top and 100 at the bottom, so dragging UP
 * makes a channel QUIETER - backwards from every mixer ever built, and
 * silently wrong rather than merely ugly. `gtk_range_set_inverted()'
 * would fix it in one call and IS NOT IN 1.2 (checked: absent from
 * gtkrange.h and gtkscale.h).
 *
 * So the vertical scale gets its own adjustment holding 100 - value,
 * and the two are kept in step. `adj' remains the authority; `vadj' is
 * a mirror. Both handlers guard with r->setting so the mirroring
 * cannot recurse. */
struct chanrow {
    /* horizontal */
    GtkWidget     *hrow;
    GtkWidget     *hname;
    GtkWidget     *hscale;
    GtkWidget     *hmute;
    GtkWidget     *hboost;      /* extends the range - see VLHE_VOL_BOOST */
    GtkWidget     *hstate;

    /* vertical */
    GtkWidget     *vcol;
    GtkWidget     *vname;
    GtkWidget     *vval;        /* the real value - the scale's is wrong */
    GtkWidget     *vscale;
    GtkWidget     *vmute;
    GtkWidget     *vboost;      /* extends the range - see VLHE_VOL_BOOST */
    GtkWidget     *vstate;

    GtkObject     *adj;         /* the real value, 0..100, low = quiet  */
    GtkObject     *vadj;        /* the INVERTED mirror the vscale shows  */

    int            index;       /* vsound's slot number                 */
    int            pid;         /* whose stream - checked on every set  */
    int            setting;     /* guard: we are updating the widget    */
    int            max;         /* 100, or VLHE_VOL_BOOST when boosted  */
};

static struct chanrow  g_row[VLHE_MAX_CHAN];
static int             g_nrow;
static int             g_vertical;      /* which orientation is packed  */
static GtkWidget      *g_opt_vertical;  /* the Options checkboxes, read */
static GtkWidget      *g_opt_restore;   /* by volume_collect()          */
static GtkWidget      *g_opt_progvol;   /* Save Program Levels           */
static int             g_opt_dirty;     /* unsaved Options edits        */
static int             g_loading;       /* set while WE set a widget    */
static void          (*g_dirty_cb)(void);
static unsigned long   g_seen_gen;
static int             g_seen_present;  /* and whether vsound was there */
static int             g_active;        /* the page is the one showing  */
static int             g_page;          /* PAGE_LEVELS or PAGE_OPTIONS  */
static guint           g_poll_tag;
static GtkWidget      *g_hbox_rows;     /* the horizontal presentation  */
static GtkWidget      *g_vbox_cols;     /* the vertical presentation    */
static GtkTooltips    *g_tips;          /* full names, when clipped     */
static GtkWidget      *g_nomodule;      /* shown when vsound is absent  */
/* LOADED, BUT NOTHING IS PLAYING - a third state, not an error.
 * See where it is built for why an empty frame reads as broken. */
static GtkWidget      *g_noclients;
static GtkWidget      *g_levels_frame;  /* hidden when vsound is absent */
static void          (*g_report)(const char *);
static void          (*g_page_cb)(int);
static GtkWidget      *g_notebook;

/* WHICH TAB WANTS THE BUTTONS. Page 0 is Levels - a live mixer, every
 * drag already written to the driver, so an Apply beside it would be a
 * lie. Page 1 is Options - a form whose settings reach a config file,
 * where committing on OK is what a user expects. */
#define PAGE_LEVELS     0
#define PAGE_OPTIONS    1

/* ------------------------------------------------------------------ */

static void report(const char *msg)
{
    if (g_report != NULL)
        g_report(msg);
}

/* ------------------------------------------------------------------ */
/* Writing                                                            */
/* ------------------------------------------------------------------ */

/* The adjustment moved. THIS IS THE ONLY WRITE PATH, and it is wired
 * to the ADJUSTMENT rather than to either scale - so it fires whichever
 * orientation is on screen, and the other one follows for free because
 * they share the object. */
static void on_value_changed(GtkAdjustment *adj, gpointer data)
{
    struct chanrow *r = (struct chanrow *)data;
    int vol;
    int was;

    /* NOT A USER ACTION. refresh() sets adjustments from the driver's
     * values; without this guard each refresh would write them straight
     * back, and a slider the user was dragging would fight the poll. */
    if (r->setting)
        return;

    vol = (int)adj->value;

    /* THE PID IS PASSED, NOT 0. Indices are reused when a client exits,
     * so "index 1" held across a 250ms poll may be a different program
     * by the time this fires. The driver checks and returns -ESRCH;
     * passing 0 would mean "set whoever is there now", which is exactly
     * the bug. */
    if (vlhe_set_volume(r->index, r->pid, vol) != 0) {
        /* NOT ROLLED BACK. The next refresh reads the real value and
         * puts the slider where the driver actually is - one source of
         * truth, rather than a guess about why the write failed. */
        report(STR_VOL_MSG_COULD_NOT_SET_VOLUME);
        return;
    }

    /*
     * MIRROR INTO THE VERTICAL FADER, AND THIS IS NOT COSMETIC.
     *
     * The two orientations have SEPARATE adjustments, and until now
     * only on_vvalue_changed() mirrored - vertical wrote into `adj',
     * horizontal wrote into nothing. That made horizontal drags go
     * MISSING, not merely leave a stale thumb:
     * gtk_adjustment_set_value() does not emit "value_changed" when
     * the value is unchanged, so once the vertical handler (or a
     * refresh, which sets both) had left `adj' at some value, dragging
     * the horizontal scale BACK to that same value fired nothing and
     * sent no ioctl. The slider sat where the user put it and the
     * volume did not move.
     *
     * The user's report was "Volume sometimes does not work"
     * (2026-09-18) - "sometimes" being exactly the drags that landed
     * on the value the other adjustment already held.
     *
     * TOP IS LOUD, so the vertical value is 100 - vol. Saved and
     * restored rather than cleared, for the reason spelled out in
     * on_vvalue_changed().
     */
    was = r->setting;
    r->setting = 1;
    gtk_adjustment_set_value(GTK_ADJUSTMENT(r->vadj),
                             (gfloat)(r->max - vol));
    r->setting = was;
}

/* EITHER MUTE BUTTON. There are two - one per presentation - and they
 * must agree, so the one that was not clicked is updated under the
 * `setting' guard rather than being left stale. Cheaper and more
 * reliable than trying to keep one button and reparent it. */
/* The VERTICAL fader moved. Translate and hand to the real handler.
 *
 * TOP IS LOUD: the scale shows 100 - volume, so a thumb at the top of
 * the trough is vadj->value 0 and volume 100. */
static void on_vvalue_changed(GtkAdjustment *adj, gpointer data)
{
    struct chanrow *r = (struct chanrow *)data;
    int vol;
    int was;

    if (r->setting)
        return;

    vol = r->max - (int)adj->value;

    /* MIRROR INTO `adj' WITH THE GUARD SAVED AND RESTORED, not simply
     * cleared to 0.
     *
     * THE FIRST VERSION SET IT BACK TO 0 UNCONDITIONALLY and that was
     * a real bug: refresh() sets vadj while holding the guard, this
     * handler fires, and clearing the flag on the way out left the
     * REST of refresh() unguarded - so the remaining widget updates
     * were treated as user actions. The visible symptom was thumbs
     * that disagreed with their labels and a channel reading 100 when
     * the backend said 0.
     *
     * A flag that is set and cleared rather than saved and restored
     * cannot survive being re-entered. */
    was = r->setting;
    r->setting = 1;
    gtk_adjustment_set_value(GTK_ADJUSTMENT(r->adj), (gfloat)vol);
    r->setting = was;

    if (vlhe_set_volume(r->index, r->pid, vol) != 0)
        report(STR_VOL_MSG_COULD_NOT_SET_VOLUME);
}

/* SET A ROW'S RANGE - 100, or the boost ceiling. Both adjustments,
 * since the vertical one is the inverted mirror; the caller puts the
 * values back, under the guard. */
static void set_range(struct chanrow *r, int max)
{
    r->max = max;
    GTK_ADJUSTMENT(r->adj)->upper = (gfloat) max;
    gtk_adjustment_changed(GTK_ADJUSTMENT(r->adj));
    GTK_ADJUSTMENT(r->vadj)->upper = (gfloat) max;
    gtk_adjustment_changed(GTK_ADJUSTMENT(r->vadj));
}

/* EITHER BOOST BUTTON - two, like Mute, kept in step the same way. */
static void on_boost_toggled(GtkWidget *w, gpointer data)
{
    struct chanrow *r = (struct chanrow *)data;
    GtkWidget *other;
    int on, vol, was, before;

    if (r->setting)
        return;

    on = GTK_TOGGLE_BUTTON(w)->active ? 1 : 0;
    other = (w == r->hboost) ? r->vboost : r->hboost;
    vol = (int) GTK_ADJUSTMENT(r->adj)->value;
    before = vol;

    was = r->setting;
    r->setting = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other),
                                 on ? TRUE : FALSE);
    set_range(r, on ? VLHE_VOL_BOOST : 100);
    if (vol > r->max)
        vol = r->max;
    gtk_adjustment_set_value(GTK_ADJUSTMENT(r->adj), (gfloat) vol);
    gtk_adjustment_set_value(GTK_ADJUSTMENT(r->vadj),
                             (gfloat)(r->max - vol));
    r->setting = was;

    /* UNTICKED WITH THE LEVEL ABOVE 100: it comes down, and the
     * driver is told - the slider cannot show what it does not hold. */
    if (vol != before && vlhe_set_volume(r->index, r->pid, vol) != 0)
        report(STR_VOL_MSG_COULD_NOT_SET_VOLUME);
}

static void on_mute_toggled(GtkWidget *w, gpointer data)
{
    struct chanrow *r = (struct chanrow *)data;
    GtkWidget *other;
    int on;
    int was;

    if (r->setting)
        return;

    on = GTK_TOGGLE_BUTTON(w)->active;

    /* A REAL IOCTL SINCE 2026-09-18 - VSOUND_IOC_MUTE, the channel's
     * own flag. So the failure really is a stale slot now, which is
     * what this message says: the next poll will correct the button.
     *
     * NOTHING IS REMEMBERED HERE. The level stays in the driver
     * whether muted or not, so there is no shadow to save, restore, or
     * lose when this window closes - see VSOUND_CHN_MUTED. */
    if (vlhe_set_mute(r->index, r->pid, on) != 0) {
        report(STR_VOL_MSG_COULD_NOT_CHANGE_MUTE);
        return;
    }

    /* SAVED AND RESTORED, for the reason in on_vvalue_changed. */
    other = (w == r->hmute) ? r->vmute : r->hmute;
    was = r->setting;
    r->setting = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other),
                                 on ? TRUE : FALSE);
    r->setting = was;
}

/* ------------------------------------------------------------------ */
/* Reading                                                            */
/* ------------------------------------------------------------------ */

/* Put the driver's state into the widgets.
 *
 * EVERY WRITE TO A WIDGET IS FENCED by r->setting, because setting an
 * adjustment emits value_changed exactly as a drag does and the
 * handler cannot tell them apart. */
/*
 * WHAT A ROW'S STATE COLUMN SAYS. Shared by refresh() and the
 * liveness poll, so the two cannot disagree about what "idle" means.
 *
 * TWO STATES, AND THE COLUMN NOW MEANS ONE THING: this row cannot be
 * heard, and here is why. Blank is normal.
 *
 *   muted   the user silenced it - essential, because a slider at 60
 *           with no sound is otherwise baffling
 *   MIDI    the reserved slot, which exists whether or not vmidid
 *           holds it
 *
 * `idle' WAS HERE AND IS GONE, 2026-09-19. design/07 wanted it so an
 * open-but-silent stream could be told from a free slot - but the row
 * already carries the PROGRAM NAME, so "quake" against "(free)" says
 * that already.
 *
 * THE USER TOOK IT APART: "I am trying to see what idle tells the
 * user." Nothing actionable, is the answer. An idle slot cannot be
 * reclaimed - the program owns it until it closes - so knowing it is
 * idle changes nothing you can do. And their sndserv test showed the
 * failure mode: a daemon that holds /dev/dsp and writes only when a
 * client sends audio reads `idle' almost always, which is correct and
 * tells you only that sndserv is a daemon.
 *
 * IT ALSO COST MORE THAN IT LOOKED. `active' is derived from bytes
 * moved between polls, so it needed a read every 250 ms where the
 * generation counter otherwise makes a poll free - and a playing game
 * would have flickered the label several times a second.
 *
 * THE `mixed' COUNTER STAYS IN THE DRIVER. It cost nothing, it is
 * honest, and the Status page may want it for "is this daemon wedged",
 * where liveness is the actual question.
 */
static void state_text(const struct vlhe_channel *c, char *out)
{
    if (c->pid != 0 && c->muted)
        strcpy(out, STR_VOL_TEXT_CHANNEL_MUTED);
    else if (c->midi)
        strcpy(out, STR_VOL_TEXT_CHANNEL_MIDI);
    else
        out[0] = '\0';
}

static void refresh(void)
{
    struct vlhe_channel ch[VLHE_MAX_CHAN];
    int n, i;

    /* WHAT THIS REFRESH SAW IS WHAT THE POLL COMPARES AGAINST - so a
     * refresh from anywhere (the vertical toggle's rebuild, the
     * machine hook, a show) leaves the poll's memory true. The inverse
     * of the bug below: the toggle redrew the sliders while
     * g_seen_gen sat at 0, and the next unload read as no change. */
    g_seen_present = vlhe_sound_present();
    g_seen_gen     = g_seen_present ? vlhe_generation() : 0;

    if (!g_seen_present) {
        /* THE MODULE WENT AWAY. design/09: the GUI must not block an
         * unload, and a user can rmmod from a terminal while this
         * window sits open. Showing stale sliders would be a lie. */
        if (g_levels_frame != NULL)
            gtk_widget_hide(g_levels_frame);
        if (g_noclients != NULL)
            gtk_widget_hide(g_noclients);
        if (g_nomodule != NULL)
            gtk_widget_show(g_nomodule);
        return;
    }

    if (g_nomodule != NULL)
        gtk_widget_hide(g_nomodule);

    n = vlhe_channels(ch, VLHE_MAX_CHAN);

    /*
     * THE THIRD STATE: loaded, and nothing is playing - the user,
     * 2026-09-26: *"the volume page does not update until something
     * speaks to vsound so in the previous runs when vmidi started
     * pumping thats when it showed channel volumes"*.
     *
     * IT IS THE PAGE WORKING, NOT A DELAYED REFRESH.
     * `vlhe_channels()' asks VSOUND_IOC_CHANS, which lists channels
     * that are OPEN; with no client there are none, and a mixer for
     * streams that do not exist would be an invention.
     *
     * BUT AN EMPTY FRAME READS AS BROKEN. Someone who has just
     * loaded vsound and opened this page cannot tell "nothing is
     * playing" from "this does not work" - and the first is the
     * ordinary case, since loading comes before playing.
     *
     * The frame is HIDDEN rather than shown empty, so the page says
     * one thing at a time.
     */
    if (n <= 0) {
        if (g_levels_frame != NULL)
            gtk_widget_hide(g_levels_frame);
        if (g_noclients != NULL)
            gtk_widget_show(g_noclients);
        return;
    }

    if (g_noclients != NULL)
        gtk_widget_hide(g_noclients);
    if (g_levels_frame != NULL)
        gtk_widget_show(g_levels_frame);

    for (i = 0; i < g_nrow; i++) {
        struct chanrow *r = &g_row[i];
        char label[VLHE_NAME_MAX + 24];
        char state[8];
        char valbuf[8];

        if (i >= n) {
            gtk_widget_hide(r->hrow);
            gtk_widget_hide(r->vcol);
            continue;
        }

        r->setting = 1;

        /* BOOST FOLLOWS THE PROGRAM: kept while the same pid holds
         * the slot, cleared for a new one, and forced on for a level
         * the unboosted range cannot show. */
        {
            int boosted = (r->pid == ch[i].pid && r->max > 100);

            if (ch[i].pid != 0 && ch[i].volume > 100)
                boosted = 1;
            if (ch[i].pid == 0)
                boosted = 0;
            if ((boosted ? VLHE_VOL_BOOST : 100) != r->max)
                set_range(r, boosted ? VLHE_VOL_BOOST : 100);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->hboost),
                                         boosted ? TRUE : FALSE);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->vboost),
                                         boosted ? TRUE : FALSE);
        }

        r->index = ch[i].index;
        r->pid   = ch[i].pid;

        /* A FREE SLOT KEEPS ITS ROW, greyed rather than removed.
         * design/07's reasoning, and it holds here: a slider that
         * vanishes when a game exits makes the panel jump under the
         * pointer, and a row that stays put is easier to read than a
         * list that reflows. */
        if (ch[i].pid == 0)
            sprintf(label, FMT_VOL_FREE);
        else
            sprintf(label, "%.*s", VLHE_NAME_MAX - 1, ch[i].name);

        gtk_label_set_text(GTK_LABEL(r->hname), label);
        gtk_label_set_text(GTK_LABEL(r->vname), label);

        /* THE FULL NAME IN A TOOLTIP, because the vertical column is
         * fixed at VCOL_W and a long name is clipped there. */
        if (g_tips != NULL && ch[i].pid != 0)
            gtk_tooltips_set_tip(g_tips, r->vcol, label, NULL);

        state_text(&ch[i], state);
        gtk_label_set_text(GTK_LABEL(r->hstate), state);
        gtk_label_set_text(GTK_LABEL(r->vstate), state);

        /*
         * A FREE SLOT SHOWS NO LEVEL, 2026-09-19.
         *
         * The driver reports whatever the last occupant left - and
         * vsound_chan_claim() sets vol to UNITY, so a slot that has
         * never been used reads 100. Either way the row was drawing a
         * thumb partway along and a number beside it, for a slot with
         * nothing in it. Greying made it dim, not absent, and a dim
         * "75" still reads as a value something has.
         *
         * Found by the user in a screenshot taken to look at
         * something else.
         */
        if (ch[i].pid == 0) {
            gtk_adjustment_set_value(GTK_ADJUSTMENT(r->adj), 0.0);
            gtk_adjustment_set_value(GTK_ADJUSTMENT(r->vadj),
                                     (gfloat) r->max);
            gtk_label_set_text(GTK_LABEL(r->vval), "");
        } else {
            gtk_adjustment_set_value(GTK_ADJUSTMENT(r->adj),
                                     (gfloat)ch[i].volume);
            gtk_adjustment_set_value(GTK_ADJUSTMENT(r->vadj),
                                     (gfloat)(r->max - ch[i].volume));

            sprintf(valbuf, "%d", ch[i].volume);
            gtk_label_set_text(GTK_LABEL(r->vval), valbuf);
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->hmute),
                                     ch[i].muted ? TRUE : FALSE);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->vmute),
                                     ch[i].muted ? TRUE : FALSE);

        /* A FREE SLOT CANNOT BE DRAGGED. The driver would refuse
         * (-ENODEV) and the user would get an error for touching a
         * control that was offered to them. */
        gtk_widget_set_sensitive(r->hscale, ch[i].pid != 0);
        gtk_widget_set_sensitive(r->vscale, ch[i].pid != 0);
        gtk_widget_set_sensitive(r->hmute,  ch[i].pid != 0);
        gtk_widget_set_sensitive(r->vmute,  ch[i].pid != 0);

        gtk_widget_set_sensitive(r->hboost, ch[i].pid != 0);
        gtk_widget_set_sensitive(r->vboost, ch[i].pid != 0);

        /* SHOW WHICHEVER PRESENTATION IS CURRENT. Showing both here
         * and letting volume_set_vertical() sort it out would flash
         * the other one for a frame on every refresh. */
        if (g_vertical) {
            gtk_widget_show(r->vcol);
            gtk_widget_hide(r->hrow);
        } else {
            gtk_widget_show(r->hrow);
            gtk_widget_hide(r->vcol);
        }

        r->setting = 0;
    }
}

/* THE POLL: one integer compare in the common case.
 *
 * vsound bumps its generation on volume change, open AND close, so
 * nothing is formatted, redrawn or sent to X unless something moved.
 * That is what makes 250ms cheap enough not to argue about. */
/*
 * THE POLL TRACKS PRESENCE AS WELL AS THE GENERATION - 2026-10-01, the
 * user, loading vsound ALONE from the Status page's row button: "The
 * vsound module is not loaded" stayed up; and the other way, an unload
 * left the sliders up. THE DRIVER'S GENERATION IS 0 IN TWO STATES -
 * unloaded (chanlist() fails, vlhe_generation() answers 0) and freshly
 * loaded with no client yet (a new module's counter) - and it is bumped
 * only by a CLIENT claiming, freeing or touching a channel
 * (vsound_chan.c). A full Load hid it because vmidid and vdiscd open
 * channels before anyone looks; vsound by itself, with only the pump
 * (a reader, not a channel), never moved the counter off the value
 * the unloaded state had left behind.
 *
 * ONLY WHILE THE PAGE IS SHOWING - design/47 V2. The timer is started
 * by volume_set_active(1) after an immediate refresh and removed by
 * volume_set_active(0); a hidden page costs nothing and cannot be
 * stale for longer than the show that reveals it.
 */
static gint on_poll(gpointer data)
{
    int present;
    unsigned long gen;

    (void)data;

    present = vlhe_sound_present();
    gen     = present ? vlhe_generation() : 0;
    if (present != g_seen_present || gen != g_seen_gen)
        refresh();              /* which records what it saw */

    return TRUE;                /* keep the timeout */
}

/* ------------------------------------------------------------------ */
/* Building                                                           */
/* ------------------------------------------------------------------ */

/* Build both presentations of one channel.
 *
 * BOTH TREES ARE BUILT UP FRONT. It costs a handful of widgets per
 * channel and makes the switch instant and stateless - the value is in
 * the shared adjustment, so there is nothing to carry across. */
static void build_row(struct chanrow *r, GtkWidget *hparent,
                      GtkWidget *vparent)
{
    GtkWidget *box;
    GtkWidget *align;

    /* 0 to 100 - VSOUND_VOL_MAX, which is 0..100 "like every OSS
     * control". step 1, page 10. ONE adjustment, both scales. */
    r->max  = 100;      /* Boost widens it - set_range() */
    r->adj  = gtk_adjustment_new(0.0, 0.0, 100.0, 1.0, 10.0, 0.0);
    r->vadj = gtk_adjustment_new(100.0, 0.0, 100.0, 1.0, 10.0, 0.0);

    /* ---- horizontal: name | slider | state | mute ---------------- */

    box = gtk_hbox_new(FALSE, 6);
    r->hrow = box;

    r->hname = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(r->hname), 0.0, 0.5);
    /* A FIXED LABEL WIDTH so the sliders line up down the column
     * rather than starting wherever each name ends. */
    gtk_widget_set_usize(r->hname, 110, -1);
    gtk_box_pack_start(GTK_BOX(box), r->hname, FALSE, FALSE, 0);

    r->hscale = gtk_hscale_new(GTK_ADJUSTMENT(r->adj));
    gtk_scale_set_digits(GTK_SCALE(r->hscale), 0);
    gtk_scale_set_value_pos(GTK_SCALE(r->hscale), GTK_POS_RIGHT);
    gtk_box_pack_start(GTK_BOX(box), r->hscale, TRUE, TRUE, 0);

    r->hstate = gtk_label_new("");
    /*
     * 40, NOT 34, AND THE NUMBER IS MEASURED. gdk_string_width in the
     * target's own helvetica 12:
     *
     *     MIDI    26 px
     *     muted   33 px
     *
     * 34 was sized when the column held MIDI and `idle' (20 px), and
     * "muted" arriving at 33 left ONE pixel of slack - so it clipped,
     * rendering as "nute(" on 86Box. Found in a screenshot the user
     * asked for while looking at something else entirely.
     *
     * 40 leaves seven, which covers a font marginally wider than this
     * one without reserving space for a word the column does not have.
     */
    gtk_widget_set_usize(r->hstate, 40, -1);
    gtk_box_pack_start(GTK_BOX(box), r->hstate, FALSE, FALSE, 0);

    r->hmute = vlhe_tipped(gtk_check_button_new_with_label(STR_VOL_LABEL_MUTE), STR_VOL_LABEL_MUTE_TIP);
    gtk_box_pack_start(GTK_BOX(box), r->hmute, FALSE, FALSE, 0);

    /* BOOST, AFTER MUTE - see VLHE_VOL_BOOST. Insensitive until a
     * program holds the slot, like Mute; refresh() decides. */
    r->hboost = vlhe_tipped(gtk_check_button_new_with_label(STR_VOL_LABEL_BOOST), STR_VOL_LABEL_BOOST_TIP);
    gtk_widget_set_sensitive(r->hboost, FALSE);
    gtk_box_pack_start(GTK_BOX(box), r->hboost, FALSE, FALSE, 0);

    gtk_widget_show(r->hname);
    gtk_widget_show(r->hscale);
    gtk_widget_show(r->hstate);
    gtk_widget_show(r->hmute);
    gtk_widget_show(r->hboost);
    gtk_box_pack_start(GTK_BOX(hparent), box, FALSE, FALSE, 0);

    /* ---- vertical: fader / name / mute, KMix-style --------------- */

    box = gtk_vbox_new(FALSE, 2);
    r->vcol = box;

    /* THE FIXED COLUMN WIDTH LIVES HERE, on the column rather than on
     * the label, so the fader is centred in it and every channel
     * occupies the same horizontal space whatever it is called. */
    gtk_widget_set_usize(box, VCOL_W, -1);

    /* THE MIRROR, NOT `adj' - see the struct comment. */
    r->vscale = gtk_vscale_new(GTK_ADJUSTMENT(r->vadj));
    /* THE SCALE'S OWN VALUE IS WRONG HERE AND MUST NOT BE DRAWN: it
     * would display `vadj', which is 100 - volume. So the widget's
     * value is switched off and a label above the fader shows the real
     * number, set by refresh() from the driver. */
    gtk_scale_set_draw_value(GTK_SCALE(r->vscale), FALSE);

    r->vval = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(box), r->vval, FALSE, FALSE, 0);

    /* EXPAND, with a sensible floor. The fader was fixed at 140 in a
     * panel with 200px going spare; TRUE/TRUE lets it use the height
     * the module is given, and set_usize keeps it usable when the
     * window is small. */
    gtk_widget_set_usize(r->vscale, -1, VFADER_H);
    gtk_box_pack_start(GTK_BOX(box), r->vscale, TRUE, TRUE, 0);

    /* NAME BELOW THE FADER, which is KMix's arrangement and the one
     * the user asked to see first. */
    r->vname = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(r->vname), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), r->vname, FALSE, FALSE, 0);

    /* RESERVED HEIGHT WHETHER OR NOT IT HAS TEXT. Without this the
     * one channel showing "MIDI" is a row taller than the rest and its
     * mute button sits lower than every other - visible in the first
     * vertical capture. */
    r->vstate = gtk_label_new("");
    gtk_widget_set_usize(r->vstate, -1, 16);
    gtk_box_pack_start(GTK_BOX(box), r->vstate, FALSE, FALSE, 0);

    /* AN UNLABELLED CHECKBOX under a narrow column - the word "Mute"
     * would force the column wider than the fader needs. The tooltip
     * says what it is. */
    /* TWO ROWS OF CHECKBOXES, MUTE THEN BOOST, EACH CENTRED.
     *
     * CENTRED because packed bare into the VBox a checkbox takes the
     * box's full width and its indicator sits hard left, so the
     * buttons did not line up under their faders - the first capture
     * showed one adrift from the rest.
     *
     * UNLABELLED HERE because the word would force the column wider
     * than the fader needs. THE ROWS ARE LABELLED AT THE LEFT OF THE
     * STRIP INSTEAD (see build_levels), which is the user's point:
     * "otherwise there are random buttons that only explain what they
     * are on mouse over". A tooltip is not discoverable by someone who
     * does not already suspect the control exists. */
    r->vmute = gtk_check_button_new();
        vlhe_tip(r->vmute, STR_VOL_TIP_MUTE_CHANNEL);

    align = gtk_alignment_new(0.5, 0.5, 0.0, 0.0);
    gtk_container_add(GTK_CONTAINER(align), r->vmute);
    gtk_widget_set_usize(align, -1, 20);
    gtk_box_pack_start(GTK_BOX(box), align, FALSE, FALSE, 0);
    gtk_widget_show(align);

    r->vboost = gtk_check_button_new();
    gtk_widget_set_sensitive(r->vboost, FALSE);
        vlhe_tip(r->vboost, STR_VOL_LABEL_BOOST_TIP);

    align = gtk_alignment_new(0.5, 0.5, 0.0, 0.0);
    gtk_container_add(GTK_CONTAINER(align), r->vboost);
    gtk_widget_set_usize(align, -1, 20);
    gtk_box_pack_start(GTK_BOX(box), align, FALSE, FALSE, 0);
    gtk_widget_show(align);

    gtk_widget_show(r->vval);
    gtk_widget_show(r->vscale);
    gtk_widget_show(r->vname);
    gtk_widget_show(r->vstate);
    gtk_widget_show(r->vmute);
    gtk_widget_show(r->vboost);
    gtk_box_pack_start(GTK_BOX(vparent), box, FALSE, FALSE, 0);

    /* WIRED TO THE ADJUSTMENT, NOT THE SCALES - see on_value_changed.
     * One connection serves both presentations. */
    gtk_signal_connect(r->adj, "value_changed",
                       GTK_SIGNAL_FUNC(on_value_changed), r);
    gtk_signal_connect(r->vadj, "value_changed",
                       GTK_SIGNAL_FUNC(on_vvalue_changed), r);
    gtk_signal_connect(GTK_OBJECT(r->hmute), "toggled",
                       GTK_SIGNAL_FUNC(on_mute_toggled), r);
    gtk_signal_connect(GTK_OBJECT(r->vmute), "toggled",
                       GTK_SIGNAL_FUNC(on_mute_toggled), r);
    gtk_signal_connect(GTK_OBJECT(r->hboost), "toggled",
                       GTK_SIGNAL_FUNC(on_boost_toggled), r);
    gtk_signal_connect(GTK_OBJECT(r->vboost), "toggled",
                       GTK_SIGNAL_FUNC(on_boost_toggled), r);
}

static GtkWidget *build_levels(void)
{
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *inner;
    GtkWidget *note;
    int i;

    outer = gtk_vbox_new(FALSE, 0);

    frame = gtk_frame_new(STR_VOL_FRAME_PLAYBACK_VOLUME);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, TRUE, TRUE, 0);

    inner = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(inner), 8);
    gtk_container_add(GTK_CONTAINER(frame), inner);

    /* THE TWO PRESENTATIONS, both packed, one shown. A VBox of rows
     * and an HBox of columns - see struct chanrow. */
    g_hbox_rows = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(inner), g_hbox_rows, FALSE, FALSE, 0);

    /* SPACING 8 BETWEEN COLUMNS, on top of each column's fixed width,
     * so the faders are evenly spaced with a visible gap rather than
     * abutting.
     *
     * A CAPTION COLUMN COMES FIRST, naming the two checkbox rows. The
     * user's requirement: the vertical presentation's checkboxes carry
     * no text of their own (the words would force every column wider
     * than its fader), and a tooltip is not discoverable - "otherwise
     * there are random buttons that only explain what they are on
     * mouse over". One caption per ROW costs a single column instead
     * of widening all of them. */
    g_vbox_cols = gtk_hbox_new(FALSE, 8);

    /* EXPAND, unlike the horizontal rows above.
     *
     * THE FADERS WERE ALREADY PACKED TRUE/TRUE AND STILL DID NOT GROW,
     * because expansion only propagates if EVERY container above
     * allows it - and this box was FALSE/FALSE, so the faders were
     * free to expand inside a box that was told never to. The visible
     * result was short faders with the panel's lower half empty.
     *
     * The horizontal rows stay FALSE: a row of hscales has a natural
     * height and stretching it would space the rows out rather than
     * make anything more usable. A vertical fader is the opposite -
     * its length IS its resolution. */
    gtk_box_pack_start(GTK_BOX(inner), g_vbox_cols, TRUE, TRUE, 0);

    {
        GtkWidget *cap;
        GtkWidget *lab;

        cap = gtk_vbox_new(FALSE, 2);

        /* SPACERS MATCHING THE COLUMN ABOVE the checkbox rows: the
         * value label, the fader, the name and the state line. The
         * captions have to sit level with the buttons they name, and
         * the fader's spacer must expand exactly as the fader does. */
        lab = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(cap), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        /* EXPANDS EXACTLY AS THE FADERS DO, so the Mute and Boost
         * captions stay level with the buttons they name however tall
         * the panel gets. */
        lab = gtk_label_new("");
        gtk_widget_set_usize(lab, -1, VFADER_H);
        gtk_box_pack_start(GTK_BOX(cap), lab, TRUE, TRUE, 0);
        gtk_widget_show(lab);

        lab = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(cap), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        lab = gtk_label_new("");
        gtk_widget_set_usize(lab, -1, 16);
        gtk_box_pack_start(GTK_BOX(cap), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        lab = gtk_label_new(STR_VOL_LABEL_MUTE);
        gtk_misc_set_alignment(GTK_MISC(lab), 1.0, 0.5);
        gtk_widget_set_usize(lab, -1, 20);
        gtk_box_pack_start(GTK_BOX(cap), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        lab = gtk_label_new(STR_VOL_LABEL_BOOST);
        gtk_misc_set_alignment(GTK_MISC(lab), 1.0, 0.5);
        gtk_widget_set_usize(lab, -1, 20);
        gtk_box_pack_start(GTK_BOX(cap), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        gtk_box_pack_start(GTK_BOX(g_vbox_cols), cap, FALSE, FALSE, 0);
        gtk_widget_show(cap);
    }

    g_nrow = VLHE_MAX_CHAN;
    for (i = 0; i < g_nrow; i++) {
        memset(&g_row[i], 0, sizeof g_row[i]);
        build_row(&g_row[i], g_hbox_rows, g_vbox_cols);
    }

    /* SAY WHOSE MASTER IS WHOSE. Someone reaching for these expecting
     * them to control the card will otherwise wonder why KMix
     * disagrees.
     *
     * WRAPPED, not one long line: at 622px of pane the single-line
     * version was clipped mid-word. */
    note = gtk_label_new(
        STR_VOL_LABEL_THESE_ARE_PER_PROGRAM);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    /* TOP-ALIGNED within its own allocation, and packed FALSE so the
     * slack goes to the faders rather than to the gap above this. */
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_end(GTK_BOX(inner), note, FALSE, FALSE, 4);
    gtk_widget_show(note);

    gtk_widget_show(inner);
    gtk_widget_show(frame);

    /* THE ABSENT-MODULE STATE, built now and shown by refresh(). */
    g_nomodule = gtk_label_new(STR_VOL_LABEL_VSOUND_MODULE_NOT_LOADED);
    gtk_box_pack_start(GTK_BOX(outer), g_nomodule, TRUE, TRUE, 0);

    /* AND THE LOADED-BUT-IDLE STATE. refresh() chooses between the
     * three; see its comment for why this is not an error. */
    g_noclients = gtk_label_new(STR_VOL_LABEL_NOTHING_USING_VSOUND_YET);
    /* WRAPPED, AND STILL CENTRED - unwrapped it is 727 px under the
     * target's font, wider than the pane (2026-10-07). */
    vlhe_layout_wrap(g_noclients);
    gtk_label_set_justify(GTK_LABEL(g_noclients), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), g_noclients, TRUE, TRUE, 0);

    g_levels_frame = frame;

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */

/*
 * NEITHER OPTIONS CHECKBOX ACTS UNTIL APPLY - 2026-09-19.
 *
 * Both used to take effect the instant they were clicked, which made
 * them the only controls on a page WITH the commit trio that did not
 * wait for it. design/32 section 12 recorded the vertical one as an
 * open question ("the awkward case is MIXING them on one tab: a live
 * checkbox beside controls that wait is the inconsistency users
 * notice") and left it undecided until something else landed on
 * Options. Something did - the mixer-restore setting - and the user
 * reported the behaviour as a bug, which settles it.
 *
 * So these handlers only mark the page dirty now; volume_collect()
 * reads them when Apply runs.
 */
/* Mark the Options tab dirty and tell the shell, so the sidebar and
 * the tab label get their asterisk - the same contract Sound has. */
static void mark_options_dirty(void)
{
    GtkWidget *page;

    if (g_opt_dirty)
        return;
    g_opt_dirty = 1;

    if (g_notebook != NULL) {
        page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), 1);
        if (page != NULL)
            gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(g_notebook),
                                            page, STR_VOL_TAB_OPTIONS_DIRTY);
    }
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

static void clear_options_dirty(void)
{
    GtkWidget *page;

    if (!g_opt_dirty)
        return;
    g_opt_dirty = 0;

    if (g_notebook != NULL) {
        page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), 1);
        if (page != NULL)
            gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(g_notebook),
                                            page, STR_VOL_LABEL_OPTIONS);
    }
    if (g_dirty_cb != NULL)
        g_dirty_cb();
}

static void on_options_toggled(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (g_loading)
        return;
    mark_options_dirty();
}



static GtkWidget *build_options(void)
{
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *w;
    GtkWidget *note;

    outer = gtk_vbox_new(FALSE, 0);

    /* --- appearance ------------------------------------------------ */

    frame = gtk_frame_new(STR_VOL_FRAME_APPEARANCE);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* A PREFERENCE, WHICH IS WHY IT IS HERE and not in a View menu.
     * The user's call, 2026-09-17. It is set once to taste and then
     * left alone, which is exactly what this tab is for - and a user
     * hunting for it will look in the module's own options before they
     * think to open a menu.
     *
     * IT COSTS NOTHING TO SWITCH. Both scales already exist for every
     * channel and share one GtkAdjustment, so this shows one and hides
     * the other. No value moves and nothing is rebuilt. */
    w = vlhe_tipped(gtk_check_button_new_with_label(STR_VOL_CHECK_VERTICAL_VOLUME_SLIDERS), STR_VOL_CHECK_VERTICAL_VOLUME_SLIDERS_TIP);
    /* FROM THE SAVED SETTING, NOT g_vertical - design/47 V1. At build
     * g_vertical is still 0 (the shell applies the saved value to the
     * faders a moment later, vlhe_cc.c), so this box started unticked
     * whatever the config said, and the first OK then saved 0 and
     * flipped vertical faders horizontal. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                 vlhe_ui_vertical_sliders() ? TRUE : FALSE);
    g_opt_vertical = w;
    gtk_signal_connect(GTK_OBJECT(w), "toggled",
                       GTK_SIGNAL_FUNC(on_options_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    note = gtk_label_new(
        STR_VOL_LABEL_OFF_DRAWS_FADERS_HORIZONTALLY);
    /* LEFT, EXPLICITLY. GTK 1.2 centres a multi-line label by default,
     * and setting the misc alignment moves the BLOCK without changing
     * how the lines sit within it - so the text came out centred
     * inside a left-aligned box. */
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 4);
    gtk_widget_show(note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    /* --- the card's mixer ------------------------------------------ */

    frame = gtk_frame_new(STR_VOL_FRAME_CARD_MIXER_LEVELS);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* OFF BY DEFAULT - design/09. Nobody on this system has ever had
     * levels persist, so on-by-default would surprise; off is exactly
     * today's behaviour. */
    w = vlhe_tipped(gtk_check_button_new_with_label(
            STR_VOL_CHECK_SAVE_RESTORE_SOUND_CARD), STR_VOL_CHECK_SAVE_RESTORE_SOUND_CARD_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                 vlhe_mixer_restore_enabled() ? TRUE : FALSE);
    g_opt_restore = w;
    gtk_signal_connect(GTK_OBJECT(w), "toggled",
                       GTK_SIGNAL_FUNC(on_options_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    /* THE REASON, because the behaviour is otherwise mysterious: the
     * levels reset on every module load, not merely every boot, and
     * nothing else on this platform saves them - KMix 1.1 cannot. */
    note = gtk_label_new(
        STR_VOL_LABEL_CARD_S_LEVELS_RESET);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /* FLOWED - one paragraph, wrapped at the pane's text width
     * (vlhe_layout.c). Hand breaks made it look fixed. */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 4);
    gtk_widget_show(note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    /* --- each program's level - design/33 1c, 2026-10-02 ---------- */

    frame = gtk_frame_new(STR_VOL_FRAME_PROGRAM_LEVELS);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* OFF BY DEFAULT, like the mixers above. */
    w = vlhe_tipped(gtk_check_button_new_with_label(
            STR_VOL_CHECK_SAVE_PROGRAM_LEVELS),
            STR_VOL_CHECK_SAVE_PROGRAM_LEVELS_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                 vlhe_progvol_enabled() ? TRUE : FALSE);
    g_opt_progvol = w;
    gtk_signal_connect(GTK_OBJECT(w), "toggled",
                       GTK_SIGNAL_FUNC(on_options_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    note = gtk_label_new(STR_VOL_LABEL_PROGRAM_LEVELS_NOTE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 4);
    gtk_widget_show(note);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */

void volume_sync_visibility(void)
{
    /* THE ORIENTATION AND THE ROW STATES ARE BOTH VISIBILITY, and
     * both are undone by the shell's show_all. refresh() re-decides
     * which rows exist and whether the module is present; this
     * re-decides which slider is drawn. */
    volume_set_vertical(g_vertical);
    refresh();
}

void volume_set_vertical(int vertical)
{
    g_vertical = vertical ? 1 : 0;

    /* THE CHECKBOX FOLLOWS THE FADERS - V1's other half: the shell
     * calls this at startup with the saved value, and the box must
     * show it. Under g_loading so the set does not read as an edit;
     * setting a toggle to the state it already has fires nothing. */
    if (g_opt_vertical != NULL) {
        int was = g_loading;

        g_loading = 1;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_vertical),
                                     g_vertical ? TRUE : FALSE);
        g_loading = was;
    }

    /* SWAP THE CONTAINERS, not the widgets inside them. The two
     * presentations are separate trees over one set of adjustments -
     * see struct chanrow for why that is the only arrangement that
     * produces a real vertical mixer. */
    if (g_hbox_rows != NULL) {
        if (g_vertical)
            gtk_widget_hide(g_hbox_rows);
        else
            gtk_widget_show(g_hbox_rows);
    }
    if (g_vbox_cols != NULL) {
        if (g_vertical)
            gtk_widget_show(g_vbox_cols);
        else
            gtk_widget_hide(g_vbox_cols);
    }

    /* refresh() decides which individual rows/columns are visible
     * within the shown container - a free slot past the channel count
     * must stay hidden either way. */
    refresh();
}

int volume_get_vertical(void)
{
    return g_vertical;
}

/* The notebook changed page. GTK 1.2 gives the page NUMBER here, which
 * is all we need. */
static void on_switch_page(GtkNotebook *nb, GtkNotebookPage *pg,
                           guint page, gpointer data)
{
    (void)nb;
    (void)pg;
    (void)data;

    g_page = (int)page;

    if (g_page_cb != NULL)
        g_page_cb(g_page == PAGE_OPTIONS);
}

void volume_set_page(int page)
{
    if (g_notebook != NULL)
        gtk_notebook_set_page(GTK_NOTEBOOK(g_notebook), page);
}

/*
 * READ THE OPTIONS WIDGETS. Apply and OK call this.
 *
 * ONLY THE OPTIONS TAB HAS ANYTHING TO COMMIT. Levels is LIVE - every
 * drag is already an ioctl (design/32 section 8) - so there is
 * nothing held back there.
 */
int volume_collect(void)
{
    if (g_opt_vertical != NULL) {
        int on = GTK_TOGGLE_BUTTON(g_opt_vertical)->active ? 1 : 0;

        /* BOTH HALVES, and only the first used to happen. The redraw
         * is the visible effect; the setter is what survives a
         * restart. VerticalSliders is a USER setting, so this is also
         * the call that marks ~/.vlhe/vlhe.conf dirty - without it
         * Apply wrote /etc/vlhe.conf alone and the user's file was
         * never touched. */
        volume_set_vertical(on);
        vlhe_ui_set_vertical_sliders(on);
    }

    if (g_opt_restore != NULL)
        vlhe_mixer_set_restore(GTK_TOGGLE_BUTTON(g_opt_restore)->active);
    if (g_opt_progvol != NULL)
        vlhe_progvol_set_enabled(
            GTK_TOGGLE_BUTTON(g_opt_progvol)->active ? 1 : 0);

    clear_options_dirty();
    return 0;
}

/* PUT THEM BACK - Cancel's half. */
void volume_reload(void)
{
    g_loading = 1;

    /* THE SAVED VALUE, NOT THE LIVE ONE. Cancel means "forget what I
     * typed", so the checkbox goes back to what the config holds -
     * and the display follows it, since an uncommitted Apply-less
     * toggle may already have redrawn. volume_get_vertical() would
     * return that uncommitted state and Cancel would restore the edit
     * it was meant to discard. */
    if (g_opt_vertical != NULL) {
        int saved = vlhe_ui_vertical_sliders();

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_vertical),
                                     saved ? TRUE : FALSE);
        volume_set_vertical(saved);
    }
    if (g_opt_restore != NULL)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(g_opt_restore),
            vlhe_mixer_restore_enabled() ? TRUE : FALSE);
    if (g_opt_progvol != NULL)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(g_opt_progvol),
            vlhe_progvol_enabled() ? TRUE : FALSE);

    g_loading = 0;
    clear_options_dirty();
}

/* Does this page hold unsaved edits? */
int volume_dirty(void)
{
    return g_opt_dirty;
}

void volume_set_dirty_cb(void (*cb)(void))
{
    g_dirty_cb = cb;
}

int volume_page_wants_buttons(void)
{
    return g_page == PAGE_OPTIONS;
}

GtkWidget *volume_build(void (*report_fn)(const char *),
                        void (*page_fn)(int))
{
    GtkWidget *nb;
    GtkWidget *tab;

    g_report = report_fn;
    g_page_cb = page_fn;
    g_tips = gtk_tooltips_new();

    nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);

    tab = gtk_label_new(STR_VOL_LABEL_LEVELS);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_levels(), tab);

    tab = gtk_label_new(STR_VOL_LABEL_OPTIONS);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_options(), tab);

    gtk_signal_connect(GTK_OBJECT(nb), "switch_page",
                       GTK_SIGNAL_FUNC(on_switch_page), NULL);
    g_notebook = nb;

    /* THE SAVED ORIENTATION, NOT "horizontal by default" - the real
     * half of design/47 V1, found on 86Box 2026-10-01. Pages are built
     * LAZILY, on first visit; the shell applies the saved value at
     * startup only to a page that already exists, so a Volume page
     * opened later came up horizontal with the box clear whatever was
     * saved. The -V flag still wins afterwards through the shell. */
    volume_set_vertical(vlhe_ui_vertical_sliders());

    /* FIRST READ NOW, not at the first poll, so the panel is populated
     * the moment it is shown rather than up to 250ms later. */
    g_seen_gen = vlhe_generation();
    g_seen_present = vlhe_sound_present();
    refresh();

    /* NOT STARTED HERE - volume_set_active(1) starts it when the page
     * is shown, and the shell shows the start page after building. */
    g_poll_tag = 0;

    return nb;
}

/*
 * SHOWN OR HIDDEN - the shell's word, from show_module(). The poll runs
 * only while shown (design/47 V2); a show refreshes at once so the
 * page is never older than the moment it appeared.
 */
void volume_set_active(int active)
{
    if (active) {
        if (!g_active)
            refresh();
        if (g_poll_tag == 0)
            g_poll_tag = gtk_timeout_add(POLL_MS, on_poll, NULL);
    } else if (g_poll_tag != 0) {
        gtk_timeout_remove(g_poll_tag);
        g_poll_tag = 0;
    }
    g_active = active ? 1 : 0;
}

/*
 * THE MACHINE CHANGED - a plan ran, in either direction. The shell
 * broadcasts this to every page that registers for it and this page
 * never had (vlhe_cc.c's module table left the slot NULL), so the one
 * page whose whole content is the machine learned of a Load only from
 * its poll - which, with the generation at 0, could not see it.
 */
void volume_machine(void)
{
    refresh();
}
