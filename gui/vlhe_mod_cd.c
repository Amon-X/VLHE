/*
 * vlhe_mod_cd.c - CD Settings: the drives and their discs.
 *
 * Copyright (c) 2026 Thomas Tranter
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Part of VLHE. See LICENSE.TXT for the full license text.
 *
 * ONE ROW PER DRIVE, SIZED TO vlhe_drive_count() - not to the mockup's
 * single drive. vdisc advertises VDISC_DEF_DEVS (1) unless `discs='
 * says otherwise, up to VDISC_MAX_DEVS (8), and discs attach and
 * detach into those slots AT RUNTIME with no reload. A normal machine
 * draws one row; a machine configured for four draws four, and nothing
 * here changes.
 *
 * THOSE SLOTS WERE TO BE THE CHANGER. Corrected 2026-10-03 (design/54
 * T15): the daemon is no longer the limit - one vdiscd serves every
 * drive, each with its own image and CD-audio player, attached over
 * its control channel - and the changer use case itself did not
 * survive the research (design/27 section 7; design/54 C06).
 *
 * TWO TABS:
 *
 *   Drive   the slots - attach, detach. A LIVE page: everything
 *           on it acts at once, the way putting a disc in a drive
 *           does, so it carries no OK/Apply/Cancel.
 *   Audio   CD-audio settings. A form.
 *
 * C89, GCC 2.95.2, GTK 1.2.
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "vlhe_backend.h"
#include "vlhe_strings.h"
#include "vlhe_tip.h"
#include "vlhe_priv.h"       /* vlhe_priv_can_act */
#include "vlhe_mod_cd.h"
#include "vlhe_filter.h"
#include "vlhe_mod_cdg.h"   /* cdg_drive_chosen() - the viewer follows */

#define POLL_MS         1000    /* discs change far less often than
                                 * volume; a second is plenty and this
                                 * has no generation counter to make a
                                 * check free */

#define PAGE_DRIVE      0
#define PAGE_OPTIONS    1

struct driverow {
    GtkWidget  *row;        /* the nested frame                      */
    GtkWidget  *frame;      /* same widget; named for its label      */
    GtkWidget  *image;      /* the path - a read-only GtkEntry       */
    GtkWidget  *info;       /* what the disc IS                      */
    GtkWidget  *autoload;
    GtkWidget  *cdg;        /* which drive the CD+G viewer uses - a
                             * radio, one group across the rows      */
    GtkWidget  *cdrom;      /* which drive /dev/cdrom reaches - a
                             * second group, 2026-10-03              */
    GtkWidget  *attach;
    GtkWidget  *eject;

    int         index;
    int         setting;
    char        failed[VLHE_PATH_MAX];  /* path the dialog is about */
    GtkWidget  *dialog;                 /* NULL when none is open   */
    GtkWidget  *picker;                 /* the image chooser        */
    GtkWidget  *recent;                 /* its recent-images menu   */
    /* THE RECENT PATHS THIS ROW'S MENU POINTS INTO - design/47 C6. One
     * static array used to serve every drive's picker, so opening a
     * second drive's chooser rewrote the strings the first one's menu
     * items still pointed at. Per row, there is nothing to share. */
    char        recent_path[VLHE_MAX_RECENT][VLHE_PATH_MAX];
};

static struct driverow  g_row[VLHE_MAX_DRIVE];
static int              g_nrow;
static GSList          *g_cdg_group;    /* the rows' CD+G radio group */
static GSList          *g_cdrom_group;  /* and /dev/cdrom's          */
static int              g_page;
static GtkWidget       *g_rows_box;
static GtkWidget       *g_nomodule;
static GtkWidget       *g_nodaemon;     /* module in, daemon down    */
static GtkWidget       *g_notebook;
static void           (*g_report)(const char *);
static void           (*g_page_cb)(int);

static GtkWidget       *g_opt_ndevs;
static GtkWidget       *g_opt_major;
static GtkWidget       *g_opt_mimic;
static GtkWidget       *g_opt_mimic_menu;
static GtkWidget       *g_opt_packet;
static GtkWidget       *g_opt_cdromlink;
static GtkWidget       *g_opt_reattach;     /* [CD Settings] DrivesAutoLoad */
static GtkWidget       *g_opt_cdg_follow;   /* [CDG Viewer] FollowAnyDisc */
static GtkWidget       *g_opt_cdg_stop;     /* [CDG Viewer] StopOnSwap    */
static GtkWidget       *g_root_note;   /* "needs root", hidden after Modify */
static GtkWidget       *g_opt_status;
static int              g_opt_setting;
static int              g_opt_dirty;

static void report(const char *m)
{
    if (g_report != NULL)
        g_report(m);
}

/* ------------------------------------------------------------------ */
static void build_row(struct driverow *r, GtkWidget *parent);

static void refresh(void)
{
    struct vlhe_drive d[VLHE_MAX_DRIVE];
    struct vlhe_modopts mo;
    int n, i, cdrom_linked;

    n = vlhe_drives(d, VLHE_MAX_DRIVE);
    /* IS /dev/cdrom OURS TO MOVE - the saved Options setting, which is
     * what the last load acted on. */
    cdrom_linked = (vlhe_modopts(&mo) == 0 && mo.link_cdrom);

    if (n == 0) {
        /* THE MODULE IS NOT LOADED. Same reasoning as the Volume
         * module's absent state: a user can rmmod from a terminal
         * while this window sits open, and showing drives that are not
         * there would be a lie. */
        if (g_rows_box != NULL)
            gtk_widget_hide(g_rows_box);
        if (g_nomodule != NULL)
            gtk_widget_show(g_nomodule);
        /*
         * AND HIDE THE OTHER NOTICE, which this return used to skip.
         *
         * `g_nodaemon' is shown or hidden further down, past this
         * early return - so a page that had been showing it kept
         * showing it once the drives went away, and the user saw both
         * at once on target 2026-09-21:
         *
         *     The vdisc module is not loaded.
         *     No virtual drives are available.
         *     The vdisc module is loaded, but vdiscd is not running.
         *
         * Two mutually exclusive statements, one above the other.
         * With no drives there is nothing for the daemon notice to be
         * about - the module notice says everything.
         */
        if (g_nodaemon != NULL)
            gtk_widget_hide(g_nodaemon);
        return;
    }

    if (g_nomodule != NULL)
        gtk_widget_hide(g_nomodule);
    if (g_rows_box != NULL)
        gtk_widget_show(g_rows_box);

    /*
     * THE DAEMON, SEPARATELY FROM THE MODULE. `present' means the
     * PROCESS EXISTS and nothing more (vlhe_backend.h is emphatic
     * about that), which is the right test here: a daemon that is
     * running but wedged would still answer this, and the attach
     * timeout is what covers that case.
     */
    if (g_nodaemon != NULL) {
        struct vlhe_component comp[VLHE_MAX_COMPONENTS];
        int nc = vlhe_components(comp, VLHE_MAX_COMPONENTS);
        int up = 0, j;

        for (j = 0; j < nc; j++) {
            if (comp[j].kind == VLHE_COMP_DAEMON
                && strcmp(comp[j].name, "vdiscd") == 0) {
                up = comp[j].present;
                break;
            }
        }
        if (up)
            gtk_widget_hide(g_nodaemon);
        else
            gtk_widget_show(g_nodaemon);
    }

    /*
     * MORE DRIVES THAN ROWS - BUILD THE REST NOW. design/47 C4: the
     * rows were made once at build from vlhe_drive_count(), minimum
     * one, so a page opened before Load with Drives=4 showed one row
     * until the program was restarted. Rows are only ever added, up
     * to the array; the loop below hides the ones past the count.
     */
    while (n > g_nrow && g_nrow < VLHE_MAX_DRIVE && g_rows_box != NULL) {
        memset(&g_row[g_nrow], 0, sizeof g_row[g_nrow]);
        build_row(&g_row[g_nrow], g_rows_box);
        g_nrow++;
    }

    for (i = 0; i < g_nrow; i++) {
        struct driverow *r = &g_row[i];
        char buf[VLHE_PATH_MAX + 32];

        if (i >= n) {
            gtk_widget_hide(r->row);
            continue;
        }

        r->setting = 1;
        r->index = d[i].index;

        /* THE DEVICE NAME IS THE FRAME'S LABEL, drawn in the border. */
        gtk_frame_set_label(GTK_FRAME(r->frame), d[i].node);

        if (d[i].attached) {
            /*
             * THE FULL PATH, ASKED FOR SEPARATELY - 2026-09-27.
             *
             * `d[i].image' COMES FROM `status', WHICH TRUNCATES TO 28
             * CHARACTERS (vdiscd.c cmd_status), and the user saw the
             * result on this very page: `/mnt/discs/infosoc/infosoc.i'
             * where the disc is `infosoc.ccd'. *"I think this is the
             * truncation bug at work"* - and they were right, it is
             * the same one the CD+G page hit, showing here.
             *
             * THE TRUNCATION IS NOT ITSELF A BUG, and cannot simply be
             * widened: `status' names EVERY drive in one 320-byte
             * reply, and eight paths at 255 would need 2072. So the
             * short form stays for IDENTIFYING a drive, and anything
             * that needs a real path asks the `image' verb.
             *
             * IT MATTERS MOST HERE, of all places: the field is
             * scrolled to its right-hand end just below, precisely so
             * the FILENAME shows - the user's stated priority - and
             * truncation eats the filename first.
             *
             * DISPLAY ONLY, so a failure is not fatal: the entry is
             * set_editable(FALSE) and never read back. Fall back to
             * the short form rather than showing nothing.
             */
            char full[VLHE_PATH_MAX];

            if (vlhe_drive_image(d[i].index, full, sizeof full) != 0) {
                strncpy(full, d[i].image, sizeof full - 1);
                full[sizeof full - 1] = '\0';
            }

            /* GUARDED: setting the text fires "changed", which is the
             * same signal a dropdown pick fires. */
            gtk_entry_set_text(GTK_ENTRY(r->image), full);

            /* SCROLLED TO THE END, so the FILENAME is what shows - the
             * user's priority: "the image name is the most important
             * part not necessarily the full path". A path renders
             * left to right, so its name is at the RIGHT end; putting
             * the cursor there scrolls the view to it. Exactly the
             * state KsCD's CDDB field is in. */
            gtk_entry_set_position(GTK_ENTRY(r->image), -1);

            /* WHAT THE DISC IS, unabbreviated and with room for more
             * than the track count:
             *
             *   Mixed Mode - 11 tracks, 10 audio
             *   Mode 2 / XA - 2 tracks, 2 sessions
             *
             * THE DATA-TRACK COUNT IS IMPLIED rather than printed: a
             * user reading "11 tracks, 10 audio" can subtract, and the
             * line stays short. Sessions appear only when there is
             * more than one, since 1 is the ordinary case and saying
             * so every time is noise. */
            if (d[i].kind[0])
                sprintf(buf, FMT_CD_TRACK, d[i].kind, d[i].tracks,
                        d[i].tracks == 1 ? "" : STR_CD_TEXT_PLURAL_S);
            else
                sprintf(buf, FMT_CD_TRACK_2, d[i].tracks,
                        d[i].tracks == 1 ? "" : STR_CD_TEXT_PLURAL_S);

            if (d[i].audio_tracks > 0)
                sprintf(buf + strlen(buf), FMT_CD_COUNT_AUDIO,
                        d[i].audio_tracks);
            if (d[i].sessions > 1)
                sprintf(buf + strlen(buf), FMT_CD_COUNT_SESSIONS,
                        d[i].sessions);

            gtk_label_set_text(GTK_LABEL(r->info), buf);
        } else {
            gtk_entry_set_text(GTK_ENTRY(r->image), "");
            gtk_label_set_text(GTK_LABEL(r->info), STR_CD_TEXT_NO_DISC);
        }

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->autoload),
                                     d[i].autoload ? TRUE : FALSE);

        /* THE CD+G VIEWER'S DRIVE - design/31 C9. Only worth showing
         * when there is a choice: one drive is always the viewer's.
         * Live on an empty drive too - the choice is the DRIVE, and
         * the disc may go in afterwards. */
        if (n > 1) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->cdg),
                                         d[i].index == vlhe_cdg_drive()
                                         ? TRUE : FALSE);
            gtk_widget_show(r->cdg);
        } else {
            gtk_widget_hide(r->cdg);
        }

        /* /dev/cdrom's DRIVE - only when there is a choice AND the
         * Options tab's "Point /dev/cdrom" is on: otherwise the link is
         * not ours and the radio would move nothing anyone opens. */
        if (n > 1 && cdrom_linked) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->cdrom),
                                         d[i].cdrom ? TRUE : FALSE);
            gtk_widget_show(r->cdrom);
        } else {
            gtk_widget_hide(r->cdrom);
        }

        /* AN EMPTY SLOT CANNOT BE EJECTED - the eject would fail. */
        gtk_widget_set_sensitive(r->eject, d[i].attached);
        gtk_widget_set_sensitive(r->autoload, d[i].attached);

        /*
         * `Image...' IS THE ONE THAT MUST STAY LIVE WHEN EMPTY - it
         * is how a disc gets IN. Greying it with the others made the
         * page a one-way door: eject once and there was no way to
         * attach anything again (the user, 86Box 2026-09-19).
         *
         * The three above are operations ON a disc, so an empty
         * drive genuinely cannot do them. This one is the opposite.
         */
        gtk_widget_set_sensitive(r->image, TRUE);

        gtk_widget_show(r->row);
        r->setting = 0;
    }
}

