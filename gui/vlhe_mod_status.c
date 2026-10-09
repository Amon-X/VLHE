/*
 * vlhe_mod_status.c - what is loaded, what is running, and restarts.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE for the full license text.
 *
 * FIRST IN THE SIDEBAR, above Volume, and that position is the
 * argument for the page existing at all: it is the answer to "is
 * anything wrong?", which is the question someone opens a control
 * centre with when sound has stopped working. Every other page assumes
 * its module is loaded and shows an apology when it is not; this is
 * where that fact belongs and where the fix is.
 *
 * ONE RESTART CONTROL FOR ALL THREE DAEMONS, because design/09 asks
 * for one and says it is worth solving once:
 *
 *   "If vmidi wedges... the slot stays claimed and MIDI stays dead
 *    until the module is unloaded, WHICH TAKES THE WHOLE AUDIO PATH
 *    DOWN WITH IT. The same problem exists for vsoundd and vdiscd."
 *
 * A button in each module's own tab would be three places to look for
 * one class of action, in two modules whose settings have nothing to
 * do with it.
 *
 * WHAT THIS PAGE CANNOT DO, and must not imply: tell a WEDGED daemon
 * from a quiet one. design/09 wants that and says it needs the
 * counters, which do not exist. "Running" here means the process is
 * there. The page says as much rather than showing a green light.
 *
 * C89, GCC 2.95.2, GTK 1.2.
 */

#include <gtk/gtk.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include <unistd.h>

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_layout.h"
#include "vlhe_tip.h"
#include "vlhe_conf.h"       /* vlhe_tmp_open */
#include "vlhe_priv.h"       /* vlhe_priv_can_act */
#include "vlhe_apply.h"
#include "vlhe_journal.h"
#include "vlhe_filter.h"      /* vlhe_filter_attach */
#include "vlhe_buttons.h"     /* vlhe_buttons_equalise */
#include "vlhe_mod_status.h"

/* SLOWER THAN THE VOLUME POLL. Nothing here changes on a human
 * timescale unless someone acts, and the whole page is rebuilt on a
 * refresh - so two seconds is frequent enough to notice a daemon dying
 * and cheap enough to ignore. */
#define POLL_MS 2000

struct comprow {
    GtkWidget *name;
    GtkWidget *state;
    GtkWidget *detail;
    GtkWidget *load;
    GtkWidget *apply;
    GtkWidget *restart;
    char       id[VLHE_NAME_MAX];
};

static struct comprow  g_row[VLHE_MAX_COMPONENTS];
static int             g_nrow;
static GtkWidget      *g_summary;
static GtkWidget      *g_load_btn;
static GtkWidget      *g_unload_btn;

/*
 * ---- WHAT A LOAD INCLUDES ---------------------------------------
 *
 * THREE CHECKBOXES BESIDE Load, AND THEY ARE LIVE - the user,
 * 2026-09-26: "I think for now live write on click since this helps
 * with troubleshooting. I can try a load of just vmidi and just
 * vsound etc etc."
 *
 * THAT IS THE WHOLE PURPOSE: trying combinations. The setting existed
 * and was honoured by plan_load() since 2026-09-22, with no way to
 * reach it from the GUI - design/36 section 2b item 5, "keys exist,
 * plan honours them, checkbox missing".
 *
 * LIVE RATHER THAN THE Apply/dirty FLOW, DELIBERATELY AND AGAINST THE
 * REST OF THE PROGRAM. This page is a MOD_VIEW (vlhe_cc.c:209): it
 * has no collect, no dirty and no OK/Apply/Cancel, so a setting here
 * in the normal flow would have no button to commit it. The three
 * ways out were a commit-on-Load hook, promoting the page to
 * MOD_SETTINGS, or writing on the click; the user chose the last for
 * troubleshooting, where the value of ticking a box and immediately
 * pressing Load outweighs the consistency.
 *
 * ROW 64 IS THE CASE AGAINST IT and is worth naming rather than
 * hiding: the CD tab's `load at startup' box writes on the click too,
 * against a stub backend, and lies on the target every time. The
 * difference is that THESE accessors are real and tested on both
 * backends - vlhe_component_enabled() is read by plan_load() at nine
 * sites and by the Status page's own refresh().
 *
 * `create = 0' ON THE COMMIT, so a machine with no config file writes
 * nothing and still succeeds. The value lives in memory and Load
 * reads it from there, which is the no-config session model
 * (design/33 section 1e) - and it means portable mode works without
 * the click quietly creating /etc/vlhe.conf.
 *
 * AND THEY AFFECT LOADING ONLY. The user's own condition, same
 * message: "It only affects loading. It shouldnt affect unloading."
 * That was NOT true when they asked - plan_unload() shared in_plan()
 * with the load side, so unticking a component after loading it left
 * the module in the kernel with no `rmmod' ever planned. Fixed in the
 * same change; see in_scope() in vlhe_apply.c.
 */
static GtkWidget      *g_en[3];
static int             g_en_setting;    /* set = do not act on toggled */
static GtkWidget      *g_notready;
static GtkWidget      *g_cdromline;
static void          (*g_report)(const char *);

/*
 * THE UNFINISHED-UNLOAD BANNER - design/54 7h Stage 2c, as decided
 * 2026-10-03. Shown while a load that was never unloaded has records
 * still in effect (vlhe_apply_leftover()), with Finish the unload and
 * Not now. "Not now" hides it for THIS GUI SESSION only and Load stays
 * refused; pressing Load shows it again (g_left_hidden cleared). The
 * flag is also cleared when nothing is left over, so a later leftover
 * is shown afresh.
 */
static GtkWidget      *g_left_frame;
static GtkWidget      *g_left_row;      /* the leftover's label and buttons */
static GtkWidget      *g_left_label;
static GtkWidget      *g_left_finish;
static int             g_left_hidden;

/*
 * AND WHAT NEEDS ATTENTION BESIDE IT - design/54 7h Stage 3.5c, made ONE
 * LINE on 2026-10-04 (the user: "There was barely enough room on the
 * status page for the first banner and now there was more added").
 * A finding blocks nothing - the Load carries on - so it gets a line
 * and a Review... button, and the findings with their buttons are in a
 * dialog; the leftover, which DOES block Load, keeps its row. The frame
 * takes "Needs attention" as its title only while a finding shows, so
 * the leftover banner alone is as it was. NOT EVERY POLL: finding things
 * can open a dsp node and run `modprobe -c', so they are worked out
 * when the page is shown, after anything this page runs, and every
 * FIND_EVERY polls otherwise (g_find_due counts down).
 */
#define FIND_MAX    8
#define FIND_EVERY  15
static GtkWidget          *g_rev_row;
static GtkWidget          *g_rev_label;
static struct vlhe_finding g_find[FIND_MAX];
static int                 g_nfind;
static int                 g_find_due;

/* Defined below with on_row_load; declared here because refresh()
 * asks it whether a row has a Load button to show. */
static int row_scope(const char *id);

static void report(const char *m)
{
    if (g_report != NULL)
        g_report(m);
}

/* A PROBLEM NAMES THE JOURNAL - design/54 7h Stage 4: the status line
 * says what went wrong, and where the full account of it is. */
static void report_problem(const char *m)
{
    char buf[700];

    sprintf(buf, FMT_STATUS_SEE_JOURNAL, m, vlhe_journal_path());
    report(buf);
}

/* ------------------------------------------------------------------ */