/*
 * ONLY WHEN THIS PAGE IS ON SCREEN.
 *
 * refresh() IS NOT CHEAP and the timer runs whether or not anyone is
 * looking at it. Each tick calls vlhe_drives(), which - when its kept
 * answer is over 900 ms old, or a change has forgotten it (2026-10-03):
 *
 *   - sends `status' over the control channel, and that SIGNALS
 *     vdiscd with SIGUSR1 to pull it out of its blocking GET_REQ
 *     (vdiscd_ctl.h) - once a second, forever, for a page nobody may
 *     be looking at;
 *   - (until 2026-10-05) called vlhe_locked() per drive, which
 *     opened and closed /dev/vdiscN - gone with the Lock box (design/54
 *     D64, removed at the user's decision).
 *
 * The user's question, 2026-09-21: "Do we need to be refreshing that
 * 4 times a second?" No - and not when it is hidden either.
 */
static int g_visible;

static gint on_poll(gpointer data)
{
    (void)data;
    if (g_visible)
        refresh();
    return TRUE;
}

/*
 * THE SHELL SAYS WHEN. Called from show_module(), so the page brings
 * itself up to date the moment it is shown rather than waiting for
 * the next tick - a stale page for up to a second would be worse
 * than the polling it saves.
 */
void
cd_set_active(int on)
{
    g_visible = on ? 1 : 0;
    if (g_visible)
        refresh();
}

/* ------------------------------------------------------------------ */

/*
 * THE CD+G VIEWER'S DRIVE - design/31 C9, the user 2026-10-03: "The
 * radio button was on the page where you attached the images. Defaults
 * to vdisc0 hidden when only one drive. It should be remembered."
 *
 * A radio fires "toggled" on the button going OFF as well as the one
 * going on; only the one going on means anything. Memory only, as
 * every option is since design/51 - the message says how to keep it.
 * The viewer follows on its next poll: audio_drive() asks each time.
 */
static void on_cdg_toggled(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;
    char msg[160];
    char node[32];

    if (r->setting || !GTK_TOGGLE_BUTTON(w)->active)
        return;
    if (r->index == vlhe_cdg_drive())
        return;
    if (vlhe_set_cdg_drive(r->index) != 0) {
        report(STR_CD_MSG_COULD_NOT_CHANGE_DRIVE);
        return;
    }
    sprintf(node, "/dev/vdisc%d", r->index);
    sprintf(msg, FMT_CD_MSG_CDG_DRIVE, node);
    report(msg);
    cdg_drive_chosen();     /* StopOnSwap acts now, not on returning */
}

/*
 * /dev/cdrom TO THIS DRIVE - 2026-10-03, the user's design: one
 * link made at Load, its inner half moved live by vdiscd. The same
 * radio shape as the CD+G viewer's, for the same reason.
 */
static void on_cdrom_toggled(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;
    char msg[200];
    char node[32];

    if (r->setting || !GTK_TOGGLE_BUTTON(w)->active)
        return;
    if (vlhe_set_cdrom_drive(r->index) != 0) {
        report(STR_CD_MSG_CDROM_NOT_MOVED);
        return;                 /* the next refresh puts the radio back */
    }
    sprintf(node, "/dev/vdisc%d", r->index);
    sprintf(msg, FMT_CD_MSG_CDROM_DRIVE, node);
    report(msg);
}

static void on_autoload_toggled(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;
    int on;

    if (r->setting)
        return;

    on = GTK_TOGGLE_BUTTON(w)->active;

    if (vlhe_set_autoload(r->index, on) != 0) {
        report(STR_CD_MSG_COULD_NOT_CHANGE_DRIVE);
        /* PUT IT BACK, because unlike a volume slider there is no
         * poll here that would correct it within a second. */
        r->setting = 1;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                     on ? FALSE : TRUE);
        r->setting = 0;
        return;
    }

    report(on ? STR_CD_MSG_LOADED_AT_STARTUP
              : STR_CD_MSG_NOT_LOADED_AT_STARTUP);
}

static void on_eject(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;

    (void)w;

    /* NO LOCK TO CHECK - the Lock box is gone (design/54 D64). The
     * kernel's own refusal of a drive in use is what stops an eject
     * that would pull a disc out from under a mount or a player. */
    if (vlhe_detach(r->index) != 0) {
        report(STR_CD_MSG_COULD_NOT_EJECT_DRIVE);
        return;
    }

    report(STR_CD_MSG_DISC_EJECTED);
    refresh();
}

/* "That image could not be opened - remove it from the list?"
 *
 * BUILT BY HAND BECAUSE GTK 1.2 HAS NO MESSAGE DIALOG. There is no
 * gtk_message_dialog until GTK 2; a 1.2 program assembles a GtkDialog
 * from its vbox and action area, which is what this does.
 *
 * MODAL, because it asks a question whose answer changes the list the
 * window behind it is showing.
 *
 * WHY A DIALOG AT ALL, when everything else in this module reports
 * through the status bar: the status bar states what happened, and
 * this needs an ANSWER. A question nobody notices is a question that
 * does not get answered, and the cost of missing it is a stale entry
 * that fails again next time.
 */
static void on_forget_yes(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;

    (void)w;

    /* NOTHING TO REFRESH. The recent menu is built fresh every time
     * the picker opens, so the next one sees the shortened list. */
    if (vlhe_recent_remove(r->failed) == 0)
        report(STR_CD_MSG_REMOVED_FROM_RECENT_LIST);

    gtk_widget_destroy(r->dialog);
    r->dialog = NULL;
}