static void refresh(void)
{
    struct vlhe_component c[VLHE_MAX_COMPONENTS];
    int n, i, down = 0;
    char msg[128];

    n = vlhe_components(c, VLHE_MAX_COMPONENTS);

    for (i = 0; i < g_nrow; i++) {
        if (i >= n) {
            gtk_widget_hide(g_row[i].name);
            gtk_widget_hide(g_row[i].state);
            gtk_widget_hide(g_row[i].detail);
            gtk_widget_hide(g_row[i].load);
            gtk_widget_hide(g_row[i].apply);
            gtk_widget_hide(g_row[i].restart);
            continue;
        }

        strncpy(g_row[i].id, c[i].name, VLHE_NAME_MAX - 1);
        g_row[i].id[VLHE_NAME_MAX - 1] = '\0';

        gtk_label_set_text(GTK_LABEL(g_row[i].name), c[i].name);

        /* THE WORDS DIFFER BY KIND, because "loaded" and "running"
         * are different facts and a user reading the page should not
         * have to know which things are modules. */
        if (!c[i].present) {
            gtk_label_set_text(GTK_LABEL(g_row[i].state),
                               c[i].kind == VLHE_COMP_MODULE
                                   ? STR_STATUS_TEXT_NOT_LOADED : STR_STATUS_TEXT_NOT_RUNNING);
            down++;
        } else if (c[i].kind == VLHE_COMP_MODULE) {
            gtk_label_set_text(GTK_LABEL(g_row[i].state), STR_STATUS_TEXT_LOADED);
        } else {
            char st[32];

            sprintf(st, "running (%d)", c[i].pid);
            gtk_label_set_text(GTK_LABEL(g_row[i].state), st);
        }

        gtk_label_set_text(GTK_LABEL(g_row[i].detail), c[i].detail);

        /* A MODULE GETS NO BUTTONS AT ALL - not greyed ones. The
         * user's call: "if they cant be restarted from that page do
         * they need a button at all". A greyed button is a control
         * offered only to be refused, and a screen reader announcing
         * "Restart, unavailable" three times is worse than silence.
         * The paragraph below the list says why. */
        if (c[i].can_restart)
            gtk_widget_show(g_row[i].restart);
        else
            gtk_widget_hide(g_row[i].restart);

        /*
         * LOAD GOES ON THE MODULE ROW, NOT THE DAEMON'S.
         *
         * Both rows of a pair map to the same scope (row_scope()),
         * so a button on each would be two controls doing one
         * thing - and the daemon row already has Restart, which is
         * what you want once it IS running. The module row has no
         * button at all today, which is where the user asked for
         * this one and where it has something to say.
         *
         * AND IT IS THE HONEST ROW FOR IT. Loading the scope
         * insmods the module and starts the daemon; the row that
         * names the module is the one whose state the press
         * changes first, and the one whose "not loaded" the user
         * is looking at when they reach for it.
         *
         * HIDDEN, NOT GREYED, when the component is already up -
         * the same reasoning as Restart above: a control offered
         * only to be refused is worse than no control. Pressing
         * Load on something already loaded would plan nothing and
         * report a confusing "nothing to do".
         */
        if (c[i].kind == VLHE_COMP_MODULE && !c[i].present
            && row_scope(c[i].name) >= 0)
            gtk_widget_show(g_row[i].load);
        else
            gtk_widget_hide(g_row[i].load);

        /*
         * APPLY SETTINGS - ON THE SYNTH'S ROW, AND ONLY WHILE IT IS
         * RUNNING.
         *
         * `vmidid' is the only daemon with settings a running
         * process can adopt - six of them, through its control
         * channel (design/43 6b). vdiscd's are structural and
         * vsoundd's mix rate needs no channels open, so a button on
         * those rows would be one that always reported nothing to
         * do.
         *
         * AND NOT WHEN IT IS STOPPED: there is nothing to apply to,
         * and the press would only ever produce the "not running"
         * message. Hidden rather than greyed, as Restart is.
         */
        if (c[i].present && strcmp(c[i].name, "vmidid") == 0)
            gtk_widget_show(g_row[i].apply);
        else
            gtk_widget_hide(g_row[i].apply);

        /*
         * AND THE ROW'S BUTTONS FOLLOW PRIVILEGE, LIKE THE BOTTOM PAIR
         * - design/49 T1. They were never greyed: before Modify the
         * six stayed live and each press failed with a message, while
         * the Load/Unload pair below was grey with a sidebar note
         * saying why. The same rule, in the same refresh, so Modify
         * re-applies it the same way: CAN ACT, not "is euid 0 now".
         */
        {
            gint on = vlhe_priv_can_act() ? TRUE : FALSE;

            gtk_widget_set_sensitive(g_row[i].load, on);
            gtk_widget_set_sensitive(g_row[i].apply, on);
            gtk_widget_set_sensitive(g_row[i].restart, on);
        }

        gtk_widget_show(g_row[i].name);
        gtk_widget_show(g_row[i].state);
        gtk_widget_show(g_row[i].detail);
    }

    /* ONE LINE SAYING WHETHER TO WORRY, because six rows of detail
     * answer "what is the state" and not "is anything wrong" - which
     * is the question the page is for. */
    if (down == 0)
        strcpy(msg, FMT_STATUS_EVERYTHING_LOADED_RUNNING);
    else if (down == 1)
        strcpy(msg, FMT_STATUS_ONE_COMPONENT_NOT_RUNNING);
    else
        sprintf(msg, FMT_STATUS_COMPONENTS_ARE_NOT_RUNNING, down);

    if (g_summary != NULL)
        gtk_label_set_text(GTK_LABEL(g_summary), msg);

    /*
     * WHAT A LOAD WOULD FAIL ON, SAID BEFORE IT IS PRESSED.
     *
     * The user's ask, 2026-09-23: "There should be a message no snd
     * card specified loading wont work got sound settings or
     * something like that same with vmidid and the sound font".
     *
     * BOTH ARE SETTINGS ON OTHER PAGES, which is the whole reason
     * this belongs here. A user on the Status page sees two buttons
     * and no indication that pressing one will half-work; the
     * information they need is two pages away and they have no
     * reason to suspect it. Naming the page is the actionable part -
     * "not configured" without a destination is a dead end.
     *
     * GATED ON THE COMPONENT BEING ENABLED, each independently. A
     * machine running vdisc alone has no card and wants no font, and
     * warning about either would be noise about a component the user
     * has deliberately turned off.
     *
     * A WARNING, NOT A REFUSAL - and deliberately, for the same
     * reason confirm_space() warns rather than blocks (design/32
     * section 11): the check can be wrong. The user may be about to
     * fix the font in the dialog the load prints. The buttons stay
     * live.
     *
     * THE CARD-MODULE HALF OF THIS COMMENT WENT WITH ITS WARNING,
     * 2026-09-25. It explained that an unnamed module "matters only
     * when something HOLDS DEVICE 0, which is the legacy case" -
     * true of the displacement design and false of the one that
     * replaced it, where nothing contests device 0 at all. Only the
     * font check remains.
     */
    {
        struct vlhe_font  f[VLHE_MAX_FONTS];
        char  warn[1024];
        int   nf;

        warn[0] = '\0';

        /*
         * THE CARD-MODULE WARNING IS GONE, 2026-09-25 - THE USER:
         * "the status page still warns a user to pick their sound
         * card and module the old fields that are no loner needed".
         *
         * IT SAID: "No sound card module is named - the load will
         * stop if something holds device 0.  See Sound Settings."
         *
         * EVERY CLAUSE OF THAT IS NOW FALSE. The symlink design
         * (design/38 section 9) does not contest device 0 and does
         * not unload the card: vsound takes whatever node it is
         * given and `/dev/dsp' is pointed at it. There is no
         * device-0 contest left to lose, so a blank `CardModule'
         * stops nothing.
         *
         * AND THE REFUSAL IT POINTED AT NO LONGER EXISTS. plan_load()
         * used to stop with "name the card's module in Sound
         * Settings, or rmmod it yourself, then re-run"; that string
         * is in no source file today. The warning was describing a
         * failure mode that had been deleted.
         *
         * WORSE THAN NOISE: IT SENDS THE USER TO A CONTROL THAT IS
         * ALSO GONE. "See Sound Settings" meant the Card module
         * field, which `04ae294' replaced with "Programs use". A user
         * following this advice looks for a box that is not there.
         *
         * `CardModule' IS NOW DEAD CONFIG. This was its last reader -
         * `vlhe_backend.c' still parses the key and the fake backend
         * still stores it, but nothing acts on it. Removing the key
         * itself is a separate job (design/36's 2b list) because a
         * config that still carries the line must not be broken by
         * dropping it.
         *
         * THE FONT WARNING BELOW STAYS AND IS THE MODEL for what a
         * warning here should be: it names a thing no detection can
         * supply, and vmidid genuinely cannot play without it.
         */

        /*
         * THE FONT. vmidid has nothing to play without one, so this
         * is the harder of the two: no detection can supply it.
         */
        if (vlhe_component_enabled(VLHE_ENABLE_MIDI)) {
            nf = vlhe_fonts_effective(f, VLHE_MAX_FONTS, NULL);
            if (nf <= 0) {
                if (warn[0] != '\0')
                    strcat(warn, "\n");
                strcat(warn,
                    FMT_STATUS_NO_SOUNDFONT_SET);
            } else if (f[0].bank != 0) {
                /* A SONG FONT WITH NO GENERAL MIDI FONT UNDER IT - the
                 * list is packed, so it is f[0], at bank 1, and
                 * vmidid would take it as its base font and refuse
                 * (2026-10-06, the documentation review's finding 1). */
                if (warn[0] != '\0')
                    strcat(warn, "\n");
                strcat(warn, STR_STATUS_SONG_FONT_NO_GM);
            }
        }

        /*
         * AND WHERE /dev/cdrom POINTS, SHOWN ALWAYS.
         *
         * THE USER'S COMPLAINT WAS FORGETTING IT WAS SET - "I have
         * run into times where I forgot it was set and had to swap it
         * back to my real drive" - so the VISIBILITY is the fix here
         * rather than the automation. A symlink is invisible state
         * that outlives the session that made it.
         *
         * NOT CONDITIONAL ON OUR HAVING REDIRECTED IT. If something
         * else has taken the link back - /sbin/fooze after a hardware
         * change, or the user's own hand - a conditional line would
         * vanish at exactly the moment it became interesting. With
         * the saved target on disk beside the live one, that case is
         * visible instead.
         */
        if (g_cdromline != NULL) {
            struct vlhe_cdrom_link cl;
            char line[VLHE_PATH_MAX * 2 + 96];

            if (vlhe_cdrom_link(&cl) != 0) {
                gtk_widget_hide(g_cdromline);
            } else if (cl.kind == VLHE_CDROM_SYMLINK && cl.is_ours) {
                if (cl.saved[0] != '\0')
                    sprintf(line, FMT_STATUS_DEV_CDROM_WAS,
                            cl.via[0] ? cl.via : cl.target, cl.saved);
                else
                    sprintf(line, FMT_STATUS_DEV_CDROM,
                            cl.via[0] ? cl.via : cl.target);
                gtk_label_set_text(GTK_LABEL(g_cdromline), line);
                gtk_widget_show(g_cdromline);
            } else if (cl.saved[0] != '\0') {
                /* WE CHANGED IT AND SOMETHING CHANGED IT BACK. The
                 * one case a conditional line would hide. */
                sprintf(line, FMT_STATUS_DEV_CDROM_NOT_OURS,
                        cl.kind == VLHE_CDROM_SYMLINK && cl.target[0]
                            ? cl.target : STR_STATUS_TEXT_SOMETHING_ELSE,
                        cl.saved);
                gtk_label_set_text(GTK_LABEL(g_cdromline), line);
                gtk_widget_show(g_cdromline);
            } else {
                gtk_widget_hide(g_cdromline);
            }
        }

        /*
         * SETTINGS NOT YET APPLIED - a module running with other settings
         * than the ones saved, which needs reloading. These lived on each
         * Options tab and moved here, 2026-10-07 (the user: "those belong
         * on the status page"). Only when a reload is needed: settings
         * that match are not news.
         */
        {
            struct vlhe_sound     sn;
            struct vlhe_midiopts  mo;
            struct vlhe_modopts   mp;
            char  line[200];

            line[0] = '\0';
            if (vlhe_sound(&sn) == 0 && sn.loaded
                && sn.midi_slot != sn.midi_slot_applied) {
                sprintf(line, FMT_STATUS_RELOAD_VSOUND,
                        sn.midi_slot_applied ? STR_SND_TEXT_WITH
                                             : STR_SND_TEXT_WITHOUT);
                if (warn[0] != '\0')
                    strcat(warn, "\n");
                strcat(warn, line);
            }
            if (vlhe_midiopts(&mo) == 0 && mo.loaded
                && mo.minor != mo.minor_applied) {
                sprintf(line, FMT_STATUS_RELOAD_VMIDI, mo.minor_applied);
                if (warn[0] != '\0')
                    strcat(warn, "\n");
                strcat(warn, line);
            }
            if (vlhe_modopts(&mp) == 0 && mp.loaded) {
                line[0] = '\0';
                if (mp.ndevs != mp.ndevs_applied)
                    sprintf(line, FMT_STATUS_RELOAD_VDISC, mp.ndevs_applied,
                            mp.ndevs_applied == 1 ? "" : STR_CD_TEXT_PLURAL_S);
                else if (mp.major != mp.major_applied
                         || (mp.packet_applied >= 0
                             && mp.packet != mp.packet_applied))
                    strcpy(line, STR_STATUS_RELOAD_VDISC_OTHER);
                if (line[0] != '\0') {
                    if (warn[0] != '\0')
                        strcat(warn, "\n");
                    strcat(warn, line);
                }
            }
        }

        if (g_notready != NULL) {
            if (warn[0] != '\0') {
                gtk_label_set_text(GTK_LABEL(g_notready), warn);
                gtk_widget_show(g_notready);
            } else {
                gtk_widget_hide(g_notready);
            }
        }
    }

    /*
     * THE BUTTONS FOLLOW PRIVILEGE, checked on every refresh rather
     * than once: the setuid build can GAIN root mid-session through
     * Modify, and a control that stayed grey after that would be
     * wrong in the direction a user cannot work around.
     *
     * GREYED RATHER THAN HIDDEN, unlike the per-module Restart. The
     * user's reasoning there was that a module can NEVER be restarted
     * from this page, so the button is offered only to be refused.
     * These CAN be pressed - as root - so hiding them would conceal
     * the feature instead of explaining it, and the sidebar says what
     * to do about it.
     */
    {
        /* CAN ACT, NOT "IS euid 0 NOW" - after Modify the setuid
         * build is root only inside a raised window. */
        gint on = vlhe_priv_can_act() ? TRUE : FALSE;

        if (g_load_btn != NULL)
            gtk_widget_set_sensitive(g_load_btn, on);
        if (g_unload_btn != NULL)
            gtk_widget_set_sensitive(g_unload_btn, on);
    }

    /*
     * AND THE THREE BOXES FOLLOW THE CONFIG, not the other way round.
     * Cancel re-reads the files, and `vlhe' or an editor can
     * change them under a running GUI; a box left showing the old
     * value would be this page lying about what Load will do.
     *
     * NOT GREYED WITHOUT ROOT, unlike the buttons above. Choosing
     * what a load would include is not a privileged act - it writes
     * the user's own config - and a non-root user setting it up
     * before running `vlhe.gtk' as root is an ordinary thing to do.
     *
     * g_en_setting SUPPRESSES THE HANDLER: set_active() emits
     * "toggled", and without the guard this poll would write the
     * config twice a second forever.
     */
    if (g_en[0] != NULL) {
        int k;

        g_en_setting = 1;
        for (k = 0; k < 3; k++) {
            gint want = vlhe_component_enabled(k) ? TRUE : FALSE;

            if (GTK_TOGGLE_BUTTON(g_en[k])->active != want)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_en[k]),
                                             want);
        }
        g_en_setting = 0;
    }

    /*
     * THE UNFINISHED UNLOAD - 7h Stage 2c. Read raised: the session
     * file is root's (a no-op where the program is not setuid). Every
     * poll, so it goes the moment the leftover is finished, by this
     * page, by `vlhe apply -u' or by a boot.
     */
    if (g_left_frame != NULL) {
        static const char *const lab[3] = { "Sound", "MIDI", "CD" };
        int  left, nfailed, mask = 0, k;
        char which[32], text[400];

        vlhe_root_begin();
        left = vlhe_apply_leftover(&mask, NULL);
        nfailed = vlhe_apply_leftover_failed();
        vlhe_root_end();

        if (g_find_due <= 0) {
            vlhe_root_begin();
            g_nfind = vlhe_apply_findings(g_find, FIND_MAX);
            vlhe_root_end();
            g_find_due = FIND_EVERY;
        } else {
            g_find_due--;
        }
        if (g_nfind > 0) {
            char ftext[96];

            sprintf(ftext, FMT_STATUS_ATTENTION_COUNT, g_nfind);
            gtk_label_set_text(GTK_LABEL(g_rev_label), ftext);
            gtk_widget_show(g_rev_row);
        } else {
            gtk_widget_hide(g_rev_row);
        }
        gtk_frame_set_label(GTK_FRAME(g_left_frame),
                            g_nfind > 0 ? STR_STATUS_FRAME_ATTENTION
                                        : STR_STATUS_FRAME_LEFTOVER);

        if (left <= 0) {
            g_left_hidden = 0;
            gtk_widget_hide(g_left_row);
            if (g_nfind > 0)
                gtk_widget_show(g_left_frame);
            else
                gtk_widget_hide(g_left_frame);
        } else {
            which[0] = '\0';
            for (k = 0; k < 3; k++)
                if (mask & (1 << k)) {
                    if (which[0] != '\0')
                        strcat(which, ", ");
                    strcat(which, lab[k]);
                }
            /* FAILED IS NOT A POWER LOSS - the user, 2026-10-04. A
             * FAILED record is one an unload tried and could not undo;
             * only an OPEN one was never undone. */
            if (nfailed == 0)
                sprintf(text, FMT_STATUS_LEFTOVER_TEXT, left, which);
            else if (nfailed == left)
                sprintf(text, FMT_STATUS_LEFTOVER_FAILED_TEXT, left, which);
            else
                sprintf(text, FMT_STATUS_LEFTOVER_MIXED_TEXT, left, which,
                        nfailed);
            gtk_label_set_text(GTK_LABEL(g_left_label), text);
            gtk_widget_set_sensitive(g_left_finish,
                                     vlhe_priv_can_act() ? TRUE : FALSE);
            if (g_left_hidden)
                gtk_widget_hide(g_left_row);
            else
                gtk_widget_show(g_left_row);
            if (g_left_hidden && g_nfind == 0)
                gtk_widget_hide(g_left_frame);
            else
                gtk_widget_show(g_left_frame);
        }
    }
}

/* ONLY WHILE THE PAGE IS SHOWING - design/47 T1: six /proc/modules
 * reads, three pidfile checks and eighteen label sets every 2 s are
 * nothing for a page nobody is looking at. status_set_active() starts
 * and stops it, refreshing at once on a show. */
static guint g_poll_tag;

/*
 * THE BOOT'S ONE-TIME NOTICE, SHOWN ONCE - design/54 D63, 2026-10-04
 * (the user: "one time dialog. There is no room on the status page").
 *
 * An unattended boot - a leftover finished, a CONFLICT, a FAILED that
 * kept VLHE unloaded, drift - writes what it learned to a notice beside
 * the session file, because its own output goes to /dev/null. Only
 * `vlhe status' ever read it; a GUI user never saw it. Now the Status
 * page reads it the first time it is shown, puts it in a dialog, and
 * removes it - only the copy shown (vlhe_apply_notice_clear()).
 *
 * A SETUID GUI BEFORE MODIFY may be unable to read root's notice, or to
 * remove it: a read that fails is tried again on each poll, and a
 * removal that fails is retried once Modify has been given - but the
 * dialog appears once per session either way. It names no tool: what
 * the notice says is what happened, and the journal has the rest.
 */
static int g_notice_shown;              /* shown this session         */
static int g_notice_clear_due;          /* shown, not yet removed     */
static struct vlhe_notice_seen g_notice_seen;

static void
show_notice(const char *text)
{
    GtkWidget *dlg, *sw, *t, *btn;
    char foot[400];

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_STATUS_NOTICE_TITLE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    gtk_widget_set_usize(dlg, 420, 240);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), sw, TRUE, TRUE, 4);
    gtk_widget_show(sw);

    /* A TEXT WIDGET, as show_result() uses: a variable number of lines,
     * and a label in a scrolled window does not size itself in 1.2. */
    t = gtk_text_new(NULL, NULL);
    gtk_text_set_editable(GTK_TEXT(t), FALSE);
    gtk_text_set_word_wrap(GTK_TEXT(t), TRUE);
    gtk_container_add(GTK_CONTAINER(sw), t);
    gtk_widget_show(t);

    gtk_text_freeze(GTK_TEXT(t));
    gtk_text_insert(GTK_TEXT(t), NULL, NULL, NULL, STR_STATUS_NOTICE_HEAD, -1);
    gtk_text_insert(GTK_TEXT(t), NULL, NULL, NULL, text, -1);
    sprintf(foot, FMT_STATUS_NOTICE_FOOT, vlhe_journal_path());
    gtk_text_insert(GTK_TEXT(t), NULL, NULL, NULL, foot, -1);
    gtk_text_thaw(GTK_TEXT(t));

    /* NOT MODAL AND NO NESTED MAIN LOOP - it is news, not a question,
     * and it opens from inside the page's show. */
    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CLOSE), STR_SHELL_BTN_CLOSE_TIP);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);
    gtk_widget_show(dlg);
}

static void
notice_check(void)
{
    static char text[4096];
    int n, rc;

    if (g_notice_clear_due && vlhe_priv_can_act()) {
        vlhe_root_begin();
        rc = vlhe_apply_notice_clear(&g_notice_seen);
        vlhe_root_end();
        if (rc == 0 || errno == EAGAIN)
            g_notice_clear_due = 0;     /* gone, or a new one to show */
        if (rc != 0 && errno == EAGAIN)
            g_notice_shown = 0;
    }
    if (g_notice_shown)
        return;

    vlhe_root_begin();
    n = vlhe_apply_notice_read(text, sizeof text, &g_notice_seen);
    vlhe_root_end();
    if (n < 0)
        return;                         /* unreadable for now: later */
    g_notice_shown = 1;
    if (n == 0)
        return;                         /* no notice */

    show_notice(text);
    vlhe_root_begin();
    rc = vlhe_apply_notice_clear(&g_notice_seen);
    vlhe_root_end();
    g_notice_clear_due = (rc != 0 && errno != EAGAIN);
}

static gint on_poll(gpointer data)
{
    (void)data;
    refresh();
    notice_check();
    return TRUE;
}

void status_set_active(int active)
{
    if (active) {
        g_find_due = 0;         /* findings worked out afresh on a show */
        refresh();
        notice_check();
        if (g_poll_tag == 0)
            g_poll_tag = gtk_timeout_add(POLL_MS, on_poll, NULL);
    } else if (g_poll_tag != 0) {
        gtk_timeout_remove(g_poll_tag);
        g_poll_tag = 0;
    }
}

/*
 * RUN A PLAN, AND SAY WHAT HAPPENED.
 *
 * `unload' picks the direction. Everything else is shared, because
 * the two differ only in which plan gets built - the reporting, the
 * refusal and the refresh are the same job either way.
 *
 * THE OUTPUT GOES TO A TEMPORARY FILE, not a pipe. vlhe_plan_run()
 * takes a FILE* and writes a line per step; a pipe would need a
 * reader running concurrently, and the run is synchronous. The file
 * is ours, in /tmp, named after our pid, and removed either way.
 *
 * THE WINDOW FREEZES FOR THE DURATION and that is accepted rather
 * than overlooked. An apply is a few seconds - insmod, three daemon
 * starts - where a render is minutes, which is why the render got a
 * non-blocking wait and this does not. The status line is set BEFORE
 * the run and the event queue pumped, so the user sees "loading..."
 * rather than a dead window.
 */
/*
 * THE RESULT BOX - every step, and what became of it.
 *
 * THE USER'S DESIGN, 2026-09-23, and it replaces two warnings I was
 * about to build: "a pop up for the load and unload for portable
 * mode. A info box stating what was loaded and if things failed to
 * load it can be displayed there."
 *
 * IT IS BETTER THAN A WARNING because it shows what HAPPENED rather
 * than predicting what might. The user hit exactly this: Load did
 * nothing, because no card module was named - and the plan SAID so,
 * in a line that went to a temporary file nobody read.
 *
 * SCROLLABLE. A full load is eleven steps and fits, but the CD page
 * can add a node line per drive.
 *
 * NOT VERBOSE - the user again. vlhe_plan_run() writes one short line
 * per step and this shows them as they are; no headers, no summary
 * beyond the title.
 */
/*
 * THE UNLOAD'S OUTCOME, AS THE JOURNAL GIVES IT - design/54 7h, 86Box
 * 2026-10-03. The box and the status line said "the machine is as it
 * was" whenever no component failed, over a journal verdict of four
 * records still in effect. The session file has the last word, in the
 * journal's own order: still in effect, then CONFLICT, then unreadable,
 * then as it was. `still' is vlhe_journal_outstanding_now()'s value.
 * `box' picks the box's words over the status line's.
 */
/* How many paths differ from the baseline after the last unload -
 * vlhe_journal_baseline_now(), read where `still' is. -2 not compared. */
static int g_base_diff_seen = -2;

static void
unload_outcome(int unload, int still, int nconf, char *msg, int box)
{
    if (!unload)
        strcpy(msg, box ? "\nDone.\n" : "loaded");
    else if (still > 0)
        sprintf(msg, box ? FMT_STATUS_TEXT_DONE_STILL_IN_EFFECT
                         : FMT_STATUS_MSG_UNLOADED_STILL_IN_EFFECT, still);
    else if (nconf > 0)
        sprintf(msg, box ? FMT_STATUS_TEXT_DONE_CONFLICT
                         : FMT_STATUS_MSG_UNLOADED_CONFLICT, nconf);
    else if (still == -1)
        strcpy(msg, box ? STR_STATUS_TEXT_DONE_SESSION_UNREADABLE
                        : STR_STATUS_MSG_UNLOADED_SESSION_UNREADABLE);
    /* VLHE'S OWN UNDO WAS CLEAN, BUT THE MACHINE DIFFERS FROM THE
     * BASELINE - a change someone kept, or VLHE's leftover the next
     * Load puts back. Said as such, not as "as it was" under the box's
     * own "differs from the baseline" lines (the user, 2026-10-04,
     * row 142 part 3). Not a failure, so no journal pointer. */
    else if (g_base_diff_seen > 0)
        sprintf(msg, box ? FMT_STATUS_TEXT_DONE_BASELINE_DIFFERS
                         : FMT_STATUS_MSG_UNLOADED_BASELINE_DIFFERS,
                g_base_diff_seen);
    else
        strcpy(msg, box ? STR_STATUS_TEXT_DONE_MACHINE_AS_WAS
                        : STR_STATUS_MSG_UNLOADED_AS_IT_WAS);
}

static void
show_result(int unload, int nfailed, int still, int nconf, const char *path)
{
    GtkWidget *dlg, *sw, *text, *btn;
    FILE      *fp;
    char       line[256];

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg),
                         unload ? "Unload" : "Load");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    gtk_widget_set_usize(dlg, 420, 260);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), sw,
                       TRUE, TRUE, 4);
    gtk_widget_show(sw);

    /* A TEXT WIDGET RATHER THAN A LABEL, because the content is a
     * variable number of lines and a label in a scrolled window does
     * not size itself usefully in GTK 1.2. Not editable. */
    text = gtk_text_new(NULL, NULL);
    gtk_text_set_editable(GTK_TEXT(text), FALSE);
    gtk_container_add(GTK_CONTAINER(sw), text);
    gtk_widget_show(text);

    gtk_text_freeze(GTK_TEXT(text));

    fp = (path != NULL) ? fopen(path, "r") : NULL;
    if (fp != NULL) {
        while (fgets(line, sizeof line, fp) != NULL)
            gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, line, -1);
        fclose(fp);
    }

    /*
     * THE VERDICT LAST, where the eye ends up.
     *
     * IT COUNTS COMPONENTS, NOT STEPS, since 2026-09-23. Three scoped
     * plans run now rather than one, so "stopped at step 7" named a
     * number the reader cannot locate - each plan starts its own
     * count - and said STOPPED when the run carried on to the other
     * components, which is the whole point of the change.
     */
    if (nfailed == 0) {
        char msg[256];

        unload_outcome(unload, still, nconf, msg, 1);
        gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, msg, -1);
    } else {
        char msg[96];

        if (nfailed == 1)
            strcpy(msg, FMT_STATUS_ONE_COMPONENT_FAILED_SEE);
        else
            sprintf(msg, FMT_STATUS_COMPONENTS_FAILED_SEE_ABOVE,
                    nfailed);
        gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, msg, -1);
    }

    gtk_text_thaw(GTK_TEXT(text));

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CLOSE), STR_SHELL_BTN_CLOSE_TIP);
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

static void (*g_machine_cb)(void);
static int  (*g_before_cb)(int unload);

static int  (*g_before_restart_cb)(const char *daemon);

void status_set_before_restart_cb(int (*cb)(const char *daemon))
{
    g_before_restart_cb = cb;
}

void status_set_before_cb(int (*cb)(int unload))
{
    g_before_cb = cb;
}

void status_set_machine_cb(void (*cb)(void))
{
    g_machine_cb = cb;
}

/*
 * RUN A LOAD OR UNLOAD - all three scopes, or just one.
 *
 * `only' is VLHE_ENABLE_SOUND/MIDI/CD to act on that component
 * alone, or -1 for the whole stack. The per-row Load buttons pass a
 * single scope; the page's own Load/Unload pass -1.
 *
 * ONE COMPONENT AT A TIME IS THE POINT, NOT AN OPTIMISATION. The
 * user's case, 2026-09-26: vsound fails, MIDI and CD came up anyway
 * (which the per-scope loop below already allows), and retrying
 * vsound alone must not disturb them - a full cycle would drop
 * vdiscd's loaded disc. design/43 part A.
 *
 * `forced' PLANS THAT COMPONENT WHETHER OR NOT ITS BOX IS TICKED,
 * and only the per-row buttons pass it. The press names the
 * component, so it is the consent - see vlhe_plan_build_forced().
 * The page's own Load/Unload pass 0 and keep honouring the boxes,
 * which is what those boxes are for.
 */