static void on_forget_no(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;

    (void)w;

    report(STR_CD_MSG_KEPT_RECENT_LIST);
    gtk_widget_destroy(r->dialog);
    r->dialog = NULL;
}

static void ask_forget(struct driverow *r)
{
    GtkWidget *dlg;
    GtkWidget *label;
    GtkWidget *b;
    char msg[VLHE_PATH_MAX + 160];

    if (r->dialog != NULL)
        return;                 /* one at a time */

    dlg = gtk_dialog_new();
    r->dialog = dlg;
    gtk_window_set_title(GTK_WINDOW(dlg), STR_CD_TITLE_IMAGE_NOT_AVAILABLE);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_container_border_width(GTK_CONTAINER(dlg), 8);

    /* THE PATH IS IN THE MESSAGE, because "that image" is not enough
     * to act on when ten of them are in the list. */
    sprintf(msg,
            FMT_CD_DISC_IMAGE_COULD_NOT,
            r->failed);

    label = gtk_label_new(msg);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.0);
    gtk_container_border_width(GTK_CONTAINER(label), 8);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), label,
                       TRUE, TRUE, 0);
    gtk_widget_show(label);

    /* KEEP IS THE DEFAULT, deliberately. Return should do the SAFE
     * thing, and for removable media "keep" is safe where "remove"
     * throws away a path the user may want tomorrow. */
    b = vlhe_tipped(gtk_button_new_with_label(STR_CD_BTN_KEEP), STR_CD_BTN_KEEP_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_forget_no), r);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), b,
                       TRUE, TRUE, 0);
    gtk_widget_show(b);
    gtk_widget_grab_default(b);

    b = vlhe_tipped(gtk_button_new_with_label(STR_CD_BTN_REMOVE), STR_CD_BTN_REMOVE_TIP);
    GTK_WIDGET_SET_FLAGS(b, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(b), "clicked",
                       GTK_SIGNAL_FUNC(on_forget_yes), r);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->action_area), b,
                       TRUE, TRUE, 0);
    gtk_widget_show(b);

    gtk_widget_show(dlg);
}

/* ------------------------------------------------------------------ *
 * The image picker                                                    *
 * ------------------------------------------------------------------ */

/* ONE PLACE LOADS A DISC, and the recent list lives inside it.
 *
 * THE USER'S DESIGN, after a GtkCombo on the drive row was tried and
 * reverted: "it might be better in the image loading like the file
 * picker since this would load and unload the daemon and load the
 * image if selected". Choosing is what a dialog is FOR - nothing acts
 * on a stray click, the browse and the shortcut are in one place, and
 * the daemon lifecycle has one code path rather than two.
 *
 * GtkFileSelection IS A GtkWindow WITH ITS OWN vbox, so the recent
 * list packs straight into it above the file list. GTK 1.2 has no
 * "places" sidebar to fight with.
 *
 * WHAT IT DOES NOT DO YET: attach. design/09 left the mechanism open -
 * a control channel to a running vdiscd, or a signal plus a path in
 * the config file - so OK reports what it WOULD do rather than doing
 * it. The shape is testable now; the wiring waits on that decision.
 */

static void picker_close(struct driverow *r)
{
    if (r->picker != NULL) {
        gtk_widget_destroy(r->picker);
        r->picker = NULL;
    }
}

static void on_picker_ok(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;
    const char *path;
    char msg[VLHE_PATH_MAX + 64];
    int  rc;

    (void)w;

    path = gtk_file_selection_get_filename(GTK_FILE_SELECTION(r->picker));
    if (path == NULL || path[0] == '\0') {
        picker_close(r);
        return;
    }

    strncpy(r->failed, path, VLHE_PATH_MAX - 1);
    r->failed[VLHE_PATH_MAX - 1] = '\0';
    picker_close(r);

    /* ATTACH IS NOT WIRED (see the header comment), but the FAILURE
     * path is - vlhe_attach() against the fake refuses a path that is
     * not there, which is how the "image has moved" prompt gets
     * exercised. Against the real backend this becomes the ordinary
     * attach, and the prompt keeps the same job. */
    rc = vlhe_attach(r->index, r->failed);
    if (rc == 0) {
        report(STR_CD_MSG_DISC_ATTACHED);
        refresh();
        return;
    }

    /*
     * THE DAEMON ANSWERED AND SAID WHY - 2026-10-05. Before this every
     * failure was -1 and the page guessed: an image in /root, which
     * the installed vdiscd (the `vlhe' account) cannot read, was
     * reported as "check that vdiscd is running" while it ran -
     * because THIS process is root and could open the file. Found by
     * the G01 documentation review, confirmed on 86Box with
     * /root/quake/QUAKE106.ccd.
     */
    if (rc == VLHE_ATTACH_DENIED) {
        report(STR_CD_MSG_COULD_NOT_ATTACH_DENIED);
        return;
    }
    if (rc == VLHE_ATTACH_REFUSED) {
        report(STR_CD_MSG_COULD_NOT_ATTACH_REFUSED);
        return;
    }

    /*
     * ONLY OFFER TO FORGET IT IF IT IS ACTUALLY GONE - added
     * 2026-09-21 after the user hit the other case on 86Box.
     *
     * vlhe_attach() returned -1 for EVERY failure then, so this code
     * could not tell "the image moved" from "no daemon is listening".
     * It now says which (above); what reaches here is no answer, or
     * a file vdiscd found missing. With
     * vdiscd not running - which is the state a stopped or
     * half-applied plan leaves - every attach failed and the GUI
     * offered to drop PERFECTLY GOOD IMAGES from the recent list.
     * The user watched valid entries being proposed for deletion
     * while the files sat in /root.
     *
     * THAT IS THE WORST SHAPE A BUG CAN TAKE HERE: the recent list is
     * the only record of where someone keeps their images, the
     * dialog's default is the destructive answer's neighbour, and
     * the prompt is most likely to appear exactly when the daemon is
     * down - the moment its advice is wrong.
     *
     * SO ASK THE FILESYSTEM. If the path still opens, the image did
     * not move and the failure is ours to explain, not the user's to
     * clean up after.
     */
    {
        FILE *fp = fopen(r->failed, "rb");

        if (fp != NULL) {
            fclose(fp);
            /* IT IS THERE. With no answer from vdiscd that is the
             * reason, and the Status page confirms it in a click. A
             * "missing" from a daemon that CAN be asked, for a file
             * this process sees, is a folder it cannot look into -
             * the permission answer. */
            report(rc == VLHE_ATTACH_MISSING
                   ? STR_CD_MSG_COULD_NOT_ATTACH_DENIED
                   : STR_CD_MSG_COULD_NOT_ATTACH_IMAGE);
            return;
        }
    }

    /* GENUINELY ABSENT. The likely cause is an image that moved,
     * which is what a stale recent list accumulates - so ask rather
     * than dropping it, because a path on removable media is absent
     * today and back tomorrow. */
    ask_forget(r);
    (void)msg;
}

static void on_picker_cancel(GtkWidget *w, gpointer data)
{
    picker_close((struct driverow *)data);
}

/* A recent image was chosen from the menu. Put it in the picker's
 * filename field rather than acting at once - the user still presses
 * OK, which is the confirmation the combo on the drive row lacked. */
static void on_recent_chosen(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;
    const char *path;

    if (r->picker == NULL)
        return;

    path = (const char *) gtk_object_get_user_data(GTK_OBJECT(w));
    if (path == NULL)
        return;

    gtk_file_selection_set_filename(GTK_FILE_SELECTION(r->picker), path);
}

/* The recent list, as a menu behind one button.
 *
 * A GtkOptionMenu WOULD LOOK LIKE A SETTING - a control showing a
 * current value - and this is an ACTION: "fill the field from one of
 * these". So it is a button that drops a menu, which is also what Xaw
 * gives directly (MenuButton + SimpleMenu) where it has no combo at
 * all. */
static GtkWidget *build_recent_menu(struct driverow *r)
{
    char (*paths)[VLHE_PATH_MAX] = r->recent_path;  /* C6: this row's */
    GtkWidget *opt;
    GtkWidget *menu;
    GtkWidget *item;
    int n, i;

    n = vlhe_recent(paths, VLHE_MAX_RECENT);

    opt = gtk_option_menu_new();
    menu = gtk_menu_new();

    if (n <= 0) {
        item = gtk_menu_item_new_with_label(STR_CD_MENU_NO_RECENT_IMAGES);
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);
    } else {
        /* THE FIRST ITEM IS A PROMPT, not a path, so the button does
         * not read as "this is the current selection" before anything
         * has been chosen. */
        item = gtk_menu_item_new_with_label(STR_CD_MENU_RECENT_IMAGES);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);

        for (i = 0; i < n; i++) {
            /* THE FILENAME IS THE LABEL, not the whole path: ten full
             * paths would make the menu wider than the dialog, and the
             * name is what identifies a disc - the same reasoning that
             * scrolls the entry to its end. */
            const char *base = strrchr(paths[i], '/');

            item = gtk_menu_item_new_with_label(base ? base + 1
                                                     : paths[i]);
            gtk_object_set_user_data(GTK_OBJECT(item), paths[i]);
            gtk_signal_connect(GTK_OBJECT(item), "activate",
                               GTK_SIGNAL_FUNC(on_recent_chosen), r);
            gtk_menu_append(GTK_MENU(menu), item);
            gtk_widget_show(item);
        }
    }

    gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
    return opt;
}