static void run_plan(int unload, int only, int forced)
{
    char  path[64];
    FILE *out;
    /* `n' counts steps across ALL the scopes, so a zero means nothing
     * was planned at all rather than one component being disabled.
     * `nfailed' is how many COMPONENTS failed - the only count worth
     * keeping, since a step number is per-scope now and names nothing
     * a reader can find. */
    int   n, nfailed;
    int   held_lock = 0;    /* this press holds the run lock */
    int   still = -2, nconf = 0;    /* the journal's outcome, unload */

    if (!vlhe_priv_can_act()) {
        /* THE SIDEBAR ALREADY EXPLAINS THIS STANDING - the message
         * here is for the moment of the press. THE RIGHT ADVICE FOR
         * THE BUILD (design/49 T2): in the setuid build the answer is
         * the Modify button now on this page; in the ordinary build
         * it is to start again as root. */
        report(vlhe_priv_can_unlock()
               ? STR_STATUS_TEXT_NEEDS_ROOT_MODIFY
               : STR_STATUS_TEXT_NEEDS_ROOT_START_AS_ROOT);
        return;
    }

    /*
     * A LOAD OVER AN UNFINISHED UNLOAD IS REFUSED HERE, ONCE - design/54
     * 7h Stage 2c. vlhe_plan_run() refuses it too, and per scope that
     * was three refusals for one press, after the kernel-log capture
     * had already stopped sysklogd. Asked first, so the refusal touches
     * nothing - not the settings question, not the capture - and shows
     * the banner again if "Not now" hid it.
     */
    if (!unload) {
        int left;

        vlhe_root_begin();
        left = vlhe_apply_leftover(NULL, NULL);
        vlhe_root_end();
        if (left > 0) {
            g_left_hidden = 0;
            refresh();
            report_problem(STR_STATUS_MSG_LOAD_REFUSED_LEFTOVER);
            return;
        }
    }

    /*
     * UNSAVED SETTINGS FIRST - design/36 2b item 4. A plan reads the
     * config ON DISK; a page showing something else would load the
     * old values under the user's nose, which is the trap behind row
     * 28's "you also need to apply the settings" and behind every run
     * where Load acted on one thing while the screen showed another.
     * The shell knows the pages; it asks.
     */
    if (g_before_cb != NULL && !g_before_cb(unload)) {
        report(STR_STATUS_MSG_CANCELLED_NOTHING_WAS_LOADED);
        return;
    }

    report(unload ? "unloading..." : "loading...");
    while (gtk_events_pending())
        gtk_main_iteration();

    /*
     * A FRESH PRIVATE FILE - design/49 N6. This was
     * `fopen("/tmp/vlhe-run.<pid>", "w+")', which follows a link at a
     * predictable name, and this function only runs as root. fdopen
     * rather than fopen so the O_EXCL open is the one that happens.
     */
    {
        int fd = vlhe_tmp_open(path, sizeof path, "vlhe-run");

        out = (fd >= 0) ? fdopen(fd, "w+") : NULL;
        if (out == NULL && fd >= 0)
            close(fd);
    }

    /*
     * ONE SCOPED RUN PER COMPONENT, NOT ONE RUN FOR ALL THREE.
     *
     * THE BUG THIS FIXES, reported from a target run 2026-09-23: "if
     * vsound fails to load no other modules load with all 3 enabled".
     * vlhe_plan_run() returns at the first REQUIRED failure, and
     * `insmod vsound.o' is required - so vmidi and vdisc, both marked
     * optional and both perfectly able to load without vsound, were
     * never reached.
     *
     * THEY ARE GENUINELY INDEPENDENT. The ordering rule is vsound
     * before THE CARD, not vsound before the others: vmidi needs
     * sound.o, vdisc needs nothing, and they take different slots.
     * `load.sh' has had -S (no vsound) as a supported arrangement for
     * weeks - "the card is driven directly", its own words.
     *
     * THE ORDER OF THE SCOPES IS THE ORDER THE STACK WANTS. Sound
     * first on the way in, so vsound takes minor 3 before anything
     * else registers; reversed on the way out, so vsound is removed
     * last - the same reason plan_unload() puts it last within a
     * scope.
     *
     * A SCOPE THE USER HAS DISABLED PLANS NOTHING and is skipped
     * silently - in_plan() already answers that, and a zero-step plan
     * is not a failure.
     *
     * NO STEP NUMBER IS KEPT. One existed while a single plan ran and
     * meant something; with three plans each numbering its own steps
     * it named a line the reader cannot locate. The LAST OUTPUT LINE
     * is read back instead, further down, which is what a user
     * actually wants to see.
     */
    {
        static const int order_load[3] = {
            VLHE_ENABLE_SOUND, VLHE_ENABLE_MIDI, VLHE_ENABLE_CD
        };
        static const int order_unload[3] = {
            VLHE_ENABLE_CD, VLHE_ENABLE_MIDI, VLHE_ENABLE_SOUND
        };
        const int *order = unload ? order_unload : order_load;
        int k;

        /*
         * ONE JOURNAL RUN FOR THE WHOLE PRESS, not one per scope.
         * vlhe_plan_run() opens and closes the journal itself, and
         * with three calls that wrote three headers - two of them
         * "(nothing changed)" - each claiming "the machine is as it
         * was", which no single scope can know. Holding it open
         * around the loop makes the nested begin/end pairs join this
         * one; see vlhe_journal_begin().
         */
        /*
         * ONE PRESS IS ONE RUN - design/54 7h Stage 1. Held across the
         * three scoped plans so `vlhe apply' cannot slip in between
         * them; each vlhe_plan_run() takes it again inside, nested.
         * Raised: the run directory is root's.
         */
        {
            char who[256];
            int  lk = -1;

            /* Not on a machine the runs will refuse - a refused run
             * touches nothing, the run directory included. */
            if (vlhe_apply_machine_ok(who, sizeof who) == 0) {
                vlhe_root_begin();
                lk = vlhe_apply_lock(who, sizeof who);
                vlhe_root_end();
            }
            if (lk == 1) {
                if (out != NULL)
                    fclose(out);
                report("another Load or Unload is running - try again"
                       " when it has finished");
                return;
            }
            held_lock = (lk == 0);
        }

        vlhe_journal_begin(unload);

        /*
         * THE BASELINE, ONCE PER PRESS - design/54 7h Stage 3.3, before
         * the first scope changes anything (vlhe_plan_run() skips it when
         * nested in this run). The drift dialog is the shell's. A cancel
         * ends the press with nothing changed - and no capture started,
         * since the capture begins below.
         */
        if (!unload) {
            int bc;

            vlhe_root_begin();
            bc = vlhe_apply_baseline_check(out);
            vlhe_root_end();
            if (bc != 0) {
                vlhe_journal_end();
                if (held_lock) {
                    vlhe_root_begin();
                    vlhe_apply_unlock();
                    vlhe_root_end();
                }
                if (out != NULL)
                    fclose(out);
                if (path[0] != '\0')
                    remove(path);
                if (vlhe_apply_drift_refused())
                    report_problem(STR_STATUS_MSG_LOAD_REFUSED_DRIFT);
                else
                    report(STR_STATUS_MSG_LOAD_CANCELLED_DRIFT);
                return;
            }
        }

        /*
         * THE TRACE CAPTURE, ON LOAD - [Tracing] Capture, brought back
         * 2026-10-02 as its own setting (vlhe_apply.c has what it
         * does). It needs no root since the rework of 2026-10-08 - the
         * reader opens 0444 proc files - but the raise is kept around
         * it so the reader is started the same way whichever build
         * this is; the old one, run unraised, wrote an empty trace.log
         * and said nothing.
         */
        if (!unload) {
            vlhe_root_begin();
            (void) vlhe_capture_begin(out);
            vlhe_root_end();
        }

        n = 0;
        nfailed = 0;

        for (k = 0; k < 3; k++) {
            struct vlhe_plan sp;
            int              steps, src;

            /* A SINGLE-COMPONENT RUN SKIPS THE OTHER TWO ENTIRELY -
             * not "plans them and finds nothing", which would write
             * their steps into the journal as no-ops. */
            if (only >= 0 && order[k] != only)
                continue;

            steps = vlhe_plan_build_forced(&sp, unload, order[k], forced);
            if (steps <= 0)
                continue;               /* disabled, or nothing to do */

            n += steps;
            src = vlhe_plan_run(&sp, out);
            if (src != 0) {
                nfailed++;
                /*
                 * AND THE NEXT SCOPE STILL RUNS. That is the whole
                 * change - said in the output too, so a reader sees
                 * the run continue rather than wondering why steps
                 * follow a failure.
                 */
                if (out != NULL)
                    fprintf(out, FMT_STATUS_BOX_COMPONENT_FAILED);
            }
        }

        /* WHAT THE WHOLE PRESS LEFT, SAID ONCE - each scope's own close
         * stays quiet about it while nested. Raised: the session file
         * is root's. */
        if (unload && n > 0) {
            vlhe_root_begin();
            vlhe_apply_session_close(out);
            vlhe_root_end();
        }
        still = vlhe_journal_outstanding_now();
        nconf = vlhe_journal_conflicts();
        g_base_diff_seen = unload ? vlhe_journal_baseline_now() : -2;

        vlhe_journal_end();

        /* AND CLOSED ON UNLOAD, LAST - after the teardown, so rmmod's
         * lines and any oops it provokes land inside the run. Guarded on
         * a capture being open, not on the setting. Raised, as above. */
        if (unload) {
            vlhe_root_begin();
            (void) vlhe_capture_end(out);
            vlhe_root_end();
        }

        if (held_lock) {
            vlhe_root_begin();
            vlhe_apply_unlock();
            vlhe_root_end();
        }

        if (n == 0) {
            if (out != NULL)
                fclose(out);
            report(unload ? STR_STATUS_MSG_NOTHING_TO_UNLOAD
                          : STR_STATUS_MSG_NOTHING_TO_LOAD);
            return;
        }
    }

    /* CLOSED BEFORE THE BOX READS IT - the dialog reopens the file by
     * name, and a buffered tail still in this handle would be missing
     * from what it shows. */
    if (out != NULL) {
        fclose(out);
        out = NULL;
    }

    /*
     * THE BOX, WHEN THE MODE OR THE SETTING WANTS IT.
     * vlhe_show_apply_result() decides - on for a portable copy, off
     * when installed, and either overridden in File / Preferences.
     *
     * IT REPLACES THE STATUS LINE FOR THE DETAIL, not for the
     * outcome: the line below still says what happened, so a user who
     * has turned the box off is not left guessing.
     */
    /*
     * THE MACHINE CHANGED - SAY SO BEFORE SHOWING THE RESULT, so the
     * other pages are already right by the time the user looks at
     * them. Also on a partial failure: some of it loaded. Added
     * 2026-09-24, design/36 rows 34, 44, 57, 58.
     */
    if (g_machine_cb != NULL)
        g_machine_cb();

    if (vlhe_show_apply_result())
        show_result(unload, nfailed, still, nconf, path);

    /*
     * AND THE STATUS LINE EITHER WAY. The box carries the detail; this
     * carries the outcome, so turning the box off does not leave a
     * press with no feedback at all - which is what the user hit when
     * Load silently did nothing.
     *
     * THE FAILING STEP'S OWN LINE when there is one: naming the step
     * beats "it failed", the same reasoning that put smf2wav's own
     * words on screen rather than a guess.
     */
    if (nfailed != 0) {
        FILE *in = fopen(path, "r");
        char  last[256];
        char  msg[320];

        last[0] = '\0';
        if (in != NULL) {
            char line[256];

            while (fgets(line, sizeof line, in) != NULL) {
                char *nl = strchr(line, '\n');

                if (nl != NULL)
                    *nl = '\0';
                if (line[0] != '\0') {
                    /* THE RUN WRITES IN THE SIMULATION'S VOCABULARY
                     * since 2026-10-01 - a command, or a `#' comment
                     * - so the prose is behind a `#' and some
                     * spaces; the status line wants the words. */
                    const char *t = line;

                    if (*t == '#')
                        t++;
                    while (*t == ' ')
                        t++;
                    strncpy(last, t, sizeof last - 1);
                    last[sizeof last - 1] = '\0';
                }
            }
            fclose(in);
        }

        /*
         * "step N" IS NO LONGER MEANINGFUL ON ITS OWN, because three
         * scoped plans ran and each numbers its own steps. What a
         * user needs is HOW MANY components failed and what the last
         * thing said - the step number stays out of it.
         */
        if (nfailed == 1 && last[0] != '\0')
            sprintf(msg, FMT_STATUS_ONE_COMPONENT_FAILED, last);
        else if (nfailed == 1)
            strcpy(msg, FMT_STATUS_ONE_COMPONENT_FAILED_SEE_2);
        else if (last[0] != '\0')
            sprintf(msg, FMT_STATUS_COMPONENTS_FAILED, nfailed, last);
        else
            sprintf(msg, FMT_STATUS_COMPONENTS_FAILED_SEE_DETAILS,
                    nfailed);
        report_problem(msg);
    } else {
        char msg[256];

        unload_outcome(unload, still, nconf, msg, 0);
        if (unload && (still > 0 || nconf > 0 || still == -1))
            report_problem(msg);
        else
            report(msg);
    }

    /* ONLY WHAT vlhe_tmp_open MADE - it empties `path' on failure. */
    if (path[0] != '\0')
        remove(path);

    /* WHATEVER HAPPENED, THE PAGE IS NOW WRONG. A partial run leaves
     * some of it loaded, which is exactly when an accurate list
     * matters most. */
    g_find_due = 0;         /* findings afresh after a press */
    refresh();
}

static void on_load(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    run_plan(0, -1, 0);
}

/*
 * FINISH THE UNLOAD - 7h Stage 2c. vlhe_apply_finish_leftover(), the
 * engine the boot finish uses: the scoped unloads of what is left over,
 * one journal run, under the run lock. The result box and the status
 * line give the journal's outcome, as an Unload's do.
 */
static void on_left_finish(GtkWidget *w, gpointer d)
{
    char  path[64];
    FILE *out;
    int   rc, nconf = 0, still = -2;

    (void)w; (void)d;
    if (!vlhe_priv_can_act()) {
        report(vlhe_priv_can_unlock()
               ? STR_STATUS_TEXT_NEEDS_ROOT_MODIFY
               : STR_STATUS_TEXT_NEEDS_ROOT_START_AS_ROOT);
        return;
    }
    report(STR_STATUS_MSG_FINISHING);
    while (gtk_events_pending())
        gtk_main_iteration();

    {
        int fd = vlhe_tmp_open(path, sizeof path, "vlhe-run");

        out = (fd >= 0) ? fdopen(fd, "w+") : NULL;
        if (out == NULL && fd >= 0)
            close(fd);
    }

    vlhe_root_begin();
    rc = vlhe_apply_finish_leftover("finished from the Status page", out,
                                    &nconf, &still);
    vlhe_root_end();
    g_base_diff_seen = vlhe_journal_baseline_now();
    if (out != NULL)
        fclose(out);

    if (rc == -2) {
        report(STR_STATUS_MSG_FINISH_BUSY);
    } else {
        char msg[256];

        /* STILL LEFT OVER IS NOT "AS IT WAS", whatever the journal
         * managed to count (an unload refused early sets nothing). */
        if (rc > 0 && still <= 0)
            still = rc;
        if (g_machine_cb != NULL)
            g_machine_cb();
        if (vlhe_show_apply_result())
            show_result(1, 0, still, nconf, path);
        unload_outcome(1, still, nconf, msg, 0);
        if (still > 0 || nconf > 0 || still == -1)
            report_problem(msg);
        else
            report(msg);
    }
    if (path[0] != '\0')
        remove(path);
    g_find_due = 0;         /* findings afresh after a press */
    refresh();
}

/* NOT NOW - hidden for this GUI session; Load stays refused, and
 * pressing it shows the banner again. */
static void on_left_not_now(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    g_left_hidden = 1;
    refresh();
}

/*
 * PUTTING A FINDING RIGHT FROM THE PAGE - 7h Stage 3.5c, the same
 * vlhe_apply_fix_finding() `vlhe repair' uses. Its first line of output
 * is the status line.
 */
static void run_fix(int k, int action, const char *arg)
{
    char  path[64], line[300];
    FILE *out;
    int   rc;

    if (k < 0 || k >= g_nfind)
        return;
    if (!vlhe_priv_can_act()) {
        report(vlhe_priv_can_unlock()
               ? STR_STATUS_TEXT_NEEDS_ROOT_MODIFY
               : STR_STATUS_TEXT_NEEDS_ROOT_START_AS_ROOT);
        return;
    }
    {
        int fd = vlhe_tmp_open(path, sizeof path, "vlhe-fix");

        out = (fd >= 0) ? fdopen(fd, "w+") : NULL;
        if (out == NULL && fd >= 0)
            close(fd);
    }
    vlhe_root_begin();
    rc = vlhe_apply_fix_finding(g_find[k].path, g_find[k].kind, action,
                                arg, out);
    vlhe_root_end();
    line[0] = '\0';
    if (out != NULL) {
        rewind(out);
        if (fgets(line, sizeof line, out) != NULL)
            line[strcspn(line, "\n")] = '\0';
        fclose(out);
    }
    if (path[0] != '\0')
        remove(path);
    if (line[0] != '\0') {
        if (rc < 0)
            report_problem(line);
        else
            report(line);
    }
    if (g_machine_cb != NULL)
        g_machine_cb();
    g_find_due = 0;
    refresh();
}

/*
 * REVIEW... - each finding with its own buttons, in ONE window that
 * stays open (7h Stage 3.5c; reworked 2026-10-04, the user: "the review
 * had too many dialog boxes" - a fix closed the window with a row still
 * in it, and Point at drive... opened a fourth one to pick the drive).
 *
 * A FIX IS ACTED ON IN PLACE, the rows rebuilt from fresh findings, and
 * the window closes itself when nothing is left. The status line still
 * reports each result, as before.
 *
 * THE DRIVES ARE IN THE /dev/cdrom ROW: a menu of the CD drives found,
 * beside Point at drive - or, with none found, an entry to type the
 * device into (the GUI's own way, not a pointer at the command line).
 * Never opened on its own: the user, 2026-10-04.
 */
static GtkWidget *g_rev_dlg, *g_rev_rows;
static GtkWidget *g_rev_drive[FIND_MAX];        /* per row, NULL for dsp */
static int        g_rev_typed[FIND_MAX];        /* 1: an entry, not a menu */
static char       g_rev_drv[8][VLHE_PATH_MAX];
static int        g_rev_nd;

static void review_fill(void);

static void on_rev_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    g_rev_dlg = g_rev_rows = NULL;
    gtk_main_quit();
}

static void on_rev_fix(GtkWidget *w, gpointer d)
{
    int  k = (int) (long) d / 8, act = (int) (long) d % 8;
    char arg[VLHE_PATH_MAX];

    (void)w;
    if (k < 0 || k >= g_nfind)
        return;
    if (act != VLHE_FIX_DRIVE) {
        run_fix(k, act, NULL);
    } else {
        GtkWidget *x = g_rev_drive[k];

        if (x == NULL)
            return;
        if (g_rev_typed[k]) {
            strncpy(arg, gtk_entry_get_text(GTK_ENTRY(x)), sizeof arg - 1);
            arg[sizeof arg - 1] = '\0';
            if (arg[0] != '/' || strlen(arg) < 6) {
                report(STR_STATUS_TEXT_TYPE_DRIVE);
                return;
            }
        } else {
            GtkWidget *item = GTK_OPTION_MENU(x)->menu_item;
            int i = item != NULL
                    ? (int) (long) gtk_object_get_user_data(GTK_OBJECT(item))
                    : 0;

            if (i < 0 || i >= g_rev_nd)
                return;
            strcpy(arg, g_rev_drv[i]);
        }
        run_fix(k, act, arg);
    }
    review_fill();
}

/* The /dev/cdrom row's drive chooser: a menu of the drives found, or an
 * entry when there are none. */
static GtkWidget *drive_chooser(int k)
{
    GtkWidget *x, *menu, *item;
    int i, sel;

    if (g_rev_nd == 0) {
        x = vlhe_tipped(gtk_entry_new(), STR_STATUS_TEXT_TYPE_DRIVE);
        gtk_entry_set_text(GTK_ENTRY(x), "/dev/");
        gtk_widget_set_usize(x, 110, -1);
        g_rev_typed[k] = 1;
    } else {
        x = gtk_option_menu_new();
        menu = gtk_menu_new();
        for (i = 0; i < g_rev_nd; i++) {
            item = gtk_menu_item_new_with_label(g_rev_drv[i]);
            gtk_object_set_user_data(GTK_OBJECT(item), (gpointer) (long) i);
            gtk_menu_append(GTK_MENU(menu), item);
            gtk_widget_show(item);
        }
        gtk_option_menu_set_menu(GTK_OPTION_MENU(x), menu);
        /* THE DRIVE THE BASELINE KNOWS, chosen first when it is found -
         * so putting it back is one press (2026-10-04). */
        sel = 0;
        for (i = 0; i < g_rev_nd; i++)
            if (g_find[k].was[0] != '\0'
                && strcmp(g_rev_drv[i], g_find[k].was) == 0)
                sel = i;
        gtk_option_menu_set_history(GTK_OPTION_MENU(x), sel);
        g_rev_typed[k] = 0;
    }
    g_rev_drive[k] = x;
    return x;
}

static void review_fill(void)
{
    GtkWidget *box, *lab, *row, *bb, *btn;
    int k, act;
    gint can = vlhe_priv_can_act() ? TRUE : FALSE;

    if (g_rev_dlg == NULL)
        return;
    vlhe_root_begin();
    g_nfind = vlhe_apply_findings(g_find, FIND_MAX);
    vlhe_root_end();
    if (g_nfind == 0) {
        g_find_due = 0;
        gtk_widget_destroy(g_rev_dlg);  /* nothing left: done */
        refresh();
        return;
    }
    if (g_rev_rows != NULL)
        gtk_widget_destroy(g_rev_rows);
    g_rev_rows = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(g_rev_dlg)->vbox), g_rev_rows,
                       FALSE, FALSE, 0);

    for (k = 0; k < g_nfind; k++) {
        char ftext[3 * VLHE_PATH_MAX];
        int  dsp = (g_find[k].kind == VLHE_FIND_DSP);

        g_rev_drive[k] = NULL;
        box = gtk_vbox_new(FALSE, 4);
        gtk_container_border_width(GTK_CONTAINER(box), 4);
        if (!dsp && g_find[k].was[0] != '\0')
            sprintf(ftext, FMT_STATUS_FIND_CDROM_WAS, g_find[k].path,
                    strncmp(g_find[k].now, "link ", 5) == 0
                        ? g_find[k].now + 5 : g_find[k].now,
                    g_find[k].was);
        else
            sprintf(ftext, dsp ? FMT_STATUS_FIND_DSP : FMT_STATUS_FIND_CDROM,
                    g_find[k].path,
                    strncmp(g_find[k].now, "link ", 5) == 0
                        ? g_find[k].now + 5 : g_find[k].now);
        lab = gtk_label_new(ftext);
        gtk_label_set_justify(GTK_LABEL(lab), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(lab), TRUE);
        gtk_widget_set_usize(lab, 440, -1);
        gtk_box_pack_start(GTK_BOX(box), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        row = gtk_hbox_new(FALSE, 6);
        if (!dsp) {
            GtkWidget *x = drive_chooser(k);

            gtk_widget_set_sensitive(x, can);
            gtk_box_pack_start(GTK_BOX(row), x, FALSE, FALSE, 0);
            gtk_widget_show(x);
        }
        bb = gtk_hbox_new(FALSE, 6);
        for (act = 0; act < 2; act++) {
            int fix = dsp ? (act == 0 ? VLHE_FIX_STOCK : VLHE_FIX_KEEP)
                          : (act == 0 ? VLHE_FIX_DRIVE : VLHE_FIX_REMOVE);
            const char *word = dsp
                ? (act == 0 ? STR_STATUS_BTN_RESTORE_STOCK : STR_STATUS_BTN_KEEP_IT)
                : (act == 0 ? STR_STATUS_BTN_POINT_DRIVE : STR_STATUS_BTN_REMOVE_LINK);
            const char *tip = dsp
                ? (act == 0 ? STR_STATUS_BTN_RESTORE_STOCK_TIP : STR_STATUS_BTN_KEEP_IT_TIP)
                : (act == 0 ? STR_STATUS_BTN_POINT_DRIVE_TIP : STR_STATUS_BTN_REMOVE_LINK_TIP);

            btn = vlhe_tipped(gtk_button_new_with_label(word), tip);
            gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                               GTK_SIGNAL_FUNC(on_rev_fix),
                               (gpointer) (long) (k * 8 + fix));
            gtk_widget_set_sensitive(btn, can);
            gtk_box_pack_start(GTK_BOX(bb), btn, FALSE, FALSE, 0);
            gtk_widget_show(btn);
        }
        vlhe_buttons_equalise(bb);
        gtk_box_pack_start(GTK_BOX(row), bb, FALSE, FALSE, 0);
        gtk_widget_show(bb);
        gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
        gtk_widget_show(row);
        gtk_box_pack_start(GTK_BOX(g_rev_rows), box, FALSE, FALSE, 4);
        gtk_widget_show(box);
    }
    gtk_widget_show(g_rev_rows);
}