static void open_picker(struct driverow *r)
{
    GtkWidget *sel;
    GtkWidget *hbox;
    GtkWidget *label;
    char title[64];

    if (r->picker != NULL) {
        gdk_window_raise(r->picker->window);
        return;                 /* one at a time */
    }

    sprintf(title, STR_CD_TITLE_SELECT_DISC_IMAGE);
    sel = gtk_file_selection_new(title);
    vlhe_filesel_fit(sel);         /* a long path must not widen it */

    /* NO Create / Rename / Delete - see vlhe_mod_render.c's picker
     * for why. This one is a chooser too. */
    gtk_file_selection_hide_fileop_buttons(GTK_FILE_SELECTION(sel));
    r->picker = sel;
    /* CLEARED HOWEVER IT CLOSES. OK and Cancel NULL the pointer
     * themselves; the window manager's close button did not, and the
     * "one at a time" test above then raised a dead widget and this
     * row's Browse went dead with it - the same fault Preferences had
     * (vlhe_cc.c, open_prefs). */
    gtk_signal_connect(GTK_OBJECT(sel), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed), &r->picker);

    /* THE RECENT ROW, ABOVE THE FILE LIST. Packed at the START of the
     * dialog's own vbox so it reads as the shortcut it is, before the
     * longer route. */
    hbox = gtk_hbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(hbox), 6);

    label = gtk_label_new(STR_CD_LABEL_RECENTLY_USED);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
    gtk_widget_show(label);

    r->recent = build_recent_menu(r);
    gtk_box_pack_start(GTK_BOX(hbox), r->recent, TRUE, TRUE, 0);
    gtk_widget_show(r->recent);

    gtk_box_pack_start(GTK_BOX(GTK_FILE_SELECTION(sel)->main_vbox),
                       hbox, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(GTK_FILE_SELECTION(sel)->main_vbox),
                          hbox, 0);
    gtk_widget_show(hbox);

    /*
     * "Files of type", THE SAME WIDGET Render MIDI's picker HAS -
     * missing here until 2026-09-22, when the user tested the two
     * side by side: "Loads image fine but needs the filter".
     *
     * THE THREE PATTERNS ARE THE THREE BACKENDS. backend_cue.c,
     * backend_ccd.c and backend_iso.c are what can actually be
     * opened, so the list is not a guess about what a disc image
     * might be called - offering .nrg or .mds would invite a file
     * nothing here can read.
     *
     * `*.cue' AND `*.ccd' ARE THE DESCRIPTORS, NOT THE DATA. A cue
     * sheet names its .bin and a CloneCD .ccd names its .img, so the
     * file to PICK is the small text one and the big binary beside it
     * is not separately selectable. That is why .bin and .img are
     * absent from the list rather than overlooked - picking one would
     * fail, and a filter that shows unopenable files is worse than
     * none.
     *
     * `;' SEPARATES THE THREE - vlhe_filter.c's matcher has no
     * character classes, so `*.[ci]*' would match nothing silently.
     * That was written here first and caught by a host test.
     *
     * DEFAULTS TO THE IMAGE TYPES, NOT TO "All files". Render MIDI
     * starts on All because a user may legitimately render a .mid
     * with any name; here the three extensions are the whole world of
     * what works, and a filter the user has to go and switch on is a
     * filter most will never find.
     */
    {
        static const struct vlhe_filter image_types[] = {
            { STR_CD_FILTER_IMAGES,
                                       "*.cue;*.ccd;*.iso" },
            { STR_CD_FILTER_CUE,                "*.cue"     },
            { STR_CD_FILTER_CCD,                   "*.ccd"     },
            { STR_CD_FILTER_ISO,                "*.iso"     },
            { STR_CD_FILTER_ALL,                     NULL        },
            { NULL, NULL }
        };

        vlhe_filter_attach(sel, image_types, 0);
    }

    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->ok_button),
                       "clicked", GTK_SIGNAL_FUNC(on_picker_ok), r);
    gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(sel)->cancel_button),
                       "clicked", GTK_SIGNAL_FUNC(on_picker_cancel), r);

    gtk_widget_show(sel);
}

static void on_attach(GtkWidget *w, gpointer data)
{
    struct driverow *r = (struct driverow *)data;

    (void)w;

    open_picker(r);
}

/* ------------------------------------------------------------------ */

static void build_row(struct driverow *r, GtkWidget *parent)
{
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *controls;

    /* EACH DRIVE IS ITS OWN NESTED FRAME - the user's suggestion, and
     * it solves the problem the two-line row only postponed.
     *
     * THE PATH IS UNBOUNDED AND EVERYTHING ELSE IS NOT. On one line it
     * shared width with Lock, Image... and Eject, so a long path ran
     * into them; the two-line version gave it more room but still
     * capped it at the window minus three buttons. A frame per drive
     * gives the path A LINE OF ITS OWN at the full pane width, with
     * the controls on a separate line beneath, so nothing competes
     * with it.
     *
     * THE FRAME'S LABEL CARRIES THE DEVICE NAME, which also retires the
     * fixed 88px column the name used to need - a GtkFrame draws its
     * label in the border, so the name costs no layout space at all.
     *
     * GTK 1.2 NESTS FRAMES FREELY, and the stock theme's ETCHED_IN
     * gives an inner frame a lighter groove than the outer one, so the
     * hierarchy reads without any styling from us. */
    frame = gtk_frame_new("");
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    r->frame = frame;

    vbox = gtk_vbox_new(FALSE, 2);
    gtk_container_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* ---- the path: a READ-ONLY GtkEntry, not a label ------------- */

    /* AN ENTRY SOLVES WHAT THREE ROUNDS OF TRUNCATION DID NOT, and the
     * user found the reference for it on their own desktop: KsCD's
     * CDDB-server field holds a string wider than the box and shows
     * the END of it, scrolled with the cursor, no scrollbar.
     *
     * WHY IT IS RIGHT HERE. The path is a VALUE, not a caption - an
     * entry says so visually, and it gives three things a truncated
     * label cannot: the full text is always present, it can be
     * selected and copied, and Home/End/arrows navigate it.
     *
     * AND IT DELETES A WHOLE MECHANISM. A `path_fit' helper measured
     * the string with gdk_string_width() against the frame's
     * allocation and dropped leading directory components until it
     * fitted. It WORKED, and it was still wrong: the allocation is 1
     * before layout, so the first draw showed the full path, the pane
     * grew, a horizontal scrollbar appeared, and Eject and Image...
     * were off-screen for THREE SECONDS until two poll cycles had run.
     * The user watched it happen. An entry needs no measurement, no
     * font metrics, no precomputed budget and no size-allocate
     * handler - it simply holds the string and shows what fits.
     *
     * NOT EDITABLE: the path is displayed, and Image... is how it
     * changes. Selectable and scrollable all the same. */
    /* A READ-ONLY GtkEntry - IT ONLY EVER SHOWS WHAT IS ATTACHED.
     *
     * A GtkCombo WAS TRIED HERE AND REVERTED, 2026-09-17. Putting the
     * recent list behind this entry conflated two different jobs -
     * DISPLAYING the attached path and CHOOSING a new one - so a
     * selection acted immediately, with no confirmation and no way to
     * browse for anything else.
     *
     * AND IT CRASHED. Selecting fired "changed" on the entry, the
     * handler attached and then rebuilt the list with
     * gtk_combo_set_popdown_strings - while the combo's popup was
     * still being dismissed. Rebuilding a combo's list from inside its
     * own selection handler destroys the widget that is mid-signal.
     * r->setting did not help: the reentry is through GTK's internals,
     * not through our code.
     *
     * THE RECENT LIST LIVES IN THE IMAGE PICKER INSTEAD (the user's
     * call), where choosing is what the dialog is FOR - see
     * open_picker(). One place loads a disc, nothing acts on a stray
     * click, and the daemon lifecycle happens in one code path.
     *
     * IT ALSO PORTS BETTER: Xaw has no combo at all, where a
     * MenuButton + SimpleMenu inside a dialog is a direct equivalent
     * of what the picker needs. */
    r->image = gtk_entry_new();
    gtk_entry_set_editable(GTK_ENTRY(r->image), FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), r->image, FALSE, FALSE, 0);

    /* ---- what the disc is --------------------------------------- */

    r->info = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(r->info), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), r->info, FALSE, FALSE, 0);

    /* ---- the controls, on their own line ------------------------ */

    controls = gtk_hbox_new(FALSE, 6);

    /*
     * NO LOCK BOX - REMOVED 2026-10-05 (design/54 D64, the user: "Lets
     * remove it as you proposed"). 2.2's lock is ONE flag for every
     * CD-ROM (cdrom.c:260), so a box drawn per drive locked them all,
     * the real drive included, and outlived the Unload; the box showed
     * vdisc's per-drive flag, which the kernel's own open/close also
     * writes, so it could toggle under the user. And what it was for is
     * already the kernel's: CDROMEJECT is refused while anything holds
     * the drive (`use_count != 1', cdrom.c:1466), so a mounted or
     * playing disc cannot be ejected with or without it.
     */

    /* LOAD AT STARTUP - per-drive state rather than an action, set
     * once.
     *
     * "LOAD AT STARTUP", NOT "AUTO-MOUNT" - the mockup's word, and the
     * wrong one. Mounting is /etc/fstab's job; this only decides
     * whether the DISC IS IN THE DRIVE when vdiscd next comes up. A
     * user who wants it mounted writes an fstab line, the same as for
     * a real drive.
     *
     * AND IT IS THE ONE THING ON THIS TAB THAT IS NOT IMMEDIATE. The
     * setting is stored at once; the effect is at next start. The
     * status line says so rather than leaving the user to wonder
     * whether it did anything. */
    r->autoload = vlhe_tipped(gtk_check_button_new_with_label(STR_CD_CHECK_LOAD_STARTUP), STR_CD_CHECK_LOAD_STARTUP_TIP);
    gtk_box_pack_start(GTK_BOX(controls), r->autoload, FALSE, FALSE, 0);

    /* THE CD+G VIEWER'S DRIVE - one radio group across every row, so
     * exactly one drive is the viewer's. Shown by the refresh only
     * when there is more than one drive. */
    r->cdg = vlhe_tipped(gtk_radio_button_new_with_label(g_cdg_group, STR_CD_RADIO_CDG_DRIVE), STR_CD_RADIO_CDG_DRIVE_TIP);
    g_cdg_group = gtk_radio_button_group(GTK_RADIO_BUTTON(r->cdg));
    gtk_box_pack_start(GTK_BOX(controls), r->cdg, FALSE, FALSE, 0);

    r->cdrom = vlhe_tipped(gtk_radio_button_new_with_label(g_cdrom_group, STR_CD_RADIO_CDROM), STR_CD_RADIO_CDROM_TIP);
    g_cdrom_group = gtk_radio_button_group(GTK_RADIO_BUTTON(r->cdrom));
    gtk_box_pack_start(GTK_BOX(controls), r->cdrom, FALSE, FALSE, 0);

    /* PUSHED RIGHT, away from the checkbox, so a click on Eject cannot
     * be a near-miss for Lock. */
    r->eject = vlhe_tipped(gtk_button_new_with_label(STR_CD_BTN_EJECT), STR_CD_BTN_EJECT_TIP);
    gtk_box_pack_end(GTK_BOX(controls), r->eject, FALSE, FALSE, 0);

    r->attach = vlhe_tipped(gtk_button_new_with_label(STR_CD_BTN_IMAGE), STR_CD_BTN_IMAGE_TIP);
    gtk_box_pack_end(GTK_BOX(controls), r->attach, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), controls, FALSE, FALSE, 0);

    gtk_signal_connect(GTK_OBJECT(r->autoload), "toggled",
                       GTK_SIGNAL_FUNC(on_autoload_toggled), r);
    gtk_signal_connect(GTK_OBJECT(r->cdg), "toggled",
                       GTK_SIGNAL_FUNC(on_cdg_toggled), r);
    gtk_signal_connect(GTK_OBJECT(r->cdrom), "toggled",
                       GTK_SIGNAL_FUNC(on_cdrom_toggled), r);
    gtk_signal_connect(GTK_OBJECT(r->attach), "clicked",
                       GTK_SIGNAL_FUNC(on_attach), r);
    gtk_signal_connect(GTK_OBJECT(r->eject), "clicked",
                       GTK_SIGNAL_FUNC(on_eject), r);

    gtk_widget_show(r->image);
    gtk_widget_show(r->info);
    gtk_widget_show(r->autoload);
    gtk_widget_show(r->attach);
    gtk_widget_show(r->eject);
    gtk_widget_show(controls);
    gtk_widget_show(vbox);

    r->row = frame;
    gtk_box_pack_start(GTK_BOX(parent), frame, FALSE, FALSE, 0);
}