static void on_review(GtkWidget *w, gpointer d)
{
    GtkWidget *btn;

    (void)w; (void)d;
    vlhe_root_begin();
    g_nfind = vlhe_apply_findings(g_find, FIND_MAX);
    vlhe_root_end();
    if (g_nfind == 0) {
        g_find_due = 0;
        refresh();
        return;
    }
    g_rev_nd = vlhe_apply_cd_drives(g_rev_drv, 8);
    if (g_rev_nd < 0)
        g_rev_nd = 0;

    g_rev_dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(g_rev_dlg), STR_STATUS_FRAME_ATTENTION);
    gtk_window_set_modal(GTK_WINDOW(g_rev_dlg), TRUE);
    gtk_window_set_policy(GTK_WINDOW(g_rev_dlg), FALSE, FALSE, TRUE);
    gtk_container_border_width(GTK_CONTAINER(g_rev_dlg), 8);
    btn = gtk_button_new_with_label(STR_SHELL_BTN_CLOSE);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(g_rev_dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(g_rev_dlg));
    gtk_widget_show(btn);
    gtk_signal_connect(GTK_OBJECT(g_rev_dlg), "destroy",
                       GTK_SIGNAL_FUNC(on_rev_destroy), NULL);
    review_fill();
    if (g_rev_dlg == NULL)
        return;
    gtk_widget_show(g_rev_dlg);
    gtk_main();
}

static void on_unload(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    run_plan(1, -1, 0);
}

/*
 * SIMULATE - the user's design, 2026-10-01. "Commands run on load",
 * then "Commands run on unload", for the boxes ticked, with the
 * run-time names stated as assumptions - and nothing run, nothing
 * probed, no root. A first-run user can read exactly what Load would
 * do before letting it, and Save... keeps the text for a bug report:
 * "here is what Load would have tried on my machine".
 *
 * THE TEXT LIVES IN A TEMP FILE until the box closes, so Save... is a
 * copy rather than a second simulation that might differ.
 */
static char g_sim_path[256];

static void sim_save_ok(GtkWidget *w, gpointer d)
{
    GtkWidget  *sel = (GtkWidget *) d;
    const char *to = gtk_file_selection_get_filename(GTK_FILE_SELECTION(sel));
    FILE *in, *out;
    char  buf[512];
    size_t n;

    (void)w;
    if (to == NULL || *to == '\0' || g_sim_path[0] == '\0') {
        gtk_widget_destroy(sel);
        return;
    }
    in = fopen(g_sim_path, "r");
    out = (in != NULL) ? fopen(to, "w") : NULL;
    if (in == NULL || out == NULL) {
        if (in != NULL)
            fclose(in);
        report(STR_STATUS_MSG_COULD_NOT_SAVE_SIMULATION);
        gtk_widget_destroy(sel);
        return;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            break;
    fclose(in);
    fclose(out);
    report(STR_STATUS_MSG_SIMULATION_SAVED);
    gtk_widget_destroy(sel);
}

/* Where the simulation goes by default: beside the user's own config,
 * or in their home - the same place Save Draft offers, for the same
 * reason: somewhere that is theirs. */
static void sim_default(char *out, int max)
{
    const char *u = vlhe_user_conf_path();
    const char *h = getenv("HOME");
    char *slash;

    out[0] = '\0';
    if (u != NULL && (int) strlen(u) + 24 < max) {
        strcpy(out, u);
        slash = strrchr(out, '/');
        if (slash != NULL)
            slash[1] = '\0';
        else
            out[0] = '\0';
    }
    if (out[0] == '\0' && h != NULL && (int) strlen(h) + 24 < max)
        sprintf(out, "%s/", h);
    if ((int) strlen(out) + 21 < max)
        strcat(out, "vlhe-simulation.txt");
}

/* THE SAME CHOOSER SAVE DRAFT USES - the user, 2026-10-01: "The
 * Simulate save button should use that GTKFileSelection widget".
 * Seeded with a directory and filtered by type, where before it opened
 * on the current directory with a bare filename. */
static void sim_save(GtkWidget *w, gpointer d)
{
    GtkWidget *sel;
    char deflt[VLHE_PATH_MAX];
    static const struct vlhe_filter txt_types[] = {
        { STR_STATUS_FILTER_TEXT, "*.txt" },
        { STR_STATUS_FILTER_ALL,      NULL    },
        { NULL, NULL }
    };

    (void)w; (void)d;
    sel = gtk_file_selection_new(STR_STATUS_TITLE_SAVE_SIMULATION);
    vlhe_filesel_fit(sel);         /* a long path must not widen it */
    /* A chooser, not a file manager - no Create / Rename / Delete. */
    gtk_file_selection_hide_fileop_buttons(GTK_FILE_SELECTION(sel));
    sim_default(deflt, (int) sizeof deflt);
    gtk_file_selection_set_filename(GTK_FILE_SELECTION(sel), deflt);
    vlhe_filter_attach(sel, txt_types, 0);
    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->ok_button),
                       "clicked", GTK_SIGNAL_FUNC(sim_save_ok), sel);
    gtk_signal_connect_object(
        GTK_OBJECT(GTK_FILE_SELECTION(sel)->cancel_button), "clicked",
        GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(sel));
    gtk_window_set_modal(GTK_WINDOW(sel), TRUE);
    gtk_widget_show(sel);
}

/* Write the simulation to the temp file and load it into the box.
 * Called at open and whenever the verbose toggle changes. */
static GtkWidget *g_sim_text;
static int        g_sim_verbose;

static void sim_render(void)
{
    FILE *fp;
    int   fd;
    char  line[256];

    if (g_sim_path[0] != '\0')
        unlink(g_sim_path);
    /* ONLY WHAT vlhe_tmp_open MADE - A5b: O_EXCL, 0600, ours. */
    fd = vlhe_tmp_open(g_sim_path, (int) sizeof g_sim_path, "vlhe-sim");
    fp = (fd >= 0) ? fdopen(fd, "w") : NULL;
    if (fp == NULL) {
        if (fd >= 0)
            close(fd);
        g_sim_path[0] = '\0';
        report(STR_STATUS_MSG_COULD_NOT_WRITE_SIMULATION);
        return;
    }
    vlhe_plan_simulate(fp, g_sim_verbose);
    fclose(fp);

    if (g_sim_text == NULL)
        return;
    gtk_text_freeze(GTK_TEXT(g_sim_text));
    gtk_text_set_point(GTK_TEXT(g_sim_text), 0);
    gtk_text_forward_delete(GTK_TEXT(g_sim_text),
                            gtk_text_get_length(GTK_TEXT(g_sim_text)));
    fp = fopen(g_sim_path, "r");
    if (fp != NULL) {
        while (fgets(line, sizeof line, fp) != NULL)
            gtk_text_insert(GTK_TEXT(g_sim_text), NULL, NULL, NULL, line, -1);
        fclose(fp);
    }
    gtk_text_thaw(GTK_TEXT(g_sim_text));
}

static void sim_verbose_toggled(GtkWidget *w, gpointer d)
{
    (void)d;
    g_sim_verbose = GTK_TOGGLE_BUTTON(w)->active ? 1 : 0;
    sim_render();
}

static void on_simulate(GtkWidget *w, gpointer d)
{
    GtkWidget *dlg, *sw, *btn;

    (void)w; (void)d;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), STR_STATUS_BTN_SIMULATE);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);
    gtk_widget_set_usize(dlg, 600, 400);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), sw, TRUE, TRUE, 4);
    gtk_widget_show(sw);

    g_sim_text = gtk_text_new(NULL, NULL);
    gtk_text_set_editable(GTK_TEXT(g_sim_text), FALSE);
    gtk_container_add(GTK_CONTAINER(sw), g_sim_text);
    gtk_widget_show(g_sim_text);
    sim_render();

    /* THE TWO FORMS - the user, 2026-10-01: "a simulate verbose and a
     * regular simulate. The regular should show the commands ...
     * Verbose can give the explanations or show it as bash comments."
     * Both are valid shell; the box opens on the regular one. */
    btn = vlhe_tipped(gtk_check_button_new_with_label(STR_STATUS_CHECK_EXPLAIN_EACH_STEP), STR_STATUS_CHECK_EXPLAIN_EACH_STEP_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn),
                                 g_sim_verbose ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(btn), "toggled",
                       GTK_SIGNAL_FUNC(sim_verbose_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_widget_show(btn);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_SAVE), STR_STATUS_BTN_SAVE_TIP);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(btn), "clicked",
                       GTK_SIGNAL_FUNC(sim_save), NULL);
    gtk_widget_show(btn);

    btn = vlhe_tipped(gtk_button_new_with_label(STR_SHELL_BTN_CLOSE), STR_SHELL_BTN_CLOSE_TIP);
    GTK_WIDGET_SET_FLAGS(btn, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), btn,
                       FALSE, FALSE, 0);
    gtk_signal_connect_object(GTK_OBJECT(btn), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(dlg));
    gtk_widget_show(btn);
    gtk_widget_grab_default(btn);

    /* Save... and Close the same size as every other dialog's
     * buttons; the check button beside them is left as it is. */
    vlhe_buttons_equalise(GTK_DIALOG(dlg)->action_area);

    gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
                       GTK_SIGNAL_FUNC(gtk_main_quit), NULL);
    gtk_widget_show(dlg);
    gtk_main();

    g_sim_text = NULL;
    if (g_sim_path[0] != '\0') {
        unlink(g_sim_path);
        g_sim_path[0] = '\0';
    }
}

/*
 * WHICH SCOPE A ROW BELONGS TO.
 *
 * The six rows are three modules and their three daemons, and each
 * pair is one scope - loading "vmidi" and loading "vmidid" are the
 * same plan, because a module without its daemon makes no sound and
 * a daemon without its module has nothing to open. So both rows of a
 * pair drive the same button action, and that is deliberate rather
 * than a simplification: there is no useful plan that loads one
 * half.
 *
 * -1 for anything unrecognised, which disables the button rather
 * than guessing. A component added to vlhe_components() and not to
 * this table gets no Load button and no wrong behaviour.
 */
static int row_scope(const char *id)
{
    if (strcmp(id, "vsound") == 0 || strcmp(id, "vsoundd") == 0)
        return VLHE_ENABLE_SOUND;
    if (strcmp(id, "vmidi") == 0 || strcmp(id, "vmidid") == 0)
        return VLHE_ENABLE_MIDI;
    if (strcmp(id, "vdisc") == 0 || strcmp(id, "vdiscd") == 0)
        return VLHE_ENABLE_CD;
    return -1;
}

/*
 * LOAD JUST THIS COMPONENT - the user's ask, 2026-09-26.
 *
 * WHY IT IS ON EVERY ROW RATHER THAN BEING ONE BUTTON SOMEWHERE.
 * The page already shows each component's state independently, and
 * the case it exists for is one of them being wrong: vsound failed
 * while MIDI and CD came up, so the thing to retry is vsound, and
 * retrying all three would unload the two that are working - taking
 * vdiscd's loaded disc with them.
 *
 * IT RESPECTS THE TICKBOXES. `vlhe_plan_build_scoped()' asks
 * in_plan(), so a component whose Include-in-load box is clear
 * plans nothing and the press reports that rather than acting.
 */
static void on_row_load(GtkWidget *w, gpointer data)
{
    struct comprow *r     = (struct comprow *) data;
    int             scope = row_scope(r->id);

    (void)w;

    if (scope < 0) {
        report(STR_STATUS_MSG_NOTHING_LOAD_COMPONENT);
        return;
    }
    run_plan(0, scope, 1);
}

/*
 * APPLY THE SAVED SETTINGS TO A RUNNING DAEMON - design/43 6b.
 *
 * NOT A SAVE. The settings pages write the config; this makes the
 * daemon that is already running obey what is on disk, without the
 * stop-and-start that drops its channel and silences whatever is
 * playing.
 *
 * THE USER MEASURED WHY IT IS WANTED, 2026-09-26: gmstriving at
 * 22050, changed the setting to 44100, pressed Apply, stopped
 * playmidi, waited past the 3 s release, started again - still
 * 22050. Nothing told the daemon.
 *
 * vmidid ONLY, FOR NOW, and the button is hidden on the other two
 * rows rather than greyed - the same rule Restart follows. vdiscd's
 * settings are structural (the drive count, the image) and vsoundd's
 * mix rate needs no channels open, so neither has anything a running
 * process could adopt.
 *
 * THE RATE IS REPORTED SEPARATELY BECAUSE IT DOES NOT LAND NOW. The
 * daemon defers it to its next release, so a user who presses this
 * mid-piece hears five settings change and not the sixth. Saying so
 * is design/43 6b3's requirement, and this line is the minimum of
 * it - the page does not yet MARK which controls are deferred.
 */
static void on_apply_settings(GtkWidget *w, gpointer data)
{
    struct comprow *r = (struct comprow *) data;
    int             rc;
    char            msg[160];

    (void)w;

    rc = vlhe_apply_synth();
    if (rc < 0) {
        sprintf(msg, FMT_STATUS_NOT_RUNNING_NOTHING_APPLY, r->id);
        report(msg);
        return;
    }
    if (rc > 0) {
        sprintf(msg, FMT_STATUS_SETTING_S_WERE_REFUSED, rc);
        report(msg);
        return;
    }
    report(STR_STATUS_MSG_SETTINGS_APPLIED_SAMPLE_RATE);
}

static void on_restart(GtkWidget *w, gpointer data)
{
    struct comprow *r = (struct comprow *) data;
    char msg[96];

    (void)w;

    /*
     * UNSAVED SETTINGS FIRST, AND ONLY THE ONES THIS DAEMON READS -
     * 2026-09-27. A restart starts the daemon from the config ON
     * DISK, so a soundfont picked and not saved would be ignored and
     * the restart would fail exactly as before, which is the case
     * this button was asked for.
     *
     * THE SHELL ASKS, because only it knows which pages are dirty.
     * It cancels by returning 0.
     */
    if (g_before_restart_cb != NULL && !g_before_restart_cb(r->id))
        return;

    if (vlhe_restart(r->id) != 0) {
        sprintf(msg, FMT_STATUS_COULD_NOT_RESTART, r->id);
        report(msg);
        return;
    }

    sprintf(msg, FMT_STATUS_RESTARTED, r->id);
    report(msg);
    refresh();
}

/* ------------------------------------------------------------------ */

static void build_row(struct comprow *r, GtkWidget *parent)
{
    GtkWidget *hbox;

    hbox = gtk_hbox_new(FALSE, 6);

    r->name = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(r->name), 0.0, 0.5);
    gtk_widget_set_usize(r->name, 70, -1);
    gtk_box_pack_start(GTK_BOX(hbox), r->name, FALSE, FALSE, 0);

    r->state = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(r->state), 0.0, 0.5);
    gtk_widget_set_usize(r->state, 110, -1);
    gtk_box_pack_start(GTK_BOX(hbox), r->state, FALSE, FALSE, 0);

    r->detail = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(r->detail), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), r->detail, TRUE, TRUE, 0);

    /* RESTART ONLY, AND NO RELOAD - the user's call, 2026-09-17, and
     * it is right about what these daemons are.
     *
     * A "Reload" BUTTON WAS BUILT HERE AND REMOVED THE SAME HOUR. The
     * idea came from /etc/init.d/skeleton's commented `reload)' case,
     * which design/09 calls worth having because "it would let the GUI
     * apply a config change WITHOUT STOPPING AUDIO" - and I read a
     * PROPOSAL as a description of something that exists.
     *
     * IT DID NOT THEN. vmidid took its fonts, voice cap, gain and law
     * only as COMMAND-LINE ARGUMENTS; vdiscd took -d N and an image path
     * the same way. A reload button would have done nothing at all.
     *
     * CORRECTED 2026-10-03 (design/54 T12): vmidid HAS since gained a
     * control channel (lib/vmidid_ctl.c) and each row an "Apply
     * Settings" button below, which sends the settings a running synth
     * can take (design/43 6b); the rate waits for its next pause. Fonts
     * and the voice array still need a Restart. vdiscd has a channel
     * too, for discs rather than settings. */
    /*
     * LOAD, BEFORE RESTART - the user's placement, 2026-09-26:
     * "a load button on each row above the daemon restart buttons".
     *
     * `above' IS THE COLUMN ORDER, not a second line. Every row is
     * one hbox and the two buttons sit side by side at its right;
     * packing Load first puts it left of Restart, which is the same
     * reading order a vertical stack would give and costs no height
     * on a page that is already tall. Height matters here - the
     * local font is ~30% larger than the target's (CLAUDE.md
     * section 1), so a row that fits here can still scroll there.
     *
     * AND THE ORDER IS THE LIFECYCLE: load it, then restart it.
     */
    r->load = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_LOAD), STR_STATUS_BTN_LOAD_TIP);
    gtk_widget_set_usize(r->load, 70, -1);
    gtk_signal_connect(GTK_OBJECT(r->load), "clicked",
                       GTK_SIGNAL_FUNC(on_row_load), r);
    gtk_box_pack_start(GTK_BOX(hbox), r->load, FALSE, FALSE, 0);


    /*
     * APPLY SETTINGS, BETWEEN Load AND Restart - the user's
     * placement, 2026-09-26, and the order is the lifecycle: load
     * it, adjust it, and restart it only if that is not enough.
     */
    r->apply = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_APPLY_SETTINGS), STR_STATUS_BTN_APPLY_SETTINGS_TIP);
    gtk_widget_set_usize(r->apply, 110, -1);
    gtk_signal_connect(GTK_OBJECT(r->apply), "clicked",
                       GTK_SIGNAL_FUNC(on_apply_settings), r);
    gtk_box_pack_start(GTK_BOX(hbox), r->apply, FALSE, FALSE, 0);


    r->restart = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_RESTART), STR_STATUS_BTN_RESTART_TIP);
    gtk_widget_set_usize(r->restart, 85, -1);
    gtk_signal_connect(GTK_OBJECT(r->restart), "clicked",
                       GTK_SIGNAL_FUNC(on_restart), r);
    gtk_box_pack_start(GTK_BOX(hbox), r->restart, FALSE, FALSE, 0);
    /* NOT SHOWN HERE - refresh() decides, and the shell's show_all()
     * would undo it anyway. See status_sync_visibility(). */


    gtk_widget_show(hbox);
    gtk_box_pack_start(GTK_BOX(parent), hbox, FALSE, FALSE, 0);
}

/* RE-APPLY WHAT show_all() TRAMPLED.
 *
 * THE SHELL CALLS gtk_widget_show_all() ON EVERY PAGE, and it is
 * RECURSIVE - so the Restart buttons this page hides for modules come
 * straight back, and the page is a row taller than it should be until
 * the first poll two seconds later corrects it. The user saw exactly
 * that: "when started they all had restart buttons and then the ones
 * that needed them kept them it also had a scroll bar on load".
 *
 * THE SAME BUG AS THE VOLUME MODULE'S, which has
 * volume_sync_visibility() for the same reason - and I did not carry
 * the lesson across. Any module hiding something at build time needs
 * this. */
void status_sync_visibility(void)
{
    refresh();
}

/*
 * ONE OF THE THREE Include-in-load BOXES WAS CLICKED.
 *
 * THE GUARD IS NOT DEFENSIVE PADDING. gtk_toggle_button_set_active()
 * emits "toggled", so refresh() putting the boxes back in step with
 * the config would re-enter here and write the value it had just
 * read. `g_en_setting' is raised around every programmatic set.
 *
 * A FAILED WRITE PUTS THE BOX BACK. Leaving it ticked after the file
 * could not be written would be the GUI showing a setting the
 * machine does not have - the row 58 mistake, a label disagreeing
 * with what is really in force.
 */
static void on_enable_toggled(GtkWidget *w, gpointer data)
{
    int  which = (int)(long)data;
    int  on;
    char msg[160];

    if (g_en_setting)
        return;
    if (which < 0 || which > 2)
        return;

    on = GTK_TOGGLE_BUTTON(w)->active ? 1 : 0;
    /* MEMORY ONLY - design/51, 2026-10-01: only the File menu writes.
     * The setting governs the next Load because plan_load() reads it
     * from memory; it used to vlhe_commit() here as well. */
    vlhe_set_component_enabled(which, on);

    sprintf(msg, FMT_STATUS_WILL_LOADED_FILE_SAVE,
            which == VLHE_ENABLE_SOUND ? "Sound" :
            which == VLHE_ENABLE_MIDI  ? "MIDI"  : "CD",
            on ? "" : "not ");
    report(msg);
}