static GtkWidget *build_drive(void)
{
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *inner;
    int i, n;

    outer = gtk_vbox_new(FALSE, 0);

    frame = gtk_frame_new(STR_SHELL_REVIEW_DRIVES);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, TRUE, TRUE, 0);

    inner = gtk_vbox_new(FALSE, 4);
    gtk_container_border_width(GTK_CONTAINER(inner), 8);
    gtk_container_add(GTK_CONTAINER(frame), inner);

    g_rows_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(inner), g_rows_box, FALSE, FALSE, 0);

    /* AS MANY ROWS AS THE MACHINE ADVERTISES, asked at build time.
     * VLHE_MAX_DRIVE is the ceiling the array is sized to; the count
     * is what gets drawn. */
    n = vlhe_drive_count();
    if (n < 1)
        n = 1;                  /* room for one, so an unloaded module
                                 * still has a shape to show */
    if (n > VLHE_MAX_DRIVE)
        n = VLHE_MAX_DRIVE;
    g_nrow = n;

    for (i = 0; i < g_nrow; i++) {
        memset(&g_row[i], 0, sizeof g_row[i]);
        build_row(&g_row[i], g_rows_box);
    }

    /* THE NOTE ABOUT LOCKING WENT WITH THE LOCK BOX (design/54 D64) -
     * and with it the line that, at four drives, made this page taller
     * than the pane and gave it both scrollbars (the user, 2026-10-05). */

    gtk_widget_show(g_rows_box);
    gtk_widget_show(inner);
    gtk_widget_show(frame);

    g_nomodule = gtk_label_new(STR_CD_LABEL_VDISC_MODULE_NOT_LOADED);
    gtk_box_pack_start(GTK_BOX(outer), g_nomodule, TRUE, TRUE, 0);

    /*
     * AND A SECOND ABSENT STATE: THE MODULE IS IN BUT THE DAEMON IS
     * NOT. Added 2026-09-21 after the user met it on 86Box.
     *
     * The module gives you drives; vdiscd is what OPENS an image, so
     * with the module loaded and the daemon down the page showed a
     * full set of working-looking controls where every Image... ended
     * in failure. Worse, it then offered to forget the image (see
     * on_picker_ok), because vlhe_attach() cannot distinguish a
     * missing file from a missing daemon.
     *
     * THAT STATE IS NOT EXOTIC - it is what a stopped or
     * half-completed `vlhe apply' leaves, which is exactly when
     * someone opens this page to find out what is wrong.
     */
    g_nodaemon = gtk_label_new(
        STR_CD_LABEL_VDISC_MODULE_LOADED_BUT);
    gtk_box_pack_start(GTK_BOX(outer), g_nodaemon, FALSE, FALSE, 8);

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */

/* The Options tab: vdisc's module parameters.
 *
 * REPLACES THE MOCKUP'S "AUDIO" TAB, whose fields described nothing
 * vdisc offers - "audio source: digital passthrough / WAV
 * substitution" when CD audio is simply read from the image and mixed
 * by vsound, with no second route to choose between.
 *
 * WHAT BELONGS HERE INSTEAD is what design/09 says: the settings that
 * need root and a reload. The user's reference for the shape is KDE's
 * own Font settings panel - a labelled value with a spin box beside
 * it.
 *
 * NAMED "Options" TO MATCH VOLUME'S SECOND TAB, at the user's
 * instruction, and the word means the same thing in both: the things
 * you configure, as against the things you act on.
 */

static void modopts_refresh(void);
static int major_selection_unusable(void);
static GtkWidget *build_major_menu(int which, int current);
static void (*g_cd_dirty_cb)(void);   /* the shell's sidebar marker */

/*
 * CAN THIS SESSION CHANGE A MACHINE SETTING? design/49 N7.
 *
 * vlhe_can_administer() rests on access(), which on 2.2 tests the
 * REAL uid (fs/open.c:295). In the setuid build that is never 0, so
 * after Modify these controls could never be enabled.
 * vlhe_priv_can_act() is the effective question; either suffices.
 */
static int
page_admin(void)
{
    return vlhe_can_administer() || vlhe_priv_can_act();
}

static void major_menus_sync(int admin, int mimic);
static void on_mimic_toggled(GtkWidget *w, gpointer data);


/* A spin button changed. Nothing is written until Apply - this is a
 * FORM, unlike the Drive tab. */
/* THE TAB SAYS SO TOO - 2026-10-01. The Sound page renames its tab
 * "Options *" on the first edit (vlhe_mod_sound.c); this page only
 * told the sidebar, so a user who changed Drives saw nothing on the
 * page itself and went to Load - caught by the unsaved-settings
 * dialog, which was the net doing the tab's job. */
static void opt_tab_mark(int dirty)
{
    GtkWidget *page;

    if (g_notebook == NULL)
        return;
    page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), PAGE_OPTIONS);
    if (page != NULL)
        gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(g_notebook), page,
                                        dirty ? STR_CD_TAB_OPTIONS_DIRTY : STR_CD_TAB_OPTIONS);
}

static void on_opt_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;

    if (g_opt_setting)
        return;

    /* THE FLAG BEFORE THE CALLBACK - the sidebar's marker reads
     * cd_dirty() to decide whether this row gets its star, so calling
     * it first (the C6 rewrite of 2026-10-01 did) drew the tab's star
     * and not the row's; the row caught up only when another page
     * dirtied. Seen on 86Box the same day.
     *
     * AND THE LABEL ON EVERY EDIT, THE FIRST INCLUDED - design/47 C6.
     * The first edit used to return after marking the sidebar, so the
     * status line under the controls caught up only on the second. */
    if (!g_opt_dirty) {
        g_opt_dirty = 1;
        opt_tab_mark(1);
        if (g_cd_dirty_cb != NULL)
            g_cd_dirty_cb();
    }
    modopts_refresh();
}

/* Build one major menu from the backend's filtered list.
 *
 * IN-USE ENTRIES ARE SHOWN AND DISABLED, not hidden. A missing option
 * is a puzzle - "why can I not pick 25?" - where a greyed one with its
 * reason beside it is an answer. It is also the only channel that
 * carries the information: GTK 1.2 has no accessibility layer, so what
 * is not on screen does not exist. */