GtkWidget *status_build(void (*report_fn)(const char *))
{
    GtkWidget   *outer;
    GtkWidget   *frame;
    GtkWidget   *vbox;
    GtkWidget   *note;
    int i;

    g_report = report_fn;

    outer = gtk_vbox_new(FALSE, 0);

    /*
     * THE UNFINISHED-UNLOAD BANNER, AT THE TOP - 7h Stage 2c. First on
     * the page because it is why Load will refuse. NOT SHOWN HERE:
     * refresh() decides, and GTK 1.2's show_all() is recursive, so the
     * hide in refresh() at the end of build() is what wins.
     */
    {
        GtkWidget *bv, *bb, *nn;

        g_left_frame = gtk_frame_new(STR_STATUS_FRAME_LEFTOVER);
        gtk_frame_set_shadow_type(GTK_FRAME(g_left_frame),
                                  GTK_SHADOW_ETCHED_IN);
        gtk_container_border_width(GTK_CONTAINER(g_left_frame), 8);
        gtk_box_pack_start(GTK_BOX(outer), g_left_frame, FALSE, FALSE, 0);

        bv = gtk_vbox_new(FALSE, 8);
        gtk_container_border_width(GTK_CONTAINER(bv), 8);
        gtk_container_add(GTK_CONTAINER(g_left_frame), bv);
        gtk_widget_show(bv);

        /* THE LEFTOVER'S ROW - hidden and shown on its own, so Not now
         * can hide it while a finding row stays. */
        g_left_row = gtk_vbox_new(FALSE, 4);
        gtk_box_pack_start(GTK_BOX(bv), g_left_row, FALSE, FALSE, 0);
        {
        GtkWidget *bv_frame = bv;

        bv = g_left_row;
        g_left_label = gtk_label_new("");
        gtk_label_set_justify(GTK_LABEL(g_left_label), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(g_left_label), 0.0, 0.0);
        vlhe_layout_wrap(g_left_label);
        /* THE SAME WRAP WIDTH AS THE PAGE NOTE, and for its reason -
         * see the long comment there. */
        gtk_box_pack_start(GTK_BOX(bv), g_left_label, FALSE, FALSE, 0);
        gtk_widget_show(g_left_label);

        bb = gtk_hbox_new(FALSE, 6);
        g_left_finish = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_FINISH_UNLOAD), STR_STATUS_BTN_FINISH_UNLOAD_TIP);
        gtk_signal_connect(GTK_OBJECT(g_left_finish), "clicked",
                           GTK_SIGNAL_FUNC(on_left_finish), NULL);
        gtk_box_pack_start(GTK_BOX(bb), g_left_finish, FALSE, FALSE, 0);
        gtk_widget_show(g_left_finish);

        nn = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_NOT_NOW), STR_STATUS_BTN_NOT_NOW_TIP);
        gtk_signal_connect(GTK_OBJECT(nn), "clicked",
                           GTK_SIGNAL_FUNC(on_left_not_now), NULL);
        gtk_box_pack_start(GTK_BOX(bb), nn, FALSE, FALSE, 0);
        gtk_widget_show(nn);

        vlhe_buttons_equalise(bb);
        gtk_box_pack_start(GTK_BOX(bv), bb, FALSE, FALSE, 0);
        gtk_widget_show(bb);
        bv = bv_frame;
        }

        /* ONE LINE FOR THE FINDINGS, and Review... - the user,
         * 2026-10-04, short of room. Shown by refresh(). */
        {
            GtkWidget *rb;

            g_rev_row = gtk_hbox_new(FALSE, 8);
            gtk_box_pack_start(GTK_BOX(bv), g_rev_row, FALSE, FALSE, 0);
            g_rev_label = gtk_label_new("");
            gtk_misc_set_alignment(GTK_MISC(g_rev_label), 0.0, 0.5);
            gtk_box_pack_start(GTK_BOX(g_rev_row), g_rev_label, FALSE, FALSE, 0);
            gtk_widget_show(g_rev_label);
            rb = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_REVIEW),
                             STR_STATUS_BTN_REVIEW_TIP);
            gtk_signal_connect(GTK_OBJECT(rb), "clicked",
                               GTK_SIGNAL_FUNC(on_review), NULL);
            gtk_box_pack_start(GTK_BOX(g_rev_row), rb, FALSE, FALSE, 0);
            gtk_widget_show(rb);
        }
    }

    frame = gtk_frame_new(STR_STATUS_FRAME_COMPONENTS);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    g_summary = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_summary), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_summary, FALSE, FALSE, 0);
    gtk_widget_show(g_summary);

    /* A GAP BETWEEN THE SUMMARY AND THE ROWS, so the one-line answer
     * reads as a heading rather than as a seventh component. */
    note = gtk_label_new("");
    gtk_widget_set_usize(note, -1, 6);
    gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
    gtk_widget_show(note);

    g_nrow = VLHE_MAX_COMPONENTS;
    for (i = 0; i < g_nrow; i++) {
        memset(&g_row[i], 0, sizeof g_row[i]);
        build_row(&g_row[i], vbox);
    }

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* WHAT "RUNNING" DOES AND DOES NOT MEAN. design/09 wants a way to
     * see that a daemon is WEDGED rather than merely quiet, and says
     * it needs the counters - which do not exist. Saying so is better
     * than a status line that quietly overpromises. */
    /* WHY THE MODULE BUTTONS ARE GREY, ON SCREEN RATHER THAN IN A
     * TOOLTIP. A tooltip is not discoverable by someone who has not
     * already guessed the control exists, and the Xaw build will have
     * no tooltips at all - so anything that matters is text.
     *
     * AND "NEEDS ROOT" WOULD BE MISLEADING, which is why it is not
     * the whole sentence. Root is the smaller half: design/09 records
     * that rmmod FAILS while anything holds the device, and that
     * unloading vsound takes the whole audio path down with it.
     * Running as root would not enable these buttons, so saying only
     * "needs root" would send someone off to try it. */
    /*
     * ---- LOAD AND UNLOAD ------------------------------------
     *
     * THE BUTTONS THE USER ASKED FOR, 2026-09-23: "again i have no
     * way to test the modules from the gui". The Status page showed
     * what was loaded and offered no way to change it, so trying a
     * build meant dropping to a terminal - precisely the friction
     * portable mode exists to remove.
     *
     * THEY CALL vlhe_plan_run() DIRECTLY, not the init script. The
     * GUI already links the plan builder, so shelling out would be a
     * fork and a shell to reach a function already in this address
     * space, and would trade the plan per-step reporting for an exit
     * status. The init script keeps the job only it can do: boot.
     *
     * ROOT IS THE GATE, and the sidebar already says so when it is
     * missing. A portable copy ships no setuid build (design/33), so
     * running it as root IS the documented way there.
     */
    /*
     * IN A FRAME OF ITS OWN, like Components above - the user,
     * 2026-09-23: "the load unload and everything below that should
     * be in a box like the components otherwise the load button is
     * right against the border between the side bar".
     *
     * They are right and it is not only cosmetic: packed straight
     * into `outer' the buttons got the vbox's zero border, so Load
     * sat flush against the sidebar divider while every other control
     * on the page is inset by eight. A frame gives them the same
     * inset AND groups them with the note that explains them, which
     * is what makes the two read as one thing.
     */
    frame = gtk_frame_new(STR_SHELL_BTN_APPLY);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);
    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /*
     * INCLUDE IN LOAD - the three Enable boxes, above the buttons
     * they govern so the page reads "what, then do it".
     *
     * ON ONE ROW, because they are one decision made three ways and a
     * vertical stack of three would push the note into a scrollbar -
     * the failure row 71 and the Mixing frame both hit on this page's
     * neighbours. Three short labels fit a line at the target's font
     * (CLAUDE.md section 1: this workstation renders ~30% taller, so
     * a layout that fits HERE fits there).
     */
    {
        GtkWidget *erow = gtk_hbox_new(FALSE, 12);
        GtkWidget *lab  = gtk_label_new(STR_STATUS_LABEL_INCLUDE_LOAD);
        static const char *name[3] = { STR_STATUS_CHECK_INCLUDE_SOUND,
                                       STR_STATUS_CHECK_INCLUDE_MIDI,
                                       STR_STATUS_CHECK_INCLUDE_CD };
        int k;

        gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.5);
        gtk_box_pack_start(GTK_BOX(erow), lab, FALSE, FALSE, 0);
        gtk_widget_show(lab);

        g_en_setting = 1;
        for (k = 0; k < 3; k++) {
            g_en[k] = gtk_check_button_new_with_label(name[k]);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_en[k]),
                                         vlhe_component_enabled(k)
                                             ? TRUE : FALSE);
            gtk_signal_connect(GTK_OBJECT(g_en[k]), "toggled",
                               GTK_SIGNAL_FUNC(on_enable_toggled),
                               (gpointer)(long)k);
            gtk_box_pack_start(GTK_BOX(erow), g_en[k], FALSE, FALSE, 0);
            gtk_widget_show(g_en[k]);
        }
        g_en_setting = 0;

        {
            vlhe_tip(g_en[VLHE_ENABLE_SOUND], STR_STATUS_CHECK_INCLUDE_SOUND_TIP);
            vlhe_tip(g_en[VLHE_ENABLE_MIDI], STR_STATUS_CHECK_INCLUDE_MIDI_TIP);
            vlhe_tip(g_en[VLHE_ENABLE_CD], STR_STATUS_CHECK_INCLUDE_CD_TIP);
        }

        gtk_box_pack_start(GTK_BOX(vbox), erow, FALSE, FALSE, 0);
        gtk_widget_show(erow);
    }

    {
        GtkWidget *bbox = gtk_hbox_new(FALSE, 6);

        g_load_btn = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_LOAD_MAIN), STR_STATUS_BTN_LOAD_MAIN_TIP);
        gtk_signal_connect(GTK_OBJECT(g_load_btn), "clicked",
                           GTK_SIGNAL_FUNC(on_load), NULL);
        gtk_box_pack_start(GTK_BOX(bbox), g_load_btn, FALSE, FALSE, 0);
        gtk_widget_show(g_load_btn);

        g_unload_btn = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_UNLOAD), STR_STATUS_BTN_UNLOAD_TIP);
        gtk_signal_connect(GTK_OBJECT(g_unload_btn), "clicked",
                           GTK_SIGNAL_FUNC(on_unload), NULL);
        gtk_box_pack_start(GTK_BOX(bbox), g_unload_btn, FALSE, FALSE, 0);
        gtk_widget_show(g_unload_btn);

        /* SIMULATE - live for everyone, it runs nothing. See on_simulate. */
        {
            GtkWidget *sim = vlhe_tipped(gtk_button_new_with_label(STR_STATUS_BTN_SIMULATE), STR_STATUS_BTN_SIMULATE_TIP);

            gtk_signal_connect(GTK_OBJECT(sim), "clicked",
                               GTK_SIGNAL_FUNC(on_simulate), NULL);
            /* AT THE RIGHT, flush with the Restart buttons above - the
             * user, 2026-10-01. Both frames share the pane's width and
             * the same inner border, so pack_end lands on the same
             * edge the Components table's last column does. */
            gtk_box_pack_end(GTK_BOX(bbox), sim, FALSE, FALSE, 0);
            gtk_widget_show(sim);
        }

        {
        }

        /* The three the same size, like the dialogs' buttons - the
         * user, 2026-10-01, seeing them on the target. */
        vlhe_buttons_equalise(bbox);

        gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);
        gtk_widget_show(bbox);
    }

    /*
     * NOT SHOWN HERE - refresh() decides, every poll, and a machine
     * with both settings in place must never see this line.
     *
     * AND IT IS NOT IN THE SHELL'S show_all() PATH BY ACCIDENT: GTK
     * 1.2's show_all() is RECURSIVE and undoes a deliberate hide
     * (CLAUDE.md section 1), which is why the per-module Restart
     * buttons needed the same treatment. refresh() runs at the end of
     * build() and again every two seconds, so the hide always wins.
     */
    /* NOT SHOWN HERE either - refresh() decides, and show_all() is
     * recursive. Packed before the warnings so the order reads
     * state-then-problems. */
    g_cdromline = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(g_cdromline), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(g_cdromline), 0.0, 0.0);
    /* PADDING RATHER THAN A SPACER LABEL, because this line is hidden
     * most of the time and a separate spacer would leave a gap behind
     * when it goes. Measured from a capture: with none, the line
     * reads as the first sentence of the note below it rather than as
     * state. */
    gtk_box_pack_start(GTK_BOX(vbox), g_cdromline, FALSE, FALSE, 6);

    g_notready = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(g_notready), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(g_notready), 0.0, 0.0);
    vlhe_layout_wrap(g_notready);       /* reload lines are long */
    gtk_box_pack_start(GTK_BOX(vbox), g_notready, FALSE, FALSE, 0);

    note = gtk_label_new(
        /* FOUR LINES, DOWN FROM ELEVEN. The long version explained
         * WHY - that rmmod fails while the device is held, that
         * unloading vsound takes the audio path with it, that telling
         * a wedged daemon from a quiet one needs counters nobody has
         * built - and it pushed the page into a scrollbar.
         *
         * THAT DETAIL IS REASONING, NOT INSTRUCTION. It belongs in
         * design/32 section 11, where it is.
         *
         * AND THE THIRD LINE WENT TOO - 2026-09-29. It said
         * *"Running" means the process exists, not that it is
         * working.* True, and it put the page one line over on the
         * target: the user's capture shows it clipped at the bottom
         * edge with everything above it fitting. Removing it and its
         * blank separator recovers 40px, which is the whole of the
         * overflow there.
         *
         * MEASURE THE TARGET, NOT THIS WORKSTATION. A first pass at
         * this measured the page HERE - 677x670 against a 640x518
         * content area - and concluded 152px were still missing
         * after the cut, which would have meant removing the other
         * two lines as well. That figure is this machine's 18px font
         * against the target's 14px, the difference CLAUDE.md
         * records as ~30% on every label. The target needed one
         * line. */
        STR_STATUS_LABEL_PAGE_NOTE);
    gtk_label_set_justify(GTK_LABEL(note), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.0);
    /*
     * WRAPPED TO THE FRAME, NOT BROKEN BY HAND - 2026-09-29.
     *
     * The two paragraphs above carried their own newlines, chosen
     * for a width somebody guessed at, so the label was exactly as
     * wide as the longest hand-broken line and the space to its
     * right was wasted. The \n between the paragraphs stays: that
     * is a real break, not a line ending.
     *
     * AND THE usize IS THE WRAP WIDTH, not a floor under it - which
     * two attempts here got wrong in opposite directions.
     *
     * WITHOUT ONE the label wraps to GTK's fallback and takes three
     * lines. gtklabel.c's size_request is explicit: given
     * `aux-info->width' (what set_usize sets) it splits to that;
     * otherwise MIN(the width of a fixed sample sentence, screen/2,
     * longest_paragraph) - 289px here. THERE IS NO PATH THAT WRAPS
     * TO THE ALLOCATION, so packing TRUE/TRUE cannot help:
     * measured, a label with usize 380 in a 620-wide box is
     * ALLOCATED 620 and still wraps at 380.
     *
     * SO THE NUMBER IS THE WIDTH AND IT HAD BETTER BE RIGHT. 380
     * was a deliberate under-estimate on the theory that it was a
     * floor, and it left two fifths of the frame empty - the user,
     * with a capture: "why is there still wasted space to the right
     * of them?"
     *
     * DERIVED, NOT READ OFF A CAPTURE HERE. The window is 800x600
     * by construction and the sidebar 160 (WIN_W and SIDEBAR_W in
     * vlhe_cc.c, private to it - the arithmetic is repeated here
     * rather than exporting two constants for one call site). Both
     * are the same on either machine, so this gives the same answer
     * on the target, where a pixel count measured on this
     * workstation would not. That is the 18px/14px trap CLAUDE.md
     * records, and this page has already fallen into it once
     * tonight.
     *
     * A size_allocate HANDLER WOULD BE EXACT AND IS A LOOP: setting
     * usize from the allocation re-requests, which re-allocates.
     * Nothing else in this tree does it.
     */
    /*
     * THE WIDTH IS COMPUTED NOW - vlhe_layout.c, 2026-10-07. This was
     * 800 - 160 - 15 - 65 = 560, reached by consequence: 585 gave the
     * target a horizontal bar, the frame was measured asking 20 px more
     * than its label, and 560 was chosen to miss low. vlhe_layout
     * walks the frames and measures the scrollbar instead.
     */
    vlhe_layout_wrap(note);
    gtk_box_pack_start(GTK_BOX(vbox), note, TRUE, TRUE, 4);
    gtk_widget_show(note);

    refresh();
    /* The 2 s refresh is started by status_set_active(1) when the page
     * is shown, not here. */

    gtk_widget_show(outer);
    return outer;
}