static GtkWidget *build_major_menu(int which, int current)
{
    struct vlhe_major mj[VLHE_MAX_MAJORS];
    GtkWidget *opt;
    GtkWidget *menu;
    GtkWidget *item;
    int n, k, sel;

    n = vlhe_majors(which, mj, VLHE_MAX_MAJORS);

    opt = gtk_option_menu_new();
    menu = gtk_menu_new();

    /* OPEN ON THE FIRST AVAILABLE ENTRY, NOT THE FIRST ENTRY.
     *
     * A REAL BUG THE USER CAUGHT: the impersonation list starts at 25,
     * 25 was in use on the test machine, and ticking the checkbox
     * therefore selected a major that CANNOT BE USED - and marked the
     * form dirty, so Apply would have committed it.
     *
     * gtk_option_menu_set_history() does not care whether the item it
     * selects is sensitive, so "index 0" is not a safe default for a
     * list that is filtered. Find the first one that is. */
    sel = -1;
    for (k = 0; k < n; k++) {
        if (!mj[k].in_use) {
            sel = k;
            break;
        }
    }
    /* EVERY ONE TAKEN is possible - a machine with several proprietary
     * CD-ROM drivers loaded could use up the impersonation list. Fall
     * back to index 0 so the widget has a valid history, and let
     * modopts_refresh() say the selection is unusable rather than
     * pretending it is fine. A menu where nothing can be chosen needs
     * to SAY so; greying it alone would read as "not available yet". */
    if (sel < 0)
        sel = 0;

    for (k = 0; k < n; k++) {
        char lab[96];

        if (mj[k].in_use)
            sprintf(lab, FMT_CD_USE, mj[k].num, mj[k].what);
        else
            sprintf(lab, "%d - %.60s", mj[k].num, mj[k].what);

        item = gtk_menu_item_new_with_label(lab);
        if (mj[k].in_use)
            gtk_widget_set_sensitive(item, FALSE);
        gtk_object_set_user_data(GTK_OBJECT(item),
                                 (gpointer)(long) mj[k].num);
        gtk_signal_connect(GTK_OBJECT(item), "activate",
                           GTK_SIGNAL_FUNC(on_opt_changed), NULL);
        gtk_menu_append(GTK_MENU(menu), item);
        gtk_widget_show(item);

        /* AN EXPLICIT CURRENT VALUE WINS over the first-available
         * default, but only if it is actually selectable. */
        if (mj[k].num == current && !mj[k].in_use)
            sel = k;
    }

    gtk_option_menu_set_menu(GTK_OPTION_MENU(opt), menu);
    gtk_option_menu_set_history(GTK_OPTION_MENU(opt), sel);
    return opt;
}

/* One menu live, the other greyed. Neither is live without root. */
static void major_menus_sync(int admin, int mimic)
{
    (void) admin;               /* live for everyone since 2026-10-01 */
    if (g_opt_major != NULL)
        gtk_widget_set_sensitive(g_opt_major, !mimic);
    if (g_opt_mimic_menu != NULL)
        gtk_widget_set_sensitive(g_opt_mimic_menu, mimic);
}

static void on_mimic_toggled(GtkWidget *w, gpointer data)
{
    (void)data;

    if (g_opt_setting)
        return;

    /* THE UNCHOSEN MENU KEEPS ITS SELECTION rather than resetting -
     * someone toggling back and forth to re-read the warning should
     * not lose what they picked. */
    major_menus_sync(page_admin(),
                     GTK_TOGGLE_BUTTON(w)->active);

    g_opt_dirty = 1;
    modopts_refresh();
}

/* Is the live menu's selection one that cannot be used?
 *
 * ASKED OF THE WIDGET rather than tracked alongside it: the menu's
 * history is the truth about what is selected, and a second copy of
 * that state is a second thing to keep in step. */
static int major_selection_unusable(void)
{
    struct vlhe_major mj[VLHE_MAX_MAJORS];
    GtkWidget *live;
    GtkWidget *menu;
    GtkWidget *active;
    int n, k, num, which;

    if (g_opt_mimic == NULL)
        return 0;

    if (GTK_TOGGLE_BUTTON(g_opt_mimic)->active) {
        live = g_opt_mimic_menu;
        which = VLHE_MAJ_MIMIC;
    } else {
        live = g_opt_major;
        which = VLHE_MAJ_LOCAL;
    }
    if (live == NULL)
        return 0;

    menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(live));
    if (menu == NULL)
        return 0;

    active = gtk_menu_get_active(GTK_MENU(menu));
    if (active == NULL)
        return 0;

    num = (int)(long) gtk_object_get_user_data(GTK_OBJECT(active));

    n = vlhe_majors(which, mj, VLHE_MAX_MAJORS);
    for (k = 0; k < n; k++)
        if (mj[k].num == num)
            return mj[k].in_use;

    return 0;
}

static void modopts_refresh(void)
{
    struct vlhe_modopts m;
    char msg[160];
    int needs_reload;

    if (vlhe_modopts(&m) != 0)
        return;

    /* THE CONFIGURED VALUE AGAINST THE RUNNING ONE. They differ after
     * an Apply, and until the module is reloaded - which is the state
     * a user needs told about, because nothing else on screen would
     * show it. */
    /* PACKET IS NOT COMPARED: the running module does not report it
     * (packet_applied is always -1, vlhe_backend.h). Comparing it
     * against 0 was half of why this said "Reload vdisc" for ever -
     * design/47 C1. */
    needs_reload = m.loaded &&
        (m.ndevs != m.ndevs_applied ||
         m.major != m.major_applied ||
         (m.packet_applied >= 0 && m.packet != m.packet_applied));

    if (major_selection_unusable())
        strcpy(msg, FMT_CD_MAJOR_ALREADY_USE_PICK);
    else if (!m.loaded)
        strcpy(msg, FMT_CD_VDISC_NOT_LOADED_THESE);
    else if (needs_reload)
        sprintf(msg, FMT_CD_MODULE_RUNNING_DRIVE_RELOAD,
                m.ndevs_applied, m.ndevs_applied == 1 ? "" : STR_CD_TEXT_PLURAL_S);
    else if (g_opt_dirty)
        strcpy(msg, FMT_CD_NOT_APPLIED_YET);
    else
        strcpy(msg, FMT_CD_THESE_MATCH_RUNNING_MODULE);

    gtk_label_set_text(GTK_LABEL(g_opt_status), msg);
}

static GtkWidget *build_options(void)
{
    struct vlhe_modopts m;
    GtkWidget *outer;
    GtkWidget *frame;
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *w;
    GtkObject *adj;
    int admin;

    vlhe_modopts(&m);
    admin = page_admin();

    outer = gtk_vbox_new(FALSE, 0);

    frame = gtk_frame_new(STR_MID_LABEL_DRIVER);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_border_width(GTK_CONTAINER(frame), 8);
    gtk_box_pack_start(GTK_BOX(outer), frame, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* ---- the drive count ----------------------------------------- */

    hbox = gtk_hbox_new(FALSE, 6);

    w = gtk_label_new(STR_CD_LABEL_VIRTUAL_DRIVES);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.5);
    gtk_widget_set_usize(w, 150, -1);
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    /* A SPIN BUTTON, which is the widget in the user's reference (KDE's
     * font-size field). 1 to 8 - VDISC_MIN/MAX_NDEVS - with no wrap,
     * because 8 to 1 in one click would be a surprise rather than a
     * convenience. */
    adj = gtk_adjustment_new((gfloat) m.ndevs,
                             (gfloat) VLHE_MIN_NDEVS,
                             (gfloat) VLHE_MAX_NDEVS,
                             1.0, 1.0, 0.0);
    g_opt_ndevs = gtk_spin_button_new(GTK_ADJUSTMENT(adj), 0.0, 0);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(g_opt_ndevs), FALSE);
    /* LIVE FOR EVERYONE - the 2026-10-01 decision; see the Sound
     * page's build_device(). The note at the end says what needs root. */
    (void) admin;
    /* THE ARROWS NEED A SHADOW - gtkspinbutton.c:324 defaults
     * shadow_type to GTK_SHADOW_NONE and its paint function draws the
     * panel's box only when it is not NONE, so the arrows sit on bare
     * background. Same one-liner as the MIDI page's labelled_row();
     * this page builds its own rows, so it needs its own copy. */
    gtk_spin_button_set_shadow_type(GTK_SPIN_BUTTON(g_opt_ndevs),
                                    GTK_SHADOW_IN);
    gtk_signal_connect(adj, "value_changed",
                       GTK_SIGNAL_FUNC(on_opt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_opt_ndevs, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_ndevs);

    w = gtk_label_new(STR_CD_LABEL_DEV_VDISC0_UPWARDS);
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    /* ---- the block major: TWO MENUS, ONE LIVE -------------------- */

    /* BOTH ARE ALWAYS VISIBLE and the checkbox decides which is
     * enabled - the user's design, and it says something a single
     * swapped menu could not: the greyed-out top one keeps stating
     * "this is the normal choice, and you have moved away from it".
     *
     * A FLAT LIST OF FOURTEEN WAS TRIED FIRST and mixed the two kinds
     * together, which invites picking a real driver's major by
     * scrolling rather than by deciding. */

    hbox = gtk_hbox_new(FALSE, 6);

    w = gtk_label_new(STR_CD_LABEL_PRESENT_DRIVE_AS);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.5);
    gtk_widget_set_usize(w, 150, -1);
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    /* THE 60-63 NOTE IS THE MENU'S TOOLTIP since 2026-10-03 - the user:
     * "Doing that will give us one line to add the two option at the
     * bottom". It sat on a line of its own under this row. */
    g_opt_major = vlhe_tipped(build_major_menu(VLHE_MAJ_LOCAL, m.major),
                              STR_CD_LABEL_60_63_KERNEL_S);
    /* FALSE/FALSE - the menu takes its natural width.
     *
     * STRETCHED, THE CLICK TARGET DID NOT MATCH THE DRAWN BUTTON (the
     * user, 2026-09-17): a GtkOptionMenu given more room than it asked
     * for draws its frame at the allocated width but behaves as though
     * it were still its natural size, so part of the visible button
     * does not respond. Left over from pairing the two menus by width;
     * the checkbox between them already makes the pairing obvious. */
    gtk_box_pack_start(GTK_BOX(hbox), g_opt_major, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_major);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    /* ---- the opt-in ---------------------------------------------- */

    g_opt_mimic = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CD_CHECK_IMPERSONATE_PERIOD_CD_ROM), STR_CD_CHECK_IMPERSONATE_PERIOD_CD_ROM_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_mimic),
                                 m.mimic ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_opt_mimic), "toggled",
                       GTK_SIGNAL_FUNC(on_mimic_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_opt_mimic, FALSE, FALSE, 4);
    gtk_widget_show(g_opt_mimic);

    /* THE WARNING NAMES WHO NEEDS IT AND WHO DOES NOT, which is the
     * user's wording and is more useful than a bare caution: someone
     * reading it can tell whether it applies to them. cdda2wav works
     * at any major, which is the fact that makes this optional at
     * all. */
    /* THE USER'S WORDING, and three things in it are deliberate.
     *
     * "TO OUR KNOWLEDGE" - because that is the truth. cdda2wav, KsCD
     * and grip were each MEASURED working at our own major; nothing
     * surveyed every program on the platform, and a flat "only
     * cdparanoia needs this" claims more than was tested.
     *
     * THE PROGRAMS ARE NAMED rather than described as "everything
     * else", so a reader can tell whether their case is covered.
     *
     * AND THE FUTURE DRIVE IS THE CASE THE FIRST VERSION MISSED. The
     * collision is not only against hardware present today: someone
     * whose machine is clear picks 25, installs a Matsushita drive a
     * year later, and it will not load - with nothing pointing back
     * at this checkbox. That is the worst version of the failure,
     * because the cause is not just far away in the system, it is far
     * away in TIME. */
    /* WRAPPED TO THE MEASURED WIDTH, not to a guess. The frame's
     * interior ends at x=768 and the longest line reached x=600 - 168
     * pixels spare, about 24 characters - so the same text folds from
     * seven lines into five and the tab stops needing a vertical
     * scroll. Measured off a capture rather than estimated, because
     * two labels in this module were already clipped by arithmetic
     * against a proportional font. */
    w = gtk_label_new(
        STR_CD_LABEL_OUR_KNOWLEDGE_ONLY_CDPARANOIA);
    gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.0);
    /* 580 - the widest of four being tested in one boot;
     * see build_device() in vlhe_mod_sound.c. */
    gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
    gtk_widget_set_usize(w, 580, -1);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    hbox = gtk_hbox_new(FALSE, 6);

    w = gtk_label_new(STR_CD_LABEL_IMPERSONATE_AS);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.5);
    gtk_widget_set_usize(w, 150, -1);
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    g_opt_mimic_menu = build_major_menu(VLHE_MAJ_MIMIC, m.major);
    gtk_box_pack_start(GTK_BOX(hbox), g_opt_mimic_menu, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_mimic_menu);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

    /* WHICHEVER IS NOT CHOSEN IS GREYED, decided once here and by the
     * toggle thereafter. */
    major_menus_sync(admin, m.mimic);

    /* ---- the packet capability ------------------------------------ */

    g_opt_packet = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CD_CHECK_ANSWER_MMC_PACKET_COMMANDS), STR_CD_CHECK_ANSWER_MMC_PACKET_COMMANDS_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_packet),
                                 m.packet ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_opt_packet), "toggled",
                       GTK_SIGNAL_FUNC(on_opt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_opt_packet, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_packet);

    w = gtk_label_new(
        STR_CD_LABEL_NEEDED_RIP_AUDIO_CDPARANOIA);
    gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.0);
    /* 580 - the widest of four being tested in one boot;
     * see build_device() in vlhe_mod_sound.c. */
    gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
    gtk_widget_set_usize(w, 580, -1);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    /* ---- reattaching at startup --------------------------------------
     *
     * THE MASTER SWITCH FOR THE DRIVE ROWS' "Load at startup" BOXES -
     * [CD Settings] DrivesAutoLoad, vdiscd -r. Added 2026-10-06 (the
     * user, "3 fix"): those boxes' tip sent users to setup-vlhe for it,
     * and the GUI does not send users to the command line - so it gets
     * the control itself. A machine setting like everything above, live
     * for everyone; the note at the end says what needs root.
     */
    g_opt_reattach = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CD_CHECK_REATTACH_DRIVES), STR_CD_CHECK_REATTACH_DRIVES_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_reattach),
                                 m.drives_autoload ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_opt_reattach), "toggled",
                       GTK_SIGNAL_FUNC(on_opt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_opt_reattach, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_reattach);

    /* ---- /dev/cdrom ------------------------------------------------
     *
     * OFF BY DEFAULT AND SEPARATE FROM EVERYTHING ABOVE, because it
     * is the only control on this page that changes a path OUTSIDE
     * anything we own.
     *
     * COREL SHIPS /dev/cdrom POINTING AT THE REAL DRIVE - measured,
     * a symlink to /dev/hdb on a stock install - and KsCD hardcodes
     * that path. So this takes the link over rather than supplying a
     * missing one, which is why the label says "point" and the note
     * says what happens on unload.
     */
    g_opt_cdromlink = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CD_CHECK_POINT_DEV_CDROM), STR_CD_CHECK_POINT_DEV_CDROM_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_cdromlink),
                                 m.link_cdrom ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_opt_cdromlink), "toggled",
                       GTK_SIGNAL_FUNC(on_opt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_opt_cdromlink, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_cdromlink);

    w = gtk_label_new(
        STR_CD_LABEL_CD_PLAYERS_RIPPERS_LOOK);
    gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.0);
    /* 580 - the widest of four being tested in one boot;
     * see build_device() in vlhe_mod_sound.c. */
    gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
    gtk_widget_set_usize(w, 580, -1);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);

    /* ---- the CD+G viewer's two rules, on the line the 60-63 note gave
     * up (2026-10-03) - the user's own files, so never greyed: a
     * non-root user may set them. */
    hbox = gtk_hbox_new(FALSE, 6);
    w = gtk_label_new(STR_CD_LABEL_CDG_VIEWER);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.5);
    gtk_widget_set_usize(w, 150, -1);
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 0);
    gtk_widget_show(w);
    g_opt_cdg_follow = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CD_CHECK_CDG_FOLLOW_ANY), STR_CD_CHECK_CDG_FOLLOW_ANY_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_cdg_follow),
                                 vlhe_cdg_follow_any() ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_opt_cdg_follow), "toggled",
                       GTK_SIGNAL_FUNC(on_opt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_opt_cdg_follow, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_cdg_follow);
    g_opt_cdg_stop = vlhe_tipped(gtk_check_button_new_with_label(
        STR_CD_CHECK_CDG_STOP_ON_SWAP), STR_CD_CHECK_CDG_STOP_ON_SWAP_TIP);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_cdg_stop),
                                 vlhe_cdg_stop_on_swap() ? TRUE : FALSE);
    gtk_signal_connect(GTK_OBJECT(g_opt_cdg_stop), "toggled",
                       GTK_SIGNAL_FUNC(on_opt_changed), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), g_opt_cdg_stop, FALSE, FALSE, 0);
    gtk_widget_show(g_opt_cdg_stop);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 4);
    gtk_widget_show(hbox);

    /* ---- what state these are in ---------------------------------- */

    g_opt_status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_opt_status), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_opt_status, FALSE, FALSE, 4);
    gtk_widget_show(g_opt_status);

    gtk_widget_show(vbox);
    gtk_widget_show(frame);

    /* ---- and why they may not be editable ------------------------- */

    {
        /* SAID, NOT JUST GREYED. Three greyed controls with no
         * explanation is a puzzle; GTK 1.2 has no accessibility layer,
         * so text on screen is the only channel that carries this at
         * all.
         *
         * BUILT ALWAYS AND SHOWN ONLY WHEN TRUE, so Modify can hide it
         * without rebuilding the page (cd_privilege_changed). */
        w = gtk_label_new(vlhe_priv_can_unlock()
            ? STR_SND_TEXT_NEEDS_ROOT_MODIFY
            : STR_SND_TEXT_NEEDS_ROOT_RUN_AS_ROOT);
        gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.0);
        /* 580 - the widest of four being tested in one boot;
         * see build_device() in vlhe_mod_sound.c. */
        gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
        gtk_widget_set_usize(w, 580, -1);
        gtk_box_pack_start(GTK_BOX(outer), w, FALSE, FALSE, 8);
        g_root_note = w;
        if (!admin)
            gtk_widget_show(w);
    }

    modopts_refresh();

    gtk_widget_show(outer);
    return outer;
}

/* ------------------------------------------------------------------ */

static void on_switch_page(GtkNotebook *nb, GtkNotebookPage *pg,
                           guint page, gpointer data)
{
    (void)nb; (void)pg; (void)data;

    g_page = (int)page;
    if (g_page_cb != NULL)
        g_page_cb(g_page == PAGE_OPTIONS);
}

/*
 * READ THE WIDGETS INTO THE BACKEND. Apply and OK call this.
 *
 * Added 2026-09-19 with MIDI's and Volume's - sound_collect() was
 * the only one, so Apply reported "No changes to save" on every
 * other page however much had been changed.
 */
/* THE DIRTY BIT WAS KEPT AND NEVER PUBLISHED. g_opt_dirty has been
 * set by on_opt_changed() all along, but nothing could read it, so
 * the shell's sidebar and the quit guard did not know this page had
 * unsaved edits (the user, 2026-09-19: "CD settings no dirty bit").
 *
 * KNOWN AND NOT ADDRESSED HERE: changing a value and changing it back
 * leaves this set, because it tracks THAT an edit happened rather
 * than comparing against the saved value. The user has called that
 * acceptable for now. Fixing it means holding a copy of the saved
 * state per control and comparing on every change - worth doing when
 * the false "unsaved changes" prompt actually bites someone. */
int cd_dirty(void)
{
    return g_opt_dirty;
}

void cd_set_dirty_cb(void (*cb)(void))
{
    g_cd_dirty_cb = cb;
}

int cd_collect(void)
{
    struct vlhe_modopts m;
    GtkWidget          *menu;
    GtkWidget          *active;

    /* From the backend, so the _applied fields and `loaded' survive a
     * page that does not display them. */
    if (vlhe_modopts(&m) != 0)
        return -1;

    if (g_opt_ndevs != NULL)
        m.ndevs = gtk_spin_button_get_value_as_int(
                      GTK_SPIN_BUTTON(g_opt_ndevs));

    if (g_opt_packet != NULL)
        m.packet = GTK_TOGGLE_BUTTON(g_opt_packet)->active ? 1 : 0;
    if (g_opt_cdromlink != NULL)
        m.link_cdrom = GTK_TOGGLE_BUTTON(g_opt_cdromlink)->active ? 1 : 0;
    if (g_opt_reattach != NULL)
        m.drives_autoload = GTK_TOGGLE_BUTTON(g_opt_reattach)->active ? 1 : 0;

    if (g_opt_mimic != NULL)
        m.mimic = GTK_TOGGLE_BUTTON(g_opt_mimic)->active ? 1 : 0;

    /* THE MAJOR COMES FROM WHICHEVER MENU IS IN FORCE. The mimic list
     * is a second menu behind a checkbox, so reading the wrong one
     * would write a major the user cannot see. */
    menu = NULL;
    if (m.mimic && g_opt_mimic_menu != NULL)
        menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_opt_mimic_menu));
    else if (g_opt_major != NULL)
        menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(g_opt_major));

    active = (menu != NULL) ? gtk_menu_get_active(GTK_MENU(menu)) : NULL;
    if (active != NULL)
        m.major = (int)(long) gtk_object_get_user_data(GTK_OBJECT(active));

    if (vlhe_set_modopts(&m) != 0) {
        /* SAY WHAT - 2026-10-01. The setter refuses a major on neither
         * list, Drives outside 1..8, or a switch that is not 0/1; the
         * menus and the spin button cannot produce those, so this is a
         * hand-edited value - and it used to fail in silence. */
        char msg[160];

        sprintf(msg, FMT_CD_CD_SETTINGS_REFUSED_MAJOR,
                m.major, m.ndevs, VLHE_MAX_NDEVS);
        report(msg);
        return -1;
    }

    /* THE VIEWER'S TWO RULES - the user's file, beside the machine's
     * settings on this tab. The viewer may now be on another drive. */
    if (g_opt_cdg_follow != NULL)
        (void) vlhe_set_cdg_follow_any(
            GTK_TOGGLE_BUTTON(g_opt_cdg_follow)->active ? 1 : 0);
    if (g_opt_cdg_stop != NULL)
        (void) vlhe_set_cdg_stop_on_swap(
            GTK_TOGGLE_BUTTON(g_opt_cdg_stop)->active ? 1 : 0);
    cdg_drive_chosen();

    /*
     * SAVED, SO THE PAGE IS CLEAN - and this was MISSING, so the
     * star against CD Settings never went away once it appeared (the
     * user, 86Box 2026-09-21). g_opt_dirty was set in four places
     * and cleared in none.
     *
     * ONLY ON THE SUCCESS PATH: a collect that failed has committed
     * nothing, and the edits are still unsaved. Same rule as
     * midi_collect().
     */
    g_opt_dirty = 0;
    opt_tab_mark(0);
    return 0;
}

/* PUT THEM BACK - Cancel's half. */
void cd_reload(void)
{
    struct vlhe_modopts m;

    if (vlhe_modopts(&m) != 0)
        return;

    /*
     * SETTING WIDGETS IS NOT A USER EDIT. Without this every
     * set_value() below fires on_opt_changed and Cancel would leave
     * the page dirtier than it found it - the same guard
     * midi_reload() has had since it was written, and this function
     * was missing it.
     */
    g_opt_setting = 1;

    if (g_opt_ndevs != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_opt_ndevs),
                                  (gfloat) m.ndevs);
    if (g_opt_packet != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_packet),
                                     m.packet ? TRUE : FALSE);
    if (g_opt_cdromlink != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_cdromlink),
                                     m.link_cdrom ? TRUE : FALSE);
    if (g_opt_reattach != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_reattach),
                                     m.drives_autoload ? TRUE : FALSE);
    if (g_opt_cdg_follow != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_cdg_follow),
                                     vlhe_cdg_follow_any() ? TRUE : FALSE);
    if (g_opt_cdg_stop != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_cdg_stop),
                                     vlhe_cdg_stop_on_swap() ? TRUE : FALSE);
    if (g_opt_mimic != NULL)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_opt_mimic),
                                     m.mimic ? TRUE : FALSE);

    /*
     * THE MAJOR, BY VALUE - design/47 C3, 2026-09-30. This used to say
     * the menus were not restored because an INDEX against a rebuilt
     * list is how design/32 records the GUI picking an in-use major.
     * True of an index; so the item is found by the major it CARRIES
     * (every item has it as user_data - cd_collect() reads it that
     * way), in whichever menu is in force. Before this, Cancel put
     * every other control back and left the menu on the cancelled
     * major, which the next OK or Apply then wrote.
     */
    {
        GtkWidget *opt = (m.mimic && g_opt_mimic_menu != NULL)
                         ? g_opt_mimic_menu : g_opt_major;
        GtkWidget *menu = (opt != NULL)
                          ? gtk_option_menu_get_menu(GTK_OPTION_MENU(opt))
                          : NULL;
        GList     *l;
        int        i = 0;

        for (l = (menu != NULL) ? GTK_MENU_SHELL(menu)->children : NULL;
             l != NULL; l = l->next, i++) {
            if ((int)(long) gtk_object_get_user_data(GTK_OBJECT(l->data))
                == m.major) {
                gtk_option_menu_set_history(GTK_OPTION_MENU(opt), i);
                break;
            }
        }
    }

    g_opt_setting = 0;
    g_opt_dirty = 0;        /* Cancel restored them: nothing unsaved */
    opt_tab_mark(0);
}

int cd_page_wants_buttons(void)
{
    return g_page == PAGE_OPTIONS;
}

void cd_set_page(int page)
{
    if (g_notebook != NULL)
        gtk_notebook_set_page(GTK_NOTEBOOK(g_notebook), page);
}

GtkWidget *cd_build(void (*report_fn)(const char *),
                    void (*page_fn)(int))
{
    GtkWidget *nb;
    GtkWidget *tab;

    g_report = report_fn;
    g_page_cb = page_fn;

    nb = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_TOP);

    tab = gtk_label_new(STR_CD_LABEL_DRIVE);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_drive(), tab);

    tab = gtk_label_new(STR_MID_TAB_OPTIONS);
    gtk_widget_show(tab);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_options(), tab);

    gtk_signal_connect(GTK_OBJECT(nb), "switch_page",
                       GTK_SIGNAL_FUNC(on_switch_page), NULL);
    g_notebook = nb;

    refresh();
    gtk_timeout_add(POLL_MS, on_poll, NULL);

    return nb;
}

/*
 * THE MACHINE CHANGED. modopts_refresh() ran at build and when the
 * user edited an UNRELATED control - never after a Load - so the page
 * said "vdisc is not loaded" while grip held /dev/vdisc0 open
 * (design/36 row 44; the user: "may be even before grip was started",
 * and it was - it was wrong from the moment Load finished). The shell
 * calls this when the page is shown and after Load/Unload.
 */
void cd_machine(void)
{
    modopts_refresh();
}

/*
 * MODIFY WAS PRESSED - design/49 N7. Re-apply the sensitivity of the
 * five machine controls and the note, and nothing else: no value is
 * reloaded (see sound_privilege_changed()). The major menus follow
 * the mimic toggle's CURRENT state, not the saved one, so an edit
 * made before unlocking is respected.
 */
void
cd_privilege_changed(void)
{
    int admin = page_admin();

    /* Nothing to re-grey since 2026-10-01; the note comes and goes. */
    if (g_opt_mimic != NULL)
        major_menus_sync(admin, GTK_TOGGLE_BUTTON(g_opt_mimic)->active);
    if (g_root_note != NULL) {
        if (admin)
            gtk_widget_hide(g_root_note);
        else
            gtk_widget_show(g_root_note);
    }
}
